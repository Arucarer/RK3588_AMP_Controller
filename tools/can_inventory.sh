#!/bin/sh
# Read-only board inventory.
set -eu

echo "=== CAN interfaces and hardware paths ==="
for path in /sys/class/net/*; do
    [ -f "$path/type" ] || continue
    [ "$(cat "$path/type")" = 280 ] || continue
    name=${path##*/}
    echo "INTERFACE: $name"
    readlink -f "$path/device" || true
    readlink -f "$path/device/of_node" || true
    ip -details -statistics link show "$name"
done

echo "=== Device-tree CAN nodes ==="
find /sys/firmware/devicetree/base -type d -name 'can@*' |
while IFS= read -r node; do
    echo "$node"
    for prop in compatible status; do
        if [ -f "$node/$prop" ]; then
            printf '%s: ' "$prop"
            tr '\000' ' ' < "$node/$prop"
            echo
        fi
    done
done

echo "=== Drivers ==="
dmesg | grep -i -E 'rockchip.*can|can.*rockchip' || true
