#!/usr/bin/env python3
import argparse
import fcntl
import hashlib
import json
import os
import shutil
import stat
import struct
import tempfile
import zlib
from pathlib import Path

import ota_release

# 整条迁移、部署链路准备好之前保持 False。
ACTIVATE = False

NONE = 0xffffffff
META_BYTES = 4096
CHUNK = 1024 * 1024
BY_NAME = Path("/dev/block/by-name")


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest_fd(fd, size):
    h = hashlib.sha256()
    offset = 0
    while offset < size:
        data = os.pread(fd, min(CHUNK, size - offset), offset)
        require(bool(data), "读取提前结束")
        h.update(data)
        offset += len(data)
    return h.hexdigest()


def decode(record):
    require(len(record) == META_BYTES, "元数据长度错误")
    require(record[:8] == b"AMPAB001", "元数据 magic 错误")
    require(struct.unpack_from("<II", record, 8) == (1, META_BYTES),
            "元数据版本错误")
    require(not any(record[40:4092]), "元数据保留字段非零")
    require(zlib.crc32(record[:4092]) ==
            struct.unpack_from("<I", record, 4092)[0],
            "元数据 CRC 错误")

    generation, confirmed, pending, tries, trial = struct.unpack_from(
        "<QIIII", record, 16
    )
    require(generation > 0 and confirmed in (0, 1),
            "元数据状态错误")
    if pending == NONE:
        require(tries == 0 and trial == NONE,
                "无 pending 时存在试启动状态")
    else:
        require(pending in (0, 1) and pending != confirmed,
                "pending 槽错误")
        require(tries <= 3 and trial in (NONE, pending),
                "试启动状态错误")
    return generation, confirmed, pending, tries, trial


def select(fd):
    records = [os.pread(fd, META_BYTES, i * META_BYTES)
               for i in range(2)]
    valid = []
    for i, record in enumerate(records):
        try:
            valid.append((i, decode(record)))
        except RuntimeError:
            pass
    require(valid, "两份元数据均无效；禁止自动初始化")
    if len(valid) == 2:
        if valid[0][1][0] == valid[1][1][0]:
            require(records[0] == records[1], "同代次元数据冲突")
        selected = max(valid, key=lambda item: item[1][0])
    else:
        selected = valid[0]
    return selected[0], selected[1], records


def stage_record(state, target):
    generation, confirmed, pending, tries, trial = state
    require(target in (0, 1), "目标槽必须为 A 或 B")
    require(confirmed in (0, 1), "已确认槽非法")
    require(generation > 0, "代次非法")
    require(pending == NONE and trial == NONE and tries == 0,
            "已有升级事务")
    require(target != confirmed, "禁止升级已确认槽")
    require(generation < 0xffffffffffffffff, "代次溢出")

    record = bytearray(META_BYTES)
    record[:8] = b"AMPAB001"
    struct.pack_into("<IIQIIII", record, 8,
                     1, META_BYTES, generation + 1,
                     confirmed, target, 3, NONE)
    struct.pack_into("<I", record, 4092, zlib.crc32(record[:4092]))
    return bytes(record)


def boot_identity():
    values = {}
    wanted = {"amp_ab.slot", "amp_ab.generation", "amp_ab.trial", "root"}
    for word in Path("/proc/cmdline").read_text().split():
        key, separator, value = word.partition("=")
        if separator and key in wanted:
            require(key not in values, "启动参数重复：" + key)
            values[key] = value

    require(set(values) == wanted, "不是完整 A/B 启动上下文")
    require(values["amp_ab.slot"] in ("A", "B"), "启动槽错误")
    require(values["amp_ab.generation"].isdigit(), "启动代次错误")
    require(values["amp_ab.trial"] in ("0", "1"), "trial 参数错误")
    require(values["root"].startswith("PARTUUID="), "root 不是 PARTUUID")

    slot = int(values["amp_ab.slot"] == "B")
    root = BY_NAME / ("rootfs_b" if slot else "rootfs")
    root_stat = root.stat()
    require(stat.S_ISBLK(root_stat.st_mode), "rootfs 不是块设备")
    require(os.stat("/").st_dev == root_stat.st_rdev,
            "实际挂载根分区与启动槽不一致")

    uuid = values["root"][len("PARTUUID="):]
    require("/" not in uuid and uuid, "非法 PARTUUID")
    require((Path("/dev/disk/by-partuuid") / uuid).stat().st_rdev ==
            root_stat.st_rdev, "root PARTUUID 不匹配")
    return slot, int(values["amp_ab.generation"]), values["amp_ab.trial"]


def partition(name):
    path = (BY_NAME / name).resolve(strict=True)
    info = path.stat()
    require(stat.S_ISBLK(info.st_mode), name + " 不是块设备")
    sys = Path("/sys/dev/block") / (
        str(os.major(info.st_rdev)) + ":" + str(os.minor(info.st_rdev))
    )
    sys = sys.resolve(strict=True)
    require((sys / "partition").exists(), name + " 不是分区")
    return {
        "name": name,
        "path": path,
        "dev": info.st_rdev,
        "parent": sys.parent,
        "start": int((sys / "start").read_text()),
        "size": int((sys / "size").read_text()) * 512,
    }


def check_layout(parts):
    require(len({p["dev"] for p in parts}) == len(parts),
            "分区设备重复")
    require(len({p["parent"] for p in parts}) == 1,
            "分区不在同一磁盘")
    ordered = sorted(parts, key=lambda p: p["start"])
    for left, right in zip(ordered, ordered[1:]):
        require(left["start"] * 512 + left["size"] <= right["start"] * 512,
                "分区范围重叠")


def require_unused(devices):
    # 检查当前可见进程的挂载命名空间。
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            lines = (entry / "mountinfo").read_text().splitlines()
        except FileNotFoundError:
            continue
        except PermissionError:
            raise RuntimeError("无法检查进程挂载状态：" + entry.name)
        for line in lines:
            major, minor = map(int, line.split()[2].split(":"))
            require(os.makedev(major, minor) not in devices,
                    "目标分区正在挂载")

    for line in Path("/proc/swaps").read_text().splitlines()[1:]:
        require(os.stat(line.split()[0]).st_rdev not in devices,
                "目标分区正在用作 swap")

    for dev in devices:
        sys = Path("/sys/dev/block") / (
            str(os.major(dev)) + ":" + str(os.minor(dev))
        )
        require(not any((sys / "holders").iterdir()),
                "目标分区存在设备映射持有者")


def open_partition(part, write=False):
    fd = os.open(part["path"],
                 (os.O_RDWR | os.O_SYNC if write else os.O_RDONLY)
                 | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        require(os.fstat(fd).st_rdev == part["dev"], "设备身份变化")
        sector = struct.unpack(
            "=I", fcntl.ioctl(fd, 0x1268, bytes(4))
        )[0]  # BLKSSZGET
        size = struct.unpack(
            "=Q", fcntl.ioctl(fd, 0x80081272, bytes(8))
        )[0]  # BLKGETSIZE64
        require(sector == 512 and size == part["size"],
                "分区大小或扇区规格变化")
        return fd
    except BaseException:
        os.close(fd)
        raise


def write_all(fd, data, offset):
    view = memoryview(data)
    while view:
        count = os.pwrite(fd, view, offset)
        require(count > 0, "写入未推进")
        offset += count
        view = view[count:]


def install_image(source, part, item):
    require(item["size"] <= part["size"], "镜像超过分区容量")
    fd = open_partition(part, write=True)
    try:
        with source.open("rb") as stream:
            offset = 0
            while True:
                data = stream.read(CHUNK)
                if not data:
                    break
                write_all(fd, data, offset)
                offset += len(data)
        require(offset == item["size"], "镜像大小变化")
        os.fsync(fd)

        # 使读回尽量绕过旧的块设备页缓存；失败即停止。
        fcntl.ioctl(fd, 0x1261)  # BLKFLSBUF
        require(digest_fd(fd, item["size"]) == item["sha256"],
                part["name"] + " 写后读回校验失败")
        print("VERIFIED:", part["name"], flush=True)
    finally:
        os.close(fd)

class PendingCommitUncertain(RuntimeError):
    """元数据写入已开始；失败不代表新记录一定无效。"""


def verify_snapshot(work, pubkey):
    # 必须先完成认证和内容验收，之后才能进入任何块设备写入。
    ota_release.verify(work, pubkey)
    manifest = json.loads((work / "release.json").read_text())
    ota_release.verify_images(work, manifest)
    return manifest


def require_metadata_unchanged(fd, baseline):
    require(select(fd) == baseline,
            "元数据发生变化；停止升级")


def require_boot_unchanged(identity):
    require(boot_identity() == identity,
            "启动身份发生变化；停止升级")


def commit_pending(fd, baseline, record):
    selected, state, records = baseline
    next_state = decode(record)

    require(next_state[0] == state[0] + 1,
            "pending 代次不连续")
    require(next_state[1] == state[1],
            "pending 不得改变已确认槽")
    require(state[2:] == (NONE, 0, NONE),
            "当前已有升级事务")
    require(next_state[2] in (0, 1)
            and next_state[2] != state[1]
            and next_state[3:] == (3, NONE),
            "pending 状态非法")

    require_metadata_unchanged(fd, baseline)
    other = selected ^ 1

    try:
        # 从第一次写入开始，任何错误都按“结果不确定”处理。
        write_all(fd, record, other * META_BYTES)
        os.fsync(fd)
        fcntl.ioctl(fd, 0x1261)  # BLKFLSBUF

        require(os.pread(fd, META_BYTES, other * META_BYTES) == record,
                "pending 完整记录读回不一致")

        final_selected, final_state, final_records = select(fd)
        require(final_selected == other and final_state == next_state,
                "pending 选中状态不一致")
        require(final_records[selected] == records[selected],
                "旧有效副本发生变化")
    except Exception as exc:
        raise PendingCommitUncertain(
            "pending 提交结果不确定；禁止自动重试或重启，"
            "必须重新读取两份元数据"
        ) from exc


def execute_update(work, manifest, targets, meta, baseline, identity):
    running, boot_generation, trial = identity
    selected, state, records = baseline

    require(running in (0, 1), "运行槽非法")
    require(state[1] == running, "运行槽不是已确认槽")
    require(state[2:] == (NONE, 0, NONE),
            "存在未结束的升级事务")

    target = 1 - running
    suffix = "_b" if target else ""
    require(set(targets) == {"boot", "amp", "rootfs"},
            "目标镜像集合错误")

    for name, part in targets.items():
        require(part["name"] == name + suffix,
                "目标分区不是非活动槽：" + name)
        require(0 < manifest["images"][name]["size"] <= part["size"],
                "镜像超过目标容量：" + name)

    # 提前发现代次溢出，不能写完镜像才发现无法 stage。
    record = stage_record(state, target)
    devices = {part["dev"] for part in targets.values()}
    require(len(devices) == 3, "目标分区重复")

    for name in ("boot", "amp", "rootfs"):
        require_boot_unchanged(identity)
        require_metadata_unchanged(meta, baseline)
        require_unused(devices)
        install_image(work / (name + ".img"),
                      targets[name], manifest["images"][name])

    # install_image 返回表示该镜像已刷新并完成读回哈希校验。
    require_boot_unchanged(identity)
    require_unused(devices)
    commit_pending(meta, baseline, record)
    return target, decode(record)[0]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--pubkey", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--commit", action="store_true")
    args = parser.parse_args()

    require(os.geteuid() == 0, "必须以 root 运行")
    require(not args.commit or ACTIVATE,
            "写入开关关闭；本轮只允许预演")
    for tool in ("openssl", "fdtget", "debugfs", "e2fsck"):
        require(shutil.which(tool), "缺少验收工具：" + tool)

    bundle = args.bundle.resolve(strict=True)
    pubkey = args.pubkey.resolve(strict=True)
    require(bundle not in pubkey.parents, "可信公钥不能来自发布包")
    require(pubkey.is_file(), "可信公钥不是普通文件")
    args.work_dir.mkdir(parents=True, exist_ok=True)

    lock = os.open("/run/amp-ab.lock",
                   os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW,
                   0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

        # 私有副本贯穿验收和写入，原发布目录之后不再参与写盘。
        with tempfile.TemporaryDirectory(
                prefix="ab-update-", dir=args.work_dir) as temporary:
            work = Path(temporary)
            for name in ("release.json", "release.sig",
                         "boot.img", "amp.img", "rootfs.img"):
                source = bundle / name
                require(source.is_file() and not source.is_symlink(),
                        "发布文件非法：" + name)
                shutil.copyfile(source, work / name)

            manifest = verify_snapshot(work, pubkey)

            identity = boot_identity()
            running, boot_generation, trial = identity
            names = ("uboot", "misc", "boot", "recovery", "amp",
                     "rootfs", "oem", "userdata",
                     "boot_b", "amp_b", "rootfs_b", "abmeta")
            parts = {name: partition(name) for name in names}
            check_layout(list(parts.values()))

            target = 1 - running
            suffix = "_b" if target else ""
            targets = {name: parts[name + suffix]
                       for name in ("boot", "amp", "rootfs")}
            require_unused({p["dev"] for p in targets.values()} |
                           {parts["abmeta"]["dev"]})

            meta = open_partition(parts["abmeta"], write=args.commit)
            try:
                selected, state, records = select(meta)
                generation, confirmed, pending, tries, last_trial = state
                require(confirmed == running and pending == NONE
                        and tries == 0 and last_trial == NONE,
                        "当前槽未确认或已有 pending")

                if trial == "0":
                    require(generation == boot_generation,
                            "已确认启动代次不匹配")
                else:
                    # 已确认的试启动：保留副本必须证明本次确认。
                    prior = decode(records[selected ^ 1])
                    require(prior[0] == boot_generation
                            and prior[2] == running
                            and prior[4] == running
                            and generation == boot_generation + 1,
                            "无法证明本次试启动已成功确认")

                for name, part in targets.items():
                    require(manifest["images"][name]["size"] <= part["size"],
                            name + " 镜像超过目标容量")
                    print("TARGET:", name, "->", part["path"])

                stage_record(state, target)
                if not args.commit:
                    print("PLAN OK: target=" + ("B" if target else "A"))
                    print("未写镜像、未写元数据、未重启")
                    return

                target, staged_generation = execute_update(
                    work, manifest, targets, meta,
                    (selected, state, records), identity
                )
                print("STAGED: slot=" + ("B" if target else "A")
                      + " tries=3 generation=" + str(staged_generation))
                print("未自动重启")
            finally:
                os.close(meta)
    finally:
        os.close(lock)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit(
            "UPDATE STOPPED: " + str(exc)
            + "\n如已进入写入阶段，不要直接重试；先重新读取元数据和目标槽。"
        )