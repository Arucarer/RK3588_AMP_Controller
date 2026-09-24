#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shared_memory.h"
#include "../common/amp_protocol.h"

static volatile AMPSharedIPC *const ipc =
    (volatile AMPSharedIPC *)(uintptr_t)AMP_IPC_PHYS_ADDR;

static inline void ipc_barrier(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

void amp_ipc_init(void)
{
    /*
     * This initial protocol assumes RTOS data caching is disabled.
     * Barriers alone do not make non-coherent cached memory safe.
     */
    uint64_t sctlr;

    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
    configASSERT((sctlr & (1ULL << 2)) == 0);

    ipc->magic = 0;
    ipc_barrier();

    ipc->version = AMP_IPC_VERSION;

    ipc->request_seq = 0;
    ipc->command = 0;
    ipc->arg0 = 0;
    ipc->arg1 = 0;

    ipc->response_seq = 0;
    ipc->status = AMP_STATUS_OK;
    ipc->result = 0;

    ipc_barrier();
    ipc->magic = AMP_IPC_MAGIC;
    ipc_barrier();
}

void amp_ipc_task(void *argument)
{
    uint32_t last_seq = 0;

    (void)argument;

    for (;;) {
        uint32_t seq = ipc->request_seq;

        if (seq != last_seq) {
            uint32_t command;
            uint32_t arg0;
            uint32_t arg1;
            uint32_t status;
            uint32_t result;

            /*
             * Observe the published sequence before reading payload.
             * Linux must not modify the request until it receives a reply.
             */
            ipc_barrier();

            command = ipc->command;
            arg0 = ipc->arg0;
            arg1 = ipc->arg1;

            if (command == AMP_CMD_ADD) {
                status = AMP_STATUS_OK;
                result = arg0 + arg1;
            } else {
                status = AMP_STATUS_BAD_COMMAND;
                result = 0;
            }

            ipc->status = status;
            ipc->result = result;

            ipc_barrier();
            ipc->response_seq = seq;
            ipc_barrier();

            last_seq = seq;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}