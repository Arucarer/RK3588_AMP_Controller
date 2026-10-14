#!/usr/bin/env python3
import copy
import hashlib
import json
import struct
import sys
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]  # PROJECT：项目根目录，用于定位被测模块
sys.path.insert(0, str(PROJECT / "tools"))

import ab_gpt_prepare as gpt


def make_header(current, backup, entries_lba, plan, disk_guid, entries):
    # 1. 分配可写字节缓冲区，保存到 raw。
    raw = bytearray(512)
    raw[:8] = b"EFI PART"

    # 2. 按指定二进制布局写入缓冲区。
    struct.pack_into("<IIII", raw, 8, 0x10000, 92, 0, 0)
    struct.pack_into("<QQQQ", raw, 24, current, backup, 34, plan["last_usable_sector"])
    raw[56:72] = disk_guid.bytes_le
    struct.pack_into("<QIII", raw, 72, entries_lba, 128, 128, zlib.crc32(entries))
    struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]))
    # 3. 返回 bytes(raw)。
    return bytes(raw)


class GptPrepareTests(unittest.TestCase):
    def setUp(self):
        # 1. 创建测试临时目录，保存到 self.temp。
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.backup = self.root / "backup"
        self.backup.mkdir()
        self.output = self.root / "prepared"

        # 2. 解析 JSON 文本，保存到 self.plan。
        self.plan = json.loads((PROJECT / "docs/ab-layout-draft-v1.json").read_text())
        self.disk_guid = uuid.uuid4()
        self.type_guid = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
        self.original_guids = []
        entries = bytearray(128 * 128)

        # 3. 遍历 self.plan["expected_current_partitions"]，逐项处理 part。
        for part in self.plan["expected_current_partitions"]:
            offset = (part["number"] - 1) * 128
            partition_guid = uuid.uuid4()
            self.original_guids.append(str(partition_guid))

            entries[offset : offset + 16] = self.type_guid.bytes_le
            entries[offset + 16 : offset + 32] = partition_guid.bytes_le
            struct.pack_into(
                "<QQQ", entries, offset + 32, part["start_sector"], part["end_sector"], 0
            )
            name = part["name"].encode("utf-16le")
            entries[offset + 56 : offset + 56 + len(name)] = name

        # 4. 生成不可变字节数据，保存到 self.old_entries。
        self.old_entries = bytes(entries)
        sectors = self.plan["disk_sectors"]

        # 5. 生成不可变字节数据，保存到 files。
        files = {
            "protective-mbr.bin": bytes(512),
            "primary-header.bin": make_header(
                1, sectors - 1, 2, self.plan, self.disk_guid, self.old_entries
            ),
            "primary-entries.bin": self.old_entries,
            "backup-header.bin": make_header(
                sectors - 1, 1, sectors - 33, self.plan, self.disk_guid, self.old_entries
            ),
            "backup-entries.bin": self.old_entries,
        }
        # 6. 遍历 files.items()，逐项处理 name, data。
        for name, data in files.items():
            (self.backup / name).write_bytes(data)

        # 7. 调用 str，保存到 self.report。
        self.report = {
            "sectors": sectors,
            "disk_guid": str(self.disk_guid),
            "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
        }
        self.write_report()

    def write_report(self):
        # 1. 写入文本文件。
        (self.backup / "report.json").write_text(json.dumps(self.report))

    def prepare(self, plan=None):
        # 1. 返回 gpt.prepare(self.backup, self.plan if plan is None else plan, self.output)。
        return gpt.prepare(self.backup, self.plan if plan is None else plan, self.output)

    def replace_backup_file(self, name, data):
        # 更新摘要，以便进一步测试 GPT 自身 CRC 校验。
        (self.backup / name).write_bytes(data)
        # 1. 调用 hashlib.sha256(data).hexdigest，保存到 self.report["sha256"][name]。
        self.report["sha256"][name] = hashlib.sha256(data).hexdigest()
        self.write_report()

    def test_preserves_existing_partition_uuids(self):
        # 1. 调用 self.prepare，保存到 result。
        result = self.prepare()
        parts = result["partitions"]

        # 2. 断言实际值与预期值一致。
        self.assertEqual(len(parts), 12)
        self.assertEqual([p["partuuid"] for p in parts[:8]], self.original_guids)
        self.assertEqual(len({p["partuuid"] for p in parts}), 12)
        self.assertEqual([p["name"] for p in parts[8:]], ["boot_b", "amp_b", "rootfs_b", "abmeta"])

        # 3. 读取二进制文件，保存到 new_entries。
        new_entries = (self.output / "primary-entries.bin").read_bytes()
        self.assertEqual(new_entries[: 7 * 128], self.old_entries[: 7 * 128])

        # userdata 只能改变结束扇区字段。
        old_userdata = bytearray(self.old_entries[7 * 128 : 8 * 128])
        new_userdata = bytearray(new_entries[7 * 128 : 8 * 128])
        old_userdata[40:48] = new_userdata[40:48]
        self.assertEqual(old_userdata, new_userdata)

    def test_new_primary_and_backup_crc_valid(self):
        # 1. 调用 self.prepare。
        self.prepare()
        entries = (self.output / "primary-entries.bin").read_bytes()
        self.assertEqual(entries, (self.output / "backup-entries.bin").read_bytes())

        # 2. 遍历 ("primary-header.bin", "backup-header.bin")，逐项处理 name。
        for name in ("primary-header.bin", "backup-header.bin"):
            header = gpt.header_decode((self.output / name).read_bytes())
            parts = gpt.decode_entries(entries, header)
            self.assertEqual(len(parts), 12)

    def test_corrupt_backup_hash_rejected(self):
        # 1. 设置 path，供后续逻辑使用。
        path = self.backup / "primary-entries.bin"
        data = bytearray(path.read_bytes())
        data[16] ^= 1
        path.write_bytes(data)

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "摘要"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_corrupt_header_crc_rejected(self):
        # 1. 设置 name，供后续逻辑使用。
        name = "primary-header.bin"
        data = bytearray((self.backup / name).read_bytes())
        data[56] ^= 1
        self.replace_backup_file(name, bytes(data))

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "头 CRC"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_corrupt_entries_crc_rejected(self):
        # 1. 分配可写字节缓冲区，保存到 data。
        data = bytearray(self.old_entries)
        data[16] ^= 1
        # 2. 遍历 ("primary-entries.bin", "backup-entries.bin")，逐项处理 name。
        for name in ("primary-entries.bin", "backup-entries.bin"):
            self.replace_backup_file(name, bytes(data))

        # 3. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "数组 CRC"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_changed_existing_partition_rejected(self):
        # 1. 调用 copy.deepcopy，保存到 plan。
        plan = copy.deepcopy(self.plan)
        plan["proposed_partitions"][2]["start_sector"] += 1

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "前七"):
            self.prepare(plan)
        self.assertFalse(self.output.exists())

    def test_overlap_rejected(self):
        # 1. 调用 copy.deepcopy，保存到 plan。
        plan = copy.deepcopy(self.plan)
        part = plan["proposed_partitions"][8]
        part["start_sector"] -= 1
        part["end_sector"] = part["start_sector"] + part["sectors"] - 1

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "重叠"):
            self.prepare(plan)
        self.assertFalse(self.output.exists())

    def test_existing_output_not_overwritten(self):
        # 1. 创建所需目录。
        self.output.mkdir()
        marker = self.output / "keep.txt"
        marker.write_text("keep")

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual(marker.read_text(), "keep")
        self.assertEqual(list(self.output.iterdir()), [marker])


if __name__ == "__main__":
    unittest.main(verbosity=2)
