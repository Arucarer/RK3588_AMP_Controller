#include "amp_transport.h"

#if !defined(__aarch64__)
#error "This transport implementation requires AArch64."
#endif

static inline void transport_barrier(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

int amp_transport_init(volatile AMPTransport *transport)
{
    volatile uint8_t *bytes;
    size_t i;

    if (transport == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /*
     * 初始化期间禁止对端访问。
     * magic 清零不是运行中重置的同步协议。
     */
    transport->magic = 0;
    transport_barrier();

    bytes = (volatile uint8_t *)transport;

    for (i = sizeof(transport->magic);
         i < sizeof(*transport);
         ++i) {
        bytes[i] = 0;
    }

    transport->version = AMP_TRANSPORT_VERSION;
    transport->layout_size = (uint32_t)sizeof(*transport);
    transport->frame_size = AMP_MSG_FRAME_SIZE;

    /* 所有布局及通道状态就绪后，最后发布 magic。 */
    transport_barrier();
    transport->magic = AMP_TRANSPORT_MAGIC;
    transport_barrier();

    return AMP_TRANSPORT_OK;
}

int amp_transport_ready(const volatile AMPTransport *transport)
{
    if (transport == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    if (transport->magic != AMP_TRANSPORT_MAGIC)
        return AMP_TRANSPORT_NOT_READY;

    transport_barrier();

    if (transport->version != AMP_TRANSPORT_VERSION ||
        transport->layout_size != (uint32_t)sizeof(*transport) ||
        transport->frame_size != AMP_MSG_FRAME_SIZE) {
        return AMP_TRANSPORT_NOT_READY;
    }

    return AMP_TRANSPORT_OK;
}

int amp_channel_send(volatile AMPChannel *channel,
                     const AMPMessage *message)
{
    uint8_t frame[AMP_MSG_FRAME_SIZE];
    uint32_t published;
    uint32_t consumed;
    size_t i;
    int result;

    if (channel == NULL || message == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /*
     * 本通道只能有一个发送者。
     * Linux 多个线程或进程必须在上层串行化。
     */
    published = channel->published;
    consumed = channel->consumed;

    if (published != consumed)
        return AMP_TRANSPORT_BUSY;

    /* 确认对端已复制完上一帧，再重用消息槽。 */
    transport_barrier();

    result = amp_message_encode(frame, message);
    if (result != AMP_CODEC_OK)
        return result;

    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        channel->frame[i] = frame[i];

    /*
     * 传输计数器与消息 sequence 无关。
     * unsigned 回绕允许发生，单槽模式下只比较是否相等。
     */
    transport_barrier();
    channel->published = published + 1U;
    transport_barrier();

    return AMP_TRANSPORT_OK;
}

int amp_channel_receive(volatile AMPChannel *channel,
                        AMPMessage *message)
{
    uint8_t frame[AMP_MSG_FRAME_SIZE];
    uint32_t published;
    uint32_t consumed;
    size_t i;

    if (channel == NULL || message == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /*
     * 本通道只能有一个接收者。
     * published 不相等时，发送方必须保持 frame 不变。
     */
    published = channel->published;
    consumed = channel->consumed;

    if (published == consumed)
        return AMP_TRANSPORT_EMPTY;

    /* 观察到发布标志后，再读取完整消息。 */
    transport_barrier();

    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        frame[i] = channel->frame[i];

    /*
     * 本地副本已完成，释放共享消息槽。
     * 后续 CRC 校验和命令执行使用本地副本。
     *
     * consumed 仅表示已取走消息，不表示命令执行成功。
     * 执行结果必须通过 RESPONSE 消息返回。
     */
    transport_barrier();
    channel->consumed = published;
    transport_barrier();

    return amp_message_decode(frame, message);
}