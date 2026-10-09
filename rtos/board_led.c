#include <stdint.h>

#include "board_led.h"

#define GPIO1_BASE        UINT64_C(0xFEC20000)

#define GPIO_DR_L         0x00U
#define GPIO_DDR_L        0x08U
#define GPIO_VERSION      0x78U

#define GPIO_V2           UINT32_C(0x01000C2B)
#define GPIO_V2_1         UINT32_C(0x0101157C)

#define LED_BIT           3U
#define LED_MASK          (UINT32_C(1) << LED_BIT)
#define LED_WRITE_MASK    (UINT32_C(1) << (LED_BIT + 16U))

static uint32_t read_reg(unsigned int offset)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(uintptr_t)(GPIO1_BASE + offset);

    uint32_t value = *reg;

    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}

static void write_reg(unsigned int offset, uint32_t value)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(uintptr_t)(GPIO1_BASE + offset);

    *reg = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

unsigned int board_led_get(void)
{
    return (read_reg(GPIO_DR_L) & LED_MASK) ? 1U : 0U;
}

int board_led_set(unsigned int on)
{
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
