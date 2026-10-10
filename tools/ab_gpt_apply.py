#!/usr/bin/env python3
import argparse
import json
import os
import stat
import tempfile
from pathlib import Path

import ab_gpt_prepare as prepare
import ab_migrate_preflight as preflight


class GptWriteUncertain(RuntimeError):
    """写入已经开始，不能假定磁盘仍保持原布局。"""


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def write_full(fd, data, offset):
    view = memoryview(data)
    while view:
        count = os.pwrite(fd, view, offset)
        require(count > 0, "GPT 写入未推进")
        view = view[count:]
        offset += count


def same_gpt(fd, sectors, expected_primary, expected_backup, entries):
    primary = preflight.read_gpt(fd, 1, sectors)
    backup = preflight.read_gpt(fd, sectors - 1, sectors)
    require(primary["header"] == expected_primary,
            "主 GPT 头读回不一致")
    require(backup["header"] == expected_backup,
            "备 GPT 头读回不一致")
    require(primary["entries"] == backup["entries"] == entries,
            "GPT 数组读回不一致")


def validate_transition(old_primary, old_backup, new_primary, new_backup,
                        old_entries, new_entries, plan):
    op = prepare.header_decode(old_primary)
    ob = prepare.header_decode(old_backup)
    np = prepare.header_decode(new_primary)
    nb = prepare.header_decode(new_backup)

    sectors = plan["disk_sectors"]
    require(plan["sector_bytes"] == 512, "扇区规格错误")

    for primary, backup in ((op, ob), (np, nb)):
        require(primary["current"] == 1
                and primary["backup"] == sectors - 1
                and backup["current"] == sectors - 1
                and backup["backup"] == 1,
                "GPT 主备指针错误")
        for field in ("first", "last", "disk_guid", "count", "entry_size"):
            require(primary[field] == backup[field],
                    "主备 GPT 规格不一致")

    # 转换仅允许改变数组 CRC 和头 CRC，其他头字段保持原样。
    for before, after in ((old_primary, new_primary),
                          (old_backup, new_backup)):
        left, right = bytearray(before), bytearray(after)
        for start, end in ((16, 20), (88, 92)):
            left[start:end] = right[start:end]
        require(left == right, "GPT 头出现未授权变化")

    require(op["last"] == plan["last_usable_sector"],
            "GPT 可用边界与计划不一致")

    old_parts = prepare.decode_entries(old_entries, op)
    prepare.decode_entries(old_entries, ob)
    new_parts = prepare.decode_entries(new_entries, np)
    prepare.decode_entries(new_entries, nb)

    for actual, expected in (
        (old_parts, plan["expected_current_partitions"]),
        (new_parts, plan["proposed_partitions"]),
    ):
        require(len(actual) == len(expected), "分区数量不匹配")
        previous = op["first"] - 1
        for part, wanted in zip(actual, expected):
            for field in prepare.FIELDS:
                require(part[field] == wanted[field],
                        "分区布局不匹配：" + wanted["name"])
            require(previous < part["start_sector"]
                    and part["end_sector"] <= op["last"],
                    "分区重叠或越界")
            previous = part["end_sector"]

    require(len(old_parts) == 8 and len(new_parts) == 12,
            "仅支持旧八分区到新十二分区")

    require(new_entries[:7 * 128] == old_entries[:7 * 128],
            "前七个 GPT 项发生变化")

    old_userdata = bytearray(old_entries[7 * 128:8 * 128])
    new_userdata = bytearray(new_entries[7 * 128:8 * 128])
    old_userdata[40:48] = new_userdata[40:48]
    require(old_userdata == new_userdata,
            "userdata 除结束扇区外发生变化")

    require([p["partuuid"] for p in old_parts] ==
            [p["partuuid"] for p in new_parts[:8]],
            "旧 PARTUUID 未保留")
    guids = [p["partuuid"] for p in new_parts]
    require(len(set(guids)) == 12
            and "00000000-0000-0000-0000-000000000000" not in guids,
            "新 PARTUUID 重复或为空")

    require(new_entries[12 * 128:] == old_entries[12 * 128:],
            "第十二项以后发生变化")

    array_sectors = (len(new_entries) + 511) // 512
    require(2 <= np["entries_lba"]
            and np["entries_lba"] + array_sectors <= np["first"],
            "主 GPT 数组越界")
    require(np["last"] < nb["entries_lba"]
            and nb["entries_lba"] + array_sectors <= sectors - 1,
            "备 GPT 数组越界")

    return np, nb


def apply_transaction(fd, plan, old_files, new_files,
                      flush, writer=write_full):
    """
    调用者负责证明 userdata 已缩容并保持未挂载。
    flush 必须提供真实同步，失败必须抛出异常。
    此核心不调用分区表重读，不自动复位或重试。
    """
    require(callable(flush), "必须提供刷新函数")

    old_entries = old_files["primary-entries.bin"]
    new_entries = new_files["primary-entries.bin"]
    require(old_entries == old_files["backup-entries.bin"],
            "旧主备数组不一致")
    require(new_entries == new_files["backup-entries.bin"],
            "新主备数组不一致")

    np, nb = validate_transition(
        old_files["primary-header.bin"],
        old_files["backup-header.bin"],
        new_files["primary-header.bin"],
        new_files["backup-header.bin"],
        old_entries, new_entries, plan,
    )

    sectors = plan["disk_sectors"]

    # 所有写入前检查必须在第一次 writer 调用之前完成。
    same_gpt(
        fd, sectors,
        old_files["primary-header.bin"],
        old_files["backup-header.bin"],
        old_entries,
    )

    operations = (
        ("backup-entries.bin", nb["entries_lba"]),
        ("backup-header.bin", nb["current"]),
        ("primary-entries.bin", np["entries_lba"]),
        ("primary-header.bin", np["current"]),
    )

    try:
        for name, lba in operations:
            data = new_files[name]
            writer(fd, data, lba * 512)
            flush(fd)
            require(os.pread(fd, len(data), lba * 512) == data,
                    "GPT 写后读回不一致：" + name)

            if name == "backup-header.bin":
                # 主 GPT 在备用 GPT 验证通过前保持原样。
                backup = preflight.read_gpt(fd, sectors - 1, sectors)
                require(backup["header"] ==
                        new_files["backup-header.bin"]
                        and backup["entries"] == new_entries,
                        "备用 GPT 校验失败")
                require(os.pread(fd, 512, 512) ==
                        old_files["primary-header.bin"],
                        "主 GPT 头意外变化")

        same_gpt(
            fd, sectors,
            new_files["primary-header.bin"],
            new_files["backup-header.bin"],
            new_entries,
        )
    except Exception as exc:
        raise GptWriteUncertain(
            "GPT 写入结果不确定；保留恢复环境，"
            "不得自动重试、重启或继续部署"
        ) from exc


def load_files(directory):
    return {
        name: (directory / name).read_bytes()
        for name in (
            "primary-header.bin", "primary-entries.bin",
            "backup-header.bin", "backup-entries.bin"
        )
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--backup-dir", required=True, type=Path)
    parser.add_argument("--plan", required=True, type=Path)
    args = parser.parse_args()

    # 本轮入口只接受普通文件，明确拒绝所有块设备和软链接。
    info = args.image.lstat()
    require(stat.S_ISREG(info.st_mode),
            "本轮只允许普通磁盘镜像文件")

    plan = json.loads(args.plan.read_text())
    old_files = load_files(args.backup_dir)

    # 使用已有转换器重新验证备份并生成新布局。
    with tempfile.TemporaryDirectory(prefix="amp-gpt-apply-") as temp:
        prepared = Path(temp) / "prepared"
        prepare.prepare(args.backup_dir, plan, prepared)
        new_files = load_files(prepared)

        fd = os.open(args.image, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            opened = os.fstat(fd)
            require(stat.S_ISREG(opened.st_mode)
                    and (opened.st_dev, opened.st_ino) ==
                        (info.st_dev, info.st_ino),
                    "镜像身份变化")
            require(opened.st_size == plan["disk_sectors"] * 512,
                    "镜像容量不匹配")

            # 该入口仅用于没有实际文件系统的合成 GPT 镜像测试。
            apply_transaction(
                fd, plan, old_files, new_files,
                flush=os.fsync,
            )
        finally:
            os.close(fd)

    print("PASS: 普通镜像 GPT 转换、刷新、读回验证")
    print("未访问开发板；此入口不负责文件系统缩容")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit("GPT APPLY STOPPED: " + str(exc))