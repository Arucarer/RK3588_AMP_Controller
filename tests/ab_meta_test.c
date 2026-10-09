/* 新建：tests/ab_meta_test.c
 *
 * 仅在 Ubuntu 内存中检查编码、损坏检测和副本选择。
 * 不代表已验证 eMMC 掉电安全。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../common/amp_ab_meta.h"

static uint8_t copy0[AMP_AB_META_BYTES];
static uint8_t copy1[AMP_AB_META_BYTES];
static uint8_t damaged[AMP_AB_META_BYTES];

int main(void)
{
    AMPABState initial;
    AMPABState pending;
    AMPABState decoded;
    AMPABState conflict;
    AMPABMetaSelection selected;
    size_t i;

    amp_ab_init(&initial);

    assert(amp_ab_meta_encode(&initial, copy0) == AMP_AB_OK);
    assert(amp_ab_meta_decode(copy0, &decoded) == AMP_AB_OK);

    assert(decoded.generation == initial.generation);
    assert(decoded.confirmed_slot == AMP_AB_SLOT_A);
    assert(decoded.pending_slot == AMP_AB_SLOT_NONE);
    assert(decoded.tries_remaining == 0);
    assert(decoded.trial_slot == AMP_AB_SLOT_NONE);

    /* 固定格式和小端编码。 */
    assert(memcmp(copy0, "AMPAB001", 8) == 0);
    assert(copy0[8] == 1 && copy0[9] == 0);
    assert(copy0[12] == 0 && copy0[13] == 0x10);
    assert(copy0[16] == 1 && copy0[17] == 0);

    /* 两份完全相同，选择副本 0。 */
    memcpy(copy1, copy0, sizeof(copy1));
    assert(amp_ab_meta_select(copy0, copy1, &selected) == AMP_AB_OK);
    assert(selected.valid_mask == 3);
    assert(selected.selected_copy == 0);

    /* 新代次胜出，与副本位置无关。 */
    assert(amp_ab_stage(&initial, AMP_AB_SLOT_B, &pending) == AMP_AB_OK);
    assert(amp_ab_meta_encode(&pending, copy1) == AMP_AB_OK);

    assert(amp_ab_meta_select(copy0, copy1, &selected) == AMP_AB_OK);
    assert(selected.selected_copy == 1);
    assert(selected.state.generation == pending.generation);

    assert(amp_ab_meta_select(copy1, copy0, &selected) == AMP_AB_OK);
    assert(selected.selected_copy == 0);

    /* 逐字节翻转一位：包括头部、字段、保留区及 CRC。 */
    for (i = 0; i < AMP_AB_META_BYTES; ++i) {
        memcpy(damaged, copy1, sizeof(damaged));
        damaged[i] ^= 1U;

        assert(amp_ab_meta_decode(damaged, &decoded)
               == AMP_AB_META_INVALID);

        assert(amp_ab_meta_select(copy0, damaged, &selected)
               == AMP_AB_OK);
        assert(selected.valid_mask == 1);
        assert(selected.selected_copy == 0);
    }

    /* 两个各自合法的状态若同代次但不同内容，必须报冲突。 */
    conflict = initial;
    conflict.generation = pending.generation;
    assert(amp_ab_meta_encode(&conflict, damaged) == AMP_AB_OK);
    assert(amp_ab_meta_select(damaged, copy1, &selected)
           == AMP_AB_META_CONFLICT);

    /* 全零、全 FF 均不能触发自动初始化。 */
    memset(copy0, 0, sizeof(copy0));
    memset(copy1, 0xff, sizeof(copy1));
    assert(amp_ab_meta_select(copy0, copy1, &selected)
           == AMP_AB_META_INVALID);

    /* 非法状态不能生成有效记录。 */
    initial.generation = 0;
    assert(amp_ab_meta_encode(&initial, copy0)
           == AMP_AB_META_INVALID);

    puts("PASS: 固定格式、小端编码、CRC损坏检测、双副本选择与同代次冲突");
    return 0;
}