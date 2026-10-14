#ifndef BOARD_LED_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define BOARD_LED_H /* 定义头文件保护标记，避免本文件被重复包含 */

/*
 * 前提：
 * Linux 已释放工作 LED；
 * GPIO1 时钟和 GPIO1_A3 引脚复用已配置完成。
 *
 * 返回 0：成功；-1：版本或寄存器读回检查失败。
 */
int board_led_init(void);
/* 按逻辑亮灭状态生成 GPIO 写入值；使用硬件写掩码更新 LED 引脚并检查结果。 */
int board_led_set(unsigned int on);

/* 返回输出锁存值：0 灭、1 亮；不等于光学反馈。 */
unsigned int board_led_get(void);

#endif /* 结束 BOARD_LED_H 对应的条件编译区段 */
