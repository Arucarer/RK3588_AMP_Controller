#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "FreeRTOS.h"     /* 引入 FreeRTOS 内核类型、配置及基础接口 */
#include "task.h"         /* 引入 FreeRTOS 任务、延时及调度接口 */
#include "monitor_task.h" /* 引入任务心跳、故障位及健康快照接口 */

static TickType_t last_beat[2]; /* last_beat：IPC 和控制任务最近一次心跳的 Tick 时间 */
static TickType_t last_check;   /* last_check：监控任务最近一次检查的 Tick 时间 */
static uint32_t seen;           /* seen：已收到心跳的任务位图 */
static uint32_t checks;         /* checks：健康检查累计次数 */
static uint32_t latched_faults; /* latched_faults：启动以来锁存的故障位图 */
static uint32_t led_fault;      /* led_fault：LED 驱动故障标志 */
static int initialized;         /* initialized：模块初始化完成标志 */

static uint32_t ticks_to_ms(TickType_t ticks)
{
    /* 1. 用 64 位中间值将 Tick 转为毫秒。 */
    /* 2. 超过 32 位范围时饱和到 UINT32_MAX。 */

    uint64_t value = (uint64_t)ticks * 1000U / configTICK_RATE_HZ;

    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* 调用者必须持有任务临界区。 */
static uint32_t get_faults(TickType_t now)
{
    /* 1. 检查 IPC、控制任务及监控自身的心跳时限。 */
    /* 2. 合并 LED 故障，返回当前故障位图。 */

    const TickType_t timeout = pdMS_TO_TICKS(1000);
    uint32_t faults = 0;

    if ((TickType_t)(now - last_beat[MONITOR_IPC]) > timeout)
        faults |= MONITOR_FAULT_IPC;

    if ((TickType_t)(now - last_beat[MONITOR_CONTROL]) > timeout)
        faults |= MONITOR_FAULT_CONTROL;

    if ((TickType_t)(now - last_check) > timeout)
        faults |= MONITOR_FAULT_SELF;

    if (led_fault)
        faults |= MONITOR_FAULT_LED;

    return faults;
}

void monitor_beat(unsigned int task_id)
{
    /* 1. 校验初始化状态与任务编号。 */
    /* 2. 在临界区更新时间戳和已见任务位图。 */

    TickType_t now;

    configASSERT(initialized);
    configASSERT(task_id < 2U);

    taskENTER_CRITICAL();

    now = xTaskGetTickCount();
    last_beat[task_id] = now;
    seen |= 1U << task_id;

    taskEXIT_CRITICAL();
}

void monitor_report_led_fault(void)
{
    /* 1. 检查监控已初始化。 */
    /* 2. 在临界区设置 LED 故障并锁存故障位。 */

    configASSERT(initialized);

    taskENTER_CRITICAL();
    led_fault = 1;
    latched_faults |= MONITOR_FAULT_LED;
    taskEXIT_CRITICAL();
}

void monitor_snapshot(MonitorSnapshot *snapshot)
{
    /* 1. 检查输出指针并进入临界区。 */
    /* 2. 计算当前及历史故障，复制各任务健康信息。 */
    /* 3. 退出临界区，向调用方提供一致的状态快照。 */

    TickType_t now;
    uint32_t faults;

    configASSERT(initialized);
    configASSERT(snapshot != NULL);

    taskENTER_CRITICAL();

    now = xTaskGetTickCount();
    faults = get_faults(now);
    latched_faults |= faults;

    snapshot->ready = (seen == 3U && checks != 0);
    snapshot->faults = faults;
    snapshot->latched_faults = latched_faults;
    snapshot->checks = checks;
    snapshot->ipc_age_ms = ticks_to_ms(now - last_beat[MONITOR_IPC]);
    snapshot->control_age_ms = ticks_to_ms(now - last_beat[MONITOR_CONTROL]);
    snapshot->monitor_age_ms = ticks_to_ms(now - last_check);
    snapshot->led_fault = led_fault;

    taskEXIT_CRITICAL();
}

static void monitor_task(void *argument)
{
    /* 1. 初始化周期检查时间。 */
    /* 2. 周期检查任务心跳并累计健康检查次数。 */
    /* 3. 锁存故障，延时等待下一轮检查。 */

    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t wake;

    (void)argument;

    configASSERT(period > 0);
    configASSERT(pdMS_TO_TICKS(1000) > period);

    wake = xTaskGetTickCount();

    for (;;) {
        TickType_t now;

        taskENTER_CRITICAL();

        now = xTaskGetTickCount();

        /*
         * 先检查，再刷新自己的时间：
         * 如果 MonitorTask 曾长时间未获调度，保留记录。
         */
        latched_faults |= get_faults(now);
        last_check = now;
        ++checks;

        taskEXIT_CRITICAL();

        vTaskDelayUntil(&wake, period);
    }
}

int monitor_init(void)
{
    /* 1. 初始化任务心跳及故障统计。 */
    /* 2. 创建监控任务并检查创建结果。 */

    BaseType_t result;
    TickType_t now;

    if (initialized)
        return -1;

    now = xTaskGetTickCount();

    last_beat[0] = now;
    last_beat[1] = now;
    last_check = now;
    seen = 0;
    checks = 0;
    latched_faults = 0;
    led_fault = 0;
    initialized = 1;

    result = xTaskCreate(monitor_task, "Monitor", configMINIMAL_STACK_SIZE, NULL, 4, NULL);

    if (result != pdPASS) {
        initialized = 0;
        return -1;
    }

    return 0;
}
