#!/usr/bin/env python3
import os
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]  # PROJECT：项目根目录，用于定位被测模块
sys.path.insert(0, str(PROJECT / "tools"))
sys.path.insert(0, str(PROJECT / "tests"))

import ab_gpt_apply as apply
import ab_migrate_preflight as preflight
import ab_gpt_prepare_test as fixtures


class GptApplyTests(unittest.TestCase):
    def setUp(self):
        # 只使用 fixture 的 setUp，不重复运行它的测试。
        self.fixture = fixtures.GptPrepareTests()
        # 1. 调用 self.fixture.setUp。
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

        # 2. 调用 self.fixture.prepare。
        self.fixture.prepare()
        self.plan = self.fixture.plan
        self.old = apply.load_files(self.fixture.backup)
        self.new = apply.load_files(self.fixture.output)

        # 3. 创建测试临时目录，保存到 self.temp。
        self.temp = tempfile.TemporaryDirectory(prefix="amp-gpt-test-")
        self.addCleanup(self.temp.cleanup)
        self.image = Path(self.temp.name) / "disk.img"

        # 4. 打开文件或设备，保存到 self.fd。
        self.fd = os.open(self.image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        self.addCleanup(os.close, self.fd)

        # 约 60 GB 的逻辑容量，实际只占用 GPT 数据所需空间。
        os.ftruncate(self.fd, self.plan["disk_sectors"] * 512)

        # 5. 调用 apply.prepare.header_decode，保存到 primary。
        primary = apply.prepare.header_decode(self.old["primary-header.bin"])
        backup = apply.prepare.header_decode(self.old["backup-header.bin"])

        # 6. 遍历 ( ("primary-entries.bin", primary["entries_lba"]), ("primary-header.bin", primary["current"]), ("backup-entries.bin", backup["entries_lba"]), ("backup-header.bin", backup["current"]), )，逐项处理 name, lba。
        for name, lba in (
            ("primary-entries.bin", primary["entries_lba"]),
            ("primary-header.bin", primary["current"]),
            ("backup-entries.bin", backup["entries_lba"]),
            ("backup-header.bin", backup["current"]),
        ):
            apply.write_full(self.fd, self.old[name], lba * 512)

        # 7. 同步文件数据到存储设备。
        os.fsync(self.fd)

    def transaction(self, writer=apply.write_full, flush=os.fsync):
        # 1. 调用 apply.apply_transaction。
        apply.apply_transaction(
            self.fd,
            self.plan,
            self.old,
            self.new,
            flush=flush,
            writer=writer,
        )

    def primary(self):
        # 1. 返回 preflight.read_gpt(self.fd, 1, self.plan["disk_sectors"])。
        return preflight.read_gpt(self.fd, 1, self.plan["disk_sectors"])

    def backup(self):
        # 1. 设置 sectors，供后续逻辑使用。
        sectors = self.plan["disk_sectors"]
        # 2. 返回 preflight.read_gpt(self.fd, sectors - 1, sectors)。
        return preflight.read_gpt(self.fd, sectors - 1, sectors)

    def test_success_write_order_and_uuid_preservation(self):
        # 1. 设置 offsets，供后续逻辑使用。
        offsets = []

        # 2. 调用 offsets.append。
        def writer(fd, data, offset):
            # 1. 调用 offsets.append。
            offsets.append(offset)
            apply.write_full(fd, data, offset)

        # 3. 调用 self.transaction。
        self.transaction(writer=writer)

        # 4. 调用 apply.prepare.header_decode，保存到 np。
        np = apply.prepare.header_decode(self.new["primary-header.bin"])
        nb = apply.prepare.header_decode(self.new["backup-header.bin"])
        self.assertEqual(
            offsets,
            [
                nb["entries_lba"] * 512,
                nb["current"] * 512,
                np["entries_lba"] * 512,
                np["current"] * 512,
            ],
        )

        # 5. 调用 self.primary，保存到 primary, backup。
        primary, backup = self.primary(), self.backup()
        self.assertEqual(primary["entries"], backup["entries"])
        self.assertEqual(len(primary["parts"]), 12)
        self.assertEqual([p["partuuid"] for p in primary["parts"][:8]], self.fixture.original_guids)

    def test_partial_backup_array_failure_keeps_primary_valid(self):
        # 1. 调用 apply.write_full。
        def writer(fd, data, offset):
            # 1. 调用 apply.write_full。
            apply.write_full(fd, data[:64], offset)
            raise OSError("injected partial write")

        # 2. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        # 3. 断言实际值与预期值一致。
        self.assertEqual(self.primary()["entries"], self.old["primary-entries.bin"])
        self.assertEqual(self.primary()["header"], self.old["primary-header.bin"])

    def test_backup_flush_failure_stops_before_primary(self):
        # 1. 设置 writes，供后续逻辑使用。
        writes = []

        # 2. 调用 writes.append。
        def writer(fd, data, offset):
            # 1. 调用 writes.append。
            writes.append(offset)
            apply.write_full(fd, data, offset)

        # 3. 调用 OSError。
        def flush(fd):
            # 1. 抛出 OSError("injected flush failure")，结束当前失败路径。
            raise OSError("injected flush failure")

        # 4. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer, flush=flush)

        # 5. 断言实际值与预期值一致。
        self.assertEqual(len(writes), 1)
        self.assertEqual(self.primary()["header"], self.old["primary-header.bin"])

    def test_primary_write_failure_keeps_new_backup_valid(self):
        # 1. 设置 calls，供后续逻辑使用。
        calls = 0

        # 2. 调用 apply.write_full。
        def writer(fd, data, offset):
            nonlocal calls
            # 1. 设置 calls，供后续逻辑使用。
            calls += 1
            # 2. 检查条件；不满足要求时报告“injected primary failure”。
            if calls == 3:
                raise OSError("injected primary failure")
            apply.write_full(fd, data, offset)

        # 3. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        # 4. 断言实际值与预期值一致。
        self.assertEqual(calls, 3)
        self.assertEqual(self.backup()["entries"], self.new["backup-entries.bin"])
        self.assertEqual(self.primary()["entries"], self.old["primary-entries.bin"])

    def test_readback_corruption_stops_transaction(self):
        # 1. 设置 writes，供后续逻辑使用。
        writes = []

        # 2. 调用 writes.append。
        def writer(fd, data, offset):
            # 1. 调用 writes.append。
            writes.append(offset)
            apply.write_full(fd, data, offset)
            os.pwrite(fd, b"\xff", offset)

        # 3. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(apply.GptWriteUncertain):
            self.transaction(writer=writer)

        # 4. 断言实际值与预期值一致。
        self.assertEqual(len(writes), 1)
        self.assertEqual(self.primary()["entries"], self.old["primary-entries.bin"])

    def test_changed_disk_rejected_before_writing(self):
        # 1. 按偏移写入设备数据。
        os.pwrite(self.fd, b"BAD PART", 512)
        writes = []

        # 2. 调用 writes.append。
        def writer(fd, data, offset):
            # 1. 调用 writes.append。
            writes.append(offset)

        # 3. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaises(RuntimeError):
            self.transaction(writer=writer)

        # 4. 断言实际值与预期值一致。
        self.assertEqual(writes, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
