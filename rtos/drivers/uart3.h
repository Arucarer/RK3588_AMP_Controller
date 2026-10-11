#ifndef AMP_UART3_H
#define AMP_UART3_H

#include <stddef.h>
#include <stdint.h>

/*
 * 单任务、轮询模式。
 * input_clock_hz 必须来自已完成的板级时钟配置。
 * format_bits 是 LCR 的低 6 位，例如：
 * 8N1=0x03，8N2=0x07，8E1=0x1B。
 */
int amp_uart3_init(uint32_t input_clock_hz,
                   uint32_t baud,
                   uint8_t format_bits,
                   int loopback);

/*
 * 超时是整次调用的总预算，单位 us，最大 1 秒。
 * done 返回实际完成字节数，调用者不能盲目重发部分请求。
 */
int amp_uart3_write(const uint8_t *data, size_t length,
                    uint32_t timeout_us, size_t *done);

int amp_uart3_read(uint8_t *data, size_t length,
                   uint32_t timeout_us, size_t *done);

#endif