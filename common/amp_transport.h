#ifndef AMP_TRANSPORT_H
#define AMP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "amp_message.h"

#define AMP_TRANSPORT_ADDR       UINT64_C(0x20FD0000)
#define AMP_TRANSPORT_MAP_SIZE   4096U
#define AMP_TRANSPORT_MAGIC      UINT32_C(0x41545032)
#define AMP_TRANSPORT_VERSION    2U

#define AMP_TRANSPORT_OK         0
#define AMP_TRANSPORT_EMPTY      1
#define AMP_TRANSPORT_BUSY       2
#define AMP_TRANSPORT_ARGUMENT  (-10)
#define AMP_TRANSPORT_NOT_READY (-11)

/*
 * 单生产者、单消费者通道。
 *
 * published：只由发送方更新。
 * consumed ：只由接收方更新。
 *
 * published == consumed：槽为空。
 * published != consumed：有一帧尚未消费。
 *
 * 三部分分别从 64 字节边界开始。
 * 这不意味着可以直接启用非一致性缓存。
 */
typedef struct {
    uint32_t published;
    uint8_t producer_reserved[60];

    uint32_t consumed;
    uint8_t consumer_reserved[60];

    uint8_t frame[AMP_MSG_FRAME_SIZE];
} AMPChannel;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t layout_size;
    uint32_t frame_size;
    uint8_t reserved[48];

    AMPChannel linux_to_rtos;
    AMPChannel rtos_to_linux;
} AMPTransport;

_Static_assert(offsetof(AMPChannel, consumed) == 64,
               "Invalid consumed offset");
_Static_assert(offsetof(AMPChannel, frame) == 128,
               "Invalid frame offset");
_Static_assert(sizeof(AMPChannel) == 384,
               "Invalid channel size");

_Static_assert(offsetof(AMPTransport, linux_to_rtos) == 64,
               "Invalid request channel offset");
_Static_assert(offsetof(AMPTransport, rtos_to_linux) == 448,
               "Invalid response channel offset");
_Static_assert(sizeof(AMPTransport) == 832,
               "Invalid transport size");
_Static_assert(sizeof(AMPTransport) <= AMP_TRANSPORT_MAP_SIZE,
               "Transport exceeds shared memory");

/*
 * 仅由 FreeRTOS 在启动时、对端尚未访问时调用。
 * 不能用它处理客户端重连或运行中的超时。
 */
int amp_transport_init(volatile AMPTransport *transport);

/* 检查布局是否已初始化。 */
int amp_transport_ready(const volatile AMPTransport *transport);

/*
 * 非阻塞发送。
 * 返回 BUSY 时未写入消息，调用者应稍后重试。
 */
int amp_channel_send(volatile AMPChannel *channel,
                     const AMPMessage *message);

/*
 * 非阻塞接收。
 * 即使收到坏帧，也会释放消息槽，然后返回编解码错误。
 * 只有返回 OK 时，message 才能用于命令处理。
 */
int amp_channel_receive(volatile AMPChannel *channel,
                        AMPMessage *message);

#endif