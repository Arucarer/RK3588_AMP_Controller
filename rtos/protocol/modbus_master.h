/*
 * 文件名称：amp_modbus_master.h
 *
 * 功能介绍：
 * 本文件定义 Modbus RTU 主站通信模块的公共接口，
 * 用于通过 UART/RS485 与外部 Modbus 从站设备进行数据交互。
 *
 * 主要功能：
 * 1. 通信接口抽象：封装时间获取、串口接收、串口发送和空闲等待接口。
 * 2. 通信参数配置：支持波特率、字符传输时间、响应超时及重试次数配置。
 * 3. Modbus 事务管理：统一管理请求发送、响应接收和执行结果。
 * 4. 超时与异常处理：定义响应超时、总线忙、帧间隔异常等错误状态。
 * 5. 安全重试机制：区分读操作与写操作，避免重复写入设备寄存器。
 *
 * 设计特点：
 * 1. 采用硬件抽象方式，不直接依赖具体 UART 驱动。
 * 2. 使用单所有者模型，避免多个任务同时操作同一条 Modbus 总线。
 * 3. 使用单调递增的微秒时间戳判断响应超时及帧间隔。
 * 4. 支持读取保持寄存器（功能码 0x03）和写入单个寄存器（功能码 0x06）
 *    的专门重试约束。
 * 5. 不自动重试写入请求，避免通信异常时产生重复写入。
 *
 * 核心函数：
 * amp_mb_master_execute()
 *
 * 输入：
 * Modbus 通信接口、通信配置参数和 8 字节请求帧。
 *
 * 输出：
 * 返回通信执行状态，并通过 AmpMbTransaction 保存响应及事务信息。
 *
 * 适用场景：
 * RK3588 Linux + FreeRTOS AMP 异构系统中的串口设备通信。
 * 可由 FreeRTOS 侧通过 UART/RS485 与 Modbus RTU 从站进行交互。
 *
 * 注意：
 * 本文件只声明接口，不包含具体的 UART 驱动及 Modbus 协议实现。
 */
#ifndef AMP_MODBUS_MASTER_H
#define AMP_MODBUS_MASTER_H
#include <stddef.h>
#include <stdint.h>


typedef struct {
    void *context;  //保存UART驱动的上下文信息
    uint64_t (*now_us)(void *); //获取单调递增的微秒时间
    int (*poll)(void *, uint8_t *, uint64_t *);
    int (*write)(void *, const uint8_t *, size_t, uint32_t, size_t *);
    void (*idle)(void *);
} AmpMbIo;


typedef struct {
    uint32_t baud, response_timeout_us, quiet_timeout_us;
    uint8_t bits_per_character, read_retries;
} AmpMbConfig;


typedef struct {
    uint8_t response[256], exception, attempts;
    size_t length, transmitted;
} AmpMbTransaction;
#define AMP_MB_TIMEOUT -10
#define AMP_MB_IO_ERROR -11
#define AMP_MB_BUS_BUSY -12
#define AMP_MB_GAP -13
#define AMP_MB_WRITE_UNKNOWN -14
/* 06 is never automatically retried. 03 is retried only on zero-byte timeout.
 * A late response to a retried identical read cannot identify the attempt.
 * No broadcast; no automatic peripheral reset or replay of partial writes. */
int amp_mb_master_execute(const AmpMbIo *, const AmpMbConfig *,
                          const uint8_t request[8], AmpMbTransaction *);
#endif
