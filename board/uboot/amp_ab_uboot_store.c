/*
 * 文件名：amp_ab_uboot_store.c
 * 作用：将 A/B 元数据存储接口适配到 Rockchip U-Boot 块设备。
 */

#include "amp_ab_uboot_store.h"

#include <boot_rkimg.h>
#include <string.h>

/* 读取 abmeta 分区中的单个逻辑块 */
static int uboot_read_block(
    void *ctx, uint64_t relative_lba,
    void *buf, size_t bytes)
{
    AmpAbUbootContext *io = (AmpAbUbootContext *)ctx;

    if (!io || !io->dev || !buf ||
        bytes != io->part.blksz ||
        relative_lba >= io->part.size)
        return -1;

    if (blk_dread(
            io->dev,
            io->part.start + relative_lba,
            1,
            buf) != 1)
        return -1;

    return 0;
}

/* 写入 abmeta 分区中的单个逻辑块 */
static int uboot_write_block(
    void *ctx, uint64_t relative_lba,
    const void *buf, size_t bytes)
{
    AmpAbUbootContext *io = (AmpAbUbootContext *)ctx;

    if (!io || !io->dev || !buf ||
        !io->write_enabled ||
        bytes != io->part.blksz ||
        relative_lba >= io->part.size)
        return -1;

    if (blk_dwrite(
            io->dev,
            io->part.start + relative_lba,
            1,
            buf) != 1)
        return -1;

    return 0;
}

/* 初始化 U-Boot A/B 元数据存储适配层 */
int amp_ab_uboot_store_init(
    AmpAbUbootContext *ctx,
    AmpAbStore *store,
    uint64_t copy_a_lba,
    uint64_t copy_b_lba)
{
    struct blk_desc *dev;

    if (!ctx || !store)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    memset(store, 0, sizeof(*store));

    /* 获取 Rockchip 当前启动设备 */
    dev = rockchip_get_bootdev();
    if (!dev)
        return -1;

    /* 根据名称查找 abmeta 分区 */
    if (part_get_info_by_name(
            dev, "abmeta", &ctx->part) < 0)
        return -1;

    /* 验证逻辑块大小及双副本偏移 */
    if (!ctx->part.blksz ||
        ctx->part.blksz != dev->blksz ||
        copy_a_lba == copy_b_lba ||
        copy_a_lba >= ctx->part.size ||
        copy_b_lba >= ctx->part.size)
        return -1;

    ctx->dev = dev;
    ctx->write_enabled = 0;

    /* 关联通用存储层 */
    store->ctx = ctx;
    store->partition_blocks = ctx->part.size;
    store->copy_lba[0] = copy_a_lba;
    store->copy_lba[1] = copy_b_lba;
    store->block_size = ctx->part.blksz;

    store->read_block = uboot_read_block;
    store->write_block = uboot_write_block;

    /* 当前 SDK 尚未确认可靠的 MMC flush 接口 */
    store->sync = NULL;

    return 0;
}