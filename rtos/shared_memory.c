#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shared_memory.h"

#include "../common/amp_message.h"
#include "../common/amp_transport.h"

static volatile AMPTransport *const transport =
    (volatile AMPTransport *)(uintptr_t)AMP_TRANSPORT_ADDR;

/* 以下状态只由 IpcTask 使用。 */
static uint64_t active_session;
static uint32_t last_sequence;

static uint32_t received_requests;
static uint32_t duplicate_requests;
static uint32_t bad_frames;
static uint32_t rejected_requests;
static uint32_t heartbeat_sequence;

static int have_cached_response;
static AMPMessage cached_request;
static AMPMessage cached_response;

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/*
 * 状态负载，共 32 字节，全部为小端 uint32：
 *
 * +0  ：状态格式版本，当前为 1
 * +4  ：FreeRTOS tick，仅用于诊断，允许回绕
 * +8  ：tick 频率
 * +12 ：格式正确的请求计数
 * +16 ：重复请求计数
 * +20 ：坏帧计数
 * +24 ：拒绝请求计数
 * +28 ：最近已处理的会话内请求序号
 */
static void fill_status(AMPMessage *message)
{
    message->payload_length = 32U;

    put_u32(message->payload + 0, 1U);
    put_u32(message->payload + 4,
            (uint32_t)xTaskGetTickCount());
    put_u32(message->payload + 8,
            (uint32_t)configTICK_RATE_HZ);
    put_u32(message->payload + 12, received_requests);
    put_u32(message->payload + 16, duplicate_requests);
    put_u32(message->payload + 20, bad_frames);
    put_u32(message->payload + 24, rejected_requests);
    put_u32(message->payload + 28, last_sequence);
}

static void response_begin(const AMPMessage *request,
                           AMPMessage *response)
{
    response->type = AMP_MSG_RESPONSE;
    response->payload_length = 0;
    response->session = request->session;
    response->sequence = request->sequence;
    response->opcode = request->opcode;
    response->status = AMP_RESULT_OK;
}

static int same_request(const AMPMessage *a,
                        const AMPMessage *b)
{
    uint32_t i;

    if (a->type != b->type ||
        a->session != b->session ||
        a->sequence != b->sequence ||
        a->opcode != b->opcode ||
        a->status != b->status ||
        a->payload_length != b->payload_length) {
        return 0;
    }

    for (i = 0; i < a->payload_length; ++i) {
        if (a->payload[i] != b->payload[i])
            return 0;
    }

    return 1;
}

static void cache_result(const AMPMessage *request,
                         const AMPMessage *response)
{
    cached_request = *request;
    cached_response = *response;
    last_sequence = request->sequence;
    have_cached_response = 1;
}

static void handle_request(const AMPMessage *request,
                           AMPMessage *response)
{
    uint32_t i;

    response_begin(request, response);

    /* 请求必须携带非零会话、非零序号，status 必须为零。 */
    if (request->session == 0 ||
        request->sequence == 0 ||
        request->status != 0) {
        response->status = AMP_RESULT_BAD_PAYLOAD;
        ++rejected_requests;
        return;
    }

    /*
     * 新会话只能通过 HELLO 建立。
     * session 由 Linux 客户端生成，不作为身份认证凭据。
     */
    if (request->session != active_session) {
        if (request->opcode != AMP_OP_HELLO) {
            response->status = AMP_RESULT_BAD_SESSION;
            ++rejected_requests;
            return;
        }

        if (request->payload_length != 0) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            ++rejected_requests;
            return;
        }

        active_session = request->session;
        last_sequence = 0;
        have_cached_response = 0;
    }

    /*
     * 同一序号只能对应完全相同的请求。
     * 重发返回缓存结果，不重复执行命令。
     */
    if (have_cached_response &&
        request->sequence == last_sequence) {
        if (same_request(request, &cached_request)) {
            ++duplicate_requests;
            *response = cached_response;
        } else {
            ++rejected_requests;
            response->status = AMP_RESULT_BAD_SEQUENCE;
        }
        return;
    }

    /*
     * 当前采用严格递增序号，不支持会话内回绕。
     * 到 UINT32_MAX 后，客户端必须建立新会话。
     */
    if (have_cached_response &&
        request->sequence < last_sequence) {
        response->status = AMP_RESULT_BAD_SEQUENCE;
        ++rejected_requests;
        return;
    }

    switch (request->opcode) {
    case AMP_OP_HELLO:
        if (request->payload_length != 0)
            response->status = AMP_RESULT_BAD_PAYLOAD;
        break;

    case AMP_OP_PING:
        response->payload_length = request->payload_length;
        for (i = 0; i < request->payload_length; ++i)
            response->payload[i] = request->payload[i];
        break;

    case AMP_OP_GET_STATUS:
        if (request->payload_length != 0) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
        } else {
            /*
             * 状态快照中的 last_sequence 包含本次请求。
             * 结果随后缓存，重复请求返回同一份快照。
             */
            last_sequence = request->sequence;
            fill_status(response);
        }
        break;

    default:
        response->status = AMP_RESULT_UNSUPPORTED;
        break;
    }

    if (response->status != AMP_RESULT_OK)
        ++rejected_requests;

    cache_result(request, response);
}

void amp_ipc_init(void)
{
    uint64_t sctlr;
    int result;

    /* 保留现阶段的数据缓存关闭前提。 */
    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
    configASSERT((sctlr & (1ULL << 2)) == 0);

    active_session = 0;
    last_sequence = 0;
    received_requests = 0;
    duplicate_requests = 0;
    bad_frames = 0;
    rejected_requests = 0;
    heartbeat_sequence = 0;
    have_cached_response = 0;

    result = amp_transport_init(transport);
    configASSERT(result == AMP_TRANSPORT_OK);
}

void amp_ipc_task(void *argument)
{
    /*
     * 使用静态缓冲区，降低 IpcTask 的栈占用。
     * 本任务只能创建一个实例。
     */
    static AMPMessage request;
    static AMPMessage pending_response;
    static AMPMessage heartbeat;

    int response_pending = 0;
    TickType_t last_heartbeat;
    const TickType_t poll_period = pdMS_TO_TICKS(10);
    const TickType_t heartbeat_period = pdMS_TO_TICKS(1000);

    (void)argument;

    configASSERT(poll_period > 0);
    configASSERT(heartbeat_period > 0);
    configASSERT(amp_transport_ready(transport) ==
                 AMP_TRANSPORT_OK);

    last_heartbeat = xTaskGetTickCount();

    for (;;) {
        int result;
        TickType_t now;

        /*
         * 已执行的命令必须保留响应，直到成功发布。
         * 返回通道被占用时，不继续执行下一条请求。
         */
        if (response_pending) {
            result = amp_channel_send(
                &transport->rtos_to_linux,
                &pending_response);

            if (result == AMP_TRANSPORT_OK) {
                response_pending = 0;
            } else {
                configASSERT(result == AMP_TRANSPORT_BUSY);
            }
        }

        if (!response_pending) {
            result = amp_channel_receive(
                &transport->linux_to_rtos,
                &request);

            if (result == AMP_TRANSPORT_OK) {
                if (request.type == AMP_MSG_REQUEST) {
                    ++received_requests;
                    handle_request(&request, &pending_response);
                    response_pending = 1;
                } else {
                    ++bad_frames;
                }
            } else if (result != AMP_TRANSPORT_EMPTY) {
                /*
                 * CRC/格式错误时，不相信 session 和 sequence，
                 * 因而不按损坏头部发送响应。
                 * 发送端通过超时发现失败。
                 */
                ++bad_frames;
            }
        }

        now = xTaskGetTickCount();

        /*
         * 响应优先于心跳。
         * 通道忙时心跳可以合并，下一轮重新生成最新状态。
         */
        if (!response_pending &&
            active_session != 0 &&
            (TickType_t)(now - last_heartbeat) >=
                heartbeat_period) {
            heartbeat.type = AMP_MSG_HEARTBEAT;
            heartbeat.session = active_session;
            heartbeat.sequence = heartbeat_sequence + 1U;
            heartbeat.opcode = AMP_OP_GET_STATUS;
            heartbeat.status = AMP_RESULT_OK;
            fill_status(&heartbeat);

            result = amp_channel_send(
                &transport->rtos_to_linux, &heartbeat);

            if (result == AMP_TRANSPORT_OK) {
                heartbeat_sequence = heartbeat.sequence;
                last_heartbeat = now;
            } else {
                configASSERT(result == AMP_TRANSPORT_BUSY);
            }
        }

        vTaskDelay(poll_period);
    }
}