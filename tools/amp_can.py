#!/usr/bin/env python3
"""SocketCAN hardware tests. Linux owns the controller."""
import argparse
import json
import select
import socket
import struct
import subprocess
import time

SOL_CAN_RAW = 101  # SOL_CAN_RAW：原始 CAN 套接字选项层级
CAN_RAW_LOOPBACK = 3  # CAN_RAW_LOOPBACK：本机 CAN 回环选项
CAN_RAW_FD_FRAMES = 5  # CAN_RAW_FD_FRAMES：CAN FD 帧接收选项
CAN_RAW_ERR_FILTER = 2  # CAN_RAW_ERR_FILTER：CAN 错误帧过滤选项

EFF = 0x80000000  # EFF：CAN 扩展帧标志位
ERR = 0x20000000  # ERR：CAN 错误帧标志位
ERROR_MASK = 0x1FFFFFFF  # ERROR_MASK：CAN 错误类别掩码
FD_LENGTHS = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    12,
    16,
    20,
    24,
    32,
    48,
    64,
}  # FD_LENGTHS：CAN FD 支持的数据长度集合


def run(*args):
    # 1. 执行子命令或封装的操作。
    subprocess.run(args, check=True)


def open_can(interface, fd=False, errors=False):
    # 1. 调用 socket.socket，保存到 s。
    s = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    # 2. 执行本段操作，捕获 BaseException。
    try:
        if fd:
            s.setsockopt(SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 1)
        if errors:
            s.setsockopt(SOL_CAN_RAW, CAN_RAW_ERR_FILTER, struct.pack("=I", ERROR_MASK))
        s.bind((interface,))
        return s
    except BaseException:
        s.close()
        raise


def frame(can_id, data, extended=False, fd=False, brs=False):
    # 1. 检查条件；不满足要求时报告“CAN ID out of range”。
    if not 0 <= can_id <= (0x1FFFFFFF if extended else 0x7FF):
        raise ValueError("CAN ID out of range")
    # 2. 检查条件；不满足要求时报告“BRS requires CAN FD”。
    if brs and not fd:
        raise ValueError("BRS requires CAN FD")
    # 3. 检查条件；不满足要求时报告“FD length must be 0..8,12,16,20,24,32,48,64”。
    if fd:
        if len(data) not in FD_LENGTHS:
            raise ValueError("FD length must be 0..8,12,16,20,24,32,48,64")
        return struct.pack(
            "=IBBBB64s", can_id | (EFF if extended else 0), len(data), 1 if brs else 0, 0, 0, data
        )
    # 4. 检查条件；不满足要求时报告“classic CAN payload exceeds 8 bytes”。
    if len(data) > 8:
        raise ValueError("classic CAN payload exceeds 8 bytes")
    # 5. 返回 struct.pack("=IBBBB8s", can_id | (EFF if extended else 0), len(data), 0, 0, 0, data)。
    return struct.pack("=IBBBB8s", can_id | (EFF if extended else 0), len(data), 0, 0, 0, data)


def decode(raw):
    # 1. 检查条件；不满足要求时报告“unexpected CAN packet size”。
    if len(raw) not in (16, 72):
        raise RuntimeError("unexpected CAN packet size")
    ident, length, flags, _, _, data = struct.unpack(
        "=IBBBB" + ("64s" if len(raw) == 72 else "8s"), raw
    )
    # 2. 检查条件；不满足要求时报告“invalid received length”。
    if length > (64 if len(raw) == 72 else 8):
        raise RuntimeError("invalid received length")
    # 3. 组装并返回包含 "id", "extended", "error", "fd", "brs", "data" 的记录。
    return {
        "id": ident & 0x1FFFFFFF,
        "extended": bool(ident & EFF),
        "error": bool(ident & ERR),
        "fd": len(raw) == 72,
        "brs": bool(flags & 1) if len(raw) == 72 else False,
        "data": data[:length].hex(),
    }


def receive(s, timeout):
    # 1. 检查条件；不满足要求时报告“CAN receive timeout”。
    if not select.select([s], [], [], timeout)[0]:
        raise TimeoutError("CAN receive timeout")
    # 2. 返回 decode(s.recv(72))。
    return decode(s.recv(72))


def authorize(args, ident, data, extended, fd, brs):
    # 1. 按条件 not args.authorize 选择处理分支。
    if not args.authorize:
        return
    # 2. 检查条件；不满足要求时报告“--authorize requires --channel 0 or 1”。
    if args.channel is None:
        raise ValueError("--authorize requires --channel 0 or 1")
    flags = int(extended) | (int(fd) << 1) | (int(brs) << 2)
    run(args.authorize, str(args.channel), hex(ident), str(flags), data.hex(), "v1")


def send_case(args, ident, data, extended=False, fd=False, brs=False):
    # 1. 调用 authorize。
    authorize(args, ident, data, extended, fd, brs)
    # 2. 在 文件访问 上下文中执行，退出时恢复或释放资源。
    with open_can(args.interface, fd=True, errors=True) as rx:
        with open_can(args.interface, fd=True) as tx:
            # Disable SocketCAN software echo on the sending socket.
            # A matching RX must come back through the hardware driver.
            tx.setsockopt(SOL_CAN_RAW, CAN_RAW_LOOPBACK, 0)
            raw = frame(ident, data, extended, fd, brs)
            if tx.send(raw) != len(raw):
                raise RuntimeError("short CAN send")
            deadline = time.monotonic() + args.timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("matching CAN frame timeout")
                got = receive(rx, remaining)
                print(json.dumps(got), flush=True)
                if got["error"]:
                    raise RuntimeError("CAN error frame received")
                if (
                    got["id"] == ident
                    and got["extended"] == extended
                    and got["fd"] == fd
                    and got["brs"] == brs
                    and got["data"] == data.hex()
                ):
                    return


def main():
    # 1. 建立命令行参数解析器，保存到 p。
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "operation", choices=["configure", "status", "test", "send", "monitor", "restart"]
    )
    p.add_argument("--interface", default="can0")
    p.add_argument("--channel", type=int, choices=[0, 1])
    p.add_argument("--authorize", help="path to amp_can_authorize; enables FreeRTOS decision")
    p.add_argument("--bitrate", type=int, default=500000)
    p.add_argument("--dbitrate", type=int, default=2000000)
    p.add_argument("--fd", action="store_true")
    p.add_argument("--loopback", action="store_true")
    p.add_argument("--restart-ms", type=int, default=0)
    p.add_argument("--timeout", type=float, default=3)
    p.add_argument("--id", type=lambda x: int(x, 0), default=0x123)
    p.add_argument("--data", default="0102030405060708")
    p.add_argument("--extended", action="store_true")
    p.add_argument("--brs", action="store_true")
    a = p.parse_args()
    # 2. 按条件 a.timeout <= 0 or a.restart_ms < 0 选择处理分支。
    if a.timeout <= 0 or a.restart_ms < 0:
        p.error("timeout must be positive; restart-ms must be nonnegative")

    # 3. 按条件 a.operation == "configure" 选择处理分支。
    if a.operation == "configure":
        run("ip", "link", "set", "dev", a.interface, "down")
        cmd = [
            "ip",
            "link",
            "set",
            "dev",
            a.interface,
            "type",
            "can",
            "bitrate",
            str(a.bitrate),
            "loopback",
            "on" if a.loopback else "off",
            "restart-ms",
            str(a.restart_ms),
        ]
        if a.fd:
            cmd += ["dbitrate", str(a.dbitrate), "fd", "on"]
        else:
            cmd += ["fd", "off"]
        run(*cmd)
        run("ip", "link", "set", "dev", a.interface, "up")
        run("ip", "-details", "-statistics", "link", "show", a.interface)
    elif a.operation == "status":
        run("ip", "-details", "-statistics", "link", "show", a.interface)
    elif a.operation == "restart":
        run("ip", "link", "set", "dev", a.interface, "type", "can", "restart")
    elif a.operation == "monitor":
        with open_can(a.interface, fd=True, errors=True) as s:
            while True:
                try:
                    print(json.dumps(receive(s, a.timeout)), flush=True)
                except TimeoutError:
                    print("TIMEOUT: no CAN frame", flush=True)
    elif a.operation == "send":
        send_case(a, a.id, bytes.fromhex(a.data), a.extended, a.fd, a.brs)
        print("PASS: matching frame received")
    else:
        cases = [
            (0x123, bytes(range(8)), False, False, False),
            (0x18FF0123, bytes(range(8)), True, False, False),
        ]
        if a.fd:
            cases += [
                (0x321, bytes(range(64)), False, True, True),
                (0x18FF0321, bytes(range(64)), True, True, True),
            ]
        for ident, data, extended, fd, brs in cases:
            send_case(a, ident, data, extended, fd, brs)
            print(
                "PASS:",
                hex(ident),
                "FD" if fd else "CLASSIC",
                "EXT" if extended else "STD",
                flush=True,
            )
        print("PASS: selected CAN cases completed")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        raise SystemExit("FAIL: " + str(exc))
