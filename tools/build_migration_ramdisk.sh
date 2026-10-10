#!/bin/bash
set -euo pipefail

sdk="$HOME/rk3588_linux_sdk"
project="$HOME/rk3588_projects/RK3588_AMP_Controller"
source_root="$sdk/buildroot/output/rockchip_rk3588_recovery/target"
output="$project/build/migration"

for tool in python3 cpio gzip; do
    command -v "$tool" >/dev/null
done

test -x "$source_root/bin/busybox"
test -x "$source_root/usr/bin/python3"

modules=(
    ab_migrate_preflight.py
    ab_gpt_prepare.py
    ab_gpt_apply.py
    ab_userdata_shrink.py
    ab_migrate_device.py
)

# 打包前检查源码语法、写入开关和布局。
python3 - "$project" <<'PY'
import ast
import json
import sys
from pathlib import Path

project = Path(sys.argv[1])
names = (
    "ab_migrate_preflight.py",
    "ab_gpt_prepare.py",
    "ab_gpt_apply.py",
    "ab_userdata_shrink.py",
    "ab_migrate_device.py",
)

for name in names:
    path = project / "tools" / name
    source = path.read_text()
    tree = ast.parse(source, filename=str(path))
    compile(tree, str(path), "exec")

    if name in ("ab_userdata_shrink.py", "ab_migrate_device.py"):
        assignments = [
            node for node in tree.body
            if isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name) and target.id == "ACTIVATE"
                for target in node.targets
            )
        ]
        if len(assignments) != 1:
            raise SystemExit("ACTIVATE 定义异常：" + name)
        value = assignments[0].value
        if not isinstance(value, ast.Constant) or value.value is not False:
            raise SystemExit("拒绝打包：写入开关未关闭：" + name)

plan = json.loads(
    (project / "docs/ab-layout-draft-v1.json").read_text()
)
assert plan["sector_bytes"] == 512
assert plan["disk_sectors"] == 120832000
assert [p["name"] for p in plan["proposed_partitions"]] == [
    "uboot", "misc", "boot", "recovery", "amp", "rootfs",
    "oem", "userdata", "boot_b", "amp_b", "rootfs_b", "abmeta",
]
print("PASS: 模块语法、布局及两个 ACTIVATE=False")
PY

mkdir -p "$output"
stage=$(mktemp -d "$output/root.XXXXXX")
ramdisk="${stage}.cpio.gz"
fit="${stage}.fit.img"

cp -a "$source_root/." "$stage/"
install -d "$stage/opt/amp-migration"

for module in "${modules[@]}"; do
    install -m 644 "$project/tools/$module" \
        "$stage/opt/amp-migration/$module"
done

install -m 644 "$project/docs/ab-layout-draft-v1.json" \
    "$stage/opt/amp-migration/layout.json"

# 独立 init，不运行普通 recovery 的 rcS 和自动更新服务。
cat > "$stage/init" <<'EOF'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
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
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev

mkdir -p /dev/pts /run /tmp
/bin/busybox mount -t devpts devpts /dev/pts
/bin/busybox mount -t tmpfs tmpfs /run
/bin/busybox mount -t tmpfs tmpfs /tmp

# 不运行 mount -a、udev、recovery 或 A/B 服务。
echo
echo "=== AMP offline migration shell ==="
echo "No automatic disk mounts, updates or migration."
echo "Migration tools: /opt/amp-migration"
echo "Device write gates are disabled."
echo
EOF
chmod 755 "$stage/etc/migration-init.sh"

cat > "$stage/etc/fstab" <<'EOF'
# Offline migration: no automatic filesystem mounts.
EOF

for tool in e2fsck resize2fs parted; do
    if ! find "$stage/usr" "$stage/sbin" \
        -name "$tool" -print -quit | grep -q .
    then
        echo "FAIL: 缺少迁移工具 $tool" >&2
        exit 1
    fi
done

# 记录镜像中实际装入的文件摘要。
python3 - "$stage" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

directory = Path(sys.argv[1]) / "opt/amp-migration"
manifest = {
    "device_write_gates": "disabled",
    "files": {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(directory.iterdir())
        if path.is_file()
    },
}
(directory / "contents.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
print("PASS: 五个 Python 模块和布局 JSON 已装入")
PY

(
    cd "$stage"
    find . -print0 |
        cpio --null -o --format=newc --owner=0:0 |
        gzip -n -9 > "$ramdisk"
)

(
    cd "$sdk"

    # SDK 配置允许引用未定义的可选变量。
    set +u
    set -a
    source output/.config
    set +a
    set -u

    export CHIP_DIR="$sdk/device/rockchip/.chips/rk3588"
    export SCRIPTS_DIR="$sdk/device/rockchip/common/scripts"
    export RK_KERNEL_IMG="$sdk/kernel/arch/arm64/boot/Image"
    export RK_KERNEL_DTB="$sdk/kernel/arch/arm64/boot/dts/rockchip/rk3588-myboard.dtb"

    test -s "$CHIP_DIR/boot4recovery.its"
    test -s "$RK_KERNEL_IMG"
    test -s "$RK_KERNEL_DTB"
    test -s kernel/resource.img

    "$SCRIPTS_DIR/mk-ramdisk.sh" \
        "$ramdisk" "$fit" boot4recovery.its

    test -s "$fit"
    u-boot/tools/dumpimage -l "$fit"
)

# 保存对应内容清单，便于核对具体 FIT 的模块版本。
cp "$stage/opt/amp-migration/contents.json" "${fit}.contents.json"
sha256sum "$fit" > "${fit}.sha256"

cat "${fit}.sha256"
echo "迁移 FIT：$fit"
echo "内容清单：${fit}.contents.json"
echo "两个设备写入开关均关闭；未刷写、未启动。"