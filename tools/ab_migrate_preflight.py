#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import stat
import struct
import uuid
import zlib
from pathlib import Path


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def read_exact(fd, offset, size):
    data = os.pread(fd, size, offset)
    require(len(data) == size, "设备读取不完整")
    return data


def read_gpt(fd, lba, sectors):
    raw = read_exact(fd, lba * 512, 512)
    require(raw[:8] == b"EFI PART", "GPT 签名错误")

    revision, header_size, crc, reserved = struct.unpack_from("<IIII", raw, 8)
    require(revision == 0x10000 and 92 <= header_size <= 512,
            "GPT 版本或头长度错误")
    require(reserved == 0, "GPT 保留字段错误")

    header = bytearray(raw[:header_size])
    struct.pack_into("<I", header, 16, 0)
    require(zlib.crc32(header) == crc, "GPT 头 CRC 错误")

    current, backup, first, last = struct.unpack_from("<QQQQ", raw, 24)
    entries_lba, count, entry_size, entries_crc = struct.unpack_from(
        "<QIII", raw, 72
    )

    require(current == lba and backup < sectors, "GPT 头位置错误")
    require(first <= last < sectors, "GPT 可用范围错误")
    require(count >= 12 and count <= 4096 and entry_size == 128,
            "暂不支持此 GPT 项规格")

    size = count * entry_size
    require(entries_lba * 512 + size <= sectors * 512,
            "GPT 数组越界")
    entries = read_exact(fd, entries_lba * 512, size)
    require(zlib.crc32(entries) == entries_crc, "GPT 数组 CRC 错误")

    parts = []
    for index in range(count):
        item = entries[index * entry_size:(index + 1) * entry_size]
        if item[:16] == bytes(16):
            continue
        start, end, attrs = struct.unpack_from("<QQQ", item, 32)
        name = item[56:128].decode("utf-16le").split("\0", 1)[0]
        parts.append({
            "number": index + 1,
            "name": name,
            "start_sector": start,
            "end_sector": end,
            "sectors": end - start + 1,
            "partuuid": str(uuid.UUID(bytes_le=item[16:32])),
            "attributes": attrs,
        })

    return {
        "current": current,
        "backup": backup,
        "first": first,
        "last": last,
        "disk_guid": str(uuid.UUID(bytes_le=raw[56:72])),
        "entries_lba": entries_lba,
        "header": raw,
        "entries": entries,
        "parts": parts,
    }


def ensure_unmounted(devices):
    # 所有可见进程命名空间均检查；无法检查时停止。
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            content = (process / "mountinfo").read_text()
        except FileNotFoundError:
            continue
        for line in content.splitlines():
            major, minor = map(int, line.split()[2].split(":"))
            require(os.makedev(major, minor) not in devices,
                    "eMMC 分区仍被挂载")

    for line in Path("/proc/swaps").read_text().splitlines()[1:]:
        require(os.stat(line.split()[0]).st_rdev not in devices,
                "eMMC 分区被用作 swap")


def save(path, data):
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    require(path.read_bytes() == data, "备份读回不一致")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="/dev/mmcblk0", type=Path)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--backup-dir", required=True, type=Path)
    args = parser.parse_args()

    require(os.geteuid() == 0, "需要 root")
    require("AMP offline migration shell" in
            Path("/etc/migration-init.sh").read_text(),
            "不是预期迁移环境")

    plan = json.loads(args.plan.read_text())
    require(plan["sector_bytes"] == 512, "扇区规格错误")
    require(plan["disk_sectors"] == 120832000, "计划磁盘容量错误")

    device = args.device.resolve(strict=True)
    info = device.stat()
    require(stat.S_ISBLK(info.st_mode), "目标不是块设备")

    sys = (Path("/sys/dev/block") /
           f"{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}").resolve()
    require(sys.name == "mmcblk0" and not (sys / "partition").exists(),
            "目标不是预期整盘")
    require((sys / "device/type").read_text().strip() == "MMC",
            "目标不是 eMMC")
    require(int((sys / "queue/logical_block_size").read_text()) == 512,
            "设备逻辑块大小不是 512")

    sectors = int((sys / "size").read_text())
    require(sectors == plan["disk_sectors"], "实际磁盘容量不匹配")

    devices = {info.st_rdev}
    for child in sys.iterdir():
        if (child / "partition").exists():
            major, minor = map(int, (child / "dev").read_text().split(":"))
            devices.add(os.makedev(major, minor))
            require(not any((child / "holders").iterdir()),
                    "分区有映射持有者")
    ensure_unmounted(devices)

    fd = os.open(device, os.O_RDONLY | os.O_CLOEXEC)
    try:
        primary = read_gpt(fd, 1, sectors)
        secondary = read_gpt(fd, sectors - 1, sectors)

        require(primary["backup"] == sectors - 1
                and secondary["backup"] == 1,
                "主备 GPT 指针不一致")
        for field in ("disk_guid", "first", "last", "entries"):
            require(primary[field] == secondary[field],
                    "主备 GPT 不一致：" + field)
        require(primary["last"] == plan["last_usable_sector"],
                "GPT 可用尾部不匹配")

        actual = primary["parts"]
        expected = plan["expected_current_partitions"]
        require(len(actual) == len(expected), "当前分区数量不匹配")
        for current, wanted in zip(actual, expected):
            for field in ("number", "name", "start_sector",
                          "end_sector", "sectors"):
                require(current[field] == wanted[field],
                        "旧布局不匹配：" + wanted["name"] + "/" + field)

        guids = [p["partuuid"] for p in actual]
        require(len(set(guids)) == len(guids)
                and str(uuid.UUID(int=0)) not in guids,
                "现有 PARTUUID 非法或重复")

        # 输出目录必须在 RAM 或外部介质，不能在目标 eMMC 上。
        parent = args.backup_dir.parent.resolve(strict=True)
        require(parent.stat().st_dev not in devices,
                "备份目录位于待迁移 eMMC")
        args.backup_dir.mkdir(mode=0o700, exist_ok=False)

        files = {
            "protective-mbr.bin": read_exact(fd, 0, 512),
            "primary-header.bin": primary["header"],
            "primary-entries.bin": primary["entries"],
            "backup-header.bin": secondary["header"],
            "backup-entries.bin": secondary["entries"],
        }
        for name, data in files.items():
            save(args.backup_dir / name, data)

        report = {
            "device": str(device),
            "sectors": sectors,
            "disk_guid": primary["disk_guid"],
            "emmc_cid": (sys / "device/cid").read_text().strip(),
            "primary_entries_lba": primary["entries_lba"],
            "backup_entries_lba": secondary["entries_lba"],
            "partitions": actual,
            "sha256": {
                name: hashlib.sha256(data).hexdigest()
                for name, data in files.items()
            },
        }
        save(args.backup_dir / "report.json",
             (json.dumps(report, indent=2) + "\n").encode())

        directory_fd = os.open(args.backup_dir, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)

        print("PASS: 旧布局、主备 GPT、未挂载状态及备份读回")
        print("备份目录：", args.backup_dir)
        print("未缩容、未修改 GPT、未写 eMMC")
        print("必须将备份转存到另一台电脑，RAM 内备份重启会丢失。")
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit("PREFLIGHT STOPPED: " + str(exc))