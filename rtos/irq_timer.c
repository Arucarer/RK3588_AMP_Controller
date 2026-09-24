#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "irq_timer.h"

#define GICR_BASE       0xFE680000ULL
#define GICR_END        0xFE780000ULL
#define TIMER_INTID     30U
#define TIMER_MASK      (1U << TIMER_INTID)


#define READ_SYS(reg) ({                         \
    uint64_t value;                              \
    __asm__ volatile("mrs %0, " #reg : "=r"(value)); \
    value;                                      \
})

#define WRITE_SYS(reg, value) do {               \
    uint64_t temp = (uint64_t)(value);            \
    __asm__ volatile("msr " #reg ", %0"           \
                     :: "r"(temp) : "memory");   \
} while (0)

/*
 * ICC register encodings:
 * SRE     S3_0_C12_C12_5
 * PMR     S3_0_C4_C6_0
 * BPR1    S3_0_C12_C12_3
 * CTLR    S3_0_C12_C12_4
 * IGRPEN1 S3_0_C12_C12_7
 * IAR1    S3_0_C12_C12_0
 * EOIR1   S3_0_C12_C12_1
 * DIR     S3_0_C12_C11_1
 */

typedef struct {
    uint64_t magic;          /* +0x00 */
    uint64_t start;          /* +0x08: Linux writes 1 */
    uint64_t stage;          /* +0x10 */
    uint64_t error;          /* +0x18 */
    uint64_t irq_count;      /* +0x20 */
    uint64_t last_intid;     /* +0x28 */
    uint64_t counter_hz;     /* +0x30 */
    uint64_t period;         /* +0x38 */
    uint64_t gicr;           /* +0x40 */
    uint64_t last_counter;   /* +0x48 */
    uint64_t unexpected;     /* +0x50 */
    uint64_t icc_ctlr;       /* +0x58 */
} TimerInfo;

static volatile TimerInfo *const info =
    (volatile TimerInfo *)(uintptr_t)0x20FE0200ULL;

static uint64_t timer_period;
static uint64_t cpu_ctlr;

static inline void barrier(void)
{
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

static inline uint32_t read32(uint64_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}

static inline void write32(uint64_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static __attribute__((noreturn)) void stop_test(uint64_t error)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    WRITE_SYS(cntp_ctl_el0, 0);
    info->error = error;
    barrier();

    for (;;)
        __asm__ volatile("wfe");
}

/* Bounded polling: no infinite wait on a GIC register. */
static int wait_clear(uint64_t address, uint32_t mask)
{
    unsigned int remaining = 1000000;

    while (remaining--) {
        if (!(read32(address) & mask))
            return 0;
        __asm__ volatile("yield");
    }

    return -1;
}

static uint64_t find_redistributor(void)
{
    uint64_t mpidr = READ_SYS(mpidr_el1);
    uint32_t affinity = (uint32_t)(
        (mpidr & 0x00FFFFFFULL) |
        ((mpidr >> 8) & 0xFF000000ULL));
    uint64_t base;

    for (base = GICR_BASE; base < GICR_END;) {
        uint64_t typer =
            *(volatile uint64_t *)(uintptr_t)(base + 0x8);

        if ((uint32_t)(typer >> 32) == affinity)
            return base;

        if (typer & (1ULL << 4)) /* Last */
            break;

        /* RD + SGI frames; add VLPI frames if present. */
        base += (typer & (1ULL << 1)) ? 0x40000 : 0x20000;
    }

    return 0;
}

void amp_wait_for_start(void)
{
    unsigned int i;
    volatile uint64_t *words =
        (volatile uint64_t *)(uintptr_t)info;

    __asm__ volatile("msr daifset, #2" ::: "memory");

    for (i = 0; i < sizeof(*info) / sizeof(uint64_t); i++)
        words[i] = 0;

    info->stage = 1;
    barrier();
    info->magic = 0x54494D4552303031ULL;
    barrier();

    /*
     * Wait until Linux has booted.
     * Deliberately poll: Linux devmem writes do not send an event.
     */
    while (info->start != 1)
        __asm__ volatile("yield");

    barrier();
}

/*
 * Called by xPortStartScheduler() with CPU IRQs masked.
 * Do not unmask IRQs here: the Port restores the first task.
 */
void amp_setup_tick(void)
{
    uint64_t base, sgi, sre, frequency;
    uint32_t value;

    info->stage = 2;
    barrier();

    /* A trap here is caught by the existing exception recorder. */
    WRITE_SYS(cntp_ctl_el0, 0);
    barrier();

    frequency = READ_SYS(cntfrq_el0);
    info->counter_hz = frequency;

    if (frequency < (uint64_t)configTICK_RATE_HZ)
        stop_test(1);

    timer_period = frequency / (uint64_t)configTICK_RATE_HZ;
    info->period = timer_period;

    base = find_redistributor();
    info->gicr = base;

    if (!base)
        stop_test(2);

    /* Wake only this CPU's Redistributor. */
    value = read32(base + 0x14);
    write32(base + 0x14, value & ~(1U << 1));
    barrier();

    if (wait_clear(base + 0x14, 1U << 2))
        stop_test(3);

    sgi = base + 0x10000;

    /* Disable this timer PPI before configuring it. */
    write32(sgi + 0x180, TIMER_MASK);
    barrier();

    if (wait_clear(base, 1U << 3)) /* GICR_CTLR.RWP */
        stop_test(4);

    /*
    * Non-secure EL1 relies on the interrupt grouping established
    * by secure firmware. IGROUPR0 may be RAZ/WI here.
    */

    /* PPI 30: level-triggered, ICFGR1 field [29:28]. */
    value = read32(sgi + 0xC04);
    write32(sgi + 0xC04, value & ~(2U << 28));

    *(volatile uint8_t *)(uintptr_t)(sgi + 0x400 + TIMER_INTID)
        = (uint8_t)(portLOWEST_USABLE_INTERRUPT_PRIORITY
                    << portPRIORITY_SHIFT);

    write32(sgi + 0x280, TIMER_MASK); /* Clear pending */
    write32(sgi + 0x380, TIMER_MASK); /* Clear active */
    barrier();

    info->stage = 3;

    sre = READ_SYS(S3_0_C12_C12_5);
    WRITE_SYS(S3_0_C12_C12_5, sre | 1);
    barrier();

    if (!(READ_SYS(S3_0_C12_C12_5) & 1))
        stop_test(6);

    WRITE_SYS(S3_0_C4_C6_0, 0xFF);
    WRITE_SYS(S3_0_C12_C12_3, 0);
    WRITE_SYS(S3_0_C12_C12_7, 1);
    barrier();

    /*
     * This FreeRTOS Port writes EOIR but does not write DIR.
     * Require combined priority-drop/deactivation mode.
     */
    cpu_ctlr = READ_SYS(S3_0_C12_C12_4);
    info->icc_ctlr = cpu_ctlr;

    if (cpu_ctlr & (1ULL << 1))
        stop_test(10);

    /* Confirm the priority width used by FreeRTOSConfig.h. */
    if (((cpu_ctlr >> 8) & 7ULL) != 4ULL)
        stop_test(11);

    write32(sgi + 0x100, TIMER_MASK);
    barrier();

    if (wait_clear(base, 1U << 3))
        stop_test(7);

    /*
    * Verify that the timer PPI enable bit is visible and set.
    * Failure requires further firmware/group-access diagnosis.
    */
    if (!(read32(sgi + 0x100) & TIMER_MASK))
        stop_test(9);

    info->stage = 4;

    WRITE_SYS(cntp_cval_el0,
              READ_SYS(cntpct_el0) + timer_period);
    WRITE_SYS(cntp_ctl_el0, 1);
    barrier();

    info->stage = 5;
    barrier();

    /*
     * Return to xPortStartScheduler().
     * IRQs remain masked until the first task is restored.
     */
}

/*
 * Called by FreeRTOS_Tick_Handler() before it permits nesting.
 * Rearm the timer to remove the level-triggered interrupt source.
 */
void amp_clear_tick(void)
{
    uint64_t now = READ_SYS(cntpct_el0);

    WRITE_SYS(cntp_cval_el0, now + timer_period);
    info->last_counter = now;
    info->irq_count++;
    info->stage = 6;

    barrier();
}

/*
 * FreeRTOS_IRQ_Handler already read ICC_IAR1_EL1.
 * The interrupt acknowledge value arrives in the first argument.
 * The Port performs EOI after this function returns.
 */
void vApplicationIRQHandler(uint64_t iar)
{
    uint32_t intid = (uint32_t)(iar & 0xFFFFFFU);

    info->last_intid = intid;

    if (intid >= 1020U && intid <= 1023U)
        return;

    if (intid == TIMER_INTID) {
        FreeRTOS_Tick_Handler();
        return;
    }

    info->unexpected++;
    stop_test(8);
}