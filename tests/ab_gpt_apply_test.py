#!/usr/bin/env python3
import os
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
sys.path.insert(0, str(PROJECT / "tests"))

import ab_gpt_apply as apply
import ab_migrate_preflight as preflight
import ab_gpt_prepare_test as fixtures


class GptApplyTests(unittest.TestCase):
    def setUp(self):
        # 只使用 fixture 的 setUp，不重复运行它的测试。
        self.fixture = fixtures.GptPrepareTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

        self.fixture.prepare()
        self.plan = self.fixture.plan
        self.old = apply.load_files(self.fixture.backup)
        self.new = apply.load_files(self.fixture.output)

        self.temp = tempfile.TemporaryDirectory(prefix="amp-gpt-test-")
        self.addCleanup(self.temp.cleanup)
        self.image = Path(self.temp.name) / "disk.img"

        self.fd = os.open(
            self.image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600
        )
        self.addCleanup(os.close, self.fd)

        # 约 60 GB 的逻辑容量，实际只占用 GPT 数据所需空间。
        os.ftruncate(self.fd, self.plan["disk_sectors"] * 512)

        primary = apply.prepare.header_decode(
            self.old["primary-header.bin"]
        )
        backup = apply.prepare.header_decode(
            self.old["backup-header.bin"]
        )

        for name, lba in (
            ("primary-entries.bin", primary["entries_lba"]),
            ("primary-header.bin", primary["current"]),
            ("backup-entries.bin", backup["entries_lba"]),
            ("backup-header.bin", backup["current"]),
        ):
            apply.write_full(self.fd, self.old[name], lba * 512)

        os.fsync(self.fd)

    def transaction(self, writer=apply.write_full, flush=os.fsync):
        apply.apply_transaction(
            self.fd, self.plan, self.old, self.new,
            flush=flush, writer=writer,
        )

    def primary(self):
        return preflight.read_gpt(
            self.fd, 1, self.plan["disk_sectors"]
        )

    def backup(self):
        sectors = self.plan["disk_sectors"]
        return preflight.read_gpt(self.fd, sectors - 1, sectors)

    def test_success_write_order_and_uuid_preservation(self):
        offsets = []

        def writer(fd, data, offset):
            offsets.append(offset)
            apply.write_full(fd, data, offset)

        self.transaction(writer=writer)

        np = apply.prepare.header_decode(self.new["primary-header.bin"])
        nb = apply.prepare.header_decode(self.new["backup-header.bin"])
        self.assertEqual(offsets, [
            nb["entries_lba"] * 512,
            nb["current"] * 512,
            np["entries_lba"] * 512,
            np["current"] * 512,
        ])

        primary, backup = self.primary(), self.backup()
        self.assertEqual(primary["entries"], backup["entries"])
        self.assertEqual(len(primary["parts"]), 12)
        self.assertEqual(
            [p["partuuid"] for p in primary["parts"][:8]],
            self.fixture.original_guids
        )

    def test_partial_backup_array_failure_keeps_primary_valid(self):
        def writer(fd, data, offset):
            apply.write_full(fd, data[:64], offset)
            raise OSError("injected partial write")

        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        self.assertEqual(self.primary()["entries"],
                         self.old["primary-entries.bin"])
        self.assertEqual(self.primary()["header"],
                         self.old["primary-header.bin"])

    def test_backup_flush_failure_stops_before_primary(self):
        writes = []

        def writer(fd, data, offset):
            writes.append(offset)
            apply.write_full(fd, data, offset)

        def flush(fd):
            raise OSError("injected flush failure")

        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer, flush=flush)

        self.assertEqual(len(writes), 1)
        self.assertEqual(self.primary()["header"],
                         self.old["primary-header.bin"])

    def test_primary_write_failure_keeps_new_backup_valid(self):
        calls = 0

        def writer(fd, data, offset):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise OSError("injected primary failure")
            apply.write_full(fd, data, offset)

        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        self.assertEqual(calls, 3)
        self.assertEqual(self.backup()["entries"],
                         self.new["backup-entries.bin"])
        self.assertEqual(self.primary()["entries"],
                         self.old["primary-entries.bin"])

    def test_readback_corruption_stops_transaction(self):
        writes = []

        def writer(fd, data, offset):
            writes.append(offset)
            apply.write_full(fd, data, offset)
            os.pwrite(fd, b"\xff", offset)

        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        self.assertEqual(len(writes), 1)
        self.assertEqual(self.primary()["entries"],
                         self.old["primary-entries.bin"])

    def test_changed_disk_rejected_before_writing(self):
        os.pwrite(self.fd, b"BAD PART", 512)
        writes = []

        def writer(fd, data, offset):
            writes.append(offset)

        with self.assertRaises(RuntimeError):
            self.transaction(writer=writer)

        self.assertEqual(writes, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)