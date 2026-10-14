#ifndef AMP_CONTROL_TASK_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_CONTROL_TASK_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* operation 字段使用的命令操作码；数值后的 U 表示无符号整数常量。 */
#define CONTROL_LED_OFF 0U   /* LED 操作码：关闭 LED */
#define CONTROL_LED_ON 1U    /* LED 操作码：使 LED 常亮 */
#define CONTROL_LED_BLINK 2U /* LED 操作码：按指定完整周期闪烁 */
#define CONTROL_LED_QUERY 3U /* LED 操作码：查询当前模式、电平和周期 */

/* status 字段使用的执行结果码，与上面的操作码分别解释。 */
#define CONTROL_OK 0U           /* 控制结果：执行成功 */
#define CONTROL_BAD_ARGUMENT 1U /* 控制结果：操作码或周期参数不合法 */
#define CONTROL_HW_ERROR 2U     /* 控制结果：LED 硬件初始化或读写失败 */

/* IPC 任务提交给 LED 控制任务的异步命令。 */
typedef struct {
    uint64_t session;   /* 请求所属的 Linux IPC 会话编号 */
    uint32_t sequence;  /* 请求序号，用于把异步结果匹配回原始命令 */
    uint32_t operation; /* LED 操作码，取值为上面的 CONTROL_LED_* */

    /* BLINK 使用：完整亮灭周期，单位 ms。 */
    uint32_t period_ms; /* 完整闪烁周期，单位 ms；非闪烁命令要求为 0 */
} ControlCommand;

/* ControlTask 执行结果结构体 */
/* LED 控制任务返回给 IPC 任务的执行结果。 */
typedef struct {
    uint64_t session;  // 会话ID，用于匹配 Linux 通信会话
    uint32_t sequence; // 请求序号，用于匹配具体命令
    uint32_t status;   // 命令执行结果，如成功、参数错误、硬件错误

    uint32_t mode;      // LED 工作模式：关闭、常亮、闪烁
    uint32_t output;    // LED 当前输出状态
    uint32_t period_ms; // LED 闪烁周期，单位 ms
} ControlResult;

/* 创建队列和任务。成功返回 0，失败返回 -1。 */
int control_init(void);

/*
 * 仅供单个 IpcTask 调用。
 * 同时只允许一条未取回结果的控制命令。
 *
 * submit：0 已入队，-1 参数错误或忙。
 * receive：1 已取回，0 暂无结果，-1 参数错误。
 */
int control_submit(const ControlCommand *command);
/* 检查结果指针并尝试读取结果队列；取回结果后清除在途标志，允许提交下一条命令。 */
int control_receive(ControlResult *result);

#endif /* 结束 AMP_CONTROL_TASK_H 对应的条件编译区段 */
