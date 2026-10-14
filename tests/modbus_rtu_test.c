#include <assert.h> /* 引入 assert 断言，用于验证测试预期 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include <stdio.h>  /* 引入标准输入输出接口，如 printf、fprintf */
#include <string.h> /* 引入内存复制、清零及字符串处理接口 */

#include "../rtos/protocol/modbus_rtu.h"  /* 引入 Modbus RTU 编解码和 CRC16 接口 */
#include "../rtos/protocol/modbus_mock.h" /* 引入 Modbus 模拟从站接口 */

static void update_crc(uint8_t request[8])
{
    /* 1. 重新计算测试帧 CRC。 */
    /* 2. 更新帧尾校验字节，使修改后的测试输入保持 CRC 有效。 */

    uint16_t crc = amp_mb_crc16(request, 6U);

    request[6] = (uint8_t)crc;
    request[7] = (uint8_t)(crc >> 8);
}

int main(void)
{
    /* 1. 准备当前测试使用的输入、模拟状态与预期结果。 */
    /* 2. 调用被测接口，断言返回值及状态变化符合预期。 */

    AmpModbusMock mock;
    uint8_t request[8], response[256], exception;
    uint16_t registers[4];
    size_t length;

    /* 已知 Modbus 请求：01 03 00 00 00 0A C5 CD。 */
    const uint8_t known[] = {1, 3, 0, 0, 0, 10};
    assert(amp_mb_crc16(known, sizeof(known)) == 0xCDC5U);

    assert(amp_mb_mock_init(&mock, 1U) == 0);
    assert(amp_mb_build_request(1, 3, 0, 4, request) == AMP_MB_OK);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(length == 13U);
    assert(amp_mb_parse_response(request, response, length, registers, 4, &exception) == AMP_MB_OK);
    assert(registers[0] == 256U && registers[1] == 603U);
    assert(registers[2] == 1U && registers[3] == 0U);

    /* 06 写入，再使用 03 读回。 */
    assert(amp_mb_build_request(1, 6, 3, 42, request) == AMP_MB_OK);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(amp_mb_parse_response(request, response, length, NULL, 0, &exception) == AMP_MB_OK);

    assert(amp_mb_build_request(1, 3, 3, 1, request) == AMP_MB_OK);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(amp_mb_parse_response(request, response, length, registers, 4, &exception) == AMP_MB_OK);
    assert(registers[0] == 42U);

    /* 响应损坏必须拒绝。 */
    response[3] ^= 1U;
    assert(amp_mb_parse_response(request, response, length, registers, 4, &exception) ==
           AMP_MB_CRC);

    /* 请求 CRC 错误必须没有响应。 */
    request[7] ^= 1U;
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 1);
    assert(length == 0U);
    request[7] ^= 1U;

    /* 地址不匹配也没有响应。 */
    request[0] = 2U;
    update_crc(request);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 1);

    /* 合法请求超出虚拟寄存器范围：异常码 02。 */
    assert(amp_mb_build_request(1, 3, 4, 1, request) == AMP_MB_OK);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(amp_mb_parse_response(request, response, length, registers, 4, &exception) ==
           AMP_MB_EXCEPTION);
    assert(exception == 2U);

    /* 不支持的功能码：异常码 01。 */
    request[1] = 4U;
    update_crc(request);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(length == 5U && response[1] == 0x84U && response[2] == 1U);

    /* 无效数量：异常码 03。 */
    assert(amp_mb_build_request(1, 3, 0, 1, request) == AMP_MB_OK);
    request[4] = 0U;
    request[5] = 0U;
    update_crc(request);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    assert(response[2] == 3U);

    /* 主站拒绝广播和无效数量。 */
    assert(amp_mb_build_request(0, 3, 0, 1, request) == AMP_MB_ARGUMENT);
    assert(amp_mb_build_request(1, 3, 0, 126, request) == AMP_MB_ARGUMENT);
    assert(amp_mb_build_request(1, 3, 65535, 2, request) == AMP_MB_ARGUMENT);

    /* 正确 CRC 但错误的从站地址不能通过主站解析。 */
    assert(amp_mb_build_request(1, 3, 0, 1, request) == AMP_MB_OK);
    assert(amp_mb_mock_reply(&mock, request, 8, response, sizeof(response), &length) == 0);
    response[0] = 2U;
    {
        uint16_t crc = amp_mb_crc16(response, length - 2U);
        response[length - 2U] = (uint8_t)crc;
        response[length - 1U] = (uint8_t)(crc >> 8);
    }
    assert(amp_mb_parse_response(request, response, length, registers, 4, &exception) ==
           AMP_MB_FORMAT);

    puts("PASS: Modbus 03/06、写后读回、CRC、地址及异常响应");
    puts("仅软件协议验证；未访问 UART、RS485 或开发板");
    return 0;
}
