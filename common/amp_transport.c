#include "amp_transport.h" /* 引入共享内存通道布局及收发接口 */

#if !defined(__aarch64__) /* 非 AArch64 编译目标进入报错分支 */
#error "This transport implementation requires AArch64."
#endif /* 结束 !defined(__aarch64__) 对应的条件编译区段 */

//在 ARM64（AArch64）平台上执行一次内存屏障，保证屏障前后的内存访问按照规定的顺序执行。
//它主要用于 Linux 和 FreeRTOS 通过共享内存通信时，保证消息数据和状态标志的访问顺序正确。
//dmb sy：CPU 层面的内存顺序约束。
//"memory"：编译器层面的内存顺序约束。
static inline void transport_barrier(void)
{
    /* 1. 执行内存屏障，保证共享数据与状态标志的访问顺序。 */

    __asm__ volatile("dmb sy" ::: "memory");
}

int amp_transport_init(volatile AMPTransport *transport)
{
    /* 1. 清除共享区就绪标志并执行屏障。 */
    /* 2. 清零通信区并写入协议布局信息。 */
    /* 3. 用内存屏障保证配置先于就绪魔数发布。 */

    volatile uint8_t *bytes;
    size_t i;
    //参数检查
    if (transport == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /* 1. 清除就绪标志，表示共享内存尚未初始化完成 */
    transport->magic = 0;
    transport_barrier();

    /* 2. 将结构体转换为字节指针，清空 magic 之后的所有数据 */
    bytes = (volatile uint8_t *)transport;

    for (i = sizeof(transport->magic); i < sizeof(*transport); ++i) {
        bytes[i] = 0;
    }

    /* 3. 设置通信协议版本、结构体大小和消息帧大小 */
    transport->version = AMP_TRANSPORT_VERSION;
    transport->layout_size = (uint32_t)sizeof(*transport);
    transport->frame_size = AMP_MSG_FRAME_SIZE;

    /* 4. 确保初始化数据先于就绪标志发布 */
    transport_barrier();

    /* 5. 写入魔数，标记共享内存初始化完成 */
    transport->magic = AMP_TRANSPORT_MAGIC;
    transport_barrier();

    return AMP_TRANSPORT_OK;
}

//它只能判断 AMPTransport 的协议头是否符合预期
int amp_transport_ready(const volatile AMPTransport *transport)
{
    /* 1. 检查共享区指针和就绪魔数。 */
    /* 2. 通过内存屏障后读取版本及布局。 */
    /* 3. 返回协议是否兼容且已经就绪。 */

    /* 1. 检查共享内存指针是否有效 */
    if (transport == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /* 2. 检查魔数，判断共享内存是否已标记初始化完成 */
    if (transport->magic != AMP_TRANSPORT_MAGIC)
        return AMP_TRANSPORT_NOT_READY;

    /* 3. 内存屏障，确保先检查魔数，再读取协议字段 */
    transport_barrier();

    /* 4. 检查协议版本、结构体大小、消息帧大小 */
    if (transport->version != AMP_TRANSPORT_VERSION ||
        transport->layout_size != (uint32_t)sizeof(*transport) ||
        transport->frame_size != AMP_MSG_FRAME_SIZE) {
        return AMP_TRANSPORT_NOT_READY;
    }

    /* 5. 检查通过，通信区协议头符合预期 */
    return AMP_TRANSPORT_OK;
}

int amp_channel_send(volatile AMPChannel *channel, const AMPMessage *message)
{
    /* 1. 检查通道与消息，并确认上一帧已被消费。 */
    /* 2. 编码消息并写入共享帧缓存。 */
    /* 3. 通过屏障发布新的消息序号。 */

    uint8_t frame[AMP_MSG_FRAME_SIZE];
    uint32_t published;
    uint32_t consumed;
    size_t i;
    int result;

    /* 1. 检查通道指针和消息指针是否有效 */
    if (channel == NULL || message == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /* 2. 读取收发计数器，判断消息槽是否空闲 */
    published = channel->published;
    consumed = channel->consumed;

    if (published != consumed)
        return AMP_TRANSPORT_BUSY;

    /* 3. 确认上一帧已被接收方取走，允许重用消息槽 */
    transport_barrier();

    /* 4. 将 AMPMessage 编码为固定长度的消息帧 */
    result = amp_message_encode(frame, message);
    if (result != AMP_CODEC_OK)
        return result;

    /* 5. 将编码后的消息帧写入共享内存 */
    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        channel->frame[i] = frame[i];

    /* 6. 内存屏障，确保消息数据先于发布标志写入 */
    transport_barrier();

    /* 7. 更新发布计数器，通知接收方有新消息 */
    channel->published = published + 1U;
    transport_barrier();

    /* 8. 消息发布成功 */
    return AMP_TRANSPORT_OK;
}

int amp_channel_receive(volatile AMPChannel *channel, AMPMessage *message)
{
    /* 1. 检查通道状态并判断是否有新帧。 */
    /* 2. 通过屏障读取帧并解码。 */
    /* 3. 发布消费序号，返回消息或解码错误。 */

    uint8_t frame[AMP_MSG_FRAME_SIZE];
    uint32_t published;
    uint32_t consumed;
    size_t i;

    /* 1. 检查通道指针和消息指针是否有效 */
    if (channel == NULL || message == NULL)
        return AMP_TRANSPORT_ARGUMENT;

    /* 2. 读取收发计数器，判断是否有新消息 */
    published = channel->published;
    consumed = channel->consumed;

    if (published == consumed)
        return AMP_TRANSPORT_EMPTY;

    /* 3. 内存屏障，确保先读取发布标志，再读取消息数据 */
    transport_barrier();

    /* 4. 将共享内存中的消息帧复制到本地缓冲区 */
    for (i = 0; i < AMP_MSG_FRAME_SIZE; ++i)
        frame[i] = channel->frame[i];

    /* 5. 内存屏障，确保消息复制完成后再释放消息槽 */
    transport_barrier();

    /* 6. 更新接收计数器，通知发送方消息已取走 */
    channel->consumed = published;
    transport_barrier();

    /* 7. 解码本地消息帧，并返回解码结果 */
    return amp_message_decode(frame, message);
}
