#include "amp_ab_store.h" /* 引入启动元数据块存储及提交接口 */

#include <string.h> /* 引入内存复制、清零及字符串处理接口 */

static int store_valid(const AmpAbStore *store)
{
    /* 1. 检查存储回调、缓冲区与容量。 */
    /* 2. 返回存储接口是否满足元数据访问要求。 */

    uint64_t blocks;
    uint64_t first;
    uint64_t second;

    if (!store || !store->read_block || !store->partition_blocks || !store->block_size ||
        store->block_size > AMP_AB_META_BYTES || AMP_AB_META_BYTES % store->block_size != 0)
        return 0;

    blocks = AMP_AB_META_BYTES / store->block_size;

    if (store->partition_blocks < blocks || store->copy_lba[0] > store->partition_blocks - blocks ||
        store->copy_lba[1] > store->partition_blocks - blocks)
        return 0;

    first = store->copy_lba[0];
    second = store->copy_lba[1];

    /* 两个完整记录的范围不得重叠。 */
    if (first <= second)
        return second - first >= blocks;

    return first - second >= blocks;
}

static int buffers_overlap(const void *a, const void *b, size_t length)
{
    /* 1. 计算两个缓冲区的地址范围。 */
    /* 2. 判断范围是否交叠，避免读写校验互相覆盖。 */

    uintptr_t x = (uintptr_t)a;
    uintptr_t y = (uintptr_t)b;

    if (x <= y)
        return y - x < length;

    return x - y < length;
}

int amp_ab_store_read(const AmpAbStore *store, unsigned int index, void *record, size_t capacity)
{
    /* 1. 校验存储接口、副本编号和输出容量。 */
    /* 2. 按块读取指定副本，读取失败时清空输出。 */
    /* 3. 返回读取结果，记录解码由上层完成。 */

    uint8_t *output = record;
    size_t offset;
    uint64_t lba;

    if (!store_valid(store) || index >= AMP_AB_STORE_COPY_COUNT || !record ||
        capacity < AMP_AB_META_BYTES)
        return AMP_AB_STORE_EINVAL;

    lba = store->copy_lba[index];

    for (offset = 0; offset < AMP_AB_META_BYTES; offset += store->block_size, ++lba) {
        if (store->read_block(store->ctx, lba, output + offset, store->block_size) != 0) {
            memset(output, 0, AMP_AB_META_BYTES);
            return AMP_AB_STORE_EIO;
        }
    }

    return AMP_AB_STORE_OK;
}

int amp_ab_store_commit_other(const AmpAbStore *store, unsigned int source_index,
                              const void *encoded_record, void *scratch, size_t scratch_len)
{
    /* 1. 校验提交状态、目标副本和缓冲区。 */
    /* 2. 写入另一副本并同步存储。 */
    /* 3. 读回并比较记录，确认提交完整。 */

    const uint8_t *input = encoded_record;
    AMPABState proposed;
    AMPABState current;
    AMPABState other;
    AMPABState verified;
    unsigned int target;
    size_t offset;
    uint64_t lba;
    int ret;

    if (!store_valid(store) || !store->write_block || source_index >= AMP_AB_STORE_COPY_COUNT ||
        !encoded_record || !scratch || scratch_len < AMP_AB_META_BYTES ||
        buffers_overlap(encoded_record, scratch, AMP_AB_META_BYTES))
        return AMP_AB_STORE_EINVAL;

    /*
     * 必须在任何写入之前拒绝缺失同步接口的情况。
     * 不再提供 VERIFIED_NO_SYNC 这种可被误用的成功状态。
     */
    if (!store->sync)
        return AMP_AB_STORE_ESYNC;

    if (amp_ab_meta_decode(input, &proposed) != AMP_AB_OK)
        return AMP_AB_STORE_EINVAL;

    ret = amp_ab_store_read(store, source_index, scratch, scratch_len);
    if (ret != AMP_AB_STORE_OK)
        return ret;

    if (amp_ab_meta_decode(scratch, &current) != AMP_AB_OK)
        return AMP_AB_STORE_EVERIFY;

    if (current.generation == UINT64_MAX || proposed.generation != current.generation + 1)
        return AMP_AB_STORE_ESTALE;

    target = source_index ^ 1U;

    /*
     * 提交时遇到读取错误采取保守策略：
     * 无法判断目标是否拥有更新状态，直接停止，不覆盖。
     */
    ret = amp_ab_store_read(store, target, scratch, scratch_len);
    if (ret != AMP_AB_STORE_OK)
        return ret;

    if (amp_ab_meta_decode(scratch, &other) == AMP_AB_OK) {
        if (other.generation > current.generation)
            return AMP_AB_STORE_ESTALE;

        if (other.generation == current.generation) {
            if (other.confirmed_slot != current.confirmed_slot ||
                other.pending_slot != current.pending_slot ||
                other.tries_remaining != current.tries_remaining ||
                other.trial_slot != current.trial_slot)
                return AMP_AB_STORE_EVERIFY;

            /* 同代次相同记录时，meta_select 固定选择副本 0。 */
            if (source_index != 0)
                return AMP_AB_STORE_ESTALE;
        }
    }

    lba = store->copy_lba[target];

    for (offset = 0; offset < AMP_AB_META_BYTES; offset += store->block_size, ++lba) {
        if (store->write_block(store->ctx, lba, input + offset, store->block_size) != 0)
            return AMP_AB_STORE_EIO;
    }

    if (store->sync(store->ctx) != 0)
        return AMP_AB_STORE_ESYNC;

    ret = amp_ab_store_read(store, target, scratch, scratch_len);
    if (ret != AMP_AB_STORE_OK)
        return ret;

    if (memcmp(input, scratch, AMP_AB_META_BYTES) != 0 ||
        amp_ab_meta_decode(scratch, &verified) != AMP_AB_OK)
        return AMP_AB_STORE_EVERIFY;

    return AMP_AB_STORE_OK;
}
