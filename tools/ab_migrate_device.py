#!/usr/bin/env python3
import argparse
import fcntl
import json
import os
import shutil
import stat
import struct
import time
from pathlib import Path

import ab_gpt_apply as gpt_apply
import ab_gpt_prepare as gpt_prepare
import ab_migrate_preflight as preflight
import ab_userdata_shrink as shrink

ACTIVATE = False  # ACTIVATE：允许执行实际操作的开关

BLKSSZGET = 0x1268  # BLKSSZGET：查询块设备逻辑扇区大小的 ioctl 命令
BLKGETSIZE64 = 0x80081272  # BLKGETSIZE64：查询块设备总字节数的 ioctl 命令
BLKFLSBUF = 0x1261  # BLKFLSBUF：刷新块设备缓存的 ioctl 命令
BLKRRPART = 0x125F  # BLKRRPART：通知内核重读分区表的 ioctl 命令


def require(ok, message):
    # 1. 按条件 not ok 选择处理分支。
    if not ok:
        raise RuntimeError(message)


def disk_devices(sys, disk_dev):
    # 1. 设置 devices，供后续逻辑使用。
    devices = {disk_dev}
    require(not any((sys / "holders").iterdir()), "整盘存在设备映射")

    # 2. 遍历 sys.iterdir()，逐项处理 child。
    for child in sys.iterdir():
        if not (child / "partition").exists():
            continue
        major, minor = map(int, (child / "dev").read_text().split(":"))
        devices.add(os.makedev(major, minor))
        require(not any((child / "holders").iterdir()), "分区存在设备映射：" + child.name)

    # 3. 返回 devices。
    return devices


def require_offline(sys, disk_dev):
    # 1. 调用 disk_devices，保存到 devices。
    devices = disk_devices(sys, disk_dev)
    require(os.stat("/").st_dev not in devices, "根文件系统位于目标 eMMC")
    preflight.ensure_unmounted(devices)
    # 2. 返回 devices。
    return devices


def check_filesystem_fits(userdata, plan):
    # 1. 设置 proposed，供后续逻辑使用。
    proposed = plan["proposed_partitions"][7]
    require(proposed["name"] == "userdata" and proposed["number"] == 8, "userdata 目标配置错误")

    # 2. 调用 shrink.filesystem_size，保存到 filesystem_bytes, block_size。
    filesystem_bytes, block_size = shrink.filesystem_size(userdata)
    partition_bytes = proposed["sectors"] * 512
    require(partition_bytes > shrink.MARGIN_BYTES, "目标容量过小")

    # 3. 设置 limit，供后续逻辑使用。
    limit = ((partition_bytes - shrink.MARGIN_BYTES) // block_size) * block_size

    # 4. 调用 require。
    require(filesystem_bytes <= limit, "userdata 尚未缩容到安全边界以内；禁止写 GPT")
    # 5. 返回 filesystem_bytes。
    return filesystem_bytes


def open_disk(device, expected_dev, sectors, commit):
    # 1. 设置 flags，供后续逻辑使用。
    flags = os.O_CLOEXEC | os.O_NOFOLLOW
    flags |= os.O_RDWR | os.O_SYNC | os.O_EXCL if commit else os.O_RDONLY

    # 2. 打开文件或设备，保存到 fd。
    fd = os.open(device, flags)
    # 3. 执行本段操作，捕获 BaseException。
    try:
        info = os.fstat(fd)
        require(
            stat.S_ISBLK(info.st_mode) and info.st_rdev == expected_dev, "打开后的磁盘身份不匹配"
        )

        sector = struct.unpack("=I", fcntl.ioctl(fd, BLKSSZGET, bytes(4)))[0]
        size = struct.unpack("=Q", fcntl.ioctl(fd, BLKGETSIZE64, bytes(8)))[0]
        require(sector == 512 and size == sectors * 512, "实际设备容量或扇区规格不匹配")
        return fd
    except BaseException:
        os.close(fd)
        raise


def flush_disk(fd):
    # 1. 同步文件数据到存储设备。
    os.fsync(fd)
    # 刷新并失效块设备缓存，然后事务核心执行读回。
    fcntl.ioctl(fd, BLKFLSBUF)


def check_kernel_partitions(sys, expected):
    # 1. 设置 actual，供后续逻辑使用。
    actual = {}
    # 2. 遍历 sys.iterdir()，逐项处理 child。
    for child in sys.iterdir():
        if not (child / "partition").exists():
            continue
        number = int((child / "partition").read_text())
        actual[number] = {
            "start": int((child / "start").read_text()),
            "size": int((child / "size").read_text()),
        }

    # 3. 调用 require。
    require(set(actual) == {p["number"] for p in expected}, "内核分区数量或编号不匹配")

    # 4. 遍历 expected，逐项处理 part。
    for part in expected:
        current = actual[part["number"]]
        require(
            current["start"] == part["start_sector"] and current["size"] == part["sectors"],
            "内核分区范围不匹配：" + part["name"],
        )


def reread_and_check(fd, sys, expected):
    # 失败不自动重试 ioctl，也不自动重启。
    fcntl.ioctl(fd, BLKRRPART)

    # 允许 sysfs 发布新分区存在短暂延迟。
    deadline = time.monotonic() + 5
    # 1. 在 True 成立期间持续处理，退出时结束本轮循环。
    while True:
        try:
            check_kernel_partitions(sys, expected)
            return
        except (RuntimeError, FileNotFoundError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def snapshot_backup(source, destination):
    # 1. 创建所需目录。
    destination.mkdir(mode=0o700)
    names = (
        "report.json",
        "protective-mbr.bin",
        "primary-header.bin",
        "primary-entries.bin",
        "backup-header.bin",
        "backup-entries.bin",
    )
    # 2. 遍历 names，逐项处理 name。
    for name in names:
        path = source / name
        require(path.is_file() and not path.is_symlink(), "备份文件非法：" + name)

        # 此处备份只包含 GPT 数据，拒绝异常大文件。
        limit = 1024 * 1024
        require(0 < path.stat().st_size <= limit, "备份文件长度异常：" + name)
        preflight.save(destination / name, path.read_bytes())


def main():
    # 1. 建立命令行参数解析器，保存到 parser。
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=Path, default=Path("/dev/mmcblk0"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--gpt-backup", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--commit", action="store_true")
    parser.add_argument(
        "--backup-saved", action="store_true", help="声明 userdata 和 GPT 备份已保存到开发板之外"
    )
    args = parser.parse_args()

    # 2. 调用 require。
    require(os.geteuid() == 0, "需要 root")
    require(not args.commit or ACTIVATE, "设备 GPT 写入开关关闭")
    require(not args.commit or args.backup_saved, "提交前必须确认备份已保存到开发板之外")
    require(
        "AMP offline migration shell" in Path("/etc/migration-init.sh").read_text(),
        "必须在迁移恢复环境执行",
    )
    require(shutil.which("e2fsck"), "缺少 e2fsck")

    # 3. 解析 JSON 文本，保存到 plan。
    plan = json.loads(args.plan.read_text())
    require(plan["sector_bytes"] == 512 and plan["disk_sectors"] == 120832000, "计划磁盘规格错误")

    # 4. 调用 args.device.resolve，保存到 device。
    device = args.device.resolve(strict=True)
    require(device == Path("/dev/mmcblk0"), "只允许预期 eMMC")

    # 5. 调用 shrink.inspect，保存到 userdata, primary, secondary, sys。
    userdata, primary, secondary, sys = shrink.inspect(device, plan)
    disk_dev = device.stat().st_rdev
    devices = require_offline(sys, disk_dev)

    # 工作目录不能位于目标盘，且必须使用新目录。
    parent = args.work_dir.parent.resolve(strict=True)
    require(parent.stat().st_dev not in devices, "工作目录位于目标 eMMC")
    args.work_dir.mkdir(mode=0o700, exist_ok=False)

    # 6. 设置 backup，供后续逻辑使用。
    backup = args.work_dir / "backup"
    snapshot_backup(args.gpt_backup, backup)
    shrink.validate_backup(backup, primary, secondary, sys)

    # 7. 设置 prepared，供后续逻辑使用。
    prepared = args.work_dir / "prepared"
    description = gpt_prepare.prepare(backup, plan, prepared)
    old_files = gpt_apply.load_files(backup)
    new_files = gpt_apply.load_files(prepared)

    # 在打开可写整盘之前，先验证全部转换内容。
    gpt_apply.validate_transition(
        old_files["primary-header.bin"],
        old_files["backup-header.bin"],
        new_files["primary-header.bin"],
        new_files["backup-header.bin"],
        old_files["primary-entries.bin"],
        new_files["primary-entries.bin"],
        plan,
    )

    # 8. 调用 check_filesystem_fits，保存到 filesystem_bytes。
    filesystem_bytes = check_filesystem_fits(userdata, plan)
    shrink.run_check(userdata, repair=False)

    # 9. 输出执行结果或诊断信息。
    print("userdata filesystem bytes:", filesystem_bytes, flush=True)
    print("旧 PARTUUID 将保留，新增分区：", flush=True)
    # 10. 遍历 description["partitions"][8:]，逐项处理 part。
    for part in description["partitions"][8:]:
        print(part["number"], part["name"], part["partuuid"], flush=True)

    # 11. 按条件 not args.commit 选择处理分支。
    if not args.commit:
        print("PLAN OK: 未写 GPT、未重读分区表")
        print("预演输出保存在：", args.work_dir)
        return

    # 长时间 e2fsck 后重新检查旧布局、备份和未挂载状态。
    userdata, primary, secondary, current_sys = shrink.inspect(device, plan)
    require(current_sys == sys, "设备 sysfs 身份变化")
    shrink.validate_backup(backup, primary, secondary, sys)
    require_offline(sys, disk_dev)
    check_filesystem_fits(userdata, plan)

    # 12. 调用 open_disk，保存到 fd。
    fd = open_disk(device, disk_dev, plan["disk_sectors"], commit=True)
    completed = False
    # 13. 执行本段操作，捕获 Exception。
    try:
        # MBR 不参与转换，结束时确认原样保留。
        mbr = (backup / "protective-mbr.bin").read_bytes()
        require(len(mbr) == 512 and os.pread(fd, 512, 0) == mbr, "保护 MBR 与备份不一致")

        def guarded_write(target_fd, data, offset):
            # 1. 调用 require_offline。
            require_offline(sys, disk_dev)
            gpt_apply.write_full(target_fd, data, offset)

        gpt_apply.apply_transaction(
            fd,
            plan,
            old_files,
            new_files,
            flush=flush_disk,
            writer=guarded_write,
        )
        completed = True

        require(os.pread(fd, 512, 0) == mbr, "保护 MBR 意外变化")

        # raw GPT 已提交；此后重读失败不等于 GPT 没写入。
        reread_and_check(fd, sys, plan["proposed_partitions"])

        gpt_apply.same_gpt(
            fd,
            plan["disk_sectors"],
            new_files["primary-header.bin"],
            new_files["backup-header.bin"],
            new_files["primary-entries.bin"],
        )
        require_offline(sys, disk_dev)

        # 重读后再次检查缩小分区中的文件系统。
        check_filesystem_fits(userdata, plan)
        shrink.run_check(userdata, repair=False)

        result = {
            "status": "GPT_COMMITTED_AND_KERNEL_VERIFIED",
            "disk_guid": primary["disk_guid"],
            "partitions": description["partitions"],
            "userdata_filesystem_bytes": shrink.filesystem_size(userdata)[0],
            "images_written": False,
            "abmeta_initialized": False,
        }
        preflight.save(
            args.work_dir / "migration-result.json",
            (json.dumps(result, indent=2) + "\n").encode(),
        )

        print("PASS: GPT 提交、读回及内核分区核对通过")
        print("未初始化 abmeta，未写入 B 槽镜像，未重启")
        print("结果目录：", args.work_dir)
    except Exception as exc:
        if completed:
            raise RuntimeError(
                "GPT 已提交，但后续验证失败。保持恢复环境，"
                "不要再次执行旧布局迁移，也不要自动重启：" + str(exc)
            ) from exc
        raise
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit(
            "MIGRATION STOPPED: " + str(exc) + "\n若已开始 GPT 写入，不得自动重试或恢复旧 GPT。"
        )
