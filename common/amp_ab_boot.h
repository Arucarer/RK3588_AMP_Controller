#ifndef AMP_AB_BOOT_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_AB_BOOT_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include "amp_ab_store.h" /* 引入启动元数据块存储及提交接口 */

/*
 * 由平台实现槽资源检查。
 *
 * 必须检查同一设备上该槽 boot、amp、rootfs 是否可用，
 * 这里只检查分区结构。镜像校验由启动器在持久化扣次后执行；
 * 校验失败禁止加载并复位，避免损坏候选槽无限重试。
 *
 * 此回调不能启动 AMP、跳转 Kernel 或修改元数据。
 * 返回 0 表示检查通过；其他值表示失败。
 */
typedef int (*AmpAbBootPreflight)(void *ctx, uint32_t slot);

enum {
    AMP_AB_BOOT_OK = 0,        /* 启动事务成功，仍须检查输出 ready 标志 */
    AMP_AB_BOOT_EINVAL = -100, /* 启动事务失败：参数或回调缺失 */
    AMP_AB_BOOT_EREAD = -101,  /* 启动事务失败：底层元数据读取错误 */
    AMP_AB_BOOT_EMETA = -102, /* 启动事务失败：没有可选的有效元数据或副本冲突 */
    AMP_AB_BOOT_ESTATE = -103,     /* 启动事务失败：状态决策或编码错误 */
    AMP_AB_BOOT_EPREFLIGHT = -104, /* 启动事务失败：目标槽资源预检未通过 */
    AMP_AB_BOOT_ECOMMIT = -105,    /* 启动事务失败：新元数据提交失败 */
    AMP_AB_BOOT_EVERIFY = -106 /* 启动事务失败：重新读取的状态与预期不一致 */
};

/*
 * 工作缓冲区约 12 KiB。
 * 不要把它放到 U-Boot 的小栈或 FreeRTOS 任务栈上。
 * 平台可使用静态存储或动态分配。
 */
/* 启动事务的双副本读取和编码缓冲区，避免在小栈上分配。 */
typedef struct {
    uint8_t copy[2][AMP_AB_META_BYTES]; /* 两份从存储读取的原始元数据记录 */
    uint8_t encoded[AMP_AB_META_BYTES]; /* 待提交的下一代元数据编码缓冲区 */
} AmpAbBootWorkspace;

/* 启动事务结果；仅成功返回且 ready 为 1 时才允许加载镜像。 */
typedef struct {
    /*
     * 只有函数返回 AMP_AB_BOOT_OK 且 ready == 1，
     * 才允许后续加载结果中的槽。
     */
    int ready; /* 本轮结果是否可用于启动；失败路径保持为 0 */

    uint32_t slot;               /* 本轮选中的启动槽编号 */
    uint64_t generation;         /* 本轮启动实际采用的元数据代次 */
    uint32_t selected_copy;      /* 最终核验选中的元数据副本编号，0 或 1 */
    uint32_t metadata_committed; /* 本轮是否完成了新元数据的持久化提交 */
    uint32_t rolled_back;        /* 本轮是否回退到原已确认槽 */

    /* 本轮实际采用的、已验证元数据状态。 */
    AMPABState state; /* 本轮实际采用并重新校验过的启动状态 */

    /* 失败时保留底层错误，便于打印诊断。 */
    int detail; /* 失败路径保留的底层错误码 */
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
int amp_ab_boot_transaction(const AmpAbStore *store, AmpAbBootPreflight preflight,
                            void *preflight_ctx, AmpAbBootWorkspace *work, AmpAbBootResult *result);

#endif /* 结束 AMP_AB_BOOT_H 对应的条件编译区段 */
