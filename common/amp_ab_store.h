#ifndef AMP_AB_STORE_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_AB_STORE_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "amp_ab_meta.h" /* 引入启动元数据编码、校验及双副本选择接口 */

#define AMP_AB_STORE_COPY_COUNT 2U /* 持久化元数据的冗余副本数量 */

enum {
    AMP_AB_STORE_OK = 0,       /* 元数据存储操作成功 */
    AMP_AB_STORE_EINVAL = -1,  /* 元数据存储参数、缓冲区或回调不合法 */
    AMP_AB_STORE_EIO = -2,     /* 元数据底层块读写失败 */
    AMP_AB_STORE_ESYNC = -3,   /* 元数据缺少同步回调或同步失败 */
    AMP_AB_STORE_EVERIFY = -4, /* 元数据读回或解码校验失败 */
    AMP_AB_STORE_ESTALE = -5   /* 元数据代次已变化或提交状态不是下一代 */
};

/* 每次回调访问一个逻辑块，返回 0 表示完整成功。 */
typedef int (*AmpAbReadBlock)(void *ctx, uint64_t relative_lba, void *buf, size_t bytes);

typedef int (*AmpAbWriteBlock)(void *ctx, uint64_t relative_lba, const void *buf, size_t bytes);

/*
 * 必须提供真实的存储刷新语义。
 * 不能用空函数返回 0，不能仅用 CPU 内存屏障代替。
 */
typedef int (*AmpAbSync)(void *ctx);

/* 元数据块存储适配接口；副本地址相对所属分区起点计算。 */
typedef struct {
    void *ctx;                 /* 透传给块读写和同步回调的平台上下文 */
    uint64_t partition_blocks; /* 元数据分区的逻辑块总数，用于边界检查 */
    uint64_t copy_lba[AMP_AB_STORE_COPY_COUNT]; /* 两份元数据记录的起始逻辑块号，相对分区起点 */
    size_t block_size;                          /* 单个逻辑块的字节数 */
    AmpAbReadBlock read_block;   /* 读取一个逻辑块的回调，返回 0 表示完整成功 */
    AmpAbWriteBlock write_block; /* 写入一个逻辑块的回调，返回 0 表示完整成功 */
    AmpAbSync sync; /* 持久化刷新回调；提交操作要求存在且成功，不能用空实现代替 */
} AmpAbStore;

/*
 * 读取完整的 AMP_AB_META_BYTES 字节。
 * 调用者必须检查返回值；失败后的缓冲区不得用于解析。
 */
int amp_ab_store_read(const AmpAbStore *store, unsigned int index, void *record, size_t capacity);

/*
 * encoded_record：完整编码的新记录。
 * scratch：至少 4096 字节，与 encoded_record 不得有任何重叠。
 *
 * 提交前重新读取两个副本，确认 source_index 仍为选中副本，
 * 并要求新 generation 恰好为当前 generation + 1。
 *
 * 写入非当前副本 → 刷新 → 完整读回比较及解码。
 *
 * 调用方必须串行化整个读取、状态转换、提交事务。
 * 本函数不提供线程锁或跨进程锁。
 *
 * 返回失败后，不得启动候选槽。
 * 写入可能已经发生，下一次操作必须重新读取两个副本。
 *
 * 不提供首次初始化：两份均无效时拒绝提交。
 */
int amp_ab_store_commit_other(const AmpAbStore *store, unsigned int source_index,
                              const void *encoded_record, void *scratch, size_t scratch_len);

#endif /* 结束 AMP_AB_STORE_H 对应的条件编译区段 */
