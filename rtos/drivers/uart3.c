#include <stddef.h>
#include <stdint.h>

#include "amp_bus_resources.h"
#include "amp_bus_handoff.h"
#include "uart3.h"

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

int amp_uart3_init(uint32_t input_clock_hz,
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