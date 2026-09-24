#ifndef AMP_PROTOCOL_H
#define AMP_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define AMP_IPC_PHYS_ADDR  0x20FD0000ULL
#define AMP_IPC_MAP_SIZE   4096U

#define AMP_IPC_MAGIC      0x414D5031U
#define AMP_IPC_VERSION    1U

#define AMP_CMD_ADD        1U

#define AMP_STATUS_OK          0U
#define AMP_STATUS_BAD_COMMAND 1U

/*
 * One outstanding request, one Linux client.
 *
 * Linux owns request fields.
 * RTOS owns response fields and initialization.
 * Sequence fields are published last.
 */
typedef struct {
    uint32_t magic;          /* 0x00 */
    uint32_t version;        /* 0x04 */
    uint32_t reserved[14];

    uint32_t request_seq;    /* 0x40 */
    uint32_t command;        /* 0x44 */
    uint32_t arg0;           /* 0x48 */
    uint32_t arg1;           /* 0x4C */
    uint32_t request_pad[12];

    uint32_t response_seq;   /* 0x80 */
    uint32_t status;         /* 0x84 */
    uint32_t result;         /* 0x88 */
    uint32_t response_pad[13];
} AMPSharedIPC;

_Static_assert(offsetof(AMPSharedIPC, request_seq) == 0x40,
               "Invalid request layout");
_Static_assert(offsetof(AMPSharedIPC, response_seq) == 0x80,
               "Invalid response layout");
_Static_assert(sizeof(AMPSharedIPC) == 0xC0,
               "Invalid IPC structure size");

#endif