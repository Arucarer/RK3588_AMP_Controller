#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "board_led.h" /* 引入板载 LED 驱动接口 */

#define GPIO1_BASE UINT64_C(0xFEC20000) /* GPIO1 控制器的 MMIO 物理基地址 */

#define GPIO_DR_L 0x00U    /* GPIO 低 16 位数据寄存器偏移，单位字节 */
#define GPIO_DDR_L 0x08U   /* GPIO 低 16 位方向寄存器偏移，单位字节 */
#define GPIO_VERSION 0x78U /* GPIO 控制器版本寄存器偏移，单位字节 */

#define GPIO_V2 UINT32_C(0x01000C2B)   /* 可识别的 GPIO V2 控制器版本值 */
#define GPIO_V2_1 UINT32_C(0x0101157C) /* 可识别的 GPIO V2.1 控制器版本值 */

#define LED_BIT 3U                        /* 板载 LED 所用 GPIO1_A3 的位编号 */
#define LED_MASK (UINT32_C(1) << LED_BIT) /* LED 数据位掩码，只选择 GPIO1_A3 */
#define LED_WRITE_MASK                                                                             \
    (UINT32_C(1) << (LED_BIT + 16U)) /* LED 写使能掩码，高 16 位控制对应低位是否更新 */

static uint32_t read_reg(unsigned int offset)
{
    /* 1. 把寄存器地址转换为 volatile 指针并读取。 */
    /* 2. 返回读取值，由调用方解释状态位。 */

    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(GPIO1_BASE + offset);

    uint32_t value = *reg;

    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

static void write_reg(unsigned int offset, uint32_t value)
{
    /* 1. 定位目标 MMIO 寄存器。 */
    /* 2. 通过 volatile 访问写入指定值。 */

    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(GPIO1_BASE + offset);

    *reg = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

unsigned int board_led_get(void)
{
    /* 1. 读取 LED GPIO 输出状态。 */
    /* 2. 按板级有效电平转换为逻辑亮灭状态。 */

    return (read_reg(GPIO_DR_L) & LED_MASK) ? 1U : 0U;
}

int board_led_set(unsigned int on)
{
    /* 1. 按逻辑亮灭状态生成 GPIO 写入值。 */
    /* 2. 使用硬件写掩码更新 LED 引脚并检查结果。 */

    uint32_t value;

    if (on > 1U)
        return -1;

    /*
     * 高 16 位：只允许修改 GPIO1_A3。
     * 低 16 位：该引脚的新输出值。
     *
     * 不使用整组寄存器的读改写。
     */
    value = LED_WRITE_MASK;

    if (on)
        value |= LED_MASK;

    write_reg(GPIO_DR_L, value);

    return board_led_get() == on ? 0 : -1;
}

int board_led_init(void)
{
    /* 1. 核对 GPIO 控制器版本，防止使用不匹配的寄存器布局。 */
    /* 2. 先设置熄灭电平，再切换 LED 引脚为输出。 */
    /* 3. 读回方向位，确认初始化结果。 */

    uint32_t version = read_reg(GPIO_VERSION);

    /*
     * V1 与 V2 的寄存器布局不同。
     * 版本不匹配时禁止继续写方向寄存器。
     */
    if (version != GPIO_V2 && version != GPIO_V2_1)
        return -1;

    /* 先设置熄灭值，再切换为输出。 */
    if (board_led_set(0) != 0)
        return -1;

    write_reg(GPIO_DDR_L, LED_WRITE_MASK | LED_MASK);

    if (!(read_reg(GPIO_DDR_L) & LED_MASK))
        return -1;

    return 0;
}
