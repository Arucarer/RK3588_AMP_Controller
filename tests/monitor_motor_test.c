/* Real monitor logic with a fake clock and one captured task function. */
#include <setjmp.h>
#include <stdio.h>
#include "support/motor_rtos/task.h"
#include "../rtos/monitor_task.h"

static TickType_t now;
static unsigned critical_depth;
static TaskFunction_t monitor_worker;
static jmp_buf stopped;
static int fail_create;

void test_enter_critical(void) { ++critical_depth; }
void test_exit_critical(void) { assert(critical_depth); --critical_depth; }
TickType_t xTaskGetTickCount(void) { return now; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, unsigned stack,
                       void *argument, UBaseType_t priority, void *handle)
{
    (void)name; (void)argument; (void)handle;
    assert(stack == configMINIMAL_STACK_SIZE && priority == 4);
    if (fail_create) return 0;
    monitor_worker = function;
    return pdPASS;
}
void vTaskDelayUntil(TickType_t *wake, TickType_t period)
{
    (void)wake;
    assert(period == 10 && !critical_depth);
    longjmp(stopped, 1);
}
static void check_once(void)
{
    if (!setjmp(stopped)) monitor_worker(NULL);
}

int main(void)
{
    MonitorSnapshot s;
    /* Legacy IPC status payload must retain the same structure and offsets. */
    _Static_assert(sizeof(MonitorSnapshot) == 32, "monitor ABI size changed");
    _Static_assert(offsetof(MonitorSnapshot, led_fault) == 28, "monitor ABI offset changed");
    fail_create = 1;
    assert(monitor_init() == -1);
    fail_create = 0;
    assert(!monitor_init());
    assert(monitor_init() == -1);
    monitor_beat(MONITOR_IPC); monitor_beat(MONITOR_CONTROL);
    monitor_snapshot(&s);
    assert(!s.ready);
    check_once(); monitor_snapshot(&s);
    assert(s.ready && !s.faults); /* Legacy user still needs only two beats. */

    monitor_require_motor(); monitor_snapshot(&s);
    assert(!s.ready && !s.faults);
    monitor_beat(MONITOR_MOTOR); monitor_snapshot(&s);
    assert(s.ready && !s.faults);
    now = 100;
    monitor_beat(MONITOR_IPC); monitor_beat(MONITOR_CONTROL);
    check_once(); monitor_snapshot(&s);
    assert(!s.faults); /* Existing convention: exactly 1000ms is not overdue. */
    now = 101;
    monitor_require_motor(); /* Requiring again must not hide a stale beat. */
    monitor_beat(MONITOR_IPC); monitor_beat(MONITOR_CONTROL);
    check_once(); monitor_snapshot(&s);
    assert(s.faults == MONITOR_FAULT_MOTOR);
    assert(s.latched_faults & MONITOR_FAULT_MOTOR);
    monitor_beat(MONITOR_MOTOR); monitor_snapshot(&s);
    assert(!s.faults && (s.latched_faults & MONITOR_FAULT_MOTOR));

    /* Existing LED fault behavior remains unchanged. */
    monitor_report_led_fault(); monitor_snapshot(&s);
    assert(s.led_fault && (s.faults & MONITOR_FAULT_LED));
    assert(s.latched_faults & MONITOR_FAULT_LED);
    now = UINT64_MAX - 1;
    monitor_beat(MONITOR_IPC); monitor_beat(MONITOR_CONTROL); monitor_beat(MONITOR_MOTOR);
    check_once();
    now = 1;
    monitor_snapshot(&s);
    assert(!(s.faults & (MONITOR_FAULT_IPC | MONITOR_FAULT_CONTROL |
                        MONITOR_FAULT_MOTOR | MONITOR_FAULT_SELF)));
    assert(!critical_depth);
    puts("PASS: monitor legacy ABI/readiness, required MotorSIM heartbeat, timeout/latch/recovery, wrap, LED");
    return 0;
}
