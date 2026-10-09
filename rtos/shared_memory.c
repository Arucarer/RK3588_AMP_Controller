#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shared_memory.h"
#include "control_task.h"
#include "monitor_task.h"
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

/* 已交给 ControlTask、尚未取回执行结果的请求。 */
static int control_pending;
static AMPMessage control_request;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/*
 * 状态负载版本 2，共 64 字节，小端 uint32。
 * +0..28：保留原状态字段，+0 的格式版本改为 2。
 * +32：监控就绪
 * +36：当前故障位
 * +40：历史故障位
 * +44：监控检查次数
 * +48：IPC 运行标记距今毫秒数
 * +52：Control 运行标记距今毫秒数
 * +56：Monitor 检查距今毫秒数
 * +60：LED 驱动故障
 */
static void fill_status(AMPMessage *message)
{
    MonitorSnapshot health;

    monitor_snapshot(&health);

    message->payload_length = 64U;

    put_u32(message->payload + 0, 2U);
    put_u32(message->payload + 4,
            (uint32_t)xTaskGetTickCount());
    put_u32(message->payload + 8,
            (uint32_t)configTICK_RATE_HZ);
    put_u32(message->payload + 12, received_requests);
    put_u32(message->payload + 16, duplicate_requests);
    put_u32(message->payload + 20, bad_frames);
    put_u32(message->payload + 24, rejected_requests);
    put_u32(message->payload + 28, last_sequence);

    put_u32(message->payload + 32, health.ready);
    put_u32(message->payload + 36, health.faults);
    put_u32(message->payload + 40, health.latched_faults);
    put_u32(message->payload + 44, health.checks);
    put_u32(message->payload + 48, health.ipc_age_ms);
    put_u32(message->payload + 52, health.control_age_ms);
    put_u32(message->payload + 56, health.monitor_age_ms);
    put_u32(message->payload + 60, health.led_fault);
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

    case AMP_OP_LED_SET:
    case AMP_OP_LED_GET:
    {
        ControlCommand command = {0};

        command.session = request->session;
        command.sequence = request->sequence;

        if (request->opcode == AMP_OP_LED_GET) {
            if (request->payload_length != 0) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            command.operation = CONTROL_LED_QUERY;
        } else {
            uint32_t mode;
            uint32_t period_ms;

            if (request->payload_length != 8U) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            mode = get_u32(request->payload);
            period_ms = get_u32(request->payload + 4);

            if (mode == AMP_LED_OFF) {
                command.operation = CONTROL_LED_OFF;
            } else if (mode == AMP_LED_ON) {
                command.operation = CONTROL_LED_ON;
            } else if (mode == AMP_LED_BLINK) {
                command.operation = CONTROL_LED_BLINK;
            } else {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            if (mode == AMP_LED_BLINK) {
                if (period_ms < 100U ||
                    period_ms > 10000U ||
                    period_ms % 20U != 0) {
                    response->status = AMP_RESULT_BAD_PAYLOAD;
                    break;
                }
            } else if (period_ms != 0) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            command.period_ms = period_ms;
        }

        /*
         * 先保留原请求。
         * GPIO 尚未执行，不能缓存或返回成功响应。
         */
        control_request = *request;

        if (control_submit(&command) != 0) {
            response->status = AMP_RESULT_BUSY;
            break;
        }

        control_pending = 1;
        return;
    }

    default:
        response->status = AMP_RESULT_UNSUPPORTED;
        break;
    }

    if (response->status != AMP_RESULT_OK)
        ++rejected_requests;

    cache_result(request, response);
}

static int collect_control_result(AMPMessage *response)
{
    ControlResult result;
    int received;

    received = control_receive(&result);
    configASSERT(received >= 0);

    if (received == 0)
        return 0;

    configASSERT(result.session == control_request.session);
    configASSERT(result.sequence == control_request.sequence);

    response_begin(&control_request, response);

    switch (result.status) {
    case CONTROL_OK:
        response->status = AMP_RESULT_OK;
        response->payload_length = 12U;

        put_u32(response->payload + 0, result.mode);
        put_u32(response->payload + 4, result.output);
        put_u32(response->payload + 8, result.period_ms);
        break;

    case CONTROL_BAD_ARGUMENT:
        response->status = AMP_RESULT_BAD_PAYLOAD;
        break;

    case CONTROL_HW_ERROR:
        response->status = AMP_RESULT_HW_ERROR;
        break;

    default:
        response->status = AMP_RESULT_INTERNAL;
        break;
    }

    if (response->status != AMP_RESULT_OK)
        ++rejected_requests;

    /*
     * 缓存实际执行结果。
     * 同一请求重发时直接返回缓存，不再次操作 GPIO。
     */
    cache_result(&control_request, response);
    control_pending = 0;

    return 1;
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
    control_pending = 0;

    result = amp_transport_init(transport);
    configASSERT(result == AMP_TRANSPORT_OK);
}

void amp_ipc_task(void *argument)
{
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
        monitor_beat(MONITOR_IPC);
        /*
         * 控制任务完成后，才产生最终响应。
         * IpcTask 不阻塞等待队列。
         */
        if (control_pending) {
            if (collect_control_result(&pending_response))
                response_pending = 1;
        }

        /*
         * 保留响应直到成功写入返回通道。
         * Linux 未消费旧消息时，不覆盖共享槽。
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

        /*
         * 控制命令尚未完成或响应尚未发布时，
         * 不处理下一条请求，也不切换会话。
         */
        if (!response_pending && !control_pending) {
            result = amp_channel_receive(
                &transport->linux_to_rtos,
                &request);

            if (result == AMP_TRANSPORT_OK) {
                if (request.type == AMP_MSG_REQUEST) {
                    ++received_requests;

                    handle_request(&request,
                                   &pending_response);

                    /*
                     * 普通命令立即产生响应。
                     * LED 命令等待 ControlTask 的执行结果。
                     */
                    if (!control_pending)
                        response_pending = 1;
                } else {
                    ++bad_frames;
                }
            } else if (result != AMP_TRANSPORT_EMPTY) {
                ++bad_frames;
            }
        }

        now = xTaskGetTickCount();

        /*
         * 心跳仍可在等待 ControlTask 时发送。
         * 已有最终响应时，优先发送响应。
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
                &transport->rtos_to_linux,
                &heartbeat);

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