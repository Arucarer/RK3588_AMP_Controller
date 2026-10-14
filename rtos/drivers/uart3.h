#ifndef AMP_UART3_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_UART3_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/*
 * 单任务、轮询模式。
 * input_clock_hz 必须来自已完成的板级时钟配置。
 * format_bits 是 LCR 的低 6 位，例如：
 * 8N1=0x03，8N2=0x07，8E1=0x1B。
 */
int amp_uart3_init(uint32_t input_clock_hz, uint32_t baud, uint8_t format_bits, int loopback);

/*
 * 超时是整次调用的总预算，单位 us，最大 1 秒。
 * done 返回实际完成字节数，调用者不能盲目重发部分请求。
 */
int amp_uart3_write(const uint8_t *data, size_t length, uint32_t timeout_us, size_t *done);

/* 校验接收缓冲区并建立截止时间；等待接收数据并逐字节读取；返回实际接收数量及退出原因。 */
int amp_uart3_read(uint8_t *data, size_t length, uint32_t timeout_us, size_t *done);

/* Requires exclusive handoff; fail closed if GIC setup is incompatible. */
int amp_uart3_rx_irq_enable(void);
/* 检查参数和接收故障状态；从接收环形缓存取出字节及时间戳；更新读取位置并返回本次接收结果。 */
int amp_uart3_rx_poll(uint8_t *byte, uint64_t *observed_us);
/* 读取硬件计数器及频率；换算为微秒时间戳。 */
uint64_t amp_uart3_now_us(void);

#endif /* 结束 AMP_UART3_H 对应的条件编译区段 */
