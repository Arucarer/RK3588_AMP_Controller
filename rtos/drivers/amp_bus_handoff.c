#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "amp_bus_handoff.h" /* 引入总线资源交接、MMIO 访问及截止时间接口 */

static uint64_t counter_now(void)
{
    /* 1. 同步指令后读取 CNTPCT_EL0。 */
    /* 2. 返回物理计数器周期值。 */

    uint64_t value;

    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(value));
    return value;
}

void amp_bus_handoff_clear(void)
{
    /* 1. 清除总线资源交接记录。 */
    /* 2. 通过内存屏障发布失效状态。 */

    /*
     * 调用时机必须早于 Linux 启动和资源交接驱动 probe。
     * 这里只操作 RTOS 保留内存，不操作外设。
     */
    amp_bus_write32(AMP_BUS_HANDOFF_ADDR, 0U);
}

int amp_bus_platform_ready(uint32_t bus)
{
    /* 1. 核对板级交接记录的标识、版本与资源状态。 */
    /* 2. 返回资源交接是否满足驱动使用条件。 */

    uint32_t magic, version, mask, hz;

    if (bus != AMP_BUS_CAN1 && bus != AMP_BUS_UART3)
        return AMP_BUS_ARGUMENT;

    magic = amp_bus_read32(AMP_BUS_HANDOFF_ADDR);
    if (magic != AMP_BUS_HANDOFF_MAGIC)
        return AMP_BUS_NOT_READY;

    version = amp_bus_read32(AMP_BUS_HANDOFF_ADDR + 4U);
    mask = amp_bus_read32(AMP_BUS_HANDOFF_ADDR + 8U);
    hz = amp_bus_read32(AMP_BUS_HANDOFF_ADDR + (bus == AMP_BUS_CAN1 ? 12U : 16U));

    if (version != 1U || (mask & ~3U) || !(mask & (1U << bus)) || hz == 0U || hz > 1000000000U)
        return AMP_BUS_NOT_READY;

    /* 核对发布标志在读取期间没有被撤销。 */
    if (amp_bus_read32(AMP_BUS_HANDOFF_ADDR) != magic)
        return AMP_BUS_NOT_READY;

    return AMP_BUS_OK;
}

uint32_t amp_bus_clock_hz(uint32_t bus)
{
    /* 1. 核对总线资源交接状态。 */
    /* 2. 返回交接记录中的输入时钟频率。 */

    if (amp_bus_require_ready(bus) != AMP_BUS_OK)
        return 0U;

    return amp_bus_read32(AMP_BUS_HANDOFF_ADDR + (bus == AMP_BUS_CAN1 ? 12U : 16U));
}

int amp_bus_require_ready(uint32_t bus)
{
    /* 1. 检查设备编号及交接记录。 */
    /* 2. 确认时钟、复位和引脚资源已就绪，返回检查结果。 */

    if (bus != AMP_BUS_CAN1 && bus != AMP_BUS_UART3)
        return AMP_BUS_ARGUMENT;

    if (!AMP_BUS_HW_ACTIVATE)
        return AMP_BUS_NOT_READY;

    return amp_bus_platform_ready(bus);
}

uint32_t amp_bus_read32(uint64_t address)
{
    /* 1. 把寄存器地址转换为 volatile 指针并读取。 */
    /* 2. 返回读取值，由调用方解释状态位。 */

    uint32_t value;

    __asm__ volatile("dsb sy" ::: "memory");
    value = *(volatile uint32_t *)(uintptr_t)address;
    __asm__ volatile("dmb sy" ::: "memory");

    return value;
}

void amp_bus_write32(uint64_t address, uint32_t value)
{
    /* 1. 定位目标 MMIO 寄存器。 */
    /* 2. 通过 volatile 访问写入指定值。 */

    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

int amp_bus_deadline_start(AmpBusDeadline *deadline, uint32_t timeout_us)
{
    /* 1. 检查输出指针、超时及计数器频率。 */
    /* 2. 把微秒超时换算为计数周期。 */
    /* 3. 保存当前计数值和等待期限。 */

    uint64_t hz;

    if (deadline == NULL || timeout_us == 0U || timeout_us > 1000000U)
        return AMP_BUS_ARGUMENT;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));

    if (hz == 0U || hz > UINT64_C(1000000000))
        return AMP_BUS_HW_ERROR;

    deadline->ticks = (hz * timeout_us + UINT64_C(999999)) / UINT64_C(1000000);
    deadline->start = counter_now();

    return AMP_BUS_OK;
}

int amp_bus_deadline_expired(const AmpBusDeadline *deadline)
{
    /* 1. 读取当前硬件计数值。 */
    /* 2. 用已消耗周期判断是否达到等待期限。 */

    return counter_now() - deadline->start >= deadline->ticks;
}
