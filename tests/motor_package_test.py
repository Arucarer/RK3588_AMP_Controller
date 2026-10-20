import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import json

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_motor_sim", ROOT / "tools/prepare_motor_sim.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class MotorPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="amp-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        for name in ("prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc",
                     "u-boot/tools/mkimage", "u-boot/tools/dumpimage"):
            tool = self.sdk / name
            tool.parent.mkdir(parents=True, exist_ok=True)
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o755)

    def test_existing_output_is_never_replaced(self):
        out = self.root / "out"
        out.mkdir()
        marker = out / "keep"
        marker.write_text("keep")
        with self.assertRaises(FileExistsError):
            package.build(out, self.sdk, 1)
        self.assertEqual(marker.read_text(), "keep")

    def test_symlink_output_is_never_followed(self):
        target = self.root / "target"
        target.mkdir()
        out = self.root / "out"
        out.symlink_to(target, target_is_directory=True)
        with self.assertRaises(FileExistsError):
            package.build(out, self.sdk, 1)
        self.assertEqual(list(target.iterdir()), [])

    def test_missing_tool_has_no_output_side_effect(self):
        out = self.root / "out"
        with self.assertRaises(ValueError):
            package.build(out, self.root / "missing-sdk", 1)
        self.assertFalse(out.exists())

    def test_failure_keeps_logs_and_explicit_failed_state(self):
        out = self.root / "out"
        with patch.object(package, "snapshot_sources"), \
                patch.object(package.subprocess, "check_output", return_value="test\n"), \
                patch.object(package.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "make")):
            with self.assertRaises(subprocess.CalledProcessError):
                package.build(out, self.sdk, 1)
        state = json.loads((out / "build-state.json").read_text())
        self.assertEqual(state["status"], "FAILED")
        self.assertFalse(state["board_verified"])
        self.assertTrue((out / "logs/firmware.log").exists())
        self.assertFalse((out / "SHA256SUMS").exists())

    def test_elf_architecture_and_dynamic_interpreter_checks(self):
        elf = self.root / "program"
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", header, 18, 183)
        struct.pack_into("<Q", header, 32, 64)
        struct.pack_into("<HH", header, 54, 56, 1)
        program_header = bytearray(56)
        elf.write_bytes(header + program_header)
        package.require_arm64_elf(elf, static=True)
        struct.pack_into("<I", program_header, 0, 3)
        elf.write_bytes(header + program_header)
        with self.assertRaises(ValueError):
            package.require_arm64_elf(elf, static=True)
        struct.pack_into("<H", header, 18, 62)
        elf.write_bytes(header + program_header)
        with self.assertRaises(ValueError):
            package.require_arm64_elf(elf)

    def test_inventory_hashes_exact_bytes(self):
        folder = self.root / "files"
        folder.mkdir()
        (folder / "a").write_bytes(b"abc")
        item = package.inventory(folder)["a"]
        self.assertEqual(item["bytes"], 3)
        self.assertEqual(item["sha256"], "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

    def test_output_cannot_pollute_snapshot_sources(self):
        with patch.object(package, "ROOT", self.root):
            with self.assertRaises(ValueError):
                package.build(self.root / "rtos/out", self.sdk, 1)
        self.assertFalse((self.root / "rtos").exists())


if __name__ == "__main__":
    unittest.main()
