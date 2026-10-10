#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import stat
import struct
import subprocess
import tempfile
from pathlib import Path

import ab_migrate_preflight as preflight

# 正式离线部署前保持关闭。
ACTIVATE = False

MARGIN_BYTES = 64 * 1024 * 1024


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def filesystem_size(device):
    fd = os.open(device, os.O_RDONLY | os.O_CLOEXEC)
    try:
        sb = os.pread(fd, 1024, 1024)
    finally:
        os.close(fd)

    require(len(sb) == 1024, "超级块读取不完整")
    require(struct.unpack_from("<H", sb, 56)[0] == 0xEF53,
            "userdata 不是 ext 文件系统")

    log_block_size = struct.unpack_from("<I", sb, 24)[0]
    require(log_block_size <= 6, "文件系统块大小不支持")
    block_size = 1024 << log_block_size

    blocks = struct.unpack_from("<I", sb, 4)[0]
    incompat = struct.unpack_from("<I", sb, 96)[0]

    if incompat & 0x80:  # EXT4_FEATURE_INCOMPAT_64BIT
        blocks |= struct.unpack_from("<I", sb, 0x150)[0] << 32

    require(blocks > 0, "文件系统块数错误")
    return blocks * block_size, block_size


def inspect(device, plan):
    info = device.stat()
    require(stat.S_ISBLK(info.st_mode), "必须指定整盘块设备")

    sys = (
        Path("/sys/dev/block") /
        f"{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}"
    ).resolve(strict=True)

    require(sys.name == "mmcblk0"
            and not (sys / "partition").exists(),
            "不是预期 eMMC 整盘")
    require((sys / "device/type").read_text().strip() == "MMC",
            "不是 eMMC")
    require(int((sys / "size").read_text()) == plan["disk_sectors"],
            "磁盘容量不匹配")

    devices = {info.st_rdev}
    for child in sys.iterdir():
        if (child / "partition").exists():
            major, minor = map(int, (child / "dev").read_text().split(":"))
            devices.add(os.makedev(major, minor))
            require(not any((child / "holders").iterdir()),
                    "分区存在设备映射")

    preflight.ensure_unmounted(devices)

    fd = os.open(device, os.O_RDONLY | os.O_CLOEXEC)
    try:
        sectors = plan["disk_sectors"]
        primary = preflight.read_gpt(fd, 1, sectors)
        secondary = preflight.read_gpt(fd, sectors - 1, sectors)
    finally:
        os.close(fd)

    require(primary["backup"] == sectors - 1
            and secondary["backup"] == 1,
            "GPT 指针不一致")
    for key in ("disk_guid", "first", "last", "entries"):
        require(primary[key] == secondary[key],
                "主备 GPT 不一致")

    expected = plan["expected_current_partitions"]
    require(len(primary["parts"]) == len(expected),
            "当前分区数量已改变")

    for current, wanted in zip(primary["parts"], expected):
        for key in ("number", "name", "start_sector",
                    "end_sector", "sectors"):
            require(current[key] == wanted[key],
                    "旧布局不匹配：" + wanted["name"])

    userdata = Path("/dev/mmcblk0p8")
    part_stat = userdata.stat()
    require(stat.S_ISBLK(part_stat.st_mode), "userdata 不是块设备")

    part_sys = (
        Path("/sys/dev/block") /
        f"{os.major(part_stat.st_rdev)}:{os.minor(part_stat.st_rdev)}"
    ).resolve(strict=True)
    require(part_sys.parent == sys, "userdata 不属于目标磁盘")
    require(int((part_sys / "partition").read_text()) == 8,
            "userdata 分区编号错误")
    require(int((part_sys / "start").read_text()) ==
            expected[7]["start_sector"],
            "userdata 起点错误")
    require(int((part_sys / "size").read_text()) ==
            expected[7]["sectors"],
            "userdata 当前容量错误")

    return userdata, primary, secondary, sys


def validate_backup(directory, primary, secondary, sys):
    report = json.loads((directory / "report.json").read_text())
    require(report["emmc_cid"] ==
            (sys / "device/cid").read_text().strip(),
            "备份来自另一块 eMMC")

    import hashlib
    expected = {
        "primary-header.bin": primary["header"],
        "primary-entries.bin": primary["entries"],
        "backup-header.bin": secondary["header"],
        "backup-entries.bin": secondary["entries"],
    }
    for name, live in expected.items():
        data = (directory / name).read_bytes()
        require(data == live, "GPT 与备份不一致：" + name)
        require(hashlib.sha256(data).hexdigest() ==
                report["sha256"][name],
                "GPT 备份摘要错误：" + name)


def run_check(device, repair):
    # 预演使用 -n；正式缩容前用 -p 自动修复安全问题。
    args = ["e2fsck", "-f", "-p" if repair else "-n", str(device)]
    result = subprocess.run(args)
    allowed = (0, 1) if repair else (0,)
    require(result.returncode in allowed,
            "文件系统检查未通过，退出码=" + str(result.returncode))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=Path, default=Path("/dev/mmcblk0"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--gpt-backup", type=Path, required=True)
    parser.add_argument("--commit", action="store_true")
    args = parser.parse_args()

    require(os.geteuid() == 0, "需要 root")
    require(not args.commit or ACTIVATE, "缩容写入开关关闭")
    require("AMP offline migration shell" in
            Path("/etc/migration-init.sh").read_text(),
            "必须在迁移恢复环境执行")

    for tool in ("e2fsck", "resize2fs"):
        require(shutil.which(tool), "缺少工具：" + tool)

    plan = json.loads(args.plan.read_text())
    require(plan["sector_bytes"] == 512, "计划扇区大小错误")
    require(plan["disk_sectors"] == 120832000, "计划磁盘容量错误")

    old = plan["expected_current_partitions"][7]
    new = plan["proposed_partitions"][7]
    require(old["name"] == new["name"] == "userdata"
            and old["number"] == new["number"] == 8
            and old["start_sector"] == new["start_sector"]
            and 0 < new["sectors"] < old["sectors"],
            "userdata 缩容计划错误")

    device = args.device.resolve(strict=True)
    userdata, primary, secondary, sys = inspect(device, plan)
    validate_backup(args.gpt_backup, primary, secondary, sys)

    current_bytes, block_size = filesystem_size(userdata)
    partition_bytes = new["sectors"] * 512
    require(current_bytes <= old["sectors"] * 512,
            "现有文件系统超出原分区")
    require(partition_bytes > MARGIN_BYTES, "目标分区过小")

    # 留出 64 MiB 余量，避免文件系统刚好顶到新分区边界。
    target_blocks = (partition_bytes - MARGIN_BYTES) // block_size
    target_bytes = target_blocks * block_size

    print("userdata:", userdata, flush=True)
    print("current filesystem bytes:", current_bytes, flush=True)
    print("new partition bytes:", partition_bytes, flush=True)
    print("target filesystem bytes:", target_bytes, flush=True)

    run_check(userdata, repair=False)

    if not args.commit:
        print("PLAN OK: 未缩容、未修改 GPT")
        return

    # 正式执行前再次检查设备和 GPT。
    userdata, current_primary, current_secondary, sys = inspect(device, plan)
    validate_backup(args.gpt_backup, current_primary, current_secondary, sys)
    run_check(userdata, repair=True)

    current_bytes, block_size = filesystem_size(userdata)
    if current_bytes > target_bytes:
        # 明确使用 KiB 单位，避免 resize2fs 默认单位歧义。
        result = subprocess.run([
            "resize2fs", str(userdata), str(target_bytes // 1024) + "K"
        ])
        require(result.returncode == 0,
                "resize2fs 失败；不得继续修改 GPT")

    run_check(userdata, repair=False)
    final_bytes, _ = filesystem_size(userdata)
    require(final_bytes <= target_bytes,
            "缩容后文件系统仍超出目标")

    fd = os.open(userdata, os.O_RDWR | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

    print("SHRINK VERIFIED:", final_bytes)
    print("GPT 尚未修改；不要直接启动正常系统，"
          "应继续受控迁移或保留旧分区布局。")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit(
            "SHRINK STOPPED: " + str(exc)
            + "\n不得继续写 GPT；若缩容已开始，应先重新离线检查文件系统。"
        )