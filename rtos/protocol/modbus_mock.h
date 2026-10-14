#ifndef AMP_MODBUS_MOCK_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_MODBUS_MOCK_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* 软件 Modbus 模拟从站，提供四个保持寄存器供协议验证。 */
typedef struct {
    uint8_t slave;         /* 模拟从站地址，合法范围 1～247 */
    uint16_t registers[4]; /* 四个模拟保持寄存器的当前值 */
} AmpModbusMock;

/* 初始化模拟从站地址和寄存器内容。 */
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
int amp_mb_mock_reply(AmpModbusMock *mock, const uint8_t *request, size_t request_length,
                      uint8_t *response, size_t capacity, size_t *response_length);

#endif /* 结束 AMP_MODBUS_MOCK_H 对应的条件编译区段 */
