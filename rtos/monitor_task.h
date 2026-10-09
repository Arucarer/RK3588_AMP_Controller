#ifndef AMP_MONITOR_TASK_H
#define AMP_MONITOR_TASK_H

#include <stdint.h>

#define MONITOR_IPC      0U
#define MONITOR_CONTROL  1U

/* 故障位，可组合。 */
#define MONITOR_FAULT_IPC      (1U << 0)
#define MONITOR_FAULT_CONTROL  (1U << 1)
#define MONITOR_FAULT_SELF     (1U << 2)
#define MONITOR_FAULT_LED      (1U << 3)

typedef struct {
    uint32_t ready;
    uint32_t faults;
    uint32_t latched_faults;
    uint32_t checks;
    uint32_t ipc_age_ms;
    uint32_t control_age_ms;
    uint32_t monitor_age_ms;
    uint32_t led_fault;
} MonitorSnapshot;

/* 调度器启动前调用一次。 */
int monitor_init(void);

/* 以下接口只在任务上下文使用，不在 ISR 中调用。 */
void monitor_beat(unsigned int task_id);
void monitor_report_led_fault(void);
void monitor_snapshot(MonitorSnapshot *snapshot);

#endif