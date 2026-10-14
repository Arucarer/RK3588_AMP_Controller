#ifndef AMP_BUS_TASK_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_BUS_TASK_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#define AMP_BUS_OP_CAN_LOOPBACK 1U  /* 总线操作码：CAN 内部回环测试 */
#define AMP_BUS_OP_UART_LOOPBACK 2U /* 总线操作码：UART 内部回环测试 */
#define AMP_BUS_OP_MODBUS_MOCK 3U   /* 总线操作码：Modbus 模拟从站测试 */
#define AMP_BUS_OP_MODBUS_RTU 4U /* 总线操作码：Modbus RTU 事务，是否支持由分发逻辑决定 */
#define AMP_BUS_OP_CAN_OPEN 5U /* 总线操作码：打开 CAN，是否支持由分发逻辑决定 */
#define AMP_BUS_OP_CAN_SEND 6U /* 总线操作码：发送 CAN 帧，是否支持由分发逻辑决定 */
#define AMP_BUS_OP_CAN_RECEIVE 7U /* 总线操作码：接收 CAN 帧，是否支持由分发逻辑决定 */
#define AMP_BUS_OP_CAN_STATUS 8U /* 总线操作码：查询 CAN 状态，是否支持由分发逻辑决定 */
#define AMP_BUS_OP_CAN_RECOVER 9U /* 总线操作码：恢复 CAN，是否支持由分发逻辑决定 */

/* IPC 提交给总线任务的命令；数据字段按 operation 解释。 */
typedef struct {
    uint64_t session;   /* 请求所属的 IPC 会话编号 */
    uint32_t sequence;  /* 请求序号，用于匹配异步结果 */
    uint32_t operation; /* 总线操作码，取 AMP_BUS_OP_* 常量 */

    /* CAN: standard ID, length 0..8, data. */
    uint32_t can_id; /* 经典 CAN 标准帧标识符，合法范围为 0～0x7FF */
    uint32_t length; /* data 中的有效字节数，CAN 回环命令最多 8 字节 */
    uint8_t data[8]; /* 命令数据；UART 回环使用首字节，Modbus 模拟使用完整 8 字节请求 */

    /* UART: data[0]. Modbus mock: complete 8-byte request. */
} AmpBusCommand;

/* 总线任务的异步执行结果及收发数据。 */
typedef struct {
    uint64_t session;   /* 对应请求的 IPC 会话编号 */
    uint32_t sequence;  /* 对应请求的序号 */
    uint32_t operation; /* 对应请求的总线操作码 */
    int32_t status;     /* 总线执行状态，取 AMP_BUS_* 结果码 */

    uint32_t length;  /* data 中有效响应数据的字节数 */
    uint8_t data[16]; /* 总线测试回读数据或 Modbus 模拟响应 */

    /* CAN: actual clock/sample point, UART: actual clock. */
    uint32_t clock_hz; /* 交接记录提供的外设输入时钟，单位 Hz */
    uint32_t sample_permille; /* CAN 采样点位置，单位千分之一位周期；其他操作不使用 */
} AmpBusResult;

/*
 * 单个 IpcTask 调用 submit/receive。
 * 一次只允许一个未取回结果的请求。
 * submit: 0成功，-1参数错误或忙。
 * receive: 1有结果，0无结果，-1参数错误。
 */
int amp_bus_task_init(void);
/* 检查参数及是否已有未完成命令；将命令放入队列；成功后标记请求在途，防止覆盖未取回结果。 */
int amp_bus_submit(const AmpBusCommand *command);
/* 检查结果指针并尝试读取结果队列；取回结果后清除在途标志，允许提交下一条命令。 */
int amp_bus_receive(AmpBusResult *result);

#endif /* 结束 AMP_BUS_TASK_H 对应的条件编译区段 */
