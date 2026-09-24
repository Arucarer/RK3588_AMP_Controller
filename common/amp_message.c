#include "amp_message.h"

#define CRC_OFFSET 44U

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u64(uint8_t *p, uint64_t value)
{
    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) |
           ((uint64_t)get_u32(p + 4) << 32);
}

/*
 * CRC-32/ISO-HDLC：
 * reflected polynomial = 0xEDB88320
 * initial value = 0xFFFFFFFF
 * final XOR = 0xFFFFFFFF
 *
 * 消息 CRC 字段在计算时视为四个零字节。
 */
static uint32_t frame_crc(const uint8_t *frame, size_t length)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; ++i) {
        uint8_t value = frame[i];

        if (i >= CRC_OFFSET && i < CRC_OFFSET + 4U)
            value = 0;

        crc ^= value;

        for (bit = 0; bit < 8U; ++bit) {
            if (crc & 1U)
                crc = (crc >> 1) ^ UINT32_C(0xEDB88320);
            else
                crc >>= 1;
        }
    }

    return crc ^ UINT32_C(0xFFFFFFFF);
}

static int valid_type(uint32_t type)
{
    return type == AMP_MSG_REQUEST ||
           type == AMP_MSG_RESPONSE ||
           type == AMP_MSG_HEARTBEAT;
}

int amp_message_encode(
    uint8_t frame[AMP_MSG_FRAME_SIZE],
    const AMPMessage *message)
{
    size_t i;
    uint32_t crc;

    if (frame == NULL || message == NULL)
        return AMP_CODEC_ARGUMENT;

    if (!valid_type(message->type))
        return AMP_CODEC_FORMAT;

    if (message->payload_length > AMP_MSG_PAYLOAD_MAX)
        return AMP_CODEC_LENGTH;

    /* 清空尾部，避免发送未初始化的栈数据。 */
    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        frame[i] = 0;

    put_u32(frame + 0, AMP_MSG_MAGIC);
    put_u32(frame + 4, AMP_MSG_VERSION);
    put_u32(frame + 8, message->type);
    put_u32(frame + 12, message->payload_length);
    put_u64(frame + 16, message->session);
    put_u32(frame + 24, message->sequence);
    put_u32(frame + 28, message->opcode);
    put_u32(frame + 32, message->status);

    /* flags、reserved 和 CRC 字段已清零。 */
    for (i = 0; i < message->payload_length; ++i)
        frame[AMP_MSG_HEADER_SIZE + i] = message->payload[i];

    crc = frame_crc(frame,
                    AMP_MSG_HEADER_SIZE + message->payload_length);
    put_u32(frame + CRC_OFFSET, crc);

    return AMP_CODEC_OK;
}

int amp_message_decode(
    const uint8_t frame[AMP_MSG_FRAME_SIZE],
    AMPMessage *message)
{
    uint32_t length;
    uint32_t type;
    uint32_t crc;
    size_t i;

    if (frame == NULL || message == NULL)
        return AMP_CODEC_ARGUMENT;

    if (get_u32(frame + 0) != AMP_MSG_MAGIC)
        return AMP_CODEC_FORMAT;

    if (get_u32(frame + 4) != AMP_MSG_VERSION)
        return AMP_CODEC_VERSION;

    length = get_u32(frame + 12);

    /* 必须先检查长度，之后才能按该长度计算 CRC。 */
    if (length > AMP_MSG_PAYLOAD_MAX)
        return AMP_CODEC_LENGTH;

    crc = frame_crc(frame, AMP_MSG_HEADER_SIZE + length);

    if (crc != get_u32(frame + CRC_OFFSET))
        return AMP_CODEC_CRC;

    type = get_u32(frame + 8);

    if (!valid_type(type))
        return AMP_CODEC_FORMAT;

    if (get_u32(frame + 36) != 0 ||
        get_u32(frame + 40) != 0)
        return AMP_CODEC_FORMAT;

    /*
     * 所有格式检查通过后才更新输出。
     * 命令是否支持、负载是否符合命令要求，由分发层检查。
     */
    message->type = type;
    message->payload_length = length;
    message->session = get_u64(frame + 16);
    message->sequence = get_u32(frame + 24);
    message->opcode = get_u32(frame + 28);
    message->status = get_u32(frame + 32);

    for (i = 0; i < length; ++i)
        message->payload[i] = frame[AMP_MSG_HEADER_SIZE + i];

    for (; i < AMP_MSG_PAYLOAD_MAX; ++i)
        message->payload[i] = 0;

    return AMP_CODEC_OK;
}