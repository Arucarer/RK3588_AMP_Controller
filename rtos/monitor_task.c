#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "monitor_task.h"

static TickType_t last_beat[2];
static TickType_t last_check;
static uint32_t seen;
static uint32_t checks;
static uint32_t latched_faults;
static uint32_t led_fault;
static int initialized;

static uint32_t ticks_to_ms(TickType_t ticks)
{
    uint64_t value =
        (uint64_t)ticks * 1000U / configTICK_RATE_HZ;

    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* 调用者必须持有任务临界区。 */
static uint32_t get_faults(TickType_t now)
{
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
    configASSERT(initialized);

    taskENTER_CRITICAL();
    led_fault = 1;
    latched_faults |= MONITOR_FAULT_LED;
    taskEXIT_CRITICAL();
}

void monitor_snapshot(MonitorSnapshot *snapshot)
{
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
    snapshot->ipc_age_ms =
        ticks_to_ms(now - last_beat[MONITOR_IPC]);
    snapshot->control_age_ms =
        ticks_to_ms(now - last_beat[MONITOR_CONTROL]);
    snapshot->monitor_age_ms =
        ticks_to_ms(now - last_check);
    snapshot->led_fault = led_fault;

    taskEXIT_CRITICAL();
}

static void monitor_task(void *argument)
{
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

    result = xTaskCreate(monitor_task,
                         "Monitor",
                         configMINIMAL_STACK_SIZE,
                         NULL,
                         4,
                         NULL);

    if (result != pdPASS) {
        initialized = 0;
        return -1;
    }

    return 0;
}