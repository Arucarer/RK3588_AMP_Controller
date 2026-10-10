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

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))

import ab_gpt_prepare as gpt


def make_header(current, backup, entries_lba, plan, disk_guid, entries):
    raw = bytearray(512)
    raw[:8] = b"EFI PART"

    struct.pack_into("<IIII", raw, 8, 0x10000, 92, 0, 0)
    struct.pack_into(
        "<QQQQ", raw, 24,
        current, backup, 34, plan["last_usable_sector"]
    )
    raw[56:72] = disk_guid.bytes_le
    struct.pack_into(
        "<QIII", raw, 72,
        entries_lba, 128, 128, zlib.crc32(entries)
    )
    struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]))
    return bytes(raw)


class GptPrepareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.backup = self.root / "backup"
        self.backup.mkdir()
        self.output = self.root / "prepared"

        self.plan = json.loads(
            (PROJECT / "docs/ab-layout-draft-v1.json").read_text()
        )
        self.disk_guid = uuid.uuid4()
        self.type_guid = uuid.UUID(
            "0fc63daf-8483-4772-8e79-3d69d8477de4"
        )
        self.original_guids = []
        entries = bytearray(128 * 128)

        for part in self.plan["expected_current_partitions"]:
            offset = (part["number"] - 1) * 128
            partition_guid = uuid.uuid4()
            self.original_guids.append(str(partition_guid))

            entries[offset:offset + 16] = self.type_guid.bytes_le
            entries[offset + 16:offset + 32] = partition_guid.bytes_le
            struct.pack_into(
                "<QQQ", entries, offset + 32,
                part["start_sector"], part["end_sector"], 0
            )
            name = part["name"].encode("utf-16le")
            entries[offset + 56:offset + 56 + len(name)] = name

        self.old_entries = bytes(entries)
        sectors = self.plan["disk_sectors"]

        files = {
            "protective-mbr.bin": bytes(512),
            "primary-header.bin": make_header(
                1, sectors - 1, 2, self.plan,
                self.disk_guid, self.old_entries
            ),
            "primary-entries.bin": self.old_entries,
            "backup-header.bin": make_header(
                sectors - 1, 1, sectors - 33, self.plan,
                self.disk_guid, self.old_entries
            ),
            "backup-entries.bin": self.old_entries,
        }
        for name, data in files.items():
            (self.backup / name).write_bytes(data)

        self.report = {
            "sectors": sectors,
            "disk_guid": str(self.disk_guid),
            "sha256": {
                name: hashlib.sha256(data).hexdigest()
                for name, data in files.items()
            },
        }
        self.write_report()

    def write_report(self):
        (self.backup / "report.json").write_text(
            json.dumps(self.report)
        )

    def prepare(self, plan=None):
        return gpt.prepare(
            self.backup,
            self.plan if plan is None else plan,
            self.output
        )

    def replace_backup_file(self, name, data):
        # 更新摘要，以便进一步测试 GPT 自身 CRC 校验。
        (self.backup / name).write_bytes(data)
        self.report["sha256"][name] = hashlib.sha256(data).hexdigest()
        self.write_report()

    def test_preserves_existing_partition_uuids(self):
        result = self.prepare()
        parts = result["partitions"]

        self.assertEqual(len(parts), 12)
        self.assertEqual(
            [p["partuuid"] for p in parts[:8]],
            self.original_guids
        )
        self.assertEqual(len({p["partuuid"] for p in parts}), 12)
        self.assertEqual(
            [p["name"] for p in parts[8:]],
            ["boot_b", "amp_b", "rootfs_b", "abmeta"]
        )

        new_entries = (
            self.output / "primary-entries.bin"
        ).read_bytes()
        self.assertEqual(
            new_entries[:7 * 128],
            self.old_entries[:7 * 128]
        )

        # userdata 只能改变结束扇区字段。
        old_userdata = bytearray(self.old_entries[7 * 128:8 * 128])
        new_userdata = bytearray(new_entries[7 * 128:8 * 128])
        old_userdata[40:48] = new_userdata[40:48]
        self.assertEqual(old_userdata, new_userdata)

    def test_new_primary_and_backup_crc_valid(self):
        self.prepare()
        entries = (self.output / "primary-entries.bin").read_bytes()
        self.assertEqual(
            entries,
            (self.output / "backup-entries.bin").read_bytes()
        )

        for name in ("primary-header.bin", "backup-header.bin"):
            header = gpt.header_decode((self.output / name).read_bytes())
            parts = gpt.decode_entries(entries, header)
            self.assertEqual(len(parts), 12)

    def test_corrupt_backup_hash_rejected(self):
        path = self.backup / "primary-entries.bin"
        data = bytearray(path.read_bytes())
        data[16] ^= 1
        path.write_bytes(data)

        with self.assertRaisesRegex(RuntimeError, "摘要"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_corrupt_header_crc_rejected(self):
        name = "primary-header.bin"
        data = bytearray((self.backup / name).read_bytes())
        data[56] ^= 1
        self.replace_backup_file(name, bytes(data))

        with self.assertRaisesRegex(RuntimeError, "头 CRC"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_corrupt_entries_crc_rejected(self):
        data = bytearray(self.old_entries)
        data[16] ^= 1
        for name in ("primary-entries.bin", "backup-entries.bin"):
            self.replace_backup_file(name, bytes(data))

        with self.assertRaisesRegex(RuntimeError, "数组 CRC"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_changed_existing_partition_rejected(self):
        plan = copy.deepcopy(self.plan)
        plan["proposed_partitions"][2]["start_sector"] += 1

        with self.assertRaisesRegex(RuntimeError, "前七"):
            self.prepare(plan)
        self.assertFalse(self.output.exists())

    def test_overlap_rejected(self):
        plan = copy.deepcopy(self.plan)
        part = plan["proposed_partitions"][8]
        part["start_sector"] -= 1
        part["end_sector"] = part["start_sector"] + part["sectors"] - 1

        with self.assertRaisesRegex(RuntimeError, "重叠"):
            self.prepare(plan)
        self.assertFalse(self.output.exists())

    def test_existing_output_not_overwritten(self):
        self.output.mkdir()
        marker = self.output / "keep.txt"
        marker.write_text("keep")

        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual(marker.read_text(), "keep")
        self.assertEqual(list(self.output.iterdir()), [marker])


if __name__ == "__main__":
    unittest.main(verbosity=2)