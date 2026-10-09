import importlib.util
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'linux'))
import amp_watchdog as wd
import amp_ab_boot_health as h

class EndSimulation(BaseException): pass

class BootHealth(unittest.TestCase):
    def scenario(self, bad=False, stale=False):
        calls = []
        checks = 0
        ticks = 0
        clock = iter(range(0, 1000, 10))
        def run(args, timeout=5):
            nonlocal checks, ticks
            calls.append(args)
            if args == [h.CONFIRM, '--check']:
                checks += 1
                return 'TRIAL slot=B generation=' + ('9' if stale and checks > 1 else '8')
            if args == [h.CONFIRM, '--commit']:
                return 'CONFIRMED slot=B generation=9'
            if args == [h.AMPCTL, 'status']:
                if bad: return 'HEALTH unavailable: old status format'
                ticks += 1
                return f'tick={ticks} HEALTH ready=1 faults=0x00000000 latched=0x00000000 checks={ticks}'
            return ''
        def read(addr):
            return {0x20FE0000: 0xA55A55A55AA55AA5, 0x20FE0010: 0x81000300,
                    0x20FE0300: 0x4652544F53303031, 0x20FE0210: 1}.get(addr, 0)
        watchdog = Mock()
        watchdog.ping.side_effect = [None, EndSimulation()]
        with patch.object(h, 'ACTIVATE', True), patch('builtins.open', return_value=io.StringIO()), \
             patch.object(h.fcntl, 'flock'), patch.object(h, 'run', side_effect=run), \
             patch.object(h, 'read64', side_effect=read), patch.object(h.time, 'sleep'), \
             patch.object(h.time, 'monotonic', side_effect=lambda: next(clock)), \
             patch.object(h.os.path, 'ismount', return_value=True), \
             patch.object(h.Path, 'read_text', return_value='amp_ab.wdt=1'), \
             patch.object(h, 'BootWatchdog', return_value=watchdog), \
             patch.object(h, 'hmi_responds'), patch.object(h.os, 'sync'), \
             patch.object(h.subprocess, 'run') as reboot:
            with self.assertRaises(RuntimeError if bad else EndSimulation): h.main()
            return calls, reboot.call_count, watchdog.ping.call_count
    def test_disabled_has_no_side_effects(self):
        with patch.object(h, 'ACTIVATE', False), patch.object(h, 'run') as run, patch.object(h, 'BootWatchdog') as w:
            h.main(); run.assert_not_called(); w.assert_not_called()
    def test_health_confirms_once_and_keeps_feeding(self):
        calls, reboot, feeds = self.scenario()
        self.assertEqual(calls.count([h.CONFIRM, '--commit']), 1)
        self.assertEqual((reboot, feeds), (0, 2))
    def test_missing_monitor_never_feeds_or_confirms_and_resets_trial(self):
        calls, reboot, feeds = self.scenario(bad=True)
        self.assertNotIn([h.CONFIRM, '--commit'], calls)
        self.assertEqual((reboot, feeds), (1, 0))
    def test_changed_generation_prevents_explicit_reset(self):
        calls, reboot, feeds = self.scenario(bad=True, stale=True)
        self.assertNotIn([h.CONFIRM, '--commit'], calls)
        self.assertEqual((reboot, feeds), (0, 0))
    def test_boot_wdt_identity_required(self):
        for line in ('', 'amp_ab.wdt=2', 'amp_ab.wdt=1 amp_ab.wdt=1'):
            with self.assertRaises(RuntimeError): h.wdt_inherited(line)
        self.assertTrue(h.wdt_inherited('amp_ab.wdt=1'))
    def test_latched_fault_is_not_healthy(self):
        with self.assertRaises(RuntimeError):
            h.health_sample('tick=3 HEALTH ready=1 faults=0x0 latched=0x1 checks=2')

class WatchdogOwnership(unittest.TestCase):
    def prepare(self, root, state='active', nowayout='1'):
        p = Path(root)
        for key, value in {'identity':'Synopsys DesignWare Watchdog', 'state':state, 'nowayout':nowayout}.items():
            (p / key).write_text(value)
        (p / 'device').mkdir()
        node = p / 'watchdog@feaf0000'; node.mkdir()
        (p / 'device/of_node').symlink_to(node)
    def test_inactive_hardware_is_not_silently_started(self):
        with tempfile.TemporaryDirectory() as root:
            self.prepare(root, state='inactive')
            with patch.object(wd.os, 'open') as op:
                with self.assertRaises(RuntimeError): wd.BootWatchdog(root)
                op.assert_not_called()
    def test_nowayout_is_required_before_open(self):
        with tempfile.TemporaryDirectory() as root:
            self.prepare(root, nowayout='0')
            with patch.object(wd.os, 'open') as op:
                with self.assertRaises(RuntimeError): wd.BootWatchdog(root)
                op.assert_not_called()
    def test_ioctl_timeout_and_keepalive(self):
        commands=[]
        def ioctl(fd, command, value, mutate):
            commands.append(command)
            if command == wd.WDIOC_GETSUPPORT:
                value[:] = struct.pack('=II32s', 0, 0, b'Synopsys DesignWare Watchdog')
            elif command in (wd.WDIOC_SETTIMEOUT, wd.WDIOC_GETTIMEOUT): value[0] = 89
        with tempfile.TemporaryDirectory() as root:
            self.prepare(root)
            with patch.object(wd.os, 'open', return_value=9), patch.object(wd.fcntl, 'ioctl', side_effect=ioctl):
                owner=wd.BootWatchdog(root); owner.ping()
                self.assertEqual(owner.timeout, 89)
                self.assertEqual(commands.count(wd.WDIOC_KEEPALIVE), 2)

if __name__ == '__main__': unittest.main()
