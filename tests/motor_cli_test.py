"""Host CLI/real motor bridge/model integration; no board or /dev/mem."""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MotorCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="amp-motor-cli-test-")
        cls.binary = Path(cls.build.name) / "amp_motorctl_fake"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
            "-Dclock_gettime=motor_test_clock_gettime",
            "-Dnanosleep=motor_test_nanosleep",
            "linux/amp_motorctl.c", "tests/support/motor_cli_fake.c",
            "rtos/motor/motor_ipc.c", "rtos/motor/motor_control.c",
            "rtos/motor/motor_sim.c", "common/amp_message.c", "-o", str(cls.binary),
        ], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="amp-motor-cli-case-")
        self.addCleanup(self.directory.cleanup)
        self.token = Path(self.directory.name) / "token"
        self.trace = Path(self.directory.name) / "trace"

    def call(self, *args, **settings):
        env = dict(os.environ)
        for key in list(env):
            if key.startswith("AMP_MOTOR_TEST_"):
                del env[key]
        env["AMP_MOTOR_TEST_TRACE"] = str(self.trace)
        env.update(settings)
        return subprocess.run([str(self.binary), "--token-file", str(self.token), *args],
                              env=env, text=True, capture_output=True, timeout=10)

    def operations(self):
        lines = self.trace.read_text().splitlines()
        self.assertEqual(sum(x.startswith("open ") for x in lines),
                         sum(x.startswith("close ") for x in lines))
        return [int(x.split()[1]) for x in lines if x.startswith("operation ")]

    def test_status_is_read_only(self):
        result = self.call("status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MODE=SIM STATE=DISABLED", result.stdout)
        self.assertFalse(self.token.exists())
        self.assertEqual(self.operations(), [0])

    def test_run_ramps_stops_and_uses_same_token_with_new_sessions(self):
        result = self.call("run", "500", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DEMAND=200", result.stdout)
        self.assertIn("STATE=STOPPING", result.stdout)
        self.assertIn("STATE=DISABLED", result.stdout.splitlines()[-1])
        ops = self.operations()
        self.assertEqual(ops[:3], [1, 5, 3])
        self.assertEqual(ops.count(1), 1)
        self.assertEqual(ops.count(3), 1)
        self.assertEqual(ops[-1], 2)
        self.assertGreater(ops.count(6), 2)
        tokens = {x for x in self.trace.read_text().splitlines() if x.startswith("token ")}
        self.assertEqual(len(tokens), 1)
        content = self.token.read_bytes()
        self.assertEqual(len(content), 8)
        self.assertNotEqual(int.from_bytes(content, "little"), 0)
        self.assertEqual(self.token.stat().st_mode & 0o777, 0o600)
        result = self.call("enable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.token.read_bytes(), content)

    def test_negative_speed(self):
        result = self.call("run", "-500", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DEMAND=-200", result.stdout)

    def test_invalid_inputs_have_no_io(self):
        for args in [("run", "3001", "1"), ("run", "1", "0"),
                     ("run", "1", "3601"), ("target", " 1"),
                     ("inject", "1", "8"), ("start", "extra"), ("unknown",)]:
            with self.subTest(args=args):
                self.assertEqual(self.call(*args).returncode, 2)
                self.assertFalse(self.token.exists())
                self.assertFalse(self.trace.exists())

    def test_rejects_symlink_shared_or_invalid_token(self):
        real = self.token.with_name("real")
        real.write_bytes(b"untouched")
        self.token.symlink_to(real)
        self.assertNotEqual(self.call("enable").returncode, 0)
        self.assertEqual(real.read_bytes(), b"untouched")
        self.token.unlink()
        self.token.write_bytes(b"bad")
        self.token.chmod(0o600)
        self.assertNotEqual(self.call("enable").returncode, 0)
        self.token.write_bytes(b"\0" * 8)
        self.assertNotEqual(self.call("enable").returncode, 0)
        self.token.write_bytes((1).to_bytes(8, "little"))
        self.token.chmod(0o644)
        self.assertNotEqual(self.call("enable").returncode, 0)
        self.assertFalse(self.trace.exists())

    def test_other_mutator_is_locked_but_status_still_works(self):
        self.token.write_bytes((1).to_bytes(8, "little"))
        self.token.chmod(0o600)
        with self.token.open("rb") as file:
            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertNotEqual(self.call("enable").returncode, 0)
            self.assertEqual(self.call("status").returncode, 0)

    def test_wrong_mode_old_firmware_short_reply_and_transport_failure(self):
        for bad in ("mode", "old", "length", "transport"):
            with self.subTest(bad=bad):
                self.assertNotEqual(self.call("status", AMP_MOTOR_TEST_BAD=bad).returncode, 0)
        self.operations()  # Close on failures too.

    def test_overtemperature_aborts_run_without_auto_restart(self):
        result = self.call("run", "500", "2", AMP_MOTOR_TEST_FAULT="temperature")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("STATE=FAULT", result.stdout)
        self.assertIn("FAULTS=0x00000004", result.stdout)
        ops = self.operations()
        self.assertEqual(ops.count(3), 1)
        self.assertEqual(ops[-1], 2)

    def test_sigint_stops_and_disables(self):
        result = self.call("run", "500", "2", AMP_MOTOR_TEST_FAULT="signal")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("STATE=STOPPING", result.stdout)
        self.assertIn("STATE=DISABLED", result.stdout.splitlines()[-1])
        self.assertEqual(self.operations()[-1], 2)

    def test_lock_acquisition_can_retry_without_repeating_commands(self):
        result = self.call("run", "500", "1", AMP_MOTOR_TEST_BUSY="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.operations().count(3), 1)
        self.assertIn("busy 1", self.trace.read_text())

    def test_lost_target_response_is_not_replayed(self):
        result = self.call("run", "500", "1", AMP_MOTOR_TEST_FAIL_OP="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outcome may be unknown", result.stderr)
        self.assertEqual(self.operations(), [1, 5, 2])


if __name__ == "__main__":
    unittest.main()
