#ifndef AMP_BUS_HANDOFF_H
#define AMP_BUS_HANDOFF_H

#include <stdint.h>

#ifndef AMP_BUS_HW_ACTIVATE
#define AMP_BUS_HW_ACTIVATE 1
#endif

#define AMP_BUS_HANDOFF_ADDR  UINT64_C(0x20FE1000)
#define AMP_BUS_HANDOFF_MAGIC UINT32_C(0x42555331)

#define AMP_BUS_CAN1  0U
#define AMP_BUS_UART3 1U

#define AMP_BUS_OK           0
#define AMP_BUS_ARGUMENT    -1
#define AMP_BUS_NOT_READY   -2
#define AMP_BUS_TIMEOUT     -3
#define AMP_BUS_HW_ERROR    -4
#define AMP_BUS_UNSUPPORTED -5

void amp_bus_handoff_clear(void);

int amp_bus_platform_ready(uint32_t bus);
int amp_bus_require_ready(uint32_t bus);

uint32_t amp_bus_clock_hz(uint32_t bus);

uint32_t amp_bus_read32(uint64_t address);
void amp_bus_write32(uint64_t address, uint32_t value);

typedef struct {
    uint64_t start;
    uint64_t ticks;
} AmpBusDeadline;

int amp_bus_deadline_start(AmpBusDeadline *deadline,
                           uint32_t timeout_us);
int amp_bus_deadline_expired(const AmpBusDeadline *deadline);

#endif
