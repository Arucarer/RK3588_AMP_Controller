#!/usr/bin/env python3
"""
AMP 发布包签名与离线验收。

依赖：openssl、fdtget、debugfs、e2fsck。
不挂载镜像，不修复文件系统，不写开发板。

签名：OpenSSL RSA/SHA256，私钥只用于发布端。
信任：verify 必须显式指定包外的可信公钥。
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

BOARD = "ATK-DLRK3588B"
LIMITS = {
    "boot": 64 * 1024 * 1024,
    "amp": 32 * 1024 * 1024,
    "rootfs": 14 * 1024 * 1024 * 1024,
}
APPS = (
    "/opt/amp/bin/ampctl",
    "/opt/amp/amp_hmi.py",
    "/opt/amp/run-hmi.sh",
)
EXPECTED_CPUS = {0x000, 0x100, 0x200, 0x400, 0x500, 0x600, 0x700}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(argv):
    result = subprocess.run(
        [str(x) for x in argv],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise ValueError(
            "命令失败：{}\n{}\n{}".format(
                " ".join(str(x) for x in argv),
                result.stdout.decode(errors="replace"),
                result.stderr.decode(errors="replace"),
            )
        )
    return result.stdout


def dependencies():
    for name in ("openssl", "fdtget", "debugfs", "e2fsck"):
        require(shutil.which(name), "缺少工具：" + name)


def regular(path):
    require(not path.is_symlink() and path.is_file(),
            "必须是普通文件且不能是符号链接：" + str(path))


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def unique_object(items):
    result = {}
    for key, value in items:
        require(key not in result, "JSON 存在重复字段：" + key)
        result[key] = value
    return result


def read_json(path):
    regular(path)
    require(path.stat().st_size <= 65536, "清单过大")
    return json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
    )


def props(path, node):
    return run(["fdtget", "-p", path, node]).decode().splitlines()


def children(path, node):
    return run(["fdtget", "-l", path, node]).decode().splitlines()


def string(path, node, prop):
    return run(["fdtget", "-t", "s", path, node, prop]).decode().strip()


def cells(path, node, prop):
    text = run(["fdtget", "-t", "x", path, node, prop]).decode()
    return [int(x, 16) for x in text.split()]


def one(path, node, prop):
    values = cells(path, node, prop)
    require(len(values) == 1, "属性不是单个 cell：" + node + "/" + prop)
    return values[0]


def blob(path, node, prop):
    text = run(["fdtget", "-t", "bx", path, node, prop]).decode()
    return bytes(int(x, 16) for x in text.split())


def fit_images(path):
    """
    当前发布配置固定使用外置数据、512 字节对齐。
    同时校验每个 image 的 SHA256。
    """
    data = path.read_bytes()
    require(len(data) >= 40, "FIT 过小")
    magic, total = struct.unpack_from(">II", data)
    require(magic == 0xD00DFEED and 40 <= total <= len(data),
            "FIT 头部无效")
    require(total % 512 == 0, "当前发布规则要求 FIT 元数据按 512 对齐")

    require(string(path, "/configurations", "default") == "conf",
            "FIT 默认配置必须为 conf")

    result = {}
    ranges = []
    for name in children(path, "/images"):
        node = "/images/" + name
        p = set(props(path, node))
        require("data" not in p, "当前发布规则不接受内嵌 FIT 数据")
        require(("data-offset" in p) != ("data-position" in p),
                "FIT 外置定位字段缺失或冲突")

        size = one(path, node, "data-size")
        if "data-offset" in p:
            start = total + one(path, node, "data-offset")
        else:
            start = one(path, node, "data-position")

        require(size > 0 and start >= total and start % 512 == 0,
                "FIT 数据大小或对齐无效：" + name)
        require(start <= len(data) and size <= len(data) - start,
                "FIT 数据越界：" + name)

        for old_start, old_end in ranges:
            require(start + size <= old_start or start >= old_end,
                    "FIT 子镜像范围重叠")
        ranges.append((start, start + size))

        payload = data[start:start + size]
        found = False
        for child in children(path, node):
            if not child.startswith("hash"):
                continue
            hn = node + "/" + child
            require("ignore" not in props(path, hn), "不允许忽略哈希")
            require(string(path, hn, "algo") == "sha256",
                    "当前发布规则只接受 SHA256")
            require(blob(path, hn, "value") ==
                    hashlib.sha256(payload).digest(),
                    "FIT SHA256 不匹配：" + name)
            found = True
        require(found, "缺少 SHA256：" + name)
        result[name] = payload

    require(result, "FIT 不包含镜像")
    return result


def check_profiles(boot, amp, boot_images, amp_images):
    conf = "/configurations/conf"
    require(set(boot_images) == {"kernel", "fdt", "resource"},
            "boot 镜像组成不匹配")
    require(set(amp_images) == {"amp3"}, "AMP 镜像组成不匹配")

    for prop, name in (("kernel", "kernel"), ("fdt", "fdt"),
                       ("multi", "resource")):
        require(string(boot, conf, prop) == name,
                "boot 配置引用不匹配：" + prop)

    require(string(amp, conf, "loadables") == "amp3",
            "AMP loadables 不匹配")

    for path, types in (
        (boot, {"kernel": "kernel", "fdt": "flat_dt", "resource": "multi"}),
        (amp, {"amp3": "firmware"}),
    ):
        for name, expected in types.items():
            node = "/images/" + name
            require(string(path, node, "type") == expected,
                    "镜像 type 不匹配")
            require(string(path, node, "arch") == "arm64",
                    "镜像架构不匹配")
            require(string(path, node, "compression") == "none",
                    "当前规则不接受压缩子镜像")

    require(string(boot, "/images/kernel", "os") == "linux",
            "Kernel OS 不匹配")
    for prop, value in (("cpu", 0x300), ("load", 0x20000000),
                        ("hyp", 0), ("thumb", 0), ("boot-on", 1)):
        require(one(amp, "/images/amp3", prop) == value,
                "AMP 属性不匹配：" + prop)
    require(len(amp_images["amp3"]) <= 0xFD0000,
            "AMP 数据进入共享 IPC 区")


def check_dtb(path):
    require("rockchip,rk3588" in
            string(path, "/", "compatible").split(),
            "DTB 不属于 RK3588")

    ac = one(path, "/cpus", "#address-cells")
    require(ac in (1, 2), "不支持的 CPU reg 编码")
    actual = []
    cpu_phandles = set()

    for name in children(path, "/cpus"):
        node = "/cpus/" + name
        p = set(props(path, node))
        if "device_type" not in p:
            continue
        if string(path, node, "device_type") != "cpu":
            continue

        values = cells(path, node, "reg")
        require(len(values) == ac, "CPU reg 长度错误")
        mpidr = 0
        for value in values:
            mpidr = (mpidr << 32) | value
        actual.append(mpidr)

        if "status" in p:
            require(string(path, node, "status") in ("okay", "ok"),
                    "预期 Linux CPU 被禁用")
        if "phandle" in p:
            cpu_phandles.add(one(path, node, "phandle"))

    require(len(actual) == 7 and set(actual) == EXPECTED_CPUS,
            "Linux CPU 集合不匹配，必须排除 MPIDR 0x300")

    # CPU 拓扑引用必须指向仍存在的 Linux CPU。
    def check_map(node):
        if "cpu" in props(path, node):
            require(one(path, node, "cpu") in cpu_phandles,
                    "cpu-map 存在悬空 CPU 引用")
        for child in children(path, node):
            check_map(node + "/" + child)

    if "cpu-map" in children(path, "/cpus"):
        check_map("/cpus/cpu-map")

    reserved = "/reserved-memory"
    require(one(path, reserved, "#address-cells") == 2 and
            one(path, reserved, "#size-cells") == 2,
            "reserved-memory cell 配置不匹配")
    require(blob(path, reserved, "ranges") == b"",
            "reserved-memory ranges 必须为空")

    target = reserved + "/rtos@20000000"
    require(cells(path, target, "reg") ==
            [0, 0x20000000, 0, 0x01000000],
            "AMP 预留区必须为 0x20000000 起的 16MiB")
    p = set(props(path, target))
    require("no-map" in p and blob(path, target, "no-map") == b"",
            "AMP 预留区缺少 no-map")
    require("reusable" not in p, "AMP 内存不能设置 reusable")
    if "status" in p:
        require(string(path, target, "status") in ("okay", "ok"),
                "AMP 内存预留被禁用")

    # 检查其他启用的静态 reserved-memory 区间不能与 AMP 重叠。
    for name in children(path, reserved):
        node = reserved + "/" + name
        if node == target:
            continue
        p = set(props(path, node))
        if "status" in p and string(path, node, "status") == "disabled":
            continue
        if "reg" not in p:
            continue
        values = cells(path, node, "reg")
        require(len(values) % 4 == 0, "reserved-memory reg 长度错误")
        for i in range(0, len(values), 4):
            start = (values[i] << 32) | values[i + 1]
            size = (values[i + 2] << 32) | values[i + 3]
            if size:
                require(start + size <= 0x20000000 or start >= 0x21000000,
                        "其他 reserved-memory 与 AMP 重叠：" + name)


def resource_dtb(data):
    require(len(data) >= 512 and data[:4] == b"RSCE",
            "resource 头部错误")
    require(struct.unpack_from("<HH", data, 4) == (0, 0),
            "不支持的 resource 版本")
    require(data[8:11] == bytes((1, 1, 1)),
            "不支持的 resource 表布局")

    count = struct.unpack_from("<I", data, 12)[0]
    require(0 < count <= 1024 and (count + 1) * 512 <= len(data),
            "resource 条目数量无效")

    names = set()
    dtbs = {}
    ranges = []
    for i in range(count):
        entry = data[(i + 1) * 512:(i + 2) * 512]
        require(entry[:4] == b"ENTR", "resource 条目标记错误")
        raw_name = entry[4:224]
        require(b"\0" in raw_name, "resource 文件名未终止")
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
        require(name not in names, "resource 文件名重复")
        names.add(name)

        hash_size, offset, size = struct.unpack_from("<III", entry, 256)
        require(hash_size <= 32, "resource hash_size 无效")
        start = offset * 512
        require(size > 0 and start >= (count + 1) * 512,
                "resource 数据位置无效")
        require(start <= len(data) and size <= len(data) - start,
                "resource 文件越界")

        for old_start, old_end in ranges:
            require(start + size <= old_start or start >= old_end,
                    "resource 文件范围重叠")
        ranges.append((start, start + size))

        if name.lower().endswith(".dtb"):
            dtbs[name] = data[start:start + size]

    # 拒绝多个硬件变体 DTB，避免运行时选到另一份。
    require(set(dtbs) == {"rk-kernel.dtb"},
            "resource 必须只包含一个 rk-kernel.dtb")
    return dtbs["rk-kernel.dtb"]


def rootfs_inventory(image, directory):
    # -n：只读；不执行修复。任何非零退出码均拒绝。
    run(["e2fsck", "-f", "-n", image])

    result = {}
    for index, name in enumerate(APPS):
        destination = directory / ("app-" + str(index))
        info = run(["debugfs", "-R", "stat " + name, image]).decode(
            errors="replace"
        )
        require(re.search(r"Type:\s+regular", info),
                "rootfs 文件缺失或不是普通文件：" + name)
        match = re.search(r"Mode:\s+([0-7]+)", info)
        require(match is not None, "无法读取权限：" + name)
        mode = int(match.group(1), 8)
        if name.endswith("ampctl") or name.endswith("run-hmi.sh"):
            require(mode & 0o111, "缺少执行权限：" + name)

        run(["debugfs", "-R",
             "dump {} {}".format(name, destination), image])
        regular(destination)
        result[name] = {
            "size": destination.stat().st_size,
            "sha256": sha256(destination),
            "mode": mode,
        }
    return result


def audit(directory):
    boot = directory / "boot.img"
    amp = directory / "amp.img"
    bi = fit_images(boot)
    ai = fit_images(amp)
    check_profiles(boot, amp, bi, ai)

    require(resource_dtb(bi["resource"]) == bi["fdt"],
            "resource DTB 与 FIT DTB 不一致")

    with tempfile.TemporaryDirectory(prefix="amp-audit-") as temp:
        temp = Path(temp)
        dtb = temp / "kernel.dtb"
        dtb.write_bytes(bi["fdt"])
        check_dtb(dtb)
        inventory = rootfs_inventory(directory / "rootfs.img", temp)

    print("PASS: DTB CPU/内存、两处DTB一致性、rootfs结构及应用文件")
    return inventory


def verify_images(directory, manifest):
    require(manifest.get("board") == BOARD, "开发板型号不匹配")
    release = manifest.get("release")
    require(isinstance(release, str) and
            re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", release),
            "release 格式无效")
    images = manifest.get("images")
    require(isinstance(images, dict) and set(images) == set(LIMITS),
            "镜像清单错误")

    for name, limit in LIMITS.items():
        item = images[name]
        require(isinstance(item, dict), "镜像元数据错误")
        require(item.get("file") == name + ".img", "镜像文件名错误")
        path = directory / (name + ".img")
        regular(path)
        size = path.stat().st_size
        require(type(item.get("size")) is int and
                0 < size <= limit and item["size"] == size,
                "镜像长度不匹配：" + name)
        require(sha256(path) == item.get("sha256"),
                "镜像哈希不匹配：" + name)


def snapshot(source, destination):
    for name in LIMITS:
        path = source / (name + ".img")
        regular(path)
        require(0 < path.stat().st_size <= LIMITS[name],
                "镜像大小不合法")
        shutil.copyfile(path, destination / path.name)


def verify(directory, public_key):
    """
    复制到私有临时目录再验证，避免验签与内容检查使用不同文件。
    发布目录本身仍应禁止其他进程修改。
    """
    with tempfile.TemporaryDirectory(prefix="amp-verify-") as temp:
        work = Path(temp)
        for name, limit in (("release.json", 65536),
                            ("release.sig", 16384)):
            src = directory / name
            regular(src)
            require(0 < src.stat().st_size <= limit, name + " 大小错误")
            shutil.copyfile(src, work / name)

        # 先验签，再信任并解析清单。
        run(["openssl", "dgst", "-sha256", "-verify", public_key,
             "-signature", work / "release.sig", work / "release.json"])

        manifest = read_json(work / "release.json")
        require(manifest.get("format_version") == 2,
                "签名发布格式错误")

        snapshot(directory, work)
        verify_images(work, manifest)
        actual = audit(work)
        require(actual == manifest.get("rootfs_files"),
                "rootfs 应用清单与签名内容不一致")

        print("PASS: 包外可信公钥验签及全部发布验收")
        print("Release:", manifest["release"])


def sign(args):
    source = Path(args.source).resolve(strict=True)
    output = Path(args.output).absolute()
    require(not output.exists(), "输出已存在，禁止覆盖")
    output.parent.mkdir(parents=True, exist_ok=True)

    # 保留老版本包不变。
    with tempfile.TemporaryDirectory(
        prefix=".amp-release-", dir=str(output.parent)
    ) as temp:
        work = Path(temp) / "bundle"
        work.mkdir()

        manifest = read_json(source / "manifest.json")
        require(manifest.get("format_version") == 1,
                "输入必须是 ota_bundle.py 创建的 V1 包")
        snapshot(source, work)
        verify_images(work, manifest)
        inventory = audit(work)

        # 与指定的发布应用源比较，不仅证明 rootfs 中“有文件”。
        app_root = Path(args.app_root).resolve(strict=True)
        for name, item in inventory.items():
            expected = app_root / name.lstrip("/")
            regular(expected)
            require(sha256(expected) == item["sha256"],
                    "rootfs 应用与发布源不一致：" + name)

        manifest["format_version"] = 2
        manifest["rootfs_files"] = inventory
        encoded = json.dumps(
            manifest, sort_keys=True, ensure_ascii=False,
            separators=(",", ":")
        ).encode("utf-8") + b"\n"

        (work / "release.json").write_bytes(encoded)
        run(["openssl", "dgst", "-sha256", "-sign", args.key,
             "-out", work / "release.sig", work / "release.json"])

        # 检查私钥对应的是用户指定的发布公钥。
        run(["openssl", "dgst", "-sha256", "-verify", args.pubkey,
             "-signature", work / "release.sig", work / "release.json"])

        require(not output.exists(), "输出目录被创建，停止发布")
        work.rename(output)

    print("Created signed release:", output)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    create = commands.add_parser("sign")
    create.add_argument("--source", required=True)
    create.add_argument("--output", required=True)
    create.add_argument("--app-root", required=True)
    create.add_argument("--key", required=True)
    create.add_argument("--pubkey", required=True)

    check = commands.add_parser("verify")
    check.add_argument("directory")
    check.add_argument("--pubkey", required=True)

    args = parser.parse_args()
    dependencies()
    if args.command == "sign":
        sign(args)
    else:
        verify(Path(args.directory).resolve(strict=True),
               Path(args.pubkey).resolve(strict=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise SystemExit("FAIL: " + str(exc))