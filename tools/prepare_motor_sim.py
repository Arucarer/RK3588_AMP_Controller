#!/usr/bin/env python3
"""Build a new, unsigned offline MotorSIM handoff directory; never deploy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def inventory(directory):
    return {p.relative_to(directory).as_posix():
            {"bytes": p.stat().st_size, "sha256": sha256(p)}
            for p in sorted(directory.rglob("*")) if p.is_file()}


def require_arm64_elf(path, static=False):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
            raise ValueError("Not a little-endian ELF64 AArch64 artifact: " + str(path))
        if static:
            offset = struct.unpack_from("<Q", header, 32)[0]
            entry_size, count = struct.unpack_from("<HH", header, 54)
            if entry_size < 56 or offset + count * entry_size > path.stat().st_size:
                raise ValueError("Invalid ELF program header table")
            for index in range(count):
                stream.seek(offset + index * entry_size)
                if struct.unpack("<I", stream.read(4))[0] == 3:
                    raise ValueError("Client unexpectedly needs a dynamic interpreter")


def snapshot_sources(destination):
    ignore = shutil.ignore_patterns(".git", "__pycache__", "*.pyc", "build")
    for name in ("rtos", "board", "common", "third_party/FreeRTOS-Kernel"):
        shutil.copytree(ROOT / name, destination / name, ignore=ignore)
    for name in ("Makefile", "linux/amp_motorctl.c", "linux/amp_ipc.c",
                 "linux/amp_ipc.h", "tools/build_motorctl.sh", "tools/prepare_motor_sim.py"):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)


def build(output, sdk, jobs):
    destination = output.expanduser().resolve()
    for source in (ROOT, ROOT / "rtos", ROOT / "board", ROOT / "common",
                   ROOT / "third_party", ROOT / "linux", ROOT / "tools"):
        if destination == source or (source != ROOT and source in destination.parents):
            raise ValueError("Output must not be inside a source directory")
    sdk = sdk.expanduser().resolve()
    compiler = sdk / "prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc"
    dumpimage = sdk / "u-boot/tools/dumpimage"
    for tool in (compiler, dumpimage, sdk / "u-boot/tools/mkimage"):
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise ValueError("Missing executable: " + str(tool))
    if not shutil.which("make"):
        raise ValueError("Missing make")
    output = output.expanduser().absolute()
    # Refuse to replace existing directories, files or symlinks, including empty dirs.
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()
    write_json(output / "build-state.json", {"status": "IN_PROGRESS", "board_verified": False})
    try:
        with tempfile.TemporaryDirectory(prefix="amp-motor-offline-") as work:
            work = Path(work)
            snapshot_sources(work)
            sources = inventory(work)
            write_json(output / "sources.json", sources)
            head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
            dirty = subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)
            env = dict(os.environ, SDK=str(sdk), CROSS_COMPILE=str(compiler)[:-3])
            env.pop("MAKEFLAGS", None)
            env.pop("MFLAGS", None)
            (output / "firmware").mkdir()
            (output / "linux").mkdir()
            (output / "logs").mkdir()
            commands = [
                ("firmware.log", ["make", "-j" + str(jobs), "SDK=" + str(sdk), "amp"]),
                ("client.log", ["sh", "tools/build_motorctl.sh", str(output / "linux/amp_motorctl")]),
            ]
            for log_name, command in commands:
                with (output / "logs" / log_name).open("w") as log:
                    subprocess.run(command, cwd=work, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, check=True)
            for name in ("amp.img", "amp_bootstrap.elf", "amp_bootstrap.bin", "amp_bootstrap.map"):
                source = work / "build" / name
                if not source.is_file() or not source.stat().st_size:
                    raise ValueError("Missing build artifact: " + name)
                shutil.copy2(source, output / "firmware" / name)
            require_arm64_elf(output / "firmware/amp_bootstrap.elf")
            require_arm64_elf(output / "linux/amp_motorctl", static=True)
            with (output / "logs/fit-list.txt").open("w") as log:
                subprocess.run([str(dumpimage), "-l", str(output / "firmware/amp.img")],
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            compiler_version = subprocess.check_output([str(compiler), "--version"], text=True).splitlines()[0]
            manifest = {
                "format_version": 1, "kind": "MOTOR_SIM_OFFLINE_HANDOFF",
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "git_head": head, "worktree_status": dirty.splitlines(),
                "source_basis": "copied current worktree, not necessarily clean HEAD",
                "sdk": str(sdk), "compiler": compiler_version,
                "simulation_only": True, "default_motor_state": "DISABLED",
                "board_verified": False, "signed_ota_release": False,
                "requires": ["matching Linux boot/rootfs with existing AMP support",
                             "existing signed coordinated A/B release process before deployment"],
            }
            (output / "NEXT_STEPS.md").write_text(
                "# MotorSIM 离线交付\n\n"
                "这里只包含新RTOS和Linux工具，不是完整或已签名的OTA包。\n"
                "未连接开发板、未刷分区、未做板端验收。SIM不是FOC或真实电机。\n\n"
                "SHA256SUMS用于完整性核对，不提供身份认证。sources.json记录实际\n"
                "源码快照摘要；若工作区有未提交修改，git_head不代表全部输入。\n\n"
                "具备板端条件后，再沿用原有签名A/B流程选择兼容boot/rootfs，\n"
                "组成发布包并核对版本。不要直接dd这里的镜像。\n\n"
                "待验收：健康确认、MotorSIM默认禁用、status/run正反转、失联停机、\n"
                "故障锁存与复位、LED/总线回归、HMI/看门狗并发、任务周期及堆栈余量。\n"
                "命令说明见项目linux/scripts/motorctl-usage.md。\n"
            )
            write_json(output / "build-state.json", {"status": "OFFLINE_BUILT", "board_verified": False})
            manifest["files"] = inventory(output)
            write_json(output / "manifest.json", manifest)
            files = inventory(output)
            (output / "SHA256SUMS").write_text("".join(
                item["sha256"] + "  " + name + "\n" for name, item in files.items()))
    except Exception as error:
        write_json(output / "build-state.json", {
            "status": "FAILED", "board_verified": False, "error": str(error)})
        raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory; must not exist")
    parser.add_argument("--sdk", type=Path, default=Path.home() / "rk3588_linux_sdk")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be 1..32")
    try:
        output = build(args.output, args.sdk, args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, "FAIL: " + str(error) + "\n")
    print("Created offline handoff:", output)
    print("No deployment, no board acceptance; not a signed OTA release.")


if __name__ == "__main__":
    main()
