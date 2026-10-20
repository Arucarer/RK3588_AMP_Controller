#ifndef AMP_MOTOR_TASK_H
#define AMP_MOTOR_TASK_H

#include "motor_sim.h"

/* SIM only; created by main and controlled through the motor IPC bridge. */
enum { AMP_MOTOR_REQUEST_CONTROL, AMP_MOTOR_REQUEST_INJECT };

typedef struct {
    uint64_t session;
    uint32_t sequence, kind;
    AmpMotorCommand command;
    int32_t value;
    uint32_t load_permille, injections;
} AmpMotorRequest;

typedef struct {
    uint32_t simulation; /* Always 1; never describes physical hardware. */
    AmpMotorState state;
    uint32_t faults;
    int32_t target_rpm, demand_rpm;
    AmpMotorFeedback feedback;
    uint64_t tick;
    uint32_t cycles, overruns;
} AmpMotorSnapshot;

typedef struct {
    uint64_t session;
    uint32_t sequence;
    int32_t status;
    AmpMotorSnapshot snapshot; /* Command acceptance, not motion completion. */
} AmpMotorReply;

/* Create before scheduler start. No reinitialization while task is alive.
 * Single client owns submit/receive, task context only, never call from ISR.
 * One request remains outstanding until its reply is consumed; no queue waits.
 * Snapshot queries may be made independently by tasks.
 * submit: 0 accepted, -1 invalid/busy/uninitialized.
 * receive/snapshot: 1 available, 0 unavailable, -1 invalid pointer.
 * IPC bridge owns control-token checks; IpcTask owns sessions/duplicate filtering.
 */
int amp_motor_task_init(void);
int amp_motor_task_submit(const AmpMotorRequest *request);
int amp_motor_task_receive(AmpMotorReply *reply);
int amp_motor_task_snapshot(AmpMotorSnapshot *snapshot);

#endif
