# 新建：tools/ab_layout_plan.py
#
# 阶段十：生成可审查的 A/B 分区候选方案，不修改磁盘。
#
# 方案：
# - 保留现有 uboot、misc、boot、recovery、amp、rootfs、oem 的位置。
# - 现有 boot/rootfs/amp 作为 A 槽，暂不改名。
# - 缩小 userdata 的尾部空间，新增 B 槽及独立元数据分区。
# - userdata 的文件系统必须在离线环境先缩小，再调整分区。
# - 禁止在 /userdata 已挂载时执行缩容。
#
# 已核对源码：
# U-Boot 的 AMP 加载路径仍固定查找 "amp"。
# Linux FIT/设备树加载、root= 参数、AMP 加载必须统一选择同一槽。
# 当前未完成这些修改，生成此文件不会启用 A/B。
#
# 数据来源：本次对话中开发板实际分区表。
# 执行迁移之前必须重新读取 GPT，检查布局仍与此一致。

import argparse
import json
from pathlib import Path

SECTOR_BYTES = 512
ALIGNMENT = 2048
DISK_SECTORS = 120832000
LAST_USABLE = 120831966

CURRENT = [
    (1, "uboot", 16384, 8192),
    (2, "misc", 24576, 8192),
    (3, "boot", 32768, 131072),
    (4, "recovery", 163840, 262144),
    (5, "amp", 425984, 65536),
    (6, "rootfs", 491520, 29360128),
    (7, "oem", 29851648, 262144),
    (8, "userdata", 30113792, 90718175),
]


def partition(number, name, start, sectors):
    return {
        "number": number,
        "name": name,
        "start_sector": start,
        "sectors": sectors,
        "end_sector": start + sectors - 1,
        "bytes": sectors * SECTOR_BYTES,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    # B 槽容量与当前 A 槽一致。
    additions = [
        (9, "boot_b", 131072),
        (10, "amp_b", 65536),
        (11, "rootfs_b", 29360128),
        (12, "abmeta", 8192),
    ]

    total_new = sum(item[2] for item in additions)
    aligned_end = ((LAST_USABLE + 1) // ALIGNMENT) * ALIGNMENT
    new_start = aligned_end - total_new

    userdata_start = CURRENT[-1][2]
    userdata_sectors = new_start - userdata_start

    if userdata_sectors <= 0 or new_start % ALIGNMENT:
        raise SystemExit("无法生成对齐且有效的候选布局")

    proposed = [partition(*item) for item in CURRENT[:-1]]
    proposed.append(partition(
        8, "userdata", userdata_start, userdata_sectors
    ))

    cursor = new_start
    for number, name, sectors in additions:
        proposed.append(partition(number, name, cursor, sectors))
        cursor += sectors

    for previous, following in zip(proposed, proposed[1:]):
        if previous["end_sector"] >= following["start_sector"]:
            raise SystemExit("候选分区存在重叠")

    if proposed[-1]["end_sector"] > LAST_USABLE:
        raise SystemExit("候选分区超出可用范围")

    plan = {
        "status": "DRAFT_NOT_EXECUTABLE",
        "board": "ATK-DLRK3588B",
        "device": "/dev/mmcblk0",
        "sector_bytes": SECTOR_BYTES,
        "disk_sectors": DISK_SECTORS,
        "last_usable_sector": LAST_USABLE,
        "expected_current_partitions": [
            partition(*item) for item in CURRENT
        ],
        "proposed_partitions": proposed,
        "slots": {
            "a": {"boot": "boot", "rootfs": "rootfs", "amp": "amp"},
            "b": {"boot": "boot_b", "rootfs": "rootfs_b", "amp": "amp_b"},
        },
        "migration_requirements": [
            "备份 userdata 并验证备份可读",
            "通过独立恢复环境启动，确保 userdata 未挂载",
            "离线检查并缩小 userdata 文件系统，之后才能缩小分区",
            "重新核对 GPT 可用项数量、主备 GPT 和所有边界",
            "为 B 槽分配独立 PARTUUID，不复制 A 槽分区 UUID",
        ],
        "bootloader_requirements": [
            "定义带版本、序号和校验的双副本持久化启动元数据",
            "在首次加载内核设备树或 AMP 镜像之前完成槽选择",
            "FIT、内核设备树、rootfs 和 AMP 使用同一个选定槽",
            "启动候选槽前持久化扣减尝试次数",
            "Linux 与 FreeRTOS 健康确认后标记启动成功",
            "启动失败后整机重启再回退，避免混用已运行的 AMP 固件",
            "定义启动卡死时的看门狗或外部复位策略",
            "接入签名验证后再接受远程升级包",
        ],
    }

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    # 不覆盖已有方案，保留审查记录。
    with output.open("x", encoding="utf-8") as stream:
        json.dump(plan, stream, ensure_ascii=False, indent=2)
        stream.write("\n")

    print(f"候选方案已保存：{output}")
    print("userdata 候选容量："
          f"{userdata_sectors * SECTOR_BYTES / (1024 ** 3):.3f} GiB")

    for item in proposed:
        print(
            f'{item["number"]:2d} {item["name"]:10s} '
            f'start={item["start_sector"]:9d} '
            f'end={item["end_sector"]:9d} '
            f'sectors={item["sectors"]:9d}'
        )

    print("仅生成方案；未缩容、未修改 GPT、未刷写、未启用 A/B。")


if __name__ == "__main__":
    main()