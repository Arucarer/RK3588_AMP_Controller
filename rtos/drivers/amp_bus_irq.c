#include <stddef.h>
#include <stdint.h>

#include "amp_bus_resources.h"
#include "amp_bus_irq.h"

typedef struct {
    AmpBusIrqHandler handler;
    void *argument;
} AmpBusIrqSlot;

static AmpBusIrqSlot can1_slot;
static AmpBusIrqSlot uart3_slot;

static AmpBusIrqSlot *find_slot(uint32_t intid)
{
    if (intid == AMP_CAN1_INTID)
        return &can1_slot;

    if (intid == AMP_UART3_INTID)
        return &uart3_slot;

    return NULL;
}

static int local_irqs_masked(void)
{
    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (UINT64_C(1) << 7)) != 0;
}

int amp_bus_irq_register(
    uint32_t intid,
    AmpBusIrqHandler handler,
    void *argument)
{
    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || handler == NULL ||
        !local_irqs_masked() || slot->handler != NULL)
        return -1;

    slot->argument = argument;
    __asm__ volatile("dmb sy" ::: "memory");
    slot->handler = handler;

    return 0;
}

int amp_bus_irq_unregister(
    uint32_t intid,
    AmpBusIrqHandler handler)
{
    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || handler == NULL ||
        !local_irqs_masked() || slot->handler != handler)
        return -1;

    /*
     * Caller must disable the peripheral interrupt and its assigned
     * GIC SPI before removing the handler.
     */
    slot->handler = NULL;
    __asm__ volatile("dmb sy" ::: "memory");
    slot->argument = NULL;

    return 0;
}

int amp_bus_irq_dispatch(uint32_t intid)
{
    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || slot->handler == NULL)
        return 0;

    slot->handler(slot->argument);
    return 1;
}