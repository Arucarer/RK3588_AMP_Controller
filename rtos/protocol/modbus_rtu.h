#ifndef AMP_MODBUS_RTU_H
#define AMP_MODBUS_RTU_H

#include <stddef.h>
#include <stdint.h>

#define AMP_MB_OK         0
#define AMP_MB_ARGUMENT  -1
#define AMP_MB_FORMAT    -2
#define AMP_MB_CRC       -3
#define AMP_MB_EXCEPTION -4

uint16_t amp_mb_crc16(const uint8_t *data, size_t length);

/* 生成 03 或 06 请求，固定 8 字节。禁止广播地址。 */
int amp_mb_build_request(uint8_t slave,
                         uint8_t function,
                         uint16_t address,
                         uint16_t value_or_count,
                         uint8_t output[8]);

/*
 * 校验针对指定请求的完整响应。
 *
 * 03: registers_capacity 必须足够容纳请求数量。
 * 06: 验证响应与请求前 6 字节一致。
 * 异常响应返回 AMP_MB_EXCEPTION，并设置 exception。
 *
 * CRC 正确不代表 RTU 帧间时序正确；时序由后续主站负责。
 */
int amp_mb_parse_response(const uint8_t request[8],
                          const uint8_t *response,
                          size_t length,
                          uint16_t *registers,
                          size_t registers_capacity,
                          uint8_t *exception);

#endif