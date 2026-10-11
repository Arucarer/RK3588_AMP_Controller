#ifndef AMP_BUS_RESOURCES_H
#define AMP_BUS_RESOURCES_H

#include <stdint.h>

/*
 * Sources:
 * kernel/arch/arm64/boot/dts/rockchip/rk3588s.dtsi
 * kernel/arch/arm64/boot/dts/rockchip/rk3588-atk-devkit.dtsi
 *
 * Resource declarations only.
 * These constants do not grant ownership or enable clocks.
 */

#define AMP_CAN1_BASE          UINT64_C(0xFEA60000)
#define AMP_CAN1_SIZE          UINT32_C(0x1000)
#define AMP_CAN1_SPI           UINT32_C(342)
#define AMP_CAN1_INTID         (AMP_CAN1_SPI + UINT32_C(32))

#define AMP_UART3_BASE         UINT64_C(0xFEB60000)
#define AMP_UART3_SIZE         UINT32_C(0x100)
#define AMP_UART3_SPI          UINT32_C(334)
#define AMP_UART3_INTID        (AMP_UART3_SPI + UINT32_C(32))
#define AMP_UART3_REG_SHIFT    2U

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

#endif