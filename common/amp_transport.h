#ifndef AMP_TRANSPORT_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_TRANSPORT_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "amp_message.h" /* 引入 IPC 消息布局、操作码及编解码接口 */

/* 共享内存基本配置 */
#define AMP_TRANSPORT_ADDR UINT64_C(0x20FD0000)  // 共享内存起始物理地址
#define AMP_TRANSPORT_MAP_SIZE 4096U             // 共享内存映射大小，4KB
#define AMP_TRANSPORT_MAGIC UINT32_C(0x41545032) // 魔数，用于识别通信结构是否有效
#define AMP_TRANSPORT_VERSION 2U                 // 通信协议版本号，当前为 V2

/* 共享内存通信状态码 */
#define AMP_TRANSPORT_OK 0            // 操作成功
#define AMP_TRANSPORT_EMPTY 1         // 消息槽为空，没有可接收的消息
#define AMP_TRANSPORT_BUSY 2          // 消息槽被占用，上一条消息尚未消费
#define AMP_TRANSPORT_ARGUMENT (-10)  // 输入参数错误
#define AMP_TRANSPORT_NOT_READY (-11) // 共享内存通信结构尚未初始化或未就绪

/* IPC 单向消息通道：通过发布状态、消费状态和消息缓冲区实现数据传输 */
/* 单向单消息槽通道，通过发布序号和消费序号交接帧所有权。 */
typedef struct {
    uint32_t published;            // 发送方发布的消息序号
    uint8_t producer_reserved[60]; // 填充60字节，使下一字段位于64字节边界

    uint32_t consumed;             // 接收方已消费的消息序号
    uint8_t consumer_reserved[60]; // 填充60字节，使消息缓冲区位于128字节偏移处

    uint8_t frame[AMP_MSG_FRAME_SIZE]; // 消息缓冲区，保存实际传输的数据帧
} AMPChannel;

/* AMP 共享内存通信结构体：管理通信协议头及 Linux/FreeRTOS 双向消息通道 */
/* Linux 与 RTOS 共用的双向 IPC 传输区，包含协议头和两个单向通道。 */
typedef struct {
    uint32_t magic;       // 魔数，用于识别共享内存通信结构
    uint32_t version;     // 通信协议版本号，当前为 V2
    uint32_t layout_size; // AMPTransport 结构体总大小，832 字节
    uint32_t frame_size;  // 单条消息帧缓冲区大小，256 字节
    uint8_t reserved[48]; // 预留48字节，使通信通道从64字节边界开始

    AMPChannel linux_to_rtos; // Linux → FreeRTOS：发送命令、接收请求
    AMPChannel rtos_to_linux; // FreeRTOS → Linux：返回响应、发送心跳
} AMPTransport;

/* 编译期内存布局校验：
 * 检查 AMPChannel 和 AMPTransport 的成员偏移及结构体大小，
 * 确保 Linux 与 FreeRTOS 使用一致的共享内存布局，
 * 防止结构体布局错误或超出共享内存映射范围。
 */
_Static_assert(offsetof(AMPChannel, consumed) == 64, "Invalid consumed offset");
_Static_assert(offsetof(AMPChannel, frame) == 128, "Invalid frame offset");
_Static_assert(sizeof(AMPChannel) == 384, "Invalid channel size");

_Static_assert(offsetof(AMPTransport, linux_to_rtos) == 64, "Invalid request channel offset");
_Static_assert(offsetof(AMPTransport, rtos_to_linux) == 448, "Invalid response channel offset");
_Static_assert(sizeof(AMPTransport) == 832, "Invalid transport size");
_Static_assert(sizeof(AMPTransport) <= AMP_TRANSPORT_MAP_SIZE, "Transport exceeds shared memory");

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
int amp_channel_send(volatile AMPChannel *channel, const AMPMessage *message);

/*
 * 非阻塞接收。
 * 即使收到坏帧，也会释放消息槽，然后返回编解码错误。
 * 只有返回 OK 时，message 才能用于命令处理。
 */
int amp_channel_receive(volatile AMPChannel *channel, AMPMessage *message);

#endif /* 结束 AMP_TRANSPORT_H 对应的条件编译区段 */
