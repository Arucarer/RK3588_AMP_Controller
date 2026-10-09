#ifndef AMP_AB_UBOOT_STORE_H
#define AMP_AB_UBOOT_STORE_H

#include <blk.h>
#include <part.h>

#include "../../common/amp_ab_store.h"

/* U-Boot 块设备上下文 */
typedef struct
{
    struct blk_desc *dev;      // 启动块设备
    disk_partition_t part;     // abmeta 分区信息
    int write_enabled;         // 写入保护，默认关闭
} AmpAbUbootContext;

/* 绑定 U-Boot 的 abmeta 存储分区 */
int amp_ab_uboot_store_init(
    AmpAbUbootContext *ctx,
    AmpAbStore *store,
    uint64_t copy_a_lba,
    uint64_t copy_b_lba);

#endif