#!/bin/sh
# 只读采集开发板 CAN 接口、设备树与驱动信息。
set -eu

# 1. 查找 CAN 网络接口并输出其硬件路径和统计。
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

# 2. 检查设备树中的 CAN 节点及其启用状态。
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

# 3. 输出内核中的 CAN 驱动日志。
echo "=== Drivers ==="
dmesg | grep -i -E 'rockchip.*can|can.*rockchip' || true
