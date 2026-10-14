
/*
 * AMP IPC 消息编解码模块
 * 负责整数的小端序转换、CRC32 校验及消息帧的编码与解码。
 */

#include "amp_message.h" /* 引入 IPC 消息布局、操作码及编解码接口 */

#define CRC_OFFSET 44U // CRC32 字段在消息帧中的字节偏移

/* 将 32 位整数按小端序写入字节数组 */
static void put_u32(uint8_t *p, uint32_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* 从小端序字节数组读取 32 位整数 */
static uint32_t get_u32(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 将 64 位整数按小端序写入字节数组 */
static void put_u64(uint8_t *p, uint64_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

/* 从小端序字节数组读取 64 位整数 */
static uint64_t get_u64(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/*
 * 计算消息帧 CRC32 校验值
 * 输入消息帧字节数组和本次参与校验的长度并且返回计算得到的CRC32值。
 */
static uint32_t frame_crc(const uint8_t *frame, size_t length)
{
    /* 1. 初始化 CRC 累加值。 */
    /* 2. 按协议的多项式逐字节计算。 */
    /* 3. 返回 CRC，用于编码或完整性检查。 */

    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t i;
    unsigned int bit;

    /* 1. 逐字节读取消息数据 */
    for (i = 0; i < length; ++i) {
        uint8_t value = frame[i];

        /* 2. 计算时跳过 CRC 字段原有值 */
        if (i >= CRC_OFFSET && i < CRC_OFFSET + 4U)
            value = 0;

        /* 3. 将当前字节与 CRC 寄存器异或 */
        crc ^= value;

        /* 4. 按位执行 CRC32 多项式计算 */
        for (bit = 0; bit < 8U; ++bit) {
            if (crc & 1U)
                crc = (crc >> 1) ^ UINT32_C(0xEDB88320);
            else
                crc >>= 1;
        }
    }

    /* 5. 最终异或，返回 CRC32 校验值 */
    return crc ^ UINT32_C(0xFFFFFFFF);
}

/* 检查消息类型是否为请求、响应或心跳 */
static int valid_type(uint32_t type)
{
    /* 1. 检查消息类型是否属于协议允许的范围。 */

    return type == AMP_MSG_REQUEST || type == AMP_MSG_RESPONSE || type == AMP_MSG_HEARTBEAT;
}

/* 将 AMPMessage 编码为固定 256 字节的消息帧 */
int amp_message_encode(uint8_t frame[AMP_MSG_FRAME_SIZE], const AMPMessage *message)
{
    /* 1. 检查消息类型、负载长度和输出缓冲区。 */
    /* 2. 按固定布局编码消息头与负载。 */
    /* 3. 计算并写入帧 CRC。 */

    size_t i;
    uint32_t crc;

    /* 1. 检查指针、消息类型及负载长度 */
    if (frame == NULL || message == NULL)
        return AMP_CODEC_ARGUMENT;

    if (!valid_type(message->type))
        return AMP_CODEC_FORMAT;

    if (message->payload_length > AMP_MSG_PAYLOAD_MAX)
        return AMP_CODEC_LENGTH;

    /* 2. 清空整个消息帧，避免发送未初始化数据 */
    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        frame[i] = 0;

    /* 3. 按协议偏移写入消息头字段，统一采用小端序 */
    put_u32(frame + 0, AMP_MSG_MAGIC);            // 偏移 0：消息魔数，占 4 字节
    put_u32(frame + 4, AMP_MSG_VERSION);          // 偏移 4：协议版本，占 4 字节
    put_u32(frame + 8, message->type);            // 偏移 8：消息类型，占 4 字节
    put_u32(frame + 12, message->payload_length); // 偏移 12：负载长度，占 4 字节
    put_u64(frame + 16, message->session);        // 偏移 16：会话 ID，占 8 字节
    put_u32(frame + 24, message->sequence);       // 偏移 24：请求序号，占 4 字节
    put_u32(frame + 28, message->opcode);         // 偏移 28：命令编号，占 4 字节
    put_u32(frame + 32, message->status);         // 偏移 32：执行状态，占 4 字节

    /* 4. 复制消息负载，flags、reserved 和 CRC 暂时为 0 */
    for (i = 0; i < message->payload_length; ++i)
        frame[AMP_MSG_HEADER_SIZE + i] = message->payload[i];

    /* 5. 计算 CRC32 并写入消息头 */
    crc = frame_crc(frame, AMP_MSG_HEADER_SIZE + message->payload_length);
    put_u32(frame + CRC_OFFSET, crc);

    /* 6. 编码成功 */
    return AMP_CODEC_OK;
}

/* 将 256 字节消息帧校验并解码为 AMPMessage */
int amp_message_decode(const uint8_t frame[AMP_MSG_FRAME_SIZE], AMPMessage *message)
{
    /* 1. 检查帧魔数、版本、字段和 CRC。 */
    /* 2. 解码消息头并检查负载范围。 */
    /* 3. 复制有效负载并返回解码结果。 */

    uint32_t length;
    uint32_t type;
    uint32_t crc;
    size_t i;

    /* 1. 检查输入和输出指针 */
    if (frame == NULL || message == NULL)
        return AMP_CODEC_ARGUMENT;

    /* 2. 检查消息魔数及协议版本 */
    if (get_u32(frame + 0) != AMP_MSG_MAGIC)
        return AMP_CODEC_FORMAT;

    if (get_u32(frame + 4) != AMP_MSG_VERSION)
        return AMP_CODEC_VERSION;

    /* 3. 读取并检查消息负载长度 */
    length = get_u32(frame + 12);

    if (length > AMP_MSG_PAYLOAD_MAX)
        return AMP_CODEC_LENGTH;

    /* 4. 重新计算 CRC32，并与消息中的校验值比较 */
    crc = frame_crc(frame, AMP_MSG_HEADER_SIZE + length);

    if (crc != get_u32(frame + CRC_OFFSET))
        return AMP_CODEC_CRC;

    /* 5. 检查消息类型及保留字段 */
    type = get_u32(frame + 8);

    if (!valid_type(type))
        return AMP_CODEC_FORMAT;

    if (get_u32(frame + 36) != 0 || get_u32(frame + 40) != 0)
        return AMP_CODEC_FORMAT;

    /* 6. 校验通过，解析消息头并保存到 AMPMessage */
    message->type = type;
    message->payload_length = length;
    message->session = get_u64(frame + 16);
    message->sequence = get_u32(frame + 24);
    message->opcode = get_u32(frame + 28);
    message->status = get_u32(frame + 32);

    /* 7. 复制有效负载数据 */
    for (i = 0; i < length; ++i)
        message->payload[i] = frame[AMP_MSG_HEADER_SIZE + i];

    /* 8. 清空负载区剩余空间 */
    for (; i < AMP_MSG_PAYLOAD_MAX; ++i)
        message->payload[i] = 0;

    /* 9. 解码成功 */
    return AMP_CODEC_OK;
}
