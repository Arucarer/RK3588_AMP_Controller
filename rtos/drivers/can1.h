#ifndef AMP_CAN1_H
#define AMP_CAN1_H

#include <stdint.h>

typedef struct {
    uint32_t id;
    uint8_t length;
    uint8_t data[8];
} AmpCanFrame;

typedef struct {
    uint32_t mode;
    uint32_t state;
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint32_t rx_fifo;
} AmpCanStatus;

/*
 * 经典 CAN 标准数据帧基础驱动，单任务调用。
 * 不支持 FD、RTR、扩展帧；不允许与中断收包同时使用。
 *
 * nominal_timing 是当前 SDK 定义的 NBTP 寄存器值，
 * 必须由后续板级时钟配置计算并提供，不能使用猜测值。
 */
int amp_can1_init(uint32_t nominal_timing, int loopback);

int amp_can1_send(const AmpCanFrame *frame, uint32_t timeout_us);
int amp_can1_receive(AmpCanFrame *frame, uint32_t timeout_us);
int amp_can1_status(AmpCanStatus *status);

#endif