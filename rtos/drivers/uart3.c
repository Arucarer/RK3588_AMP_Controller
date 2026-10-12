#include <stddef.h>
#include <stdint.h>

#include "amp_bus_resources.h"
#include "amp_bus_handoff.h"
#include "uart3.h"
#include "amp_bus_irq.h"

#define RBR_THR_DLL 0U
#define IER_DLH     1U
#define FCR         2U
#define LCR         3U
#define MCR         4U
#define LSR         5U

#define LSR_DATA    (1U << 0)
#define LSR_ERRORS  (0xFU << 1)
#define LSR_THRE    (1U << 5)
#define LSR_TEMT    (1U << 6)

static int initialized;

static uint32_t rd(uint32_t reg)
{
    return amp_bus_read32(
        AMP_UART3_BASE + ((uint64_t)reg << AMP_UART3_REG_SHIFT));
}

static void wr(uint32_t reg, uint32_t value)
{
    amp_bus_write32(
        AMP_UART3_BASE + ((uint64_t)reg << AMP_UART3_REG_SHIFT),
        value);
}

static int wait_flag(uint32_t mask, const AmpBusDeadline *deadline)
{
    for (;;) {
        uint32_t status = rd(LSR);

        if (status & LSR_ERRORS)
            return AMP_BUS_HW_ERROR;

        if (status & mask)
            return AMP_BUS_OK;

        if (amp_bus_deadline_expired(deadline))
            return AMP_BUS_TIMEOUT;

        __asm__ volatile("yield");
    }
}

static int uart_init_masked(uint32_t input_clock_hz,
                   uint32_t baud,
                   uint8_t format_bits,
                   int loopback)
{
    uint64_t denominator, divisor, actual, difference;
    int result;

    initialized = 0;

    if (input_clock_hz == 0U || baud == 0U ||
        (format_bits & 0xC0U) ||
        (loopback != 0 && loopback != 1))
        return AMP_BUS_ARGUMENT;

    denominator = UINT64_C(16) * baud;
    divisor = ((uint64_t)input_clock_hz + denominator / 2U) /
              denominator;

    if (divisor == 0U || divisor > 65535U)
        return AMP_BUS_ARGUMENT;

    actual = input_clock_hz / (16U * divisor);
    difference = actual > baud ? actual - baud : baud - actual;

    if (difference * 100U > (uint64_t)baud * 2U)
        return AMP_BUS_ARGUMENT;

    result = amp_bus_require_ready(AMP_BUS_UART3);
    if (result != AMP_BUS_OK)
        return result;

    /*
     * 前置条件：板级交接已复位并使 UART 空闲。
     * 本阶段禁用 UART 中断和 DMA。
     */
    wr(LCR, format_bits);
    wr(IER_DLH, 0U);

    wr(LCR, format_bits | 0x80U);
    if (!(rd(LCR) & 0x80U))
        return AMP_BUS_HW_ERROR;

    wr(RBR_THR_DLL, (uint32_t)(divisor & 0xFFU));
    wr(IER_DLH, (uint32_t)((divisor >> 8) & 0xFFU));

    if ((rd(RBR_THR_DLL) & 0xFFU) != (divisor & 0xFFU) ||
        (rd(IER_DLH) & 0xFFU) != ((divisor >> 8) & 0xFFU))
        return AMP_BUS_HW_ERROR;

    wr(LCR, format_bits);
    if ((rd(LCR) & 0xBFU) != format_bits)
        return AMP_BUS_HW_ERROR;

    wr(FCR, 0x07U); /* FIFO enable + RX/TX reset */
    wr(MCR, loopback ? 0x10U : 0U);

    if ((rd(MCR) & 0x10U) != (loopback ? 0x10U : 0U))
        return AMP_BUS_HW_ERROR;

    initialized = 1;
    return AMP_BUS_OK;
}

int amp_uart3_write(const uint8_t *data, size_t length,
                    uint32_t timeout_us, size_t *done)
{
    AmpBusDeadline deadline;
    int result;

    if (done == NULL)
        return AMP_BUS_ARGUMENT;
    *done = 0U;

    if (data == NULL || length == 0U || length > 256U)
        return AMP_BUS_ARGUMENT;

    if (!initialized)
        return AMP_BUS_NOT_READY;

    result = amp_bus_require_ready(AMP_BUS_UART3);
    if (result != AMP_BUS_OK)
        return result;

    result = amp_bus_deadline_start(&deadline, timeout_us);
    if (result != AMP_BUS_OK)
        return result;

    while (*done < length) {
        result = wait_flag(LSR_THRE, &deadline);
        if (result != AMP_BUS_OK)
            return result;

        wr(RBR_THR_DLL, data[*done]);
        ++*done;
    }

    /* 必须等待移位寄存器发送结束，而不只是 FIFO 空。 */
    return wait_flag(LSR_TEMT, &deadline);
}

int amp_uart3_read(uint8_t *data, size_t length,
                   uint32_t timeout_us, size_t *done)
{
    AmpBusDeadline deadline;
    int result;

    if (done == NULL)
        return AMP_BUS_ARGUMENT;
    *done = 0U;

    if (data == NULL || length == 0U || length > 256U)
        return AMP_BUS_ARGUMENT;

    if (!initialized)
        return AMP_BUS_NOT_READY;

    result = amp_bus_require_ready(AMP_BUS_UART3);
    if (result != AMP_BUS_OK)
        return result;

    result = amp_bus_deadline_start(&deadline, timeout_us);
    if (result != AMP_BUS_OK)
        return result;

    while (*done < length) {
        result = wait_flag(LSR_DATA, &deadline);
        if (result != AMP_BUS_OK)
            return result;

        data[*done] = (uint8_t)rd(RBR_THR_DLL);
        ++*done;
    }

    return AMP_BUS_OK;
}
/* IRQ-owned RX queue. Timestamps describe ISR observation of FIFO bytes. */
#define RX_CAP 256U
static volatile unsigned rx_head, rx_tail;
static volatile int rx_fault;
static uint8_t rx_bytes[RX_CAP];
static uint64_t rx_times[RX_CAP];
#define GICD UINT64_C(0xfe600000)
static uint64_t irq_lock(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) :: "memory");
    return flags;
}
static void irq_unlock(uint64_t flags)
{ __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory"); }
uint64_t amp_uart3_now_us(void)
{
    uint64_t ticks, hz;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return (ticks/hz)*1000000U + (ticks%hz)*1000000U/hz;
}
static void uart_rx_irq(void *unused)
{
    unsigned budget=RX_CAP;
    (void)unused;
    while (budget--) {
        uint32_t lsr=rd(LSR), iir;
        if (lsr & LSR_ERRORS) { rx_fault=1; wr(IER_DLH,0); return; }
        if (lsr & LSR_DATA) {
            unsigned next=(rx_head+1U)%RX_CAP;
            uint8_t byte=(uint8_t)rd(RBR_THR_DLL);
            if (next==rx_tail) { rx_fault=1; wr(IER_DLH,0); return; }
            rx_bytes[rx_head]=byte;
            rx_times[rx_head]=amp_uart3_now_us();
            __asm__ volatile("dmb ish" ::: "memory");
            rx_head=next;
            continue;
        }
        iir=rd(FCR)&15U;
        if (iir & 1U) return;
        /* DesignWare busy detect: read USR to acknowledge. */
        if (iir==7U) { (void)rd(0x1fU); continue; }
        if (iir==4U || iir==12U || iir==6U) continue;
        rx_fault=1; wr(IER_DLH,0); return;
    }
    rx_fault=1; wr(IER_DLH,0); /* Bound interrupt work, prevent storms. */
}
static int gic_wait(void)
{
    AmpBusDeadline d;
    if (amp_bus_deadline_start(&d,10000U)) return AMP_BUS_HW_ERROR;
    while (amp_bus_read32(GICD)&(1U<<31))
        if (amp_bus_deadline_expired(&d)) return AMP_BUS_TIMEOUT;
    return 0;
}
int amp_uart3_rx_irq_enable(void)
{
    uint32_t id=AMP_UART3_INTID, bit=1U<<(id%32U);
    uint64_t flags, affinity;
    uint32_t ctl, group, config;
    int rc;
    if (!initialized || amp_bus_require_ready(AMP_BUS_UART3)) return AMP_BUS_NOT_READY;
    ctl=amp_bus_read32(GICD);
    group=amp_bus_read32(GICD+0x80U+(id/32U)*4U);
    config=amp_bus_read32(GICD+0xc00U+(id/16U)*4U);
    /* IGROUPR is RAZ/WI to NS accesses with security enabled.
     * Keep group checking only in the single-security-state view (DS=1). */
    if (!(ctl&(1U<<4)) || ((ctl&(1U<<6)) && !(group&bit)) ||
        (config&(2U<<((id%16U)*2U))))
        return AMP_BUS_UNSUPPORTED;
    flags=irq_lock();
    wr(IER_DLH,0);
    amp_bus_write32(GICD+0x180U+(id/32U)*4U,bit);
    rc=gic_wait();
    if (rc) goto done;
    /* Reject active IRQ rather than rerouting an in-flight interrupt. */
    if (amp_bus_read32(GICD+0x300U+(id/32U)*4U)&bit) {
        rc=AMP_BUS_HW_ERROR; goto done;
    }
    /* We only ever own this handler and this SPI. */
    (void)amp_bus_irq_unregister(id,uart_rx_irq);
    if (amp_bus_irq_register(id,uart_rx_irq,0)) { rc=AMP_BUS_HW_ERROR; goto done; }
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(affinity));
    affinity &= UINT64_C(0xff00ffffff);
    *(volatile uint64_t *)(uintptr_t)(GICD+0x6000U+id*8U)=affinity;
    *(volatile uint8_t *)(uintptr_t)(GICD+0x400U+id)=0xa0U;
    __asm__ volatile("dsb sy; isb" ::: "memory");
    if (*(volatile uint64_t *)(uintptr_t)(GICD+0x6000U+id*8U)!=affinity ||
        *(volatile uint8_t *)(uintptr_t)(GICD+0x400U+id)!=0xa0U) {
        rc=AMP_BUS_HW_ERROR; goto done;
    }
    rx_head=rx_tail=0; rx_fault=0;
    wr(FCR,0x07U);
    amp_bus_write32(GICD+0x280U+(id/32U)*4U,bit);
    amp_bus_write32(GICD+0x100U+(id/32U)*4U,bit);
    rc=gic_wait();
    if (!rc && !(amp_bus_read32(GICD+0x100U+(id/32U)*4U)&bit))
        rc=AMP_BUS_UNSUPPORTED;
    if (!rc) wr(IER_DLH,5U); /* RX data and receiver line status. */
done:
    if (rc) {
        wr(IER_DLH,0);
        amp_bus_write32(GICD+0x180U+(id/32U)*4U,bit);
        (void)gic_wait();
        rx_fault=1;
    }
    irq_unlock(flags);
    return rc;
}
int amp_uart3_rx_poll(uint8_t *byte, uint64_t *observed_us)
{
    uint64_t flags;
    int rc=0;
    if (!byte || !observed_us) return AMP_BUS_ARGUMENT;
    if (!initialized || amp_bus_require_ready(AMP_BUS_UART3)) return AMP_BUS_NOT_READY;
    flags=irq_lock();
    if (rx_fault) rc=AMP_BUS_HW_ERROR;
    else if (rx_tail!=rx_head) {
        *byte=rx_bytes[rx_tail]; *observed_us=rx_times[rx_tail];
        rx_tail=(rx_tail+1U)%RX_CAP; rc=1;
    }
    irq_unlock(flags);
    return rc;
}

int amp_uart3_init(uint32_t clock, uint32_t baud, uint8_t format, int loopback)
{
    uint64_t flags=irq_lock();
    int rc=uart_init_masked(clock,baud,format,loopback);
    irq_unlock(flags);
    return rc;
}
