/* 新建：common/amp_ab_meta.c */

#include <stddef.h>      /* 引入 size_t、NULL 等基础类型与定义 */
#include "amp_ab_meta.h" /* 引入启动元数据编码、校验及双副本选择接口 */

#define CRC_OFFSET (AMP_AB_META_BYTES - 4U) /* 元数据 CRC32 字段的字节偏移，位于记录最后 4 字节 */

static const uint8_t meta_magic[8] = {'A', 'M', 'P', 'A', 'B',
                                      '0', '0', '1'}; /* meta_magic：启动元数据记录的固定魔数 */

static void put32(uint8_t *p, uint32_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    unsigned int i;

    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8U));
}

static uint32_t get32(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    uint32_t value = 0;
    unsigned int i;

    for (i = 0; i < 4; ++i)
        value |= (uint32_t)p[i] << (i * 8U);

    return value;
}

static void put64(uint8_t *p, uint64_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t get64(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* CRC-32/ISO-HDLC：反射多项式、初始值和最终异或均固定。 */
static uint32_t record_crc(const uint8_t *data, size_t length)
{
    /* 1. 初始化 CRC 累加值。 */
    /* 2. 按协议的多项式逐字节计算。 */
    /* 3. 返回 CRC，用于编码或完整性检查。 */

    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];

        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xEDB88320) : 0U);
    }

    return crc ^ UINT32_MAX;
}

int amp_ab_meta_encode(const AMPABState *state, uint8_t record[AMP_AB_META_BYTES])
{
    /* 1. 检查状态及记录缓冲区。 */
    /* 2. 编码魔数、版本、代次和槽位字段。 */
    /* 3. 计算记录 CRC 并写入尾部。 */

    size_t i;

    if (record == NULL || amp_ab_validate(state) != AMP_AB_OK)
        return AMP_AB_META_INVALID;

    for (i = 0; i < AMP_AB_META_BYTES; ++i)
        record[i] = 0;

    for (i = 0; i < sizeof(meta_magic); ++i)
        record[i] = meta_magic[i];

    put32(record + 8, 1U);
    put32(record + 12, AMP_AB_META_BYTES);
    put64(record + 16, state->generation);
    put32(record + 24, state->confirmed_slot);
    put32(record + 28, state->pending_slot);
    put32(record + 32, state->tries_remaining);
    put32(record + 36, state->trial_slot);
    put32(record + CRC_OFFSET, record_crc(record, CRC_OFFSET));

    return AMP_AB_OK;
}

int amp_ab_meta_decode(const uint8_t record[AMP_AB_META_BYTES], AMPABState *state)
{
    /* 1. 核对记录魔数、版本、大小和 CRC。 */
    /* 2. 解码启动状态字段。 */
    /* 3. 检查状态一致性后返回结果。 */

    AMPABState decoded;
    size_t i;

    if (record == NULL || state == NULL)
        return AMP_AB_META_INVALID;

    for (i = 0; i < sizeof(meta_magic); ++i) {
        if (record[i] != meta_magic[i])
            return AMP_AB_META_INVALID;
    }

    if (get32(record + 8) != 1U || get32(record + 12) != AMP_AB_META_BYTES ||
        get32(record + CRC_OFFSET) != record_crc(record, CRC_OFFSET)) {
        return AMP_AB_META_INVALID;
    }

    for (i = 40; i < CRC_OFFSET; ++i) {
        if (record[i] != 0)
            return AMP_AB_META_INVALID;
    }

    decoded.generation = get64(record + 16);
    decoded.confirmed_slot = get32(record + 24);
    decoded.pending_slot = get32(record + 28);
    decoded.tries_remaining = get32(record + 32);
    decoded.trial_slot = get32(record + 36);

    if (amp_ab_validate(&decoded) != AMP_AB_OK)
        return AMP_AB_META_INVALID;

    *state = decoded;
    return AMP_AB_OK;
}

int amp_ab_meta_select(const uint8_t copy0[AMP_AB_META_BYTES],
                       const uint8_t copy1[AMP_AB_META_BYTES], AMPABMetaSelection *selection)
{
    /* 1. 分别解码两份元数据副本。 */
    /* 2. 处理损坏副本与代次冲突。 */
    /* 3. 选择最新有效副本并返回其索引和状态。 */

    AMPABState states[2];
    uint32_t mask = 0;
    uint32_t selected;
    size_t i;

    if (copy0 == NULL || copy1 == NULL || selection == NULL)
        return AMP_AB_META_INVALID;

    if (amp_ab_meta_decode(copy0, &states[0]) == AMP_AB_OK)
        mask |= 1U;

    if (amp_ab_meta_decode(copy1, &states[1]) == AMP_AB_OK)
        mask |= 2U;

    if (mask == 0)
        return AMP_AB_META_INVALID;

    if (mask == 3U) {
        if (states[0].generation == states[1].generation) {
            for (i = 0; i < AMP_AB_META_BYTES; ++i) {
                if (copy0[i] != copy1[i])
                    return AMP_AB_META_CONFLICT;
            }

            selected = 0;
        } else {
            selected = states[1].generation > states[0].generation ? 1U : 0U;
        }
    } else {
        selected = mask == 1U ? 0U : 1U;
    }

    selection->state = states[selected];
    selection->selected_copy = selected;
    selection->valid_mask = mask;

    return AMP_AB_OK;
}
