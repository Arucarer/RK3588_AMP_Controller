/* Deterministic FreeRTOS API double. Tests real worker loop, not scheduling. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "support/motor_rtos/task.h"
#include "support/motor_rtos/queue.h"
#include "../rtos/motor/motor_task.h"

struct TestQueue { size_t size; int full; unsigned char bytes[512]; };
static struct TestQueue *queues[3];
static unsigned allocated, create_calls, fail_queue_at;
static int fail_task;
static TaskFunction_t worker;
static TickType_t tick;
static jmp_buf done;
static unsigned rounds, stop_after;
static TickType_t extra_gap;

QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    struct TestQueue *q;
    assert(count == 1 && size <= 512);
    if (++create_calls == fail_queue_at) return NULL;
    q = calloc(1, sizeof(*q)); assert(q);
    q->size = size;
    assert(allocated < 3); queues[allocated++] = q;
    return q;
}
void vQueueDelete(QueueHandle_t q)
{
    unsigned i;
    for (i = 0; i < allocated; ++i) if (queues[i] == q) {
        queues[i] = queues[--allocated]; free(q); return;
    }
    assert(0);
}
BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t wait)
{
    assert(!wait);
    if (q->full) return 0;
    memcpy(q->bytes, p, q->size); q->full = 1; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t wait)
{
    if (!xQueuePeek(q, p, wait)) return 0;
    q->full = 0; return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t q, void *p, TickType_t wait)
{
    assert(!wait);
    if (!q->full) return 0;
    memcpy(p, q->bytes, q->size); return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *p)
{
    memcpy(q->bytes, p, q->size); q->full = 1; return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t f, const char *name, unsigned stack,
                       void *arg, UBaseType_t priority, void *handle)
{
    (void)name; (void)arg; (void)handle;
    assert(stack == 1024 && priority == 2);
    if (fail_task) return 0;
    worker = f; return pdPASS;
}
TickType_t xTaskGetTickCount(void) { return tick; }
void vTaskDelayUntil(TickType_t *wake, TickType_t period)
{
    if (rounds++ >= stop_after) longjmp(done, 1);
    *wake += period;
    tick = *wake + extra_gap;
    extra_gap = 0;
}
static void run(unsigned count)
{
    rounds = 0; stop_after = count;
    if (!setjmp(done)) worker(NULL);
}
static AmpMotorReply command(AmpMotorCommand operation, int32_t value)
{
    AmpMotorRequest request = {0};
    AmpMotorReply reply;
    request.session = 42; request.sequence = 7;
    request.kind = AMP_MOTOR_REQUEST_CONTROL;
    request.command = operation; request.value = value;
    assert(!amp_motor_task_submit(&request));
    assert(amp_motor_task_submit(&request) == -1);
    run(1);
    /* Having a reply queued does not release the single outstanding slot. */
    assert(amp_motor_task_submit(&request) == -1);
    assert(amp_motor_task_receive(&reply) == 1);
    assert(reply.session == 42 && reply.sequence == 7 && reply.snapshot.simulation == 1);
    assert(amp_motor_task_receive(&reply) == 0);
    return reply;
}
int main(void)
{
    AmpMotorSnapshot s, again;
    AmpMotorReply reply;
    unsigned i;
    assert(amp_motor_task_snapshot(&s) == 0);
    assert(amp_motor_task_receive(NULL) == -1);
    for (i = 1; i <= 3; ++i) {
        create_calls = 0; fail_queue_at = i;
        assert(amp_motor_task_init() == -1 && !allocated && !worker);
    }
    fail_queue_at = 0; fail_task = 1;
    assert(amp_motor_task_init() == -1 && !allocated && !worker);
    fail_task = 0;
    assert(!amp_motor_task_init());
    assert(amp_motor_task_init() == -1);
    assert(amp_motor_task_snapshot(&s) == 1 && s.state == AMP_MOTOR_DISABLED);
    assert(amp_motor_task_snapshot(&again) == 1 && again.cycles == s.cycles);
    assert(command(AMP_MOTOR_ENABLE, 0).status == 0);
    assert(command(AMP_MOTOR_TARGET, 500).status == 0);
    assert(command(AMP_MOTOR_START, 0).status == 0);
    run(20);
    assert(amp_motor_task_snapshot(&s) == 1 && s.state == AMP_MOTOR_RUNNING);
    assert(s.demand_rpm == 200 && s.feedback.rpm > 0);
    reply = command(AMP_MOTOR_TARGET, 4000);
    assert(reply.status == -1 && reply.snapshot.target_rpm == 500);
    run(100);
    assert(amp_motor_task_snapshot(&s) == 1 && s.state == AMP_MOTOR_FAULT);
    assert(s.faults & AMP_MOTOR_FAULT_LEASE);
    run(100);
    assert(command(AMP_MOTOR_RESET_FAULT, 0).status == 0);
    assert(command(AMP_MOTOR_ENABLE, 0).status == 0);
    assert(command(AMP_MOTOR_START, 0).status == 0);
    extra_gap = 11; /* 120ms elapsed: do not hide it behind queued heartbeat. */
    reply = command(AMP_MOTOR_HEARTBEAT, 0);
    assert(reply.status == -2 && (reply.snapshot.faults & AMP_MOTOR_FAULT_PERIOD));
    assert(!reply.snapshot.feedback.valid && reply.snapshot.overruns > 0);
    run(1);
    assert(command(AMP_MOTOR_RESET_FAULT, 0).status == 0);
    {
        AmpMotorRequest request = {0};
        request.kind = AMP_MOTOR_REQUEST_INJECT;
        request.load_permille = 1001;
        assert(amp_motor_task_submit(&request) == -1);
        request.load_permille = 0;
        request.injections = AMP_MOTOR_SIM_OVERTEMPERATURE;
        assert(!amp_motor_task_submit(&request));
        run(1);
        assert(amp_motor_task_receive(&reply) == 1 && !reply.status);
        /* Injection is a SIM input; its feedback is published on next step. */
        run(1);
        assert(amp_motor_task_snapshot(&s) == 1 && s.feedback.temperature_decic == 1050);
        assert(command(AMP_MOTOR_ENABLE, 0).status == -2);
        request.injections = 0;
        assert(!amp_motor_task_submit(&request));
        run(1);
        assert(amp_motor_task_receive(&reply) == 1 && !reply.status);
        run(1);
        assert(command(AMP_MOTOR_ENABLE, 0).status == 0);
        assert(command(AMP_MOTOR_DISABLE, 0).status == 0);
    }
    /* Snapshot still updates across the unsigned tick wrap. */
    tick = UINT64_MAX;
    run(1);
    assert(amp_motor_task_snapshot(&s) == 1 && s.tick == 0);
    while (allocated) vQueueDelete(queues[0]); /* Process exit: fake worker is stopped. */
    puts("PASS: SIM task initialization/cleanup, queues, replies, snapshots, lease, timing fault, tick wrap");
    return 0;
}
