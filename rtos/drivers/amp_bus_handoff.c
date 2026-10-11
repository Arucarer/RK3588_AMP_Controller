#include <stddef.h>
#include <stdint.h>

#include "amp_bus_handoff.h"

static uint64_t counter_now(void)
{
    uint64_t value;

    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(value));
    return value;
}

void amp_bus_handoff_clear(void)
{
    /*
     * 调用时机必须早于 Linux 启动和资源交接驱动 probe。
     * 这里只操作 RTOS 保留内存，不操作外设。
     */
    amp_bus_write32(AMP_BUS_HANDOFF_ADDR, 0U);
}

int amp_bus_platform_ready(uint32_t bus)
{
    uint32_t magic, version, mask, hz;

    if (bus != AMP_BUS_CAN1 && bus != AMP_BUS_UART3)
        return AMP_BUS_ARGUMENT;

    magic = amp_bus_read32(AMP_BUS_HANDOFF_ADDR);
    if (magic != AMP_BUS_HANDOFF_MAGIC)
        return AMP_BUS_NOT_READY;

    version = amp_bus_read32(AMP_BUS_HANDOFF_ADDR + 4U);
    mask = amp_bus_read32(AMP_BUS_HANDOFF_ADDR + 8U);
    hz = amp_bus_read32(
        AMP_BUS_HANDOFF_ADDR + (bus == AMP_BUS_CAN1 ? 12U : 16U));

    if (version != 1U || (mask & ~3U) ||
        !(mask & (1U << bus)) || hz == 0U ||
        hz > 1000000000U)
        return AMP_BUS_NOT_READY;

    /* 核对发布标志在读取期间没有被撤销。 */
    if (amp_bus_read32(AMP_BUS_HANDOFF_ADDR) != magic)
        return AMP_BUS_NOT_READY;

    return AMP_BUS_OK;
}

uint32_t amp_bus_clock_hz(uint32_t bus)
{
    if (amp_bus_require_ready(bus) != AMP_BUS_OK)
        return 0U;

    return amp_bus_read32(
        AMP_BUS_HANDOFF_ADDR + (bus == AMP_BUS_CAN1 ? 12U : 16U));
}

int amp_bus_require_ready(uint32_t bus)
{
    if (bus != AMP_BUS_CAN1 && bus != AMP_BUS_UART3)
        return AMP_BUS_ARGUMENT;

    if (!AMP_BUS_HW_ACTIVATE)
        return AMP_BUS_NOT_READY;

    return amp_bus_platform_ready(bus);
}

uint32_t amp_bus_read32(uint64_t address)
{
    uint32_t value;

    __asm__ volatile("dsb sy" ::: "memory");
    value = *(volatile uint32_t *)(uintptr_t)address;
    __asm__ volatile("dmb sy" ::: "memory");

    return value;
}

void amp_bus_write32(uint64_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

int amp_bus_deadline_start(AmpBusDeadline *deadline,
                           uint32_t timeout_us)
{
    uint64_t hz;

    if (deadline == NULL || timeout_us == 0U ||
        timeout_us > 1000000U)
        return AMP_BUS_ARGUMENT;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));

    if (hz == 0U || hz > UINT64_C(1000000000))
        return AMP_BUS_HW_ERROR;

    deadline->ticks =
        (hz * timeout_us + UINT64_C(999999)) / UINT64_C(1000000);
    deadline->start = counter_now();

    return AMP_BUS_OK;
}

int amp_bus_deadline_expired(const AmpBusDeadline *deadline)
{
    return counter_now() - deadline->start >= deadline->ticks;
}