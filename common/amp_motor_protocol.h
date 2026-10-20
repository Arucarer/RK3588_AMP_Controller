#ifndef AMP_MOTOR_PROTOCOL_H
#define AMP_MOTOR_PROTOCOL_H
#include <stdint.h>

#define AMP_OP_MOTOR_STATUS 0x40U
#define AMP_OP_MOTOR_EXEC 0x41U
#define AMP_MOTOR_WIRE_VERSION 1U
#define AMP_MOTOR_WIRE_REQUEST_SIZE 32U
#define AMP_MOTOR_WIRE_STATUS_SIZE 64U

/* Wire operation values are independent of the internal C enum. */
enum {
    AMP_MOTOR_WIRE_ENABLE = 1, AMP_MOTOR_WIRE_DISABLE,
    AMP_MOTOR_WIRE_START, AMP_MOTOR_WIRE_STOP, AMP_MOTOR_WIRE_TARGET,
    AMP_MOTOR_WIRE_HEARTBEAT, AMP_MOTOR_WIRE_RESET, AMP_MOTOR_WIRE_INJECT
};

/* EXEC request: eight little-endian u32 words:
 * version, operation, signed target_rpm, load_permille, injections, reserved=0, controller token low, token high.
 * Only TARGET uses target; only INJECT uses load/injections; others must be 0.
 * Nonzero controller token is independent of the short-lived IPC session.
 * It identifies cooperative ownership, not authentication or authorization.
 * STATUS request has zero bytes. Both responses use the layout below.
 * All modes are SIM=1; status means command acceptance, not motion completion.
 * Response words (64 bytes): version, mode, state, faults, signed result,
 * signed target, signed demand, signed feedback rpm/current/temp(decic),
 * feedback age(ms), valid, cycles, overruns, tick low, tick high.
 * State: 0 disabled, 1 ready, 2 running, 3 stopping, 4 fault.
 * Result: 0 accepted, -1 bad argument, -2 rejected state/feedback.
 * No structure memcpy is permitted on the wire. */
static inline uint32_t amp_motor_wire_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void amp_motor_wire_put32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline int32_t amp_motor_wire_geti32(const uint8_t *p)
{
    uint32_t v=amp_motor_wire_get32(p);
    return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX-v);
}
#endif
