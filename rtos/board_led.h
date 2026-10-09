#ifndef BOARD_LED_H
#define BOARD_LED_H

/*
 * 前提：
 * Linux 已释放工作 LED；
 * GPIO1 时钟和 GPIO1_A3 引脚复用已配置完成。
 *
 * 返回 0：成功；-1：版本或寄存器读回检查失败。
 */
int board_led_init(void);
int board_led_set(unsigned int on);

/* 返回输出锁存值：0 灭、1 亮；不等于光学反馈。 */
unsigned int board_led_get(void);

#endif