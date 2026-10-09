#ifndef AMP_AB_BOOT_H
#define AMP_AB_BOOT_H

#include "amp_ab_store.h"

/*
 * 由平台实现槽资源检查。
 *
 * 必须检查同一设备上该槽 boot、amp、rootfs 是否可用，
 * 并完成平台要求的镜像格式、兼容性及认证检查。
 *
 * 此回调不能启动 AMP、跳转 Kernel 或修改元数据。
 * 返回 0 表示检查通过；其他值表示失败。
 */
typedef int (*AmpAbBootPreflight)(void *ctx, uint32_t slot);

enum {
    AMP_AB_BOOT_OK = 0,
    AMP_AB_BOOT_EINVAL = -100,
    AMP_AB_BOOT_EREAD = -101,
    AMP_AB_BOOT_EMETA = -102,
    AMP_AB_BOOT_ESTATE = -103,
    AMP_AB_BOOT_EPREFLIGHT = -104,
    AMP_AB_BOOT_ECOMMIT = -105,
    AMP_AB_BOOT_EVERIFY = -106
};

/*
 * 工作缓冲区约 12 KiB。
 * 不要把它放到 U-Boot 的小栈或 FreeRTOS 任务栈上。
 * 平台可使用静态存储或动态分配。
 */
typedef struct {
    uint8_t copy[2][AMP_AB_META_BYTES];
    uint8_t encoded[AMP_AB_META_BYTES];
} AmpAbBootWorkspace;

typedef struct {
    /*
     * 只有函数返回 AMP_AB_BOOT_OK 且 ready == 1，
     * 才允许后续加载结果中的槽。
     */
    int ready;

    uint32_t slot;
    uint64_t generation;
    uint32_t selected_copy;
    uint32_t metadata_committed;
    uint32_t rolled_back;

    /* 本轮实际采用的、已验证元数据状态。 */
    AMPABState state;

    /* 失败时保留底层错误，便于打印诊断。 */
    int detail;
} AmpAbBootResult;

/*
 * 执行一次启动事务。
 *
 * 调用方必须保证没有其他元数据写入者。
 * 同一次启动只应执行一次成功事务，不能在每个加载入口重复调用，
 * 否则会重复扣减尝试次数。
 *
 * 本实现采用保守策略：
 * 任一副本发生底层读取错误，停止事务；
 * 副本读成功但 CRC 无效，则可选择另一份有效副本。
 *
 * 任意失败都会保持 result->ready == 0。
 * 提交失败后不得在同一次启动中自行换槽继续加载。
 */
int amp_ab_boot_transaction(
    const AmpAbStore *store,
    AmpAbBootPreflight preflight,
    void *preflight_ctx,
    AmpAbBootWorkspace *work,
    AmpAbBootResult *result);

#endif