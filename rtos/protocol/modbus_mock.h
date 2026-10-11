#ifndef AMP_MODBUS_MOCK_H
#define AMP_MODBUS_MOCK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t slave;
    uint16_t registers[4];
} AmpModbusMock;

int amp_mb_mock_init(AmpModbusMock *mock, uint8_t slave);

/*
 * 处理完整的 8 字节请求。
 * 返回 0 表示已生成响应；返回 1 表示无响应。
 * 返回 -1 表示调用参数错误。
 *
 * 无效 CRC、错误从站地址均无响应。
 * 完整响应的输出缓冲必须至少 13 字节。
 * 仅供测试，不访问 UART/RS485。
 */
int amp_mb_mock_reply(AmpModbusMock *mock,
                       const uint8_t *request,
                       size_t request_length,
                       uint8_t *response,
                       size_t capacity,
                       size_t *response_length);

#endif
