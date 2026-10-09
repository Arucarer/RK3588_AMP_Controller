/* 新建：tests/ab_state_test.c
 *
 * 在 Ubuntu 测试状态转换，不连接开发板、不操作分区。
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../common/amp_ab.h"

int main(void)
{
    AMPABState state;
    AMPABState next;
    AMPABDecision decision;

    /* 默认启动 A。 */
    amp_ab_init(&state);
    assert(amp_ab_validate(&state) == AMP_AB_OK);
    assert(amp_ab_prepare_boot(&state, &decision) == AMP_AB_OK);
    assert(decision.slot == AMP_AB_SLOT_A);
    assert(decision.must_persist == 0);

    /* 禁止覆盖当前已确认槽。 */
    assert(amp_ab_stage(&state, AMP_AB_SLOT_A, &next)
           == AMP_AB_INVALID);

    /* 发布 B，但尚未尝试启动。 */
    assert(amp_ab_stage(&state, AMP_AB_SLOT_B, &next) == AMP_AB_OK);
    state = next;

    /* 尚未启动不能标记成功。 */
    assert(amp_ab_confirm(&state, AMP_AB_SLOT_B,
                         state.generation, &next) == AMP_AB_INVALID);

    /* 同一升级事务进行中，不能再次暂存。 */
    assert(amp_ab_stage(&state, AMP_AB_SLOT_B, &next) == AMP_AB_BUSY);

    /* 模拟三次启动 B，均未收到成功确认。 */
    for (uint32_t attempt = 0; attempt < AMP_AB_MAX_TRIES; ++attempt) {
        assert(amp_ab_prepare_boot(&state, &decision) == AMP_AB_OK);
        assert(decision.slot == AMP_AB_SLOT_B);
        assert(decision.must_persist == 1);
        assert(decision.rolled_back == 0);
        assert(decision.next.tries_remaining ==
               AMP_AB_MAX_TRIES - attempt - 1U);

        /* 仅模拟持久化成功，不进行真实写盘。 */
        state = decision.next;
        assert(amp_ab_validate(&state) == AMP_AB_OK);
    }

    /* 第四次启动回退到 A。 */
    assert(amp_ab_prepare_boot(&state, &decision) == AMP_AB_OK);
    assert(decision.slot == AMP_AB_SLOT_A);
    assert(decision.rolled_back == 1);
    state = decision.next;

    /* 再次升级到 B，并确认本次启动成功。 */
    assert(amp_ab_stage(&state, AMP_AB_SLOT_B, &next) == AMP_AB_OK);
    state = next;

    assert(amp_ab_prepare_boot(&state, &decision) == AMP_AB_OK);
    state = decision.next;

    /* 旧启动代次和错误槽不得确认。 */
    assert(amp_ab_confirm(&state, AMP_AB_SLOT_B,
                         state.generation - 1U, &next) == AMP_AB_INVALID);
    assert(amp_ab_confirm(&state, AMP_AB_SLOT_A,
                         state.generation, &next) == AMP_AB_INVALID);

    assert(amp_ab_confirm(&state, AMP_AB_SLOT_B,
                         state.generation, &next) == AMP_AB_OK);
    state = next;

    assert(state.confirmed_slot == AMP_AB_SLOT_B);
    assert(state.pending_slot == AMP_AB_SLOT_NONE);

    assert(amp_ab_prepare_boot(&state, &decision) == AMP_AB_OK);
    assert(decision.slot == AMP_AB_SLOT_B);
    assert(decision.must_persist == 0);

    /* B 确认后，可以向 A 发布下一版。 */
    assert(amp_ab_stage(&state, AMP_AB_SLOT_A, &next) == AMP_AB_OK);

    /* 代次禁止回绕。 */
    state.generation = UINT64_MAX;
    assert(amp_ab_stage(&state, AMP_AB_SLOT_A, &next) == AMP_AB_OVERFLOW);

    /* 非法状态必须被拒绝。 */
    amp_ab_init(&state);
    state.tries_remaining = 1;
    assert(amp_ab_validate(&state) == AMP_AB_INVALID);

    puts("PASS: A/B 状态转换、三次试启动回退、成功确认与代次检查");
    return 0;
}