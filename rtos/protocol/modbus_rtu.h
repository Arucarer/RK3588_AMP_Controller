#ifndef AMP_MODBUS_RTU_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_MODBUS_RTU_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#define AMP_MB_OK 0         /* Modbus 处理结果：成功 */
#define AMP_MB_ARGUMENT -1  /* Modbus 处理结果：参数或调用配置无效 */
#define AMP_MB_FORMAT -2    /* Modbus 处理结果：响应帧格式或字段不符合请求 */
#define AMP_MB_CRC -3       /* Modbus 处理结果：CRC16 校验失败 */
#define AMP_MB_EXCEPTION -4 /* Modbus 处理结果：从站返回异常响应 */

/* 初始化 CRC 累加值；按协议的多项式逐字节计算；返回 CRC，用于编码或完整性检查。 */
uint16_t amp_mb_crc16(const uint8_t *data, size_t length);

/* 生成 03 或 06 请求，固定 8 字节。禁止广播地址。 */
int amp_mb_build_request(uint8_t slave, uint8_t function, uint16_t address, uint16_t value_or_count,
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
int amp_mb_parse_response(const uint8_t request[8], const uint8_t *response, size_t length,
                          uint16_t *registers, size_t registers_capacity, uint8_t *exception);

#endif /* 结束 AMP_MODBUS_RTU_H 对应的条件编译区段 */
