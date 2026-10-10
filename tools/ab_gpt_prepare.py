#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import struct
import uuid
import zlib
from pathlib import Path

from ab_migrate_preflight import require

SECTOR = 512
FIELDS = ("number", "name", "start_sector", "end_sector", "sectors")


def header_decode(raw):
    require(len(raw) == SECTOR, "GPT 头长度错误")
    require(raw[:8] == b"EFI PART", "GPT 签名错误")

    revision, size, crc, reserved = struct.unpack_from("<IIII", raw, 8)
    require(revision == 0x10000 and 92 <= size <= 512,
            "GPT 头规格错误")
    require(reserved == 0, "GPT 保留字段错误")

    check = bytearray(raw[:size])
    struct.pack_into("<I", check, 16, 0)
    require(zlib.crc32(check) == crc, "GPT 头 CRC 错误")

    current, backup, first, last = struct.unpack_from("<QQQQ", raw, 24)
    entries_lba, count, entry_size, entries_crc = struct.unpack_from(
        "<QIII", raw, 72
    )
    require(12 <= count <= 4096 and entry_size == 128,
            "GPT 项规格不支持")

    return {
        "size": size,
        "current": current,
        "backup": backup,
        "first": first,
        "last": last,
        "disk_guid": raw[56:72],
        "entries_lba": entries_lba,
        "count": count,
        "entry_size": entry_size,
        "entries_crc": entries_crc,
    }


def decode_entries(data, header):
    require(len(data) == header["count"] * 128, "GPT 数组长度错误")
    require(zlib.crc32(data) == header["entries_crc"],
            "GPT 数组 CRC 错误")
    result = []

    for index in range(header["count"]):
        item = data[index * 128:(index + 1) * 128]
        if item[:16] == bytes(16):
            continue

        start, end = struct.unpack_from("<QQ", item, 32)
        require(end >= start, "GPT 分区范围错误")
        result.append({
            "number": index + 1,
            "name": item[56:128].decode("utf-16le").split("\0", 1)[0],
            "start_sector": start,
            "end_sector": end,
            "sectors": end - start + 1,
            "partuuid": str(uuid.UUID(bytes_le=item[16:32])),
        })
    return result


def update_header(raw, entries):
    result = bytearray(raw)
    struct.pack_into("<I", result, 88, zlib.crc32(entries))
    struct.pack_into("<I", result, 16, 0)
    size = struct.unpack_from("<I", result, 12)[0]
    struct.pack_into("<I", result, 16, zlib.crc32(result[:size]))
    return bytes(result)


def prepare(backup, plan, output):
    require(plan["sector_bytes"] == 512, "计划扇区规格错误")
    sectors = plan["disk_sectors"]
    report = json.loads((backup / "report.json").read_text())
    require(report["sectors"] == sectors, "备份磁盘容量不匹配")

    filenames = (
        "protective-mbr.bin",
        "primary-header.bin",
        "primary-entries.bin",
        "backup-header.bin",
        "backup-entries.bin",
    )
    files = {}
    for name in filenames:
        path = backup / name
        require(path.is_file() and not path.is_symlink(),
                "备份文件非法：" + name)
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() ==
                report["sha256"][name],
                "备份摘要不匹配：" + name)
        files[name] = data

    primary = header_decode(files["primary-header.bin"])
    secondary = header_decode(files["backup-header.bin"])

    require(primary["current"] == 1
            and primary["backup"] == sectors - 1
            and secondary["current"] == sectors - 1
            and secondary["backup"] == 1,
            "GPT 主备位置不匹配")

    for field in ("first", "last", "disk_guid", "count", "entry_size"):
        require(primary[field] == secondary[field],
                "主备头不一致：" + field)

    require(primary["last"] == plan["last_usable_sector"],
            "计划可用范围不匹配")
    require(str(uuid.UUID(bytes_le=primary["disk_guid"])) ==
            report["disk_guid"], "磁盘 GUID 不匹配")

    old_entries = files["primary-entries.bin"]
    require(old_entries == files["backup-entries.bin"],
            "主备数组不一致")
    old_parts = decode_entries(old_entries, primary)
    decode_entries(files["backup-entries.bin"], secondary)

    expected = plan["expected_current_partitions"]
    proposed = plan["proposed_partitions"]
    require(len(old_parts) == len(expected) == 8,
            "旧布局必须恰好为八分区")
    require(len(proposed) == 12, "目标布局必须为十二分区")

    for actual, wanted in zip(old_parts, expected):
        require(all(actual[k] == wanted[k] for k in FIELDS),
                "旧布局不匹配：" + wanted["name"])

    require(proposed[:7] == expected[:7],
            "前七个分区不得改变")
    require(proposed[7]["name"] == "userdata"
            and proposed[7]["number"] == 8
            and proposed[7]["start_sector"] ==
                expected[7]["start_sector"]
            and proposed[7]["sectors"] < expected[7]["sectors"],
            "userdata 必须保持起点并缩小")

    require([p["name"] for p in proposed[8:]] ==
            ["boot_b", "amp_b", "rootfs_b", "abmeta"],
            "新增分区名称错误")

    previous_end = primary["first"] - 1
    for index, part in enumerate(proposed, 1):
        require(part["number"] == index, "分区编号错误")
        require(part["sectors"] > 0
                and part["end_sector"] ==
                    part["start_sector"] + part["sectors"] - 1,
                "分区长度错误")
        require(previous_end < part["start_sector"]
                and part["end_sector"] <= primary["last"],
                "分区重叠或越界")
        previous_end = part["end_sector"]

    # 验证 GPT 数组仍位于可用分区范围之外。
    array_sectors = (len(old_entries) + 511) // 512
    require(2 <= primary["entries_lba"]
            and primary["entries_lba"] + array_sectors <= primary["first"],
            "主 GPT 数组位置非法")
    require(primary["last"] < secondary["entries_lba"]
            and secondary["entries_lba"] + array_sectors <= sectors - 1,
            "备 GPT 数组位置非法")

    entries = bytearray(old_entries)

    # 只改 userdata 的结束扇区，保留类型、UUID、属性及名称。
    struct.pack_into("<Q", entries, 7 * 128 + 40,
                     proposed[7]["end_sector"])

    used_guids = {p["partuuid"] for p in old_parts}
    require(len(used_guids) == 8
            and str(uuid.UUID(int=0)) not in used_guids,
            "旧 PARTUUID 非法或重复")

    # boot_b/amp_b/rootfs_b/abmeta 使用旧对应分区的类型 GUID。
    source_numbers = (3, 5, 6, 5)
    for part, source_number in zip(proposed[8:], source_numbers):
        index = part["number"] - 1
        item = bytearray(128)
        source = old_entries[(source_number - 1) * 128:
                             source_number * 128]
        item[:16] = source[:16]

        new_guid = uuid.uuid4()
        while str(new_guid) in used_guids:
            new_guid = uuid.uuid4()
        used_guids.add(str(new_guid))
        item[16:32] = new_guid.bytes_le
        struct.pack_into("<QQQ", item, 32,
                         part["start_sector"], part["end_sector"], 0)
        name = part["name"].encode("utf-16le")
        require(len(name) <= 72, "GPT 名称过长")
        item[56:56 + len(name)] = name
        entries[index * 128:(index + 1) * 128] = item

    entries = bytes(entries)
    new_primary = update_header(files["primary-header.bin"], entries)
    new_secondary = update_header(files["backup-header.bin"], entries)
    parts = decode_entries(entries, header_decode(new_primary))
    decode_entries(entries, header_decode(new_secondary))

    require(entries[:7 * 128] == old_entries[:7 * 128],
            "前七项意外变化")
    require([p["partuuid"] for p in parts[:8]] ==
            [p["partuuid"] for p in old_parts],
            "旧 PARTUUID 意外变化")

    output.mkdir(mode=0o700, exist_ok=False)
    result = {
        "primary-header.bin": new_primary,
        "primary-entries.bin": entries,
        "backup-header.bin": new_secondary,
        "backup-entries.bin": entries,
    }
    for name, data in result.items():
        with (output / name).open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        require((output / name).read_bytes() == data,
                "输出读回失败")

    description = {
        "status": "PREPARED_NOT_APPLIED",
        "disk_sectors": sectors,
        "disk_guid": report["disk_guid"],
        "partitions": parts,
        "writes": [
            {"file": "backup-entries.bin",
             "lba": secondary["entries_lba"]},
            {"file": "backup-header.bin",
             "lba": secondary["current"]},
            {"file": "primary-entries.bin",
             "lba": primary["entries_lba"]},
            {"file": "primary-header.bin",
             "lba": primary["current"]},
        ],
        "sha256": {
            name: hashlib.sha256(data).hexdigest()
            for name, data in result.items()
        },
        "warning": (
            "布局转换文件，不是自动执行脚本。"
            "写盘前必须完成 userdata 文件系统离线缩容及容量核对。"
        ),
    }
    with (output / "prepared.json").open("x") as stream:
        json.dump(description, stream, indent=2)
        stream.write("\n")

    print("PASS: 新 GPT 数据生成，原八个 PARTUUID 保留")
    print("输出：", output)
    print("未访问块设备，未缩容，未修改实际 GPT")
    return description


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backup-dir", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.backup_dir,
            json.loads(args.plan.read_text()),
            args.output)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        raise SystemExit("PREPARE STOPPED: " + str(exc))