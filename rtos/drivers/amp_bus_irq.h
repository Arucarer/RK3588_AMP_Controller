#ifndef AMP_BUS_IRQ_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_BUS_IRQ_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

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
int amp_bus_irq_register(uint32_t intid, AmpBusIrqHandler handler, void *argument);

/* 检查中断号及中断屏蔽前置条件；清除对应回调和参数。 */
int amp_bus_irq_unregister(uint32_t intid, AmpBusIrqHandler handler);

/*
 * Returns 1 when dispatched, 0 when no handler is installed.
 */
int amp_bus_irq_dispatch(uint32_t intid);

#endif /* 结束 AMP_BUS_IRQ_H 对应的条件编译区段 */
