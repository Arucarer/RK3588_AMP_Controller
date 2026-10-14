# 新建：tools/ota_bundle.py
#
# 阶段十第一步：生成并校验升级包。
#
# 已核对当前 SDK：
# CONFIG_ANDROID_AB 未启用，CONFIG_BOOTCOUNT 未启用；
# 当前分区没有 boot_a/boot_b、rootfs_a/rootfs_b。
#
# 本模块只生成和检查升级包，不刷写、不修改分区、不切换启动槽。
# SHA256 用于完整性检查，不提供发布者身份认证。
# 当前交付不等于已经实现 A/B OTA 或自动回滚。
#
# 升级包同时记录 Linux boot、rootfs 和 AMP 镜像，
# 后续再接入签名验证、非活动槽写入及启动确认机制。

import argparse
import hashlib
import json
import re
import shutil
import tempfile
from pathlib import Path

BOARD = "ATK-DLRK3588B"  # BOARD：升级包适用的开发板型号
COMPONENTS = ("boot", "rootfs", "amp")  # COMPONENTS：升级包包含的镜像组件名称
# 当前分区容量，仅用于检查现有镜像大小，不作为未来 A/B 分区方案。
LIMITS = {
    "boot": 64 * 1024 * 1024,
    "rootfs": 14 * 1024 * 1024 * 1024,
    "amp": 32 * 1024 * 1024,
}  # LIMITS：各镜像组件的容量上限，单位字节


def sha256(path):
    # 1. 计算 SHA-256 摘要，保存到 digest。
    digest = hashlib.sha256()
    # 2. 在 文件访问 上下文中执行，退出时恢复或释放资源。
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    # 3. 返回 digest.hexdigest()。
    return digest.hexdigest()


def check(bundle):
    # 1. 调用 bundle.resolve，保存到 bundle。
    bundle = bundle.resolve()
    manifest_path = bundle / "manifest.json"

    # 2. 检查条件；不满足要求时报告“manifest.json 过大”。
    if manifest_path.stat().st_size > 65536:
        raise ValueError("manifest.json 过大")

    # 3. 解析 JSON 文本，保存到 manifest。
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    # 4. 检查条件；不满足要求时报告“不支持的升级包格式”。
    if manifest.get("format_version") != 1:
        raise ValueError("不支持的升级包格式")

    # 5. 检查条件；不满足要求时报告“开发板型号不匹配”。
    if manifest.get("board") != BOARD:
        raise ValueError("开发板型号不匹配")

    # 6. 调用 manifest.get，保存到 version。
    version = manifest.get("release")
    # 7. 检查条件；不满足要求时报告“release 格式无效”。
    if not isinstance(version, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", version
    ):
        raise ValueError("release 格式无效")

    # 8. 调用 manifest.get，保存到 images。
    images = manifest.get("images")
    # 9. 检查条件；不满足要求时报告“必须包含 boot、rootfs、amp 三个组件”。
    if not isinstance(images, dict) or set(images) != set(COMPONENTS):
        raise ValueError("必须包含 boot、rootfs、amp 三个组件")

    # 10. 遍历 COMPONENTS，逐项处理 name。
    for name in COMPONENTS:
        info = images[name]
        if not isinstance(info, dict):
            raise ValueError(f"{name}: 元数据无效")

        expected_name = name + ".img"
        if info.get("file") != expected_name:
            raise ValueError(f"{name}: 文件名无效")

        path = bundle / expected_name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"{name}: 镜像必须是普通文件")

        size = path.stat().st_size
        if not 0 < size <= LIMITS[name]:
            raise ValueError(f"{name}: 镜像为空或超过当前分区容量")

        if type(info.get("size")) is not int or info["size"] != size:
            raise ValueError(f"{name}: 文件大小不匹配")

        expected_hash = info.get("sha256")
        if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
            raise ValueError(f"{name}: SHA256 格式无效")

        if sha256(path) != expected_hash:
            raise ValueError(f"{name}: SHA256 不匹配")

        print(f"OK {name}: {size} bytes")

    # 11. 输出执行结果或诊断信息。
    print(f"Bundle integrity OK: {BOARD}, release={version}")
    print("未验证签名、启动兼容性或 A/B 回滚能力；未写入任何分区。")


def create(args):
    # 1. 检查条件；不满足要求时报告“release 只能包含字母、数字、点、下划线和连字符”。
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", args.release):
        raise ValueError("release 只能包含字母、数字、点、下划线和连字符")

    # 2. 调用 Path(args.output).absolute，保存到 output。
    output = Path(args.output).absolute()
    output.parent.mkdir(parents=True, exist_ok=True)

    # 3. 检查条件；不满足要求时报告“输出目录已存在，请使用新的版本目录”。
    if output.exists():
        raise ValueError("输出目录已存在，请使用新的版本目录")

    # 4. 设置 manifest，供后续逻辑使用。
    manifest = {
        "format_version": 1,
        "board": BOARD,
        "release": args.release,
        "images": {},
    }

    # 暂存目录与最终目录在同一文件系统；完成校验后再改名。
    with tempfile.TemporaryDirectory(prefix=".amp-ota-", dir=output.parent) as temporary:
        staging = Path(temporary) / "bundle"
        staging.mkdir()

        for name in COMPONENTS:
            source = Path(getattr(args, name)).resolve(strict=True)
            if not source.is_file():
                raise ValueError(f"{name}: 输入不是普通文件")

            size = source.stat().st_size
            if not 0 < size <= LIMITS[name]:
                raise ValueError(f"{name}: 输入大小无效")

            destination = staging / (name + ".img")
            shutil.copyfile(source, destination)

            manifest["images"][name] = {
                "file": destination.name,
                "size": destination.stat().st_size,
                "sha256": sha256(destination),
            }

        (staging / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

        check(staging)

        # 不允许覆盖已有发布目录；此工具应由单个打包进程运行。
        if output.exists():
            raise ValueError("输出目录已被创建，取消发布")

        staging.rename(output)

    # 5. 输出执行结果或诊断信息。
    print(f"Created: {output}")


def main():
    # 1. 建立命令行参数解析器，保存到 parser。
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    # 2. 调用 commands.add_parser，保存到 pack。
    pack = commands.add_parser("create")
    pack.add_argument("--release", required=True)
    pack.add_argument("--boot", required=True)
    pack.add_argument("--rootfs", required=True)
    pack.add_argument("--amp", required=True)
    pack.add_argument("--output", required=True)

    # 3. 调用 commands.add_parser，保存到 verify。
    verify = commands.add_parser("check")
    verify.add_argument("directory")

    # 4. 解析命令行参数，保存到 args。
    args = parser.parse_args()

    # 5. 执行本段操作，捕获 (OSError, ValueError, TypeError, KeyError)。
    try:
        if args.command == "create":
            create(args)
        else:
            check(Path(args.directory))
    except (OSError, ValueError, TypeError, KeyError) as exc:
        raise SystemExit(f"ERROR: {exc}")


if __name__ == "__main__":
    main()
