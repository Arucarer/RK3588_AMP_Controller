#include "amp_ab_boot.h"

#include <string.h>

static int state_equal(const AMPABState *a, const AMPABState *b)
{
    return a->generation == b->generation &&
           a->confirmed_slot == b->confirmed_slot &&
           a->pending_slot == b->pending_slot &&
           a->tries_remaining == b->tries_remaining &&
           a->trial_slot == b->trial_slot;
}

static int load_metadata(
    const AmpAbStore *store,
    AmpAbBootWorkspace *work,
    AMPABMetaSelection *selection,
    int *detail)
{
    int ret;
    unsigned int i;

    for (i = 0; i < 2; ++i) {
        ret = amp_ab_store_read(
            store, i, work->copy[i], AMP_AB_META_BYTES);

        if (ret != AMP_AB_STORE_OK) {
            *detail = ret;
            return AMP_AB_BOOT_EREAD;
        }
    }

    ret = amp_ab_meta_select(
        work->copy[0], work->copy[1], selection);

    if (ret != AMP_AB_OK) {
        *detail = ret;
        return AMP_AB_BOOT_EMETA;
    }

    return AMP_AB_BOOT_OK;
}

int amp_ab_boot_transaction(
    const AmpAbStore *store,
    AmpAbBootPreflight preflight,
    void *preflight_ctx,
    AmpAbBootWorkspace *work,
    AmpAbBootResult *result)
{
    AMPABMetaSelection selected;
    AMPABMetaSelection verified;
    AMPABDecision decision;
    unsigned int expected_copy;
    int ret;

    if (!result)
        return AMP_AB_BOOT_EINVAL;

    /* 包括失败路径：绝不遗留上一次 ready 状态。 */
    memset(result, 0, sizeof(*result));

    if (!store || !preflight || !work)
        return AMP_AB_BOOT_EINVAL;

    ret = load_metadata(store, work, &selected, &result->detail);
    if (ret != AMP_AB_BOOT_OK)
        return ret;

    ret = amp_ab_prepare_boot(&selected.state, &decision);
    if (ret != AMP_AB_OK) {
        result->detail = ret;
        return AMP_AB_BOOT_ESTATE;
    }

    /*
     * 在消耗尝试次数前检查槽资源。
     * 此时尚不允许启动 AMP 或 Kernel。
     */
    ret = preflight(preflight_ctx, decision.slot);
    if (ret != 0) {
        result->detail = ret;
        return AMP_AB_BOOT_EPREFLIGHT;
    }

    if (decision.must_persist) {
        ret = amp_ab_meta_encode(&decision.next, work->encoded);
        if (ret != AMP_AB_OK) {
            result->detail = ret;
            return AMP_AB_BOOT_ESTATE;
        }

        expected_copy = selected.selected_copy ^ 1U;

        /*
         * commit_other 会重新检查当前代次，并要求同步回调。
         * work->encoded 和 work->copy[0] 是独立缓冲区。
         */
        ret = amp_ab_store_commit_other(
            store,
            selected.selected_copy,
            work->encoded,
            work->copy[0],
            AMP_AB_META_BYTES);

        if (ret != AMP_AB_STORE_OK) {
            result->detail = ret;
            return AMP_AB_BOOT_ECOMMIT;
        }

        /*
         * 提交后重新读取两份记录，确认最新选中结果
         * 正是本轮计算出的状态。
         */
        ret = load_metadata(store, work, &verified, &result->detail);
        if (ret != AMP_AB_BOOT_OK)
            return ret;

        if (verified.selected_copy != expected_copy ||
            !state_equal(&verified.state, &decision.next)) {
            result->detail = AMP_AB_STORE_EVERIFY;
            return AMP_AB_BOOT_EVERIFY;
        }

        result->metadata_committed = 1;
    } else {
        /*
         * 无需提交时也重新检查，防止 preflight 期间状态变化。
         * 这不是并发锁；调用方仍必须保证事务独占。
         */
        ret = load_metadata(store, work, &verified, &result->detail);
        if (ret != AMP_AB_BOOT_OK)
            return ret;

        if (verified.selected_copy != selected.selected_copy ||
            !state_equal(&verified.state, &selected.state)) {
            result->detail = AMP_AB_STORE_ESTALE;
            return AMP_AB_BOOT_EVERIFY;
        }
    }

    result->slot = decision.slot;
    result->generation = verified.state.generation;
    result->selected_copy = verified.selected_copy;
    result->rolled_back = decision.rolled_back;
    result->state = verified.state;
    result->detail = 0;

    /* 最后发布成功标志。 */
    result->ready = 1;
    return AMP_AB_BOOT_OK;
}