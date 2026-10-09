#!/bin/bash
set -euo pipefail

sdk="$HOME/rk3588_linux_sdk"
source_root="$sdk/buildroot/output/rockchip_rk3588_recovery/target"
project="$HOME/rk3588_projects/RK3588_AMP_Controller"
output="$project/build/migration"

command -v cpio >/dev/null
command -v gzip >/dev/null
test -x "$source_root/bin/busybox"

# 每次使用新目录，避免覆盖旧产物。
mkdir -p "$output"
stage=$(mktemp -d "$output/root.XXXXXX")

cp -a "$source_root/." "$stage/"

# 独立启动入口，不执行原来的 rcS、recovery 或 A/B 服务。
cat > "$stage/init" <<'EOF'
#!/bin/sh
exec /bin/busybox init
EOF
chmod 755 "$stage/init"

cat > "$stage/etc/inittab" <<'EOF'
::sysinit:/bin/sh /etc/migration-init.sh
::respawn:-/bin/sh
::shutdown:/bin/sync
EOF

cat > "$stage/etc/migration-init.sh" <<'EOF'
#!/bin/sh
set -e

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev

mkdir -p /dev/pts /run /tmp
/bin/busybox mount -t devpts devpts /dev/pts
/bin/busybox mount -t tmpfs tmpfs /run
/bin/busybox mount -t tmpfs tmpfs /tmp

# 不启动 udev/mdev 热插拔脚本，不执行 mount -a。
echo
echo "=== AMP offline migration shell ==="
echo "No automatic recovery, A/B service or disk mounts."
echo "Do not resize or change GPT until preflight passes."
echo
EOF
chmod 755 "$stage/etc/migration-init.sh"

# 不依赖旧 fstab；后续迁移显式挂载所需设备。
cat > "$stage/etc/fstab" <<'EOF'
# Offline migration: no automatic filesystem mounts.
EOF

for tool in e2fsck resize2fs parted; do
    if ! find "$stage/usr" "$stage/sbin" \
        -name "$tool" -print -quit | grep -q .
    then
        echo "Missing migration tool: $tool" >&2
        exit 1
    fi
done

ramdisk="${stage}.cpio.gz"

(
    cd "$stage"
    find . -print0 |
        cpio --null -o --format=newc --owner=0:0 |
        gzip -n -9 > "$ramdisk"
)

echo "迁移 ramdisk 已生成：$ramdisk"
echo "暂未打包 FIT、刷写或启动。"