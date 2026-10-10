#!/usr/bin/env python3
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))

import ab_migrate_device as migration


class DeviceEntryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.plan = json.loads(
            (PROJECT / "docs/ab-layout-draft-v1.json").read_text()
        )

    def make_sysfs(self):
        root = self.root / "disk"
        root.mkdir()
        for part in self.plan["proposed_partitions"]:
            child = root / ("mmcblk0p" + str(part["number"]))
            child.mkdir()
            (child / "partition").write_text(str(part["number"]))
            (child / "start").write_text(str(part["start_sector"]))
            (child / "size").write_text(str(part["sectors"]))
        return root

    def test_commit_gate_rejects_before_device_access(self):
        argv = [
            "ab_migrate_device.py",
            "--plan", "unused-plan",
            "--gpt-backup", "unused-backup",
            "--work-dir", "unused-work",
            "--commit", "--backup-saved",
        ]
        with patch.object(sys, "argv", argv), \
             patch.object(migration.os, "geteuid", return_value=0), \
             patch.object(migration, "ACTIVATE", False), \
             patch.object(migration.shrink, "inspect") as inspect:
            with self.assertRaisesRegex(RuntimeError, "开关关闭"):
                migration.main()
            inspect.assert_not_called()

    def test_missing_backup_ack_rejects_before_device_access(self):
        argv = [
            "ab_migrate_device.py",
            "--plan", "unused-plan",
            "--gpt-backup", "unused-backup",
            "--work-dir", "unused-work",
            "--commit",
        ]
        with patch.object(sys, "argv", argv), \
             patch.object(migration.os, "geteuid", return_value=0), \
             patch.object(migration, "ACTIVATE", True), \
             patch.object(migration.shrink, "inspect") as inspect:
            with self.assertRaisesRegex(RuntimeError, "备份"):
                migration.main()
            inspect.assert_not_called()

    def test_oversized_filesystem_rejected(self):
        partition_bytes = (
            self.plan["proposed_partitions"][7]["sectors"] * 512
        )
        with patch.object(
            migration.shrink, "filesystem_size",
            return_value=(partition_bytes, 4096)
        ):
            with self.assertRaisesRegex(RuntimeError, "安全边界"):
                migration.check_filesystem_fits(
                    Path("/unused"), self.plan
                )

    def test_shrunk_filesystem_accepted(self):
        partition_bytes = (
            self.plan["proposed_partitions"][7]["sectors"] * 512
        )
        size = (
            (partition_bytes - migration.shrink.MARGIN_BYTES) // 4096
        ) * 4096
        with patch.object(
            migration.shrink, "filesystem_size",
            return_value=(size, 4096)
        ):
            self.assertEqual(
                migration.check_filesystem_fits(
                    Path("/unused"), self.plan
                ),
                size,
            )

    def test_kernel_partition_ranges_verified(self):
        sysfs = self.make_sysfs()
        migration.check_kernel_partitions(
            sysfs, self.plan["proposed_partitions"]
        )

        (sysfs / "mmcblk0p9" / "start").write_text("123")
        with self.assertRaisesRegex(RuntimeError, "范围不匹配"):
            migration.check_kernel_partitions(
                sysfs, self.plan["proposed_partitions"]
            )

    def test_flush_order(self):
        calls = []
        with patch.object(
            migration.os, "fsync",
            side_effect=lambda fd: calls.append(("fsync", fd))
        ), patch.object(
            migration.fcntl, "ioctl",
            side_effect=lambda fd, cmd: calls.append(("ioctl", fd, cmd))
        ):
            migration.flush_disk(123)

        self.assertEqual(calls, [
            ("fsync", 123),
            ("ioctl", 123, migration.BLKFLSBUF),
        ])

    def test_reread_success_checks_kernel_table(self):
        sysfs = self.make_sysfs()
        with patch.object(migration.fcntl, "ioctl") as ioctl:
            migration.reread_and_check(
                123, sysfs, self.plan["proposed_partitions"]
            )
            ioctl.assert_called_once_with(123, migration.BLKRRPART)

    def test_reread_ioctl_failure_not_retried(self):
        with patch.object(
            migration.fcntl, "ioctl",
            side_effect=OSError("injected BLKRRPART failure")
        ) as ioctl, patch.object(
            migration, "check_kernel_partitions"
        ) as check:
            with self.assertRaises(OSError):
                migration.reread_and_check(
                    123, self.root, self.plan["proposed_partitions"]
                )
            ioctl.assert_called_once()
            check.assert_not_called()

    def test_stale_kernel_table_times_out(self):
        with patch.object(migration.fcntl, "ioctl"), \
             patch.object(
                 migration, "check_kernel_partitions",
                 side_effect=RuntimeError("stale kernel partition table")
             ), \
             patch.object(
                 migration.time, "monotonic", side_effect=[0, 6]
             ), \
             patch.object(migration.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "stale"):
                migration.reread_and_check(
                    123, self.root, self.plan["proposed_partitions"]
                )
            sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
    