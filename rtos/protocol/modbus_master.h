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
#ifndef AMP_MODBUS_MASTER_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_MODBUS_MASTER_H /* 定义头文件保护标记，避免本文件被重复包含 */
#include <stddef.h>         /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h>         /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* Modbus 主站的底层 I/O 回调，由调用方适配 UART 或模拟设备。 */
typedef struct {
    void *context;              //保存UART驱动的上下文信息
    uint64_t (*now_us)(void *); //获取单调递增的微秒时间
    /* 返回 1 表示收到字节，0 表示暂无数据，负值表示错误；输出字节和 us 时间戳。 */
    int (*poll)(void *, uint8_t *, uint64_t *); /* 非阻塞接收 */
    /* 参数依次为上下文、数据、长度、us 超时预算及已发送字节数输出；返回 0 表示成功。 */
    int (*write)(void *, const uint8_t *, size_t, uint32_t, size_t *); /* 发送 */
    void (*idle)(void *); /* 等待回调，用于轮询空闲时让出执行机会 */
} AmpMbIo;

/* Modbus 主站的串口时序、超时和读取重试配置。 */
typedef struct {
    uint32_t baud;                /* 串口波特率，单位 bit/s */
    uint32_t response_timeout_us; /* 每次发送及响应等待使用的超时预算，单位 us */
    uint32_t quiet_timeout_us;    /* 发送前等待总线静默的超时上限，单位 us */
    uint8_t bits_per_character; /* 每字符的总位数，包含起始位、数据位、校验位和停止位 */
    uint8_t read_retries; /* 零字节超时后允许的读请求重试次数，当前限制为 0 或 1；写请求不重试 */
} AmpMbConfig;

/* 一次 Modbus 主站事务的响应缓冲区和执行统计。 */
typedef struct {
    uint8_t response[256]; /* 接收响应帧的缓冲区 */
    uint8_t exception;     /* 从站异常响应码，无异常时为 0 */
    uint8_t attempts;      /* 本事务累计发送尝试次数，包含首次发送 */
    size_t length;         /* 当前响应缓冲区中的有效字节数 */
    size_t transmitted;    /* 最近一次发送尝试实际发出的请求字节数 */
} AmpMbTransaction;
#define AMP_MB_TIMEOUT -10  /* Modbus 主站结果：等待响应超时 */
#define AMP_MB_IO_ERROR -11 /* Modbus 主站结果：底层收发接口失败 */
#define AMP_MB_BUS_BUSY -12 /* Modbus 主站结果：等待总线静默超时 */
#define AMP_MB_GAP -13      /* Modbus 主站结果：接收帧中出现不允许的字节间隔 */
#define AMP_MB_WRITE_UNKNOWN -14 /* Modbus 主站结果：写入是否已执行无法确定，不应盲目重试 */
/* 06 is never automatically retried. 03 is retried only on zero-byte timeout.
 * A late response to a retried identical read cannot identify the attempt.
 * No broadcast; no automatic peripheral reset or replay of partial writes. */
int amp_mb_master_execute(const AmpMbIo *, const AmpMbConfig *, const uint8_t request[8],
                          AmpMbTransaction *);
#endif /* 结束 AMP_MODBUS_MASTER_H 对应的条件编译区段 */
