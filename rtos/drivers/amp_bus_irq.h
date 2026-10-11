#ifndef AMP_BUS_IRQ_H
#define AMP_BUS_IRQ_H

#include <stdint.h>

/*
 * Handler must clear its peripheral interrupt source before returning.
 *
 * The existing FreeRTOS port owns interrupt acknowledge and EOI.
 * Handlers must not repeat those operations.
 */
typedef void (*AmpBusIrqHandler)(void *argument);

/*
 * Install/remove with local IRQs masked.
 * No GIC routing, clock enable or peripheral enable is performed here.
 *
 * Returns 0 on success, -1 on unsupported INTID or occupied slot.
 */
int amp_bus_irq_register(
    uint32_t intid,
    AmpBusIrqHandler handler,
    void *argument);

int amp_bus_irq_unregister(
    uint32_t intid,
    AmpBusIrqHandler handler);

/*
 * Returns 1 when dispatched, 0 when no handler is installed.
 */
int amp_bus_irq_dispatch(uint32_t intid);

#endif