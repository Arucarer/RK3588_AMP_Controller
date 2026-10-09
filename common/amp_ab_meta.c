/* 新建：common/amp_ab_meta.c */

#include <stddef.h>
#include "amp_ab_meta.h"

#define CRC_OFFSET (AMP_AB_META_BYTES - 4U)

static const uint8_t meta_magic[8] = {
    'A', 'M', 'P', 'A', 'B', '0', '0', '1'
};

static void put32(uint8_t *p, uint32_t value)
{
    unsigned int i;

    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8U));
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0;
    unsigned int i;

    for (i = 0; i < 4; ++i)
        value |= (uint32_t)p[i] << (i * 8U);

    return value;
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) |
           ((uint64_t)get32(p + 4) << 32);
}

/* CRC-32/ISO-HDLC：反射多项式、初始值和最终异或均固定。 */
static uint32_t record_crc(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];

        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  ((crc & 1U) ? UINT32_C(0xEDB88320) : 0U);
    }

    return crc ^ UINT32_MAX;
}

int amp_ab_meta_encode(const AMPABState *state,
                       uint8_t record[AMP_AB_META_BYTES])
{
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

int amp_ab_meta_decode(const uint8_t record[AMP_AB_META_BYTES],
                       AMPABState *state)
{
    AMPABState decoded;
    size_t i;

    if (record == NULL || state == NULL)
        return AMP_AB_META_INVALID;

    for (i = 0; i < sizeof(meta_magic); ++i) {
        if (record[i] != meta_magic[i])
            return AMP_AB_META_INVALID;
    }

    if (get32(record + 8) != 1U ||
        get32(record + 12) != AMP_AB_META_BYTES ||
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
                       const uint8_t copy1[AMP_AB_META_BYTES],
                       AMPABMetaSelection *selection)
{
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
            selected = states[1].generation > states[0].generation
                     ? 1U : 0U;
        }
    } else {
        selected = mask == 1U ? 0U : 1U;
    }

    selection->state = states[selected];
    selection->selected_copy = selected;
    selection->valid_mask = mask;

    return AMP_AB_OK;
}