#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "amp_bus_resources.h" /* 引入 CAN1、UART3 的物理地址及中断资源定义 */
#include "amp_bus_irq.h"       /* 引入总线中断回调注册及分发接口 */

/* 单个外设中断的回调注册槽。 */
typedef struct {
    AmpBusIrqHandler handler; /* 中断服务回调；为空表示该槽未注册 */
    void *argument;           /* 分发中断时透传给回调的参数 */
} AmpBusIrqSlot;

static AmpBusIrqSlot can1_slot;  /* can1_slot：CAN1 中断回调及其参数 */
static AmpBusIrqSlot uart3_slot; /* uart3_slot：UART3 中断回调及其参数 */

static AmpBusIrqSlot *find_slot(uint32_t intid)
{
    /* 1. 按中断号匹配 CAN1 或 UART3 回调槽。 */
    /* 2. 不支持的中断号返回空指针。 */

    if (intid == AMP_CAN1_INTID)
        return &can1_slot;

    if (intid == AMP_UART3_INTID)
        return &uart3_slot;

    return NULL;
}

static int local_irqs_masked(void)
{
    /* 1. 读取本核中断屏蔽寄存器。 */
    /* 2. 返回 IRQ 是否已屏蔽。 */

    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (UINT64_C(1) << 7)) != 0;
}

int amp_bus_irq_register(uint32_t intid, AmpBusIrqHandler handler, void *argument)
{
    /* 1. 检查中断号、回调及中断屏蔽前置条件。 */
    /* 2. 保存回调和参数，供 IRQ 分发路径使用。 */

    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || handler == NULL || !local_irqs_masked() || slot->handler != NULL)
        return -1;

    slot->argument = argument;
    __asm__ volatile("dmb sy" ::: "memory");
    slot->handler = handler;

    return 0;
}

int amp_bus_irq_unregister(uint32_t intid, AmpBusIrqHandler handler)
{
    /* 1. 检查中断号及中断屏蔽前置条件。 */
    /* 2. 清除对应回调和参数。 */

    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || handler == NULL || !local_irqs_masked() || slot->handler != handler)
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
    /* 1. 按中断号查找回调槽。 */
    /* 2. 存在回调时调用并传递参数，返回是否已处理。 */

    AmpBusIrqSlot *slot = find_slot(intid);

    if (slot == NULL || slot->handler == NULL)
        return 0;

    slot->handler(slot->argument);
    return 1;
}
