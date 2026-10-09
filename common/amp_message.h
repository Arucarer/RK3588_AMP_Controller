#ifndef AMP_MESSAGE_H
#define AMP_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

#define AMP_MSG_MAGIC          UINT32_C(0x414D5032)
#define AMP_MSG_VERSION        2U


#define AMP_MSG_HEADER_SIZE    48U
#define AMP_MSG_FRAME_SIZE     256U
#define AMP_MSG_PAYLOAD_MAX     \
    (AMP_MSG_FRAME_SIZE - AMP_MSG_HEADER_SIZE)

/* 消息类型 */
#define AMP_MSG_REQUEST        1U
#define AMP_MSG_RESPONSE       2U
#define AMP_MSG_HEARTBEAT      3U

/* 命令 */
#define AMP_OP_PING            1U
#define AMP_OP_GET_STATUS      2U
#define AMP_OP_HELLO           3U
/* LED control */
#define AMP_OP_LED_SET          0x10U
#define AMP_OP_LED_GET          0x11U

#define AMP_LED_OFF             0U
#define AMP_LED_ON              1U
#define AMP_LED_BLINK           2U

#define AMP_RESULT_HW_ERROR     7U


/* 对端返回的处理结果 */
#define AMP_RESULT_OK          0U
#define AMP_RESULT_UNSUPPORTED 1U
#define AMP_RESULT_BAD_PAYLOAD 2U
#define AMP_RESULT_BUSY        3U
#define AMP_RESULT_INTERNAL    4U
#define AMP_RESULT_BAD_SESSION 5U
#define AMP_RESULT_BAD_SEQUENCE 6U


/* 本地编解码错误，不直接作为远端处理结果使用 */
#define AMP_CODEC_OK           0
#define AMP_CODEC_ARGUMENT    (-1)
#define AMP_CODEC_FORMAT      (-2)
#define AMP_CODEC_VERSION     (-3)
#define AMP_CODEC_LENGTH      (-4)
#define AMP_CODEC_CRC         (-5)

/*
 * 线上的消息头，共 48 字节：
 *
 * 偏移  长度  字段
 * 0x00   4   magic
 * 0x04   4   version
 * 0x08   4   type
 * 0x0c   4   payload_length
 * 0x10   8   session
 * 0x18   4   sequence
 * 0x1c   4   opcode
 * 0x20   4   status
 * 0x24   4   flags，当前必须为 0
 * 0x28   4   reserved，当前必须为 0
 * 0x2c   4   crc32
 * 0x30  ...  payload
 *
 * 此结构体仅用于程序内部，不直接复制到共享内存。
 * payload 中的多字节数据也必须使用约定的小端编码。
 */
typedef struct {
    uint32_t type;
    uint32_t payload_length;
    uint64_t session;
    uint32_t sequence;
    uint32_t opcode;
    uint32_t status;
    uint8_t payload[AMP_MSG_PAYLOAD_MAX];
} AMPMessage;

/*
 * frame 必须指向至少 AMP_MSG_FRAME_SIZE 字节的本地缓冲区。
 * 不直接对正在被另一端修改的共享内存调用编解码。
 */
int amp_message_encode(
    uint8_t frame[AMP_MSG_FRAME_SIZE],
    const AMPMessage *message);

int amp_message_decode(
    const uint8_t frame[AMP_MSG_FRAME_SIZE],
    AMPMessage *message);

#endif