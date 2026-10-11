#!/bin/sh
# Read-only runtime resource inventory.
set -eu

echo "=== Boot context ==="
cat /proc/cmdline

echo "=== CAN1 / UART3 live device tree ==="
for node in \
    /sys/firmware/devicetree/base/can@fea60000 \
    /sys/firmware/devicetree/base/serial@feb60000
do
    echo "NODE: $node"

    if [ ! -d "$node" ]; then
        echo "MISSING"
        continue
    fi

    for prop in compatible status; do
        if [ -f "$node/$prop" ]; then
            printf '%s: ' "$prop"
            tr '\000' ' ' < "$node/$prop"
            echo
        else
            echo "$prop: absent"
        fi
    done
done

echo "=== Platform devices and bound drivers ==="
for name in fea60000.can feb60000.serial; do
    path="/sys/bus/platform/devices/$name"
    echo "DEVICE: $name"

    if [ ! -d "$path" ]; then
        echo "not present"
        continue
    fi

    if [ -L "$path/driver" ]; then
        printf 'driver: '
        readlink -f "$path/driver"
    else
        echo "driver: not bound"
    fi
done

echo "=== CAN interface mapping ==="
for path in /sys/class/net/*; do
    [ -f "$path/type" ] || continue
    [ "$(cat "$path/type")" = 280 ] || continue

    echo "INTERFACE: ${path##*/}"
    readlink -f "$path/device" || true
done

echo "=== UART interface mapping ==="
for path in /sys/class/tty/ttyS*; do
    [ -e "$path" ] || continue

    echo "TTY: ${path##*/}"
    readlink -f "$path/device" || true
done

echo "=== Relevant interrupts ==="
grep -i -E 'can|feb60000|uart|serial' /proc/interrupts || true

echo "=== UART3 open file descriptors ==="
python3 - <<'PY'
from pathlib import Path
import os

targets = set()

for tty in Path("/sys/class/tty").glob("ttyS*"):
    device = (tty / "device").resolve()
    if "feb60000" in str(device):
        targets.add("/dev/" + tty.name)

print("UART3 device nodes:", sorted(targets))

for proc in Path("/proc").iterdir():
    if not proc.name.isdigit():
        continue
    try:
        descriptors = list((proc / "fd").iterdir())
    except (FileNotFoundError, PermissionError):
        continue

    for descriptor in descriptors:
        try:
            target = os.readlink(descriptor)
        except (FileNotFoundError, PermissionError):
            continue
        if target in targets:
            print("OPEN:", "pid=" + proc.name,
                  "fd=" + descriptor.name, target)
PY

echo "=== End: no device registers written ==="