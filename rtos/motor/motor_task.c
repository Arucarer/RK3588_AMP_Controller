#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "motor_task.h"

#if configTICK_RATE_HZ != 100
#error Motor_SIM_task_currently_validated_only_with_100_Hz_tick
#endif

static QueueHandle_t commands, replies, snapshots;
static int outstanding;
static AmpMotorControl control;
static AmpMotorSim simulation;
static uint32_t cycles, overruns;

/* Behavioural test defaults, not real motor ratings or FOC parameters. */
static const AmpMotorConfig control_config = {
    3000, 1000, 2000, 5000, 800, 100, 1000, 5
};
static const AmpMotorSimConfig sim_config = {
    3000, 250, 100, 1000, 100, 6000, 800
};

static AmpMotorSnapshot snapshot_now(TickType_t tick)
{
    AmpMotorSnapshot s = {0};
    s.simulation = 1;
    s.state = control.state;
    s.faults = control.faults;
    s.target_rpm = control.target_rpm;
    s.demand_rpm = amp_motor_demand_rpm(&control);
    (void)amp_motor_sim_feedback(&simulation, &s.feedback);
    s.tick = (uint64_t)tick;
    s.cycles = cycles;
    s.overruns = overruns;
    return s;
}

static int execute(const AmpMotorRequest *r)
{
    AmpMotorFeedback f;
    if (r->kind == AMP_MOTOR_REQUEST_INJECT)
        return amp_motor_sim_inject(&simulation, r->load_permille, r->injections);
    (void)amp_motor_sim_feedback(&simulation, &f);
    return amp_motor_command(&control, r->command, r->value, &f);
}

static void motor_task(void *argument)
{
    const TickType_t period = pdMS_TO_TICKS(10);
    TickType_t last = xTaskGetTickCount(), wake = last;
    (void)argument;
    configASSERT(period != 0);
    for (;;) {
        TickType_t now, elapsed;
        uint64_t elapsed_ms;
        AmpMotorFeedback f;
        AmpMotorSnapshot snapshot;
        AmpMotorRequest request;
        vTaskDelayUntil(&wake, period);
        now = xTaskGetTickCount();
        elapsed = now - last; /* TickType_t unsigned wrap is intentional. */
        if (!elapsed) continue;
        last = now;
        if (elapsed > period) {
            ++overruns;
            wake = now; /* No burst of catch-up control steps. */
        }
        /* Clamp before conversion; long gaps must not wrap into short periods. */
        elapsed_ms = elapsed > pdMS_TO_TICKS(100) ? 101 :
            (uint64_t)elapsed * 1000 / configTICK_RATE_HZ;
        if (!elapsed_ms) continue;
        (void)amp_motor_sim_feedback(&simulation, &f);
        if (elapsed_ms > 100) {
            /* Fail an enabled controller, freeze SIM and invalidate feedback.
             * Do not hide a scheduling gap by performing many healthy steps. */
            (void)amp_motor_step(&control, 101, &f);
            (void)amp_motor_sim_invalidate(&simulation);
        } else {
            (void)amp_motor_step(&control, (uint32_t)elapsed_ms, &f);
            (void)amp_motor_sim_step(&simulation, (uint32_t)elapsed_ms,
                amp_motor_demand_rpm(&control),
                control.state == AMP_MOTOR_RUNNING || control.state == AMP_MOTOR_STOPPING);
        }
        ++cycles;
        /* Process queued commands after elapsed-time checks, so a late queued
         * heartbeat cannot erase a lease expiry or timing fault. */
        if (xQueueReceive(commands, &request, 0) == pdTRUE) {
            AmpMotorReply reply = {0};
            BaseType_t sent;
            reply.session = request.session;
            reply.sequence = request.sequence;
            reply.status = execute(&request);
            reply.snapshot = snapshot_now(now);
            sent = xQueueSend(replies, &reply, 0);
            configASSERT(sent == pdTRUE);
        }
        snapshot = snapshot_now(now);
        {
            BaseType_t sent = xQueueOverwrite(snapshots, &snapshot);
            configASSERT(sent == pdTRUE);
        }
    }
}

int amp_motor_task_init(void)
{
    AmpMotorSnapshot initial;
    if (commands || replies || snapshots) return -1;
    if (amp_motor_init(&control, &control_config) ||
        amp_motor_sim_init(&simulation, &sim_config)) return -1;
    cycles = overruns = 0;
    outstanding = 0;
    commands = xQueueCreate(1, sizeof(AmpMotorRequest));
    replies = xQueueCreate(1, sizeof(AmpMotorReply));
    snapshots = xQueueCreate(1, sizeof(AmpMotorSnapshot));
    if (!commands || !replies || !snapshots) goto fail;
    initial = snapshot_now(xTaskGetTickCount());
    if (xQueueOverwrite(snapshots, &initial) != pdTRUE) goto fail;
    /* 1024 StackType_t entries = 8 KiB on the current AArch64 port. */
    if (xTaskCreate(motor_task, "MotorSIM", 1024, NULL, 2, NULL) != pdPASS)
        goto fail;
    return 0;
fail:
    if (commands) vQueueDelete(commands);
    if (replies) vQueueDelete(replies);
    if (snapshots) vQueueDelete(snapshots);
    commands = replies = snapshots = NULL;
    return -1;
}

int amp_motor_task_submit(const AmpMotorRequest *r)
{
    if (!r || !commands || !replies || outstanding ||
        (r->kind != AMP_MOTOR_REQUEST_CONTROL && r->kind != AMP_MOTOR_REQUEST_INJECT))
        return -1;
    if (r->kind == AMP_MOTOR_REQUEST_CONTROL &&
        (r->command < AMP_MOTOR_ENABLE || r->command > AMP_MOTOR_RESET_FAULT))
        return -1;
    if (r->kind == AMP_MOTOR_REQUEST_INJECT &&
        (r->load_permille > 1000 || (r->injections & ~7U))) return -1;
    /* Single producer sets the marker before publishing to the worker. */
    outstanding = 1;
    if (xQueueSend(commands, r, 0) != pdTRUE) {
        outstanding = 0;
        return -1;
    }
    return 0;
}

int amp_motor_task_receive(AmpMotorReply *reply)
{
    if (!reply) return -1;
    if (!replies || !outstanding || xQueueReceive(replies, reply, 0) != pdTRUE)
        return 0;
    outstanding = 0;
    return 1;
}

int amp_motor_task_snapshot(AmpMotorSnapshot *snapshot)
{
    if (!snapshot) return -1;
    if (!snapshots) return 0;
    return xQueuePeek(snapshots, snapshot, 0) == pdTRUE ? 1 : 0;
}
