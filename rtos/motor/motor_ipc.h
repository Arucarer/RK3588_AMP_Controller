#ifndef AMP_MOTOR_IPC_H
#define AMP_MOTOR_IPC_H
#include "../../common/amp_message.h"

/* Single caller: existing IpcTask, after its session/sequence/cache validation.
 * init is startup-only. New HELLO must not reset the motor owner or lease.
 * handle returns 1 if asynchronous, 0 for immediate response.
 * collect returns 1 with original request and reply, otherwise 0. */
void amp_motor_ipc_init(void);
int amp_motor_ipc_handle(const AMPMessage *request, AMPMessage *response);
int amp_motor_ipc_pending(void);
int amp_motor_ipc_collect(AMPMessage *original, AMPMessage *response);
#endif
