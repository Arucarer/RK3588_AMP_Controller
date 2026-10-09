#!/usr/bin/env python3
"""Bounded boot confirmation followed by health-driven watchdog ownership."""
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.request
from amp_watchdog import BootWatchdog

ACTIVATE = False
CONFIRM = '/opt/amp/bin/amp_ab_confirm'
AMPCTL = '/opt/amp/bin/ampctl'
BOOT_HEALTH_SECONDS = 45
RUN_HEALTH_GRACE_SECONDS = 20

def run(args, timeout=5):
    return subprocess.check_output(args, timeout=timeout, stderr=subprocess.STDOUT,
                                   text=True).strip()

def read64(address):
    return int(run(['busybox', 'devmem', hex(address), '64']), 16)

def wdt_inherited(cmdline):
    values = [part.split('=', 1)[1] for part in cmdline.split()
              if part.startswith('amp_ab.wdt=')]
    if len(values) != 1 or values[0] not in ('0', '1'):
        raise RuntimeError('missing or duplicate amp_ab.wdt boot argument')
    return values[0] == '1'

def health_sample(status):
    health = re.search(r'HEALTH ready=1 faults=0x0+ latched=0x0+ checks=(\d+)\b', status)
    tick = re.search(r'\btick=(\d+)\b', status)
    if not health or not tick:
        raise RuntimeError('MonitorTask missing or unhealthy: ' + status)
    return int(tick.group(1)), int(health.group(1))

def hmi_responds():
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with client.open('http://127.0.0.1:8088/api/history', timeout=5) as response:
        if not isinstance(json.load(response), list):
            raise RuntimeError('HMI history response invalid')

def main():
    if not ACTIVATE:
        print('AB health: activation disabled; no watchdog open, writes or reset')
        return
    with open('/run/amp-ab-health.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        identity = run([CONFIRM, '--check'])
        trial = identity.startswith('TRIAL ')
        if not trial and identity not in ('CONFIRMED_BOOT', 'ALREADY_CONFIRMED'):
            raise RuntimeError('unexpected boot identity: ' + identity)
        inherited = wdt_inherited(Path('/proc/cmdline').read_text())
        if trial and not inherited:
            raise RuntimeError('trial boot lacks inherited hardware watchdog')
        watchdog = BootWatchdog() if inherited else None
        hmi = None
        confirmed = not trial
        try:
            if (read64(0x20FE0000) != 0xA55A55A55AA55AA5 or
                read64(0x20FE0010) != 0x81000300 or
                read64(0x20FE0300) != 0x4652544F53303031):
                raise RuntimeError('FreeRTOS startup identity mismatch')
            if read64(0x20FE0210) == 1:
                run(['busybox', 'devmem', '0x20FE0208', '64', '1'])
            deadline = time.monotonic() + BOOT_HEALTH_SECONDS
            previous = None
            while True:
                try:
                    if read64(0x20FE0310) or read64(0x20FE0100):
                        raise RuntimeError('FreeRTOS error or exception')
                    sample = health_sample(run([AMPCTL, 'status']))
                    if previous is None or sample[0] == previous[0] or sample[1] == previous[1]:
                        previous = sample
                        raise RuntimeError('waiting for tick and monitor progress')
                    previous = sample
                    run([AMPCTL, 'ping', 'ab-health'])
                    if not os.path.ismount('/userdata'):
                        raise RuntimeError('userdata not mounted')
                    if hmi is not None and hmi.poll() is not None:
                        raise RuntimeError('HMI process exited')
                    try:
                        hmi_responds()
                    except Exception:
                        if hmi is not None:
                            raise
                        hmi = subprocess.Popen(['/opt/amp/run-hmi.sh'], start_new_session=True)
                        time.sleep(1)
                        hmi_responds()
                    if not confirmed:
                        print(run([CONFIRM, '--commit'], timeout=15), flush=True)
                        confirmed = True
                    if watchdog is None:
                        print('AB health: confirmed slot healthy; no inherited WDT', flush=True)
                        return
                    # Feed only after the complete health cycle succeeds. No
                    # independent timer thread may hide a stuck IPC/HMI check.
                    watchdog.ping()
                    deadline = time.monotonic() + RUN_HEALTH_GRACE_SECONDS
                    time.sleep(5)
                except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as exc:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('health deadline failed: ' + str(exc)) from exc
                    time.sleep(1)
        except Exception as exc:
            print('AB health failed; watchdog will no longer be fed: ' + str(exc), flush=True)
            if hmi is not None and hmi.poll() is None:
                hmi.terminate()
            if trial and not confirmed:
                # Do not decrement twice or reset on an unrelated generation.
                if run([CONFIRM, '--check']) == identity:
                    os.sync()
                    subprocess.run(['/sbin/reboot', '-f'], check=True, timeout=10)
            raise

if __name__ == '__main__':
    main()
