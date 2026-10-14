/* 新建：common/amp_ab.c */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */

#include "amp_ab.h" /* 引入 A/B 启动槽状态机接口 */

static int valid_slot(uint32_t slot)
{
    /* 1. 检查槽位是否为 A 或 B，排除无槽位及其他非法编号。 */

    return slot == AMP_AB_SLOT_A || slot == AMP_AB_SLOT_B;
}

static int advance(const AMPABState *current, AMPABState *next)
{
    /* 1. 拒绝代次达到上限的状态，防止计数回绕。 */
    /* 2. 复制当前状态并递增代次。 */

    if (current->generation == UINT64_MAX)
        return AMP_AB_OVERFLOW;

    *next = *current;
    next->generation++;
    return AMP_AB_OK;
}

void amp_ab_init(AMPABState *state)
{
    /* 1. 检查状态指针。 */
    /* 2. 初始化代次和已确认 A 槽，清除候选槽与尝试次数。 */

    if (state == NULL)
        return;

    state->generation = 1;
    state->confirmed_slot = AMP_AB_SLOT_A;
    state->pending_slot = AMP_AB_SLOT_NONE;
    state->tries_remaining = 0;
    state->trial_slot = AMP_AB_SLOT_NONE;
}

int amp_ab_validate(const AMPABState *state)
{
    /* 1. 检查指针、代次及已确认槽。 */
    /* 2. 检查候选槽与试启动槽之间的一致性。 */
    /* 3. 校验剩余尝试次数与试启动阶段是否匹配。 */

    if (state == NULL || state->generation == 0 || !valid_slot(state->confirmed_slot)) {
        return AMP_AB_INVALID;
    }

    if (state->pending_slot == AMP_AB_SLOT_NONE) {
        if (state->tries_remaining != 0 || state->trial_slot != AMP_AB_SLOT_NONE) {
            return AMP_AB_INVALID;
        }

        return AMP_AB_OK;
    }

    if (!valid_slot(state->pending_slot) || state->pending_slot == state->confirmed_slot ||
        state->tries_remaining > AMP_AB_MAX_TRIES) {
        return AMP_AB_INVALID;
    }

    if (state->trial_slot == AMP_AB_SLOT_NONE) {
        /* 刚暂存的升级尚未尝试启动。 */
        if (state->tries_remaining != AMP_AB_MAX_TRIES)
            return AMP_AB_INVALID;
    } else {
        if (state->trial_slot != state->pending_slot ||
            state->tries_remaining >= AMP_AB_MAX_TRIES) {
            return AMP_AB_INVALID;
        }
    }

    return AMP_AB_OK;
}

int amp_ab_stage(const AMPABState *current, uint32_t target_slot, AMPABState *next)
{
    /* 1. 校验当前状态及目标槽，拒绝覆盖已确认槽。 */
    /* 2. 拒绝尚未结束的候选升级事务。 */
    /* 3. 递增代次并设置候选槽、尝试次数及未试启动状态。 */

    int result;

    if (next == NULL || amp_ab_validate(current) != AMP_AB_OK || !valid_slot(target_slot) ||
        target_slot == current->confirmed_slot) {
        return AMP_AB_INVALID;
    }

    if (current->pending_slot != AMP_AB_SLOT_NONE)
        return AMP_AB_BUSY;

    result = advance(current, next);
    if (result != AMP_AB_OK)
        return result;

    next->pending_slot = target_slot;
    next->tries_remaining = AMP_AB_MAX_TRIES;
    next->trial_slot = AMP_AB_SLOT_NONE;

    return AMP_AB_OK;
}

int amp_ab_prepare_boot(const AMPABState *current, AMPABDecision *decision)
{
    /* 1. 校验状态并默认选择已确认槽。 */
    /* 2. 存在候选升级时递增代次并要求持久化。 */
    /* 3. 耗尽次数则回退，否则选择候选槽并扣减尝试次数。 */

    int result;

    if (decision == NULL || amp_ab_validate(current) != AMP_AB_OK) {
        return AMP_AB_INVALID;
    }

    decision->next = *current;
    decision->slot = current->confirmed_slot;
    decision->must_persist = 0;
    decision->rolled_back = 0;

    if (current->pending_slot == AMP_AB_SLOT_NONE)
        return AMP_AB_OK;

    result = advance(current, &decision->next);
    if (result != AMP_AB_OK)
        return result;

    decision->must_persist = 1;

    if (current->tries_remaining == 0) {
        /* 三次候选启动均未确认，回到原来已确认的槽。 */
        decision->next.pending_slot = AMP_AB_SLOT_NONE;
        decision->next.trial_slot = AMP_AB_SLOT_NONE;
        decision->next.tries_remaining = 0;
        decision->rolled_back = 1;
        return AMP_AB_OK;
    }

    decision->slot = current->pending_slot;
    decision->next.trial_slot = current->pending_slot;
    decision->next.tries_remaining--;

    return AMP_AB_OK;
}

int amp_ab_confirm(const AMPABState *current, uint32_t running_slot, uint64_t boot_generation,
                   AMPABState *next)
{
    /* 1. 核对运行槽、试启动槽和启动代次。 */
    /* 2. 递增代次并将候选槽转为已确认槽。 */
    /* 3. 清除候选及试启动状态。 */

    int result;

    if (next == NULL || amp_ab_validate(current) != AMP_AB_OK ||
        current->pending_slot == AMP_AB_SLOT_NONE || !valid_slot(running_slot) ||
        running_slot != current->pending_slot || running_slot != current->trial_slot ||
        boot_generation != current->generation) {
        return AMP_AB_INVALID;
    }

    result = advance(current, next);
    if (result != AMP_AB_OK)
        return result;

    next->confirmed_slot = running_slot;
    next->pending_slot = AMP_AB_SLOT_NONE;
    next->trial_slot = AMP_AB_SLOT_NONE;
    next->tries_remaining = 0;

    return AMP_AB_OK;
}
