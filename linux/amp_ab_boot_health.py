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

ACTIVATE = True  # ACTIVATE：允许执行实际操作的开关
CONFIRM = "/opt/amp/bin/amp_ab_confirm"  # CONFIRM：启动确认工具的路径
AMPCTL = "/opt/amp/bin/ampctl"  # AMPCTL：ampctl 可执行文件路径
BOOT_HEALTH_SECONDS = 45  # BOOT_HEALTH_SECONDS：试启动确认前的健康观察时长，单位 s
RUN_HEALTH_GRACE_SECONDS = 20  # RUN_HEALTH_GRACE_SECONDS：运行期健康检查的宽限时长，单位 s


def run(args, timeout=5):
    # 1. 返回 subprocess.check_output( args, timeout=timeout, stderr=subprocess.STDOUT, text=True ).strip()。
    return subprocess.check_output(
        args, timeout=timeout, stderr=subprocess.STDOUT, text=True
    ).strip()


def read64(address):
    # 1. 返回 int(run(["busybox", "devmem", hex(address), "64"]), 16)。
    return int(run(["busybox", "devmem", hex(address), "64"]), 16)


def wdt_inherited(cmdline):
    # 1. 调用 part.split，保存到 values。
    values = [part.split("=", 1)[1] for part in cmdline.split() if part.startswith("amp_ab.wdt=")]
    # 2. 检查条件；不满足要求时报告“missing or duplicate amp_ab.wdt boot argument”。
    if len(values) != 1 or values[0] not in ("0", "1"):
        raise RuntimeError("missing or duplicate amp_ab.wdt boot argument")
    # 3. 返回 values[0] == "1"。
    return values[0] == "1"


def health_sample(status):
    # 1. 调用 re.search，保存到 health。
    health = re.search(r"HEALTH ready=1 faults=0x0+ latched=0x0+ checks=(\d+)\b", status)
    tick = re.search(r"\btick=(\d+)\b", status)
    # 2. 按条件 not health or not tick 选择处理分支。
    if not health or not tick:
        raise RuntimeError("MonitorTask missing or unhealthy: " + status)
    # 3. 返回 int(tick.group(1)), int(health.group(1))。
    return int(tick.group(1)), int(health.group(1))


def hmi_responds():
    # 1. 调用 urllib.request.build_opener，保存到 client。
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    # 2. 在 文件访问 上下文中执行，退出时恢复或释放资源。
    with client.open("http://127.0.0.1:8088/api/history", timeout=5) as response:
        if not isinstance(json.load(response), list):
            raise RuntimeError("HMI history response invalid")


def main():
    # 1. 按条件 not ACTIVATE 选择处理分支。
    if not ACTIVATE:
        print("AB health: activation disabled; no watchdog open, writes or reset")
        return
    # 2. 在 文件访问 上下文中执行，退出时恢复或释放资源。
    with open("/run/amp-ab-health.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        identity = run([CONFIRM, "--check"])
        trial = identity.startswith("TRIAL ")
        if not trial and identity not in ("CONFIRMED_BOOT", "ALREADY_CONFIRMED"):
            raise RuntimeError("unexpected boot identity: " + identity)
        inherited = wdt_inherited(Path("/proc/cmdline").read_text())
        if trial and not inherited:
            raise RuntimeError("trial boot lacks inherited hardware watchdog")
        watchdog = BootWatchdog() if inherited else None
        hmi = None
        confirmed = not trial
        try:
            if (
                read64(0x20FE0000) != 0xA55A55A55AA55AA5
                or read64(0x20FE0010) != 0x81000300
                or read64(0x20FE0300) != 0x4652544F53303031
            ):
                raise RuntimeError("FreeRTOS startup identity mismatch")
            if read64(0x20FE0210) == 1:
                run(["busybox", "devmem", "0x20FE0208", "64", "1"])
            deadline = time.monotonic() + BOOT_HEALTH_SECONDS
            previous = None
            while True:
                try:
                    if read64(0x20FE0310) or read64(0x20FE0100):
                        raise RuntimeError("FreeRTOS error or exception")
                    sample = health_sample(run([AMPCTL, "status"]))
                    if previous is None or sample[0] == previous[0] or sample[1] == previous[1]:
                        previous = sample
                        raise RuntimeError("waiting for tick and monitor progress")
                    previous = sample
                    run([AMPCTL, "ping", "ab-health"])
                    if not os.path.ismount("/userdata"):
                        raise RuntimeError("userdata not mounted")
                    if hmi is not None and hmi.poll() is not None:
                        raise RuntimeError("HMI process exited")
                    try:
                        hmi_responds()
                    except Exception:
                        if hmi is not None:
                            raise
                        hmi = subprocess.Popen(["/opt/amp/run-hmi.sh"], start_new_session=True)
                        time.sleep(1)
                        hmi_responds()
                    if not confirmed:
                        print(run([CONFIRM, "--commit"], timeout=15), flush=True)
                        confirmed = True
                    if watchdog is None:
                        print("AB health: confirmed slot healthy; no inherited WDT", flush=True)
                        return
                    # Feed only after the complete health cycle succeeds. No
                    # independent timer thread may hide a stuck IPC/HMI check.
                    watchdog.ping()
                    deadline = time.monotonic() + RUN_HEALTH_GRACE_SECONDS
                    time.sleep(5)
                except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as exc:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("health deadline failed: " + str(exc)) from exc
                    time.sleep(1)
        except Exception as exc:
            print("AB health failed; watchdog will no longer be fed: " + str(exc), flush=True)
            if hmi is not None and hmi.poll() is None:
                hmi.terminate()
            if trial and not confirmed:
                # Do not decrement twice or reset on an unrelated generation.
                if run([CONFIRM, "--check"]) == identity:
                    os.sync()
                    subprocess.run(["/sbin/reboot", "-f"], check=True, timeout=10)
            raise


if __name__ == "__main__":
    main()
