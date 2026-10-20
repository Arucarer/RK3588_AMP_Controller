#ifndef AMP_MONITOR_TASK_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_MONITOR_TASK_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#define MONITOR_IPC 0U     /* IPC 任务在心跳数组中的索引 */
#define MONITOR_CONTROL 1U /* 控制任务在心跳数组中的索引 */
#define MONITOR_MOTOR 2U   /* MotorSIM 任务心跳，不代表真实电机健康 */

/* 故障位，可组合。 */
#define MONITOR_FAULT_IPC (1U << 0)     /* 故障位 0：IPC 任务心跳超时 */
#define MONITOR_FAULT_CONTROL (1U << 1) /* 故障位 1：控制任务心跳超时 */
#define MONITOR_FAULT_SELF (1U << 2)    /* 故障位 2：监控任务自身检查超时 */
#define MONITOR_FAULT_LED (1U << 3)     /* 故障位 3：LED 驱动异常 */
#define MONITOR_FAULT_MOTOR (1U << 4)   /* MotorSIM 任务心跳超时 */

/* 在临界区采集的任务健康快照，用于 IPC 状态上报。 */
typedef struct {
    uint32_t ready;          /* 所有必需任务均上报过心跳且监控已检查时为 1 */
    uint32_t faults;         /* 当前故障位图，按 MONITOR_FAULT_* 掩码解释 */
    uint32_t latched_faults; /* 启动以来锁存的历史故障位图 */
    uint32_t checks;         /* 监控任务累计完成的健康检查次数 */
    uint32_t ipc_age_ms;     /* 距离 IPC 任务最近一次心跳的时间，单位 ms */
    uint32_t control_age_ms; /* 距离控制任务最近一次心跳的时间，单位 ms */
    uint32_t monitor_age_ms; /* 距离监控任务最近一次检查的时间，单位 ms */
    uint32_t led_fault;      /* LED 驱动故障标志，非零表示已报告故障 */
} MonitorSnapshot;

/* 调度器启动前调用一次。 */
int monitor_init(void);
/* Call before scheduler start when MotorSIM is created. Idempotent; legacy
 * two-task users need not call it. Does not change MonitorSnapshot wire size. */
void monitor_require_motor(void);

/* 以下接口只在任务上下文使用，不在 ISR 中调用。 */
void monitor_beat(unsigned int task_id);
/* 检查监控已初始化；在临界区设置 LED 故障并锁存故障位。 */
void monitor_report_led_fault(void);
/* 检查输出指针并进入临界区；计算当前及历史故障，复制各任务健康信息；退出临界区，向调用方提供一致的状态快照。 */
void monitor_snapshot(MonitorSnapshot *snapshot);

#endif /* 结束 AMP_MONITOR_TASK_H 对应的条件编译区段 */
