#ifndef AMP_BUS_RESOURCES_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_BUS_RESOURCES_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/*
 * Sources:
 * kernel/arch/arm64/boot/dts/rockchip/rk3588s.dtsi
 * kernel/arch/arm64/boot/dts/rockchip/rk3588-atk-devkit.dtsi
 *
 * Resource declarations only.
 * These constants do not grant ownership or enable clocks.
 */

#define AMP_CAN1_BASE UINT64_C(0xFEA60000) /* CAN1 寄存器区域的 MMIO 物理基地址 */
#define AMP_CAN1_SIZE UINT32_C(0x1000)     /* CAN1 寄存器区域大小，单位字节 */
#define AMP_CAN1_SPI UINT32_C(342) /* CAN1 在设备树中的 SPI 编号，不包含 GIC 的 32 个本地中断 */
#define AMP_CAN1_INTID                                                                             \
    (AMP_CAN1_SPI + UINT32_C(32)) /* CAN1 的完整 GIC 中断号，等于 SPI 编号加 32 */

#define AMP_UART3_BASE UINT64_C(0xFEB60000) /* UART3 寄存器区域的 MMIO 物理基地址 */
#define AMP_UART3_SIZE UINT32_C(0x100)      /* UART3 寄存器区域大小，单位字节 */
#define AMP_UART3_SPI UINT32_C(334)         /* UART3 在设备树中的 SPI 编号 */
#define AMP_UART3_INTID                                                                            \
    (AMP_UART3_SPI + UINT32_C(32)) /* UART3 的完整 GIC 中断号，等于 SPI 编号加 32 */
#define AMP_UART3_REG_SHIFT 2U /* UART 寄存器编号左移 2 位，换算成 4 字节间距的地址偏移 */

/*
 * CAN1: CAN1_TX_M1 GPIO4_B3, CAN1_RX_M1 GPIO4_B2.
 * UART3: UART3_TX_M2 GPIO4_A5, UART3_RX_M2 GPIO4_A6.
 *
 * Current CAN compatible: rockchip,can-2.0.
 * Do not assume CAN FD support.
 *
 * Clock identifiers from dt-bindings are NOT MMIO offsets.
 * Clock/reset/pinmux handoff must be implemented separately.
 */

#endif /* 结束 AMP_BUS_RESOURCES_H 对应的条件编译区段 */
