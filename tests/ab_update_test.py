#!/usr/bin/env python3
import hashlib
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import amp_ab_update as update


def encode(state):
    # 1. 分配可写字节缓冲区，保存到 record。
    record = bytearray(update.META_BYTES)
    record[:8] = b"AMPAB001"
    struct.pack_into("<IIQIIII", record, 8, 1, update.META_BYTES, *state)
    struct.pack_into("<I", record, 4092, zlib.crc32(record[:4092]))
    # 2. 返回 bytes(record)。
    return bytes(record)


class UpdateTests(unittest.TestCase):
    def setUp(self):
        # 1. 创建测试临时目录，保存到 self.temp。
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

        # 2. 设置 self.state，供后续逻辑使用。
        self.state = (1, 0, update.NONE, 0, update.NONE)
        self.old = encode(self.state)
        self.meta = os.open(self.work / "meta.bin", os.O_CREAT | os.O_RDWR, 0o600)
        self.addCleanup(os.close, self.meta)
        os.write(self.meta, self.old + self.old)
        self.baseline = update.select(self.meta)
        self.identity = (0, 1, "0")

        # 3. 设置 self.targets，供后续逻辑使用。
        self.targets = {}
        self.manifest = {"images": {}}
        # 4. 遍历 enumerate(("boot", "amp", "rootfs"), 1)，逐项处理 index, name。
        for index, name in enumerate(("boot", "amp", "rootfs"), 1):
            payload = (name.encode() + b":payload") * 20
            (self.work / (name + ".img")).write_bytes(payload)
            path = self.work / (name + ".partition")
            path.write_bytes(b"\0" * 8192)

            self.targets[name] = {
                "name": name + "_b",
                "path": path,
                "dev": index,
                "size": 8192,
            }
            self.manifest["images"][name] = {
                "size": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }

    def fake_open(self, part, write=False):
        # 1. 返回 os.open(part["path"], os.O_RDWR if write else os.O_RDONLY)。
        return os.open(part["path"], os.O_RDWR if write else os.O_RDONLY)

    def execute(self):
        # 只将设备访问替换为临时普通文件。
        # 镜像写入、fsync、读回哈希和元数据编码使用真实实现。
        with patch.object(update, "boot_identity", return_value=self.identity), patch.object(
            update, "require_unused"
        ), patch.object(update, "open_partition", side_effect=self.fake_open), patch.object(
            update.fcntl, "ioctl", return_value=0
        ):
            return update.execute_update(
                self.work, self.manifest, self.targets, self.meta, self.baseline, self.identity
            )

    def assert_old_metadata(self):
        # 1. 断言实际值与预期值一致。
        self.assertEqual(os.pread(self.meta, 8192, 0), self.old + self.old)

    def test_success_stages_only_after_three_images(self):
        # 1. 调用 self.execute，保存到 target, generation。
        target, generation = self.execute()
        self.assertEqual((target, generation), (1, 2))
        selected, state, records = update.select(self.meta)
        self.assertEqual(state, (2, 0, 1, 3, update.NONE))
        self.assertEqual(records[0], self.old)

        # 2. 遍历 self.targets.items()，逐项处理 name, part。
        for name, part in self.targets.items():
            payload = (self.work / (name + ".img")).read_bytes()
            self.assertEqual(part["path"].read_bytes()[: len(payload)], payload)

    def test_active_slot_rejected_before_write(self):
        # 1. 设置 self.targets["boot"]["name"]，供后续逻辑使用。
        self.targets["boot"]["name"] = "boot"
        # 2. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(update, "install_image") as install:
            with self.assertRaisesRegex(RuntimeError, "非活动槽"):
                self.execute()
            install.assert_not_called()
        self.assert_old_metadata()

    def test_signature_failure_never_reads_manifest(self):
        # 1. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(
            update.ota_release, "verify", side_effect=RuntimeError("bad signature")
        ), patch.object(update, "install_image") as install:
            with self.assertRaisesRegex(RuntimeError, "bad signature"):
                update.verify_snapshot(self.work, self.work / "public.pem")
            install.assert_not_called()
        self.assert_old_metadata()

    def test_partial_image_write_does_not_stage(self):
        # 1. 设置 original，供后续逻辑使用。
        original = update.write_all

        # 2. 调用 bytes(data).startswith。
        def fail_rootfs(fd, data, offset):
            # rootfs 最后写入；让它留下部分数据后失败。
            if bytes(data).startswith(b"rootfs:"):
                os.pwrite(fd, bytes(data)[:7], offset)
                raise OSError("injected partial image write")
            # 1. 返回 original(fd, data, offset)。
            return original(fd, data, offset)

        # 3. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(update, "write_all", side_effect=fail_rootfs):
            with self.assertRaisesRegex(OSError, "partial image"):
                self.execute()
        self.assert_old_metadata()

    def test_image_readback_failure_does_not_stage(self):
        # 1. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(update, "digest_fd", return_value="bad-hash"):
            with self.assertRaisesRegex(RuntimeError, "读回校验失败"):
                self.execute()
        self.assert_old_metadata()

    def test_partial_pending_preserves_old_copy(self):
        # 1. 设置 original，供后续逻辑使用。
        original = update.write_all

        # 2. 调用 original。
        def fail_meta(fd, data, offset):
            # 1. 检查条件；不满足要求时报告“injected partial metadata write”。
            if fd == self.meta:
                os.pwrite(fd, bytes(data)[:64], offset)
                raise OSError("injected partial metadata write")
            # 2. 返回 original(fd, data, offset)。
            return original(fd, data, offset)

        # 3. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(update, "write_all", side_effect=fail_meta):
            with self.assertRaises(update.PendingCommitUncertain):
                self.execute()

        # 4. 断言实际值与预期值一致。
        self.assertEqual(os.pread(self.meta, 4096, 0), self.old)
        self.assertEqual(update.select(self.meta)[1], self.state)

    def test_pending_flush_failure_is_uncertain(self):
        # 1. 设置 original，供后续逻辑使用。
        original = os.fsync

        # 2. 调用 original。
        def fail_meta_sync(fd):
            # 1. 检查条件；不满足要求时报告“injected metadata flush failure”。
            if fd == self.meta:
                raise OSError("injected metadata flush failure")
            # 2. 返回 original(fd)。
            return original(fd)

        # 3. 在 测试替身 上下文中执行，退出时恢复或释放资源。
        with patch.object(update.os, "fsync", side_effect=fail_meta_sync):
            with self.assertRaises(update.PendingCommitUncertain):
                self.execute()

        # 完整记录可能已经可读，因此不能把失败理解成“未提交”。
        self.assertEqual(os.pread(self.meta, 4096, 0), self.old)

    def test_invalid_target_rejected(self):
        # 1. 在 预期异常检查 上下文中执行，退出时恢复或释放资源。
        with self.assertRaisesRegex(RuntimeError, "目标槽"):
            update.stage_record(self.state, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
