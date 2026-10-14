#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "modbus_rtu.h" /* 引入 Modbus RTU 编解码和 CRC16 接口 */

uint16_t amp_mb_crc16(const uint8_t *data, size_t length)
{
    /* 1. 初始化 CRC 累加值。 */
    /* 2. 按协议的多项式逐字节计算。 */
    /* 3. 返回 CRC，用于编码或完整性检查。 */

    uint16_t crc = 0xFFFFU;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8U; ++bit)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
    }

    return crc;
}

static uint16_t get_be16(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static int crc_valid(const uint8_t *data, size_t length)
{
    /* 1. 检查帧长度是否能够容纳 CRC。 */
    /* 2. 计算并比较帧尾 CRC，返回校验结果。 */

    uint16_t expected;

    if (length < 2U)
        return 0;

    expected = (uint16_t)data[length - 2U] | (uint16_t)((uint16_t)data[length - 1U] << 8);

    return amp_mb_crc16(data, length - 2U) == expected;
}

int amp_mb_build_request(uint8_t slave, uint8_t function, uint16_t address, uint16_t value_or_count,
                         uint8_t output[8])
{
    /* 1. 校验功能码、寄存器数量及输出容量。 */
    /* 2. 编码从站地址与请求字段。 */
    /* 3. 追加 CRC 并返回实际帧长度。 */

    uint16_t crc;

    if (output == NULL || slave == 0U || slave > 247U || (function != 3U && function != 6U))
        return AMP_MB_ARGUMENT;

    if (function == 3U && (value_or_count == 0U || value_or_count > 125U ||
                           (uint32_t)address + value_or_count > 65536U))
        return AMP_MB_ARGUMENT;

    output[0] = slave;
    output[1] = function;
    put_be16(output + 2, address);
    put_be16(output + 4, value_or_count);

    crc = amp_mb_crc16(output, 6U);
    output[6] = (uint8_t)crc;
    output[7] = (uint8_t)(crc >> 8);

    return AMP_MB_OK;
}

int amp_mb_parse_response(const uint8_t request[8], const uint8_t *response, size_t length,
                          uint16_t *registers, size_t registers_capacity, uint8_t *exception)
{
    /* 1. 检查长度、CRC、地址和功能码。 */
    /* 2. 区分异常响应与正常响应。 */
    /* 3. 解码寄存器数据或写入确认信息。 */

    uint16_t count;
    size_t i;

    if (request == NULL || response == NULL || exception == NULL)
        return AMP_MB_ARGUMENT;

    *exception = 0U;

    if (request[0] == 0U || request[0] > 247U || (request[1] != 3U && request[1] != 6U) ||
        !crc_valid(request, 8U))
        return AMP_MB_ARGUMENT;

    if (request[1] == 3U) {
        count = get_be16(request + 4);
        if (count == 0U || count > 125U || (uint32_t)get_be16(request + 2) + count > 65536U)
            return AMP_MB_ARGUMENT;
    }

    if (length < 5U || length > 256U)
        return AMP_MB_FORMAT;

    if (!crc_valid(response, length))
        return AMP_MB_CRC;

    if (response[0] != request[0])
        return AMP_MB_FORMAT;

    if (response[1] == (uint8_t)(request[1] | 0x80U)) {
        if (length != 5U || response[2] == 0U)
            return AMP_MB_FORMAT;
        *exception = response[2];
        return AMP_MB_EXCEPTION;
    }

    if (response[1] != request[1])
        return AMP_MB_FORMAT;

    if (request[1] == 6U) {
        if (length != 8U)
            return AMP_MB_FORMAT;
        for (i = 0; i < 6U; ++i)
            if (response[i] != request[i])
                return AMP_MB_FORMAT;
        return AMP_MB_OK;
    }

    count = get_be16(request + 4);

    if (length != 5U + (size_t)count * 2U || response[2] != (uint8_t)(count * 2U))
        return AMP_MB_FORMAT;

    if (registers == NULL || registers_capacity < count)
        return AMP_MB_ARGUMENT;

    for (i = 0; i < count; ++i)
        registers[i] = get_be16(response + 3U + i * 2U);

    return AMP_MB_OK;
}
