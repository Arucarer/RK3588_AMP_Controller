#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))

import ab_userdata_shrink as shrink


class Ext4ShrinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        required = ("mkfs.ext4", "e2fsck", "resize2fs", "debugfs")
        missing = [name for name in required if not shutil.which(name)]
        if missing:
            raise RuntimeError("缺少主机工具：" + ", ".join(missing))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="amp-shrink-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def run_tool(self, args):
        result = subprocess.run(
            args, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True
        )
        self.assertEqual(
            result.returncode, 0,
            "命令失败：\n" + " ".join(map(str, args)) + "\n" + result.stdout
        )
        return result.stdout

    def test_real_ext4_shrink_preserves_file(self):
        image = self.directory / "userdata.ext4"
        payload = self.directory / "payload.bin"
        recovered = self.directory / "recovered.bin"

        # 普通稀疏文件，初始 128 MiB。
        with image.open("xb") as stream:
            stream.truncate(128 * 1024 * 1024)

        content = bytes(range(256)) * 4096
        payload.write_bytes(content)

        self.run_tool([
            "mkfs.ext4", "-q", "-F", "-b", "4096", str(image)
        ])
        self.run_tool([
            "debugfs", "-w",
            "-R", f"write {payload} preserve.bin",
            str(image),
        ])

        before, block_size = shrink.filesystem_size(image)
        self.assertEqual(before, 128 * 1024 * 1024)
        self.assertEqual(block_size, 4096)

        shrink.run_check(image, repair=False)
        shrink.run_check(image, repair=True)

        # 与执行工具一致，明确指定 K 单位。
        self.run_tool([
            "resize2fs", str(image), "65536K"
        ])

        shrink.run_check(image, repair=False)
        after, after_block_size = shrink.filesystem_size(image)
        self.assertEqual(after, 64 * 1024 * 1024)
        self.assertEqual(after_block_size, block_size)

        self.run_tool([
            "debugfs", "-R",
            f"dump /preserve.bin {recovered}",
            str(image),
        ])
        self.assertTrue(recovered.is_file())
        self.assertEqual(recovered.read_bytes(), content)

        # 模拟文件系统缩容后再缩小外围容器容量。
        # 实际板上对应“先缩文件系统，再缩分区”。
        with image.open("r+b") as stream:
            stream.truncate(72 * 1024 * 1024)

        shrink.run_check(image, repair=False)
        self.assertEqual(
            shrink.filesystem_size(image)[0],
            64 * 1024 * 1024
        )

    def test_non_ext_filesystem_rejected(self):
        image = self.directory / "invalid.img"
        image.write_bytes(bytes(4096))
        with self.assertRaisesRegex(RuntimeError, "不是 ext"):
            shrink.filesystem_size(image)


if __name__ == "__main__":
    unittest.main(verbosity=2)