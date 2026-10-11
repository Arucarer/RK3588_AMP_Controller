#!/usr/bin/env python3
"""SocketCAN hardware tests. Linux owns the controller."""
import argparse
import json
import select
import socket
import struct
import subprocess
import time

SOL_CAN_RAW = 101
CAN_RAW_LOOPBACK = 3
CAN_RAW_FD_FRAMES = 5
CAN_RAW_ERR_FILTER = 2

EFF = 0x80000000
ERR = 0x20000000
ERROR_MASK = 0x1FFFFFFF
FD_LENGTHS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64}


def run(*args):
    subprocess.run(args, check=True)


def open_can(interface, fd=False, errors=False):
    s = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    try:
        if fd:
            s.setsockopt(SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 1)
        if errors:
            s.setsockopt(SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                         struct.pack("=I", ERROR_MASK))
        s.bind((interface,))
        return s
    except BaseException:
        s.close()
        raise


def frame(can_id, data, extended=False, fd=False, brs=False):
    if not 0 <= can_id <= (0x1FFFFFFF if extended else 0x7FF):
        raise ValueError("CAN ID out of range")
    if brs and not fd:
        raise ValueError("BRS requires CAN FD")
    if fd:
        if len(data) not in FD_LENGTHS:
            raise ValueError("FD length must be 0..8,12,16,20,24,32,48,64")
        return struct.pack("=IBBBB64s", can_id | (EFF if extended else 0),
                           len(data), 1 if brs else 0, 0, 0, data)
    if len(data) > 8:
        raise ValueError("classic CAN payload exceeds 8 bytes")
    return struct.pack("=IBBBB8s", can_id | (EFF if extended else 0),
                       len(data), 0, 0, 0, data)


def decode(raw):
    if len(raw) not in (16, 72):
        raise RuntimeError("unexpected CAN packet size")
    ident, length, flags, _, _, data = struct.unpack(
        "=IBBBB" + ("64s" if len(raw) == 72 else "8s"), raw)
    if length > (64 if len(raw) == 72 else 8):
        raise RuntimeError("invalid received length")
    return {
        "id": ident & 0x1FFFFFFF,
        "extended": bool(ident & EFF),
        "error": bool(ident & ERR),
        "fd": len(raw) == 72,
        "brs": bool(flags & 1) if len(raw) == 72 else False,
        "data": data[:length].hex(),
    }


def receive(s, timeout):
    if not select.select([s], [], [], timeout)[0]:
        raise TimeoutError("CAN receive timeout")
    return decode(s.recv(72))


def authorize(args, ident, data, extended, fd, brs):
    if not args.authorize:
        return
    if args.channel is None:
        raise ValueError("--authorize requires --channel 0 or 1")
    flags = int(extended) | (int(fd) << 1) | (int(brs) << 2)
    run(args.authorize, str(args.channel), hex(ident),
        str(flags), data.hex(), "v1")


def send_case(args, ident, data, extended=False, fd=False, brs=False):
    authorize(args, ident, data, extended, fd, brs)
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
                if (got["id"] == ident and got["extended"] == extended
                        and got["fd"] == fd and got["brs"] == brs
                        and got["data"] == data.hex()):
                    return


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("operation",
                   choices=["configure", "status", "test", "send",
                            "monitor", "restart"])
    p.add_argument("--interface", default="can0")
    p.add_argument("--channel", type=int, choices=[0, 1])
    p.add_argument("--authorize",
                   help="path to amp_can_authorize; enables FreeRTOS decision")
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
    if a.timeout <= 0 or a.restart_ms < 0:
        p.error("timeout must be positive; restart-ms must be nonnegative")

    if a.operation == "configure":
        run("ip", "link", "set", "dev", a.interface, "down")
        cmd = ["ip", "link", "set", "dev", a.interface, "type", "can",
               "bitrate", str(a.bitrate),
               "loopback", "on" if a.loopback else "off",
               "restart-ms", str(a.restart_ms)]
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
        run("ip", "link", "set", "dev", a.interface,
            "type", "can", "restart")
    elif a.operation == "monitor":
        with open_can(a.interface, fd=True, errors=True) as s:
            while True:
                try:
                    print(json.dumps(receive(s, a.timeout)), flush=True)
                except TimeoutError:
                    print("TIMEOUT: no CAN frame", flush=True)
    elif a.operation == "send":
        send_case(a, a.id, bytes.fromhex(a.data),
                  a.extended, a.fd, a.brs)
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
            print("PASS:", hex(ident), "FD" if fd else "CLASSIC",
                  "EXT" if extended else "STD", flush=True)
        print("PASS: selected CAN cases completed")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        raise SystemExit("FAIL: " + str(exc))
