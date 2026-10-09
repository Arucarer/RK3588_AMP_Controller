/* 新建：common/amp_ab.c */

#include <stddef.h>

#include "amp_ab.h"

static int valid_slot(uint32_t slot)
{
    return slot == AMP_AB_SLOT_A || slot == AMP_AB_SLOT_B;
}

static int advance(const AMPABState *current, AMPABState *next)
{
    if (current->generation == UINT64_MAX)
        return AMP_AB_OVERFLOW;

    *next = *current;
    next->generation++;
    return AMP_AB_OK;
}

void amp_ab_init(AMPABState *state)
{
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
    if (state == NULL ||
        state->generation == 0 ||
        !valid_slot(state->confirmed_slot)) {
        return AMP_AB_INVALID;
    }

    if (state->pending_slot == AMP_AB_SLOT_NONE) {
        if (state->tries_remaining != 0 ||
            state->trial_slot != AMP_AB_SLOT_NONE) {
            return AMP_AB_INVALID;
        }

        return AMP_AB_OK;
    }

    if (!valid_slot(state->pending_slot) ||
        state->pending_slot == state->confirmed_slot ||
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

int amp_ab_stage(const AMPABState *current,
                 uint32_t target_slot,
                 AMPABState *next)
{
    int result;

    if (next == NULL ||
        amp_ab_validate(current) != AMP_AB_OK ||
        !valid_slot(target_slot) ||
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

int amp_ab_prepare_boot(const AMPABState *current,
                        AMPABDecision *decision)
{
    int result;

    if (decision == NULL ||
        amp_ab_validate(current) != AMP_AB_OK) {
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

int amp_ab_confirm(const AMPABState *current,
                   uint32_t running_slot,
                   uint64_t boot_generation,
                   AMPABState *next)
{
    int result;

    if (next == NULL ||
        amp_ab_validate(current) != AMP_AB_OK ||
        current->pending_slot == AMP_AB_SLOT_NONE ||
        !valid_slot(running_slot) ||
        running_slot != current->pending_slot ||
        running_slot != current->trial_slot ||
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