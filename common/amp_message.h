
#ifndef AMP_MESSAGE_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_MESSAGE_H /* 定义头文件保护标记，避免本文件被重复包含 */

/*
 * AMP IPC 消息协议定义
 * 定义消息格式、命令类型、状态码及编解码接口。
 * Linux 和 FreeRTOS 使用统一协议进行消息通信。
 */

#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* 1. 消息协议基本参数 */
#define AMP_MSG_MAGIC UINT32_C(0x414D5032) // 消息魔数，用于识别合法消息帧
#define AMP_MSG_VERSION 2U                 // 消息协议版本 V2
#define AMP_MSG_HEADER_SIZE 48U            // 消息头固定长度：48 字节
#define AMP_MSG_FRAME_SIZE 256U            // 单条消息帧固定长度：256 字节
#define AMP_MSG_PAYLOAD_MAX (AMP_MSG_FRAME_SIZE - AMP_MSG_HEADER_SIZE) // 最大负载长度：208 字节

/* 2. 消息类型 */
#define AMP_MSG_REQUEST 1U   // 请求消息：Linux 向 FreeRTOS 发送命令
#define AMP_MSG_RESPONSE 2U  // 响应消息：FreeRTOS 返回命令执行结果
#define AMP_MSG_HEARTBEAT 3U // 心跳消息：检测通信会话是否存活

/* 3. IPC 命令编号 */
#define AMP_OP_PING 1U             // PING 命令：测试通信是否正常
#define AMP_OP_GET_STATUS 2U       // 状态查询：获取 FreeRTOS 运行状态
#define AMP_OP_HELLO 3U            // 会话握手：建立 Linux 与 FreeRTOS 通信会话
#define AMP_OP_LED_SET 0x10U       // LED 设置：控制 LED 工作状态
#define AMP_OP_LED_GET 0x11U       // LED 查询：获取 LED 当前状态
#define AMP_OP_CAN_AUTHORIZE 0x20U // CAN 授权：请求执行 CAN 授权操作

/* 4. LED 工作状态 */
#define AMP_LED_OFF 0U   // LED 关闭
#define AMP_LED_ON 1U    // LED 常亮
#define AMP_LED_BLINK 2U // LED 闪烁

/* 5. FreeRTOS 命令执行结果 */
#define AMP_RESULT_OK 0U           // 执行成功
#define AMP_RESULT_UNSUPPORTED 1U  // 不支持的命令
#define AMP_RESULT_BAD_PAYLOAD 2U  // 消息负载格式或参数错误
#define AMP_RESULT_BUSY 3U         // 设备或服务繁忙
#define AMP_RESULT_INTERNAL 4U     // 内部处理错误
#define AMP_RESULT_BAD_SESSION 5U  // 会话 ID 无效或不匹配
#define AMP_RESULT_BAD_SEQUENCE 6U // 请求序号无效
#define AMP_RESULT_HW_ERROR 7U     // 硬件操作失败

/* 6. 本地消息编解码结果 */
#define AMP_CODEC_OK 0          // 编解码成功
#define AMP_CODEC_ARGUMENT (-1) // 函数参数错误
#define AMP_CODEC_FORMAT (-2)   // 消息帧格式错误
#define AMP_CODEC_VERSION (-3)  // 消息协议版本不匹配
#define AMP_CODEC_LENGTH (-4)   // 消息长度不合法
#define AMP_CODEC_CRC (-5)      // CRC32 校验失败

/*
 * 7. AMP IPC 消息结构
 *
 * 消息帧固定为 256 字节，其中消息头占 48 字节，
 * 消息负载最大为 208 字节。
 *
 * 消息头布局（小端编码）：
 *
 * 偏移   长度   字段
 * 0x00    4    magic           消息魔数
 * 0x04    4    version         协议版本
 * 0x08    4    type            消息类型
 * 0x0C    4    payload_length  负载长度
 * 0x10    8    session         会话 ID
 * 0x18    4    sequence        请求序号
 * 0x1C    4    opcode          命令编号
 * 0x20    4    status          执行状态
 * 0x24    4    flags           标志位，当前为 0
 * 0x28    4    reserved        预留字段，当前为 0
 * 0x2C    4    crc32           消息 CRC32 校验值
 * 0x30   208   payload         消息负载区
 *
 * AMPMessage 仅用于程序内部的数据表示。
 * 实际传输时通过 encode/decode 完成固定格式编解码，
 * 不直接将结构体复制到共享内存。
 */
/* IPC 消息的内存表示，由编解码接口转换成固定布局的数据帧。 */
typedef struct {
    uint32_t type;                        // 消息类型：请求、响应、心跳
    uint32_t payload_length;              // 消息负载的实际长度（字节）
    uint64_t session;                     // 会话 ID，用于区分不同通信会话
    uint32_t sequence;                    // 请求序号，用于匹配请求与响应
    uint32_t opcode;                      // 命令编号，指定 FreeRTOS 执行的操作
    uint32_t status;                      // 命令执行状态，表示成功或错误
    uint8_t payload[AMP_MSG_PAYLOAD_MAX]; // 消息数据区，存放命令参数或响应数据
} AMPMessage;

/* 8. 消息编码：将 AMPMessage 转换为固定长度的字节帧 */
int amp_message_encode(uint8_t frame[AMP_MSG_FRAME_SIZE], const AMPMessage *message);

/* 9. 消息解码：校验消息帧并还原为 AMPMessage */
int amp_message_decode(const uint8_t frame[AMP_MSG_FRAME_SIZE], AMPMessage *message);

#endif /* 结束 AMP_MESSAGE_H 对应的条件编译区段 */
