#ifndef AMP_AB_STORE_H
#define AMP_AB_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "amp_ab_meta.h"

#define AMP_AB_STORE_COPY_COUNT 2U

enum {
    AMP_AB_STORE_OK = 0,
    AMP_AB_STORE_EINVAL = -1,
    AMP_AB_STORE_EIO = -2,
    AMP_AB_STORE_ESYNC = -3,
    AMP_AB_STORE_EVERIFY = -4,
    AMP_AB_STORE_ESTALE = -5
};

/* 每次回调访问一个逻辑块，返回 0 表示完整成功。 */
typedef int (*AmpAbReadBlock)(
    void *ctx, uint64_t relative_lba, void *buf, size_t bytes);

typedef int (*AmpAbWriteBlock)(
    void *ctx, uint64_t relative_lba, const void *buf, size_t bytes);

/*
 * 必须提供真实的存储刷新语义。
 * 不能用空函数返回 0，不能仅用 CPU 内存屏障代替。
 */
typedef int (*AmpAbSync)(void *ctx);

typedef struct {
    void *ctx;
    uint64_t partition_blocks;
    uint64_t copy_lba[AMP_AB_STORE_COPY_COUNT];
    size_t block_size;
    AmpAbReadBlock read_block;
    AmpAbWriteBlock write_block;
    AmpAbSync sync;
} AmpAbStore;

/*
 * 读取完整的 AMP_AB_META_BYTES 字节。
 * 调用者必须检查返回值；失败后的缓冲区不得用于解析。
 */
int amp_ab_store_read(
    const AmpAbStore *store,
    unsigned int index,
    void *record,
    size_t capacity);

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
int amp_ab_store_commit_other(
    const AmpAbStore *store,
    unsigned int source_index,
    const void *encoded_record,
    void *scratch,
    size_t scratch_len);

#endif