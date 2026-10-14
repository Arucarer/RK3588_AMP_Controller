#ifndef AMP_AB_UBOOT_STORE_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_AB_UBOOT_STORE_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <blk.h>  /* 引入 U-Boot 块设备访问接口 */
#include <part.h> /* 引入 U-Boot 分区查询接口 */

#include "../../common/amp_ab_store.h" /* 引入启动元数据块存储及提交接口 */

/* U-Boot 块设备上下文 */
/* U-Boot 元数据存储回调使用的平台上下文。 */
typedef struct {
    struct blk_desc *dev;  // 启动块设备
    disk_partition_t part; // abmeta 分区信息
    int write_enabled;     // 写入保护，默认关闭
} AmpAbUbootContext;

/* 绑定 U-Boot 的 abmeta 存储分区 */
int amp_ab_uboot_store_init(AmpAbUbootContext *ctx, AmpAbStore *store, uint64_t copy_a_lba,
                            uint64_t copy_b_lba);

#endif /* 结束 AMP_AB_UBOOT_STORE_H 对应的条件编译区段 */
