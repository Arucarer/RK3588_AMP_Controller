#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "FreeRTOS.h"                /* 引入 FreeRTOS 内核类型、配置及基础接口 */
#include "task.h"                    /* 引入 FreeRTOS 任务、延时及调度接口 */
#include "shared_memory.h"           /* 引入 RTOS 共享内存 IPC 初始化及任务入口 */
#include "control_task.h"            /* 引入 LED 控制命令、结果及任务接口 */
#include "motor/motor_ipc.h"
#include "../common/amp_motor_protocol.h"
#include "bus_task.h"                /* 引入总线命令、结果及任务队列接口 */
#include "drivers/amp_bus_handoff.h" /* 引入总线资源交接、MMIO 访问及截止时间接口 */
#define AMP_OP_BUS_EXEC 0x30U        /* IPC 操作码：向 RTOS 总线任务提交命令 */
#include "monitor_task.h"            /* 引入任务心跳、故障位及健康快照接口 */
#include "../common/amp_message.h"   /* 引入 IPC 消息布局、操作码及编解码接口 */
#include "../common/amp_transport.h" /* 引入共享内存通道布局及收发接口 */

static volatile AMPTransport *const transport = (volatile AMPTransport *)(uintptr_t)
    AMP_TRANSPORT_ADDR; /* transport：Linux 与 RTOS 共享传输区的映射地址 */

/* 1. IPC 会话与请求序号管理 */
static uint64_t active_session; // 当前 Linux 客户端的会话ID
static uint32_t last_sequence;  // 最近一次缓存请求的序号

/* 2. IPC 通信统计 */
static uint32_t received_requests;  // 接收到的请求总数
static uint32_t duplicate_requests; // 重复请求数量
static uint32_t bad_frames;         // 无效消息类型或传输错误数量
static uint32_t rejected_requests;  // 被拒绝的请求或操作失败计数
static uint32_t heartbeat_sequence; // FreeRTOS 主动发送的心跳序号

/* 3. 请求去重与响应缓存 */
static int have_cached_response;   // 是否存在有效的响应缓存：1有，0无
static AMPMessage cached_request;  // 缓存最近一次已处理的请求
static AMPMessage cached_response; // 缓存该请求对应的响应

/* 4. 异步任务执行状态 */
static int control_pending;        // ControlTask 是否有尚未完成的请求
static AMPMessage control_request; // 保存提交给 ControlTask 的原始请求
static int bus_pending;            // BusTask 是否有尚未完成的请求
static AMPMessage bus_request;     // 保存提交给 BusTask 的原始请求

static uint32_t get_u32(const uint8_t *p)
{
    /* 1. 按协议字节序读取各字节。 */
    /* 2. 移位合并为主机整数并返回。 */

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    /* 1. 将整数拆分为各字节。 */
    /* 2. 按协议字节序写入目标缓冲区。 */

    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/*
 * 状态负载版本 2，共 64 字节，小端 uint32。
 */
/* 收集 FreeRTOS 运行状态，编码为 64 字节的 IPC 状态负载 */
static void fill_status(AMPMessage *message)
{
    /* 1. 获取监控快照并设置状态负载长度。 */
    /* 2. 编码版本、Tick、IPC 计数及最近请求序号。 */
    /* 3. 编码任务健康、心跳间隔及 LED 故障。 */

    MonitorSnapshot health;        // 保存 MonitorTask 的运行状态快照
    monitor_snapshot(&health);     // 获取当前系统监控状态
    message->payload_length = 64U; // 状态负载共 64 字节（16 个 uint32_t）

    /* 1. 系统基本状态 */
    put_u32(message->payload + 0, 2U);                            // 状态数据格式版本号：2
    put_u32(message->payload + 4, (uint32_t)xTaskGetTickCount()); // FreeRTOS 当前 Tick 计数
    put_u32(message->payload + 8, (uint32_t)configTICK_RATE_HZ);  // FreeRTOS Tick 频率（Hz）

    /* 2. IPC 通信统计 */
    put_u32(message->payload + 12, received_requests);  // 已接收的请求总数
    put_u32(message->payload + 16, duplicate_requests); // 重复请求数量
    put_u32(message->payload + 20, bad_frames);         // 异常通信帧数量
    put_u32(message->payload + 24, rejected_requests);  // 被拒绝的请求数量
    put_u32(message->payload + 28, last_sequence);      // 最近处理的请求序号

    /* 3. 系统健康监控状态 */
    put_u32(message->payload + 32, health.ready);          // 监控模块是否就绪
    put_u32(message->payload + 36, health.faults);         // 当前故障标志位
    put_u32(message->payload + 40, health.latched_faults); // 历史锁存故障标志位
    put_u32(message->payload + 44, health.checks);         // 监控检查累计次数

    /* 4. 各任务运行状态与硬件故障 */
    put_u32(message->payload + 48, health.ipc_age_ms); // 距离 IPC 最近一次运行标记的时间（ms）
    put_u32(message->payload + 52,
            health.control_age_ms); // 距离 Control 最近一次运行标记的时间（ms）
    put_u32(message->payload + 56, health.monitor_age_ms); // 距离 Monitor 最近一次检查的时间（ms）
    put_u32(message->payload + 60, health.led_fault); // LED 驱动故障标志
}

/* 初始化 IPC 响应消息，继承请求的会话、序号和操作码 */
static void response_begin(const AMPMessage *request, AMPMessage *response)
{
    /* 1. 设置响应类型并清空负载。 */
    /* 2. 继承请求的会话、序号和操作码，默认返回成功。 */

    response->type = AMP_MSG_RESPONSE;      // 设置消息类型为响应
    response->payload_length = 0;           // 初始化响应负载长度为0
    response->session = request->session;   // 复制请求的会话ID
    response->sequence = request->sequence; // 复制请求序号
    response->opcode = request->opcode;     // 复制请求的操作码
    response->status = AMP_RESULT_OK;       // 初始化状态为成功
}

static int same_request(const AMPMessage *a, const AMPMessage *b)
{
    /* 1. 比较消息头与负载长度。 */
    /* 2. 逐字节比较负载，相同序号但内容不同不视为重试。 */
    /* 3. 返回请求是否完全一致。 */

    uint32_t i;

    if (a->type != b->type || a->session != b->session || a->sequence != b->sequence ||
        a->opcode != b->opcode || a->status != b->status ||
        a->payload_length != b->payload_length) {
        return 0;
    }
    /* 逐字节比较负载，避免把同序号的不同命令误判为重试。 */
    for (i = 0; i < a->payload_length; ++i) {
        if (a->payload[i] != b->payload[i])
            return 0;
    }

    return 1; //所有字段一致，返回1
}

/* 缓存最近一次 IPC 请求及响应，用于重复请求去重 */
static void cache_result(const AMPMessage *request, const AMPMessage *response)
{
    /* 1. 保存已处理请求及对应响应。 */
    /* 2. 记录序号并标记缓存有效，用于重复请求重放。 */

    cached_request = *request;         // 保存本次 Linux 请求
    cached_response = *response;       // 保存 FreeRTOS 对应的响应
    last_sequence = request->sequence; // 记录本次请求序号
    have_cached_response = 1;          // 标记当前已有缓存结果
}

/* 处理 Linux 发送的 IPC 请求：校验、去重、命令分发及响应缓存 */
static void handle_request(const AMPMessage *request, AMPMessage *response)
{
    /* 1. 初始化响应并校验会话、序号及重复请求。 */
    /* 2. 按操作码执行同步请求或提交异步任务。 */
    /* 3. 保存执行状态及可重放的响应。 */

    uint32_t i;

    /* ==================== 1. 初始化响应消息 ==================== */

    response_begin(request, response); // 复制会话ID、请求序号、操作码，初始化响应状态

    /* ==================== 2. 检查请求基本字段 ==================== */

    /* 请求必须携带非零会话、非零序号，status 必须为零 */
    if (request->session == 0 || request->sequence == 0 || request->status != 0) {
        response->status = AMP_RESULT_BAD_PAYLOAD; // 返回参数错误
        ++rejected_requests;                       // 拒绝请求计数加1
        return;
    }

    /* ==================== 3. 检查通信会话 ==================== */

    /* 会话ID不同，说明 Linux 请求建立新会话 */
    if (request->session != active_session) {

        /* 新会话只能通过 HELLO 命令建立 */
        if (request->opcode != AMP_OP_HELLO) {
            response->status = AMP_RESULT_BAD_SESSION;
            ++rejected_requests;
            return;
        }

        /* HELLO 命令不能携带 payload */
        if (request->payload_length != 0) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            ++rejected_requests;
            return;
        }

        active_session = request->session; // 更新当前会话ID
        last_sequence = 0;                 // 重置请求序号记录
        have_cached_response = 0;          // 清除旧会话的缓存有效标志
    }

    /* ==================== 4. 检查重复请求与序号 ==================== */

    /* 4.1 当前请求序号与最近缓存的请求序号相同 */
    if (have_cached_response && request->sequence == last_sequence) {

        if (same_request(request, &cached_request)) {
            ++duplicate_requests;        // 重复请求计数加1
            *response = cached_response; // 直接返回缓存结果，不重复执行
        } else {
            ++rejected_requests;
            response->status = AMP_RESULT_BAD_SEQUENCE; // 序号相同但内容不同
        }

        return;
    }

    /* 4.2 拒绝旧序号请求，当前会话不支持序号回绕 */
    if (have_cached_response && request->sequence < last_sequence) {
        response->status = AMP_RESULT_BAD_SEQUENCE;
        ++rejected_requests;
        return;
    }

    /* ==================== 5. 根据 opcode 处理具体命令 ==================== */

    switch (request->opcode) {

        /* ---------- 5.1 CAN / UART / Modbus 总线操作 ---------- */

    case AMP_OP_MOTOR_STATUS:
    case AMP_OP_MOTOR_EXEC:
        if (amp_motor_ipc_handle(request, response)) return;
        break;

    case AMP_OP_BUS_EXEC: {
        AmpBusCommand command = {0}; // 初始化总线命令结构体

        /* 总线请求固定20字节：操作码4B、CAN ID 4B、长度4B、数据8B */
        if (request->payload_length != 20U) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 保存请求身份，并解码总线操作参数 */
        command.session = request->session;
        command.sequence = request->sequence;
        command.operation = get_u32(request->payload);  // 解析总线操作类型
        command.can_id = get_u32(request->payload + 4); // 解析 CAN ID
        command.length = get_u32(request->payload + 8); // 解析有效数据长度

        /* 检查操作范围、数据长度、CAN ID 及各操作的参数约束 */
        if (command.operation < AMP_BUS_OP_CAN_LOOPBACK ||
            command.operation > AMP_BUS_OP_CAN_RECOVER || command.length > 8U ||
            ((command.operation == AMP_BUS_OP_CAN_LOOPBACK ||
              command.operation == AMP_BUS_OP_CAN_SEND) &&
             command.can_id > 0x7FFU) ||
            (command.operation != AMP_BUS_OP_CAN_LOOPBACK &&
             command.operation != AMP_BUS_OP_CAN_SEND && command.can_id != 0U) ||
            (command.operation == AMP_BUS_OP_UART_LOOPBACK && command.length != 1U) ||
            ((command.operation == AMP_BUS_OP_MODBUS_MOCK ||
              command.operation == AMP_BUS_OP_MODBUS_RTU) &&
             command.length != 8U) ||
            (command.operation == AMP_BUS_OP_CAN_OPEN && command.length != 1U) ||
            (command.operation >= AMP_BUS_OP_CAN_RECEIVE && command.length != 0U)) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 复制 payload 中的8字节总线数据 */
        for (i = 0; i < 8U; ++i)
            command.data[i] = request->payload[12U + i];

        /* 检查未使用的数据字节是否全部为0 */
        for (i = command.length; i < 8U; ++i) {
            if (command.data[i] != 0U)
                break;
        }

        if (i != 8U) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 将总线命令提交给 BusTask，提交失败表示忙碌 */
        if (amp_bus_submit(&command) != 0) {
            response->status = AMP_RESULT_BUSY;
            break;
        }

        bus_request = *request; // 保存原始请求，供执行完成后匹配响应
        bus_pending = 1;        // 标记正在等待 BusTask 执行结果
        return;                 // 暂不返回最终结果，等待 BusTask 完成
    }

        /* ---------- 5.2 CAN 请求授权检查 ---------- */

    case AMP_OP_CAN_AUTHORIZE: {
        uint32_t channel, id, flags, length;
        int valid_length;

        /* CAN 授权请求固定80字节 */
        if (request->payload_length != 80U) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 解析 CAN 通道、帧ID、标志位和数据长度 */
        channel = get_u32(request->payload);
        id = get_u32(request->payload + 4);
        flags = get_u32(request->payload + 8);
        length = get_u32(request->payload + 12);

        /* 检查数据长度是否符合 CAN / CAN FD 规则 */
        valid_length = length <= 8U || length == 12U || length == 16U || length == 20U ||
                       length == 24U || length == 32U || length == 48U || length == 64U;

        /* 检查通道、标志位、标准/扩展帧ID及 CAN FD 参数是否合法 */
        if (channel > 1U || flags > 7U || id > ((flags & 1U) ? 0x1fffffffU : 0x7ffU) ||
            !valid_length || (!(flags & 2U) && length > 8U) || ((flags & 4U) && !(flags & 2U))) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 检查64字节数据区中未使用的字节是否全部为0 */
        for (i = length; i < 64U; ++i) {
            if (request->payload[16U + i] != 0U)
                break;
        }

        if (i != 64U) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
            break;
        }

        /* 授权通过：原样返回80字节请求数据 */
        /* 注意：这里只表示请求符合授权规则，不代表 CAN 硬件发送成功 */
        response->payload_length = 80U;

        for (i = 0; i < 80U; ++i)
            response->payload[i] = request->payload[i];

        break;
    }

        /* ---------- 5.3 HELLO 会话建立命令 ---------- */

    case AMP_OP_HELLO: /* HELLO 不允许携带数据，合法时保持默认成功状态 */
        if (request->payload_length != 0)
            response->status = AMP_RESULT_BAD_PAYLOAD;
        break;

        /* ---------- 5.4 PING 通信测试命令 ---------- */

    case AMP_OP_PING:                                       // Linux 请求测试通信
        response->payload_length = request->payload_length; // 响应长度与请求一致

        /* 原样返回 Linux 发送的数据，用于测试 IPC 通信 */
        for (i = 0; i < request->payload_length; ++i)
            response->payload[i] = request->payload[i];

        break;

        /* ---------- 5.5 查询 FreeRTOS 运行状态 ---------- */

    case AMP_OP_GET_STATUS: // Linux 请求获取系统状态
        if (request->payload_length != 0) {
            response->status = AMP_RESULT_BAD_PAYLOAD;
        } else {
            last_sequence = request->sequence; // 状态快照包含本次请求序号
            fill_status(response);             // 编码64字节系统状态数据
        }
        break;

        /* ---------- 5.6 LED 设置与状态查询 ---------- */

    case AMP_OP_LED_SET: // Linux 请求控制 LED
    case AMP_OP_LED_GET: {
        ControlCommand command = {0}; // 初始化控制命令结构体

        /* 保存当前请求的会话ID和序号 */
        command.session = request->session;
        command.sequence = request->sequence;

        /* LED_GET：查询当前 LED 工作状态 */
        if (request->opcode == AMP_OP_LED_GET) {

            /* 查询命令不能携带 payload */
            if (request->payload_length != 0) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            command.operation = CONTROL_LED_QUERY;

        } else {

            /* LED_SET：设置 LED 工作模式及周期 */
            uint32_t mode;
            uint32_t period_ms;

            /* 设置命令固定8字节：模式4B + 周期4B */
            if (request->payload_length != 8U) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            /* 解码 Linux 传来的 LED 参数 */
            mode = get_u32(request->payload);
            period_ms = get_u32(request->payload + 4);

            /* 根据模式选择具体的控制操作 */
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

            /* 闪烁模式：周期必须为100～10000ms，且是20ms的整数倍 */
            if (mode == AMP_LED_BLINK) {
                if (period_ms < 100U || period_ms > 10000U || period_ms % 20U != 0) {
                    response->status = AMP_RESULT_BAD_PAYLOAD;
                    break;
                }

                /* 常亮或关闭模式：周期必须为0 */
            } else if (period_ms != 0) {
                response->status = AMP_RESULT_BAD_PAYLOAD;
                break;
            }

            command.period_ms = period_ms; // 保存合法的 LED 周期参数
        }

        /* 保存原始请求，等待 ControlTask 返回实际执行结果 */
        control_request = *request;

        /* 将 LED 命令提交给 ControlTask */
        if (control_submit(&command) != 0) {
            response->status = AMP_RESULT_BUSY;
            break;
        }

        control_pending = 1; // 标记控制命令尚未取回执行结果
        return;              // 暂不缓存或返回最终执行结果
    }

        /* ---------- 5.7 不支持的命令 ---------- */

    default:
        response->status = AMP_RESULT_UNSUPPORTED; // 返回不支持的操作码
        break;
    }

    /* ==================== 6. 统计结果并缓存响应 ==================== */

    /* 请求被拒绝或立即处理失败时，更新统计计数 */
    if (response->status != AMP_RESULT_OK)
        ++rejected_requests;

    /* 缓存本次请求和响应，供后续重复请求直接返回 */
    cache_result(request, response);
}

static int collect_control_result(AMPMessage *response)
{
    /* 1. 尝试取回异步任务结果。 */
    /* 2. 把业务结果编码为 IPC 响应。 */
    /* 3. 更新请求缓存并清除等待状态。 */

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

static int collect_bus_result(AMPMessage *response)
{
    /* 1. 尝试取回异步任务结果。 */
    /* 2. 把业务结果编码为 IPC 响应。 */
    /* 3. 更新请求缓存并清除等待状态。 */

    AmpBusResult result;
    int received;
    uint32_t i;

    received = amp_bus_receive(&result);
    configASSERT(received >= 0);
    if (received == 0)
        return 0;

    configASSERT(result.session == bus_request.session);
    configASSERT(result.sequence == bus_request.sequence);
    configASSERT(result.length <= sizeof(result.data));

    response_begin(&bus_request, response);

    /*
     * Envelope OK means a BusTask result was received.
     * Actual operation status is signed int32 encoded at payload+4.
     *
     * Response, 36 bytes:
     * operation, operation status, length, clock Hz,
     * sample point permille, data[16].
     */
    response->payload_length = 36U;
    put_u32(response->payload, result.operation);
    put_u32(response->payload + 4, (uint32_t)result.status);
    put_u32(response->payload + 8, result.length);
    put_u32(response->payload + 12, result.clock_hz);
    put_u32(response->payload + 16, result.sample_permille);

    for (i = 0; i < 16U; ++i)
        response->payload[20U + i] = result.data[i];

    if (result.status != AMP_BUS_OK)
        ++rejected_requests;

    cache_result(&bus_request, response);
    bus_pending = 0;
    return 1;
}

void amp_ipc_init(void)
{
    /* 1. 初始化共享传输区和 IPC 会话状态。 */
    /* 2. 准备请求缓存及通信计数。 */

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
    bus_pending = 0;
    amp_motor_ipc_init();

    result = amp_transport_init(transport);
    configASSERT(result == AMP_TRANSPORT_OK);
}

void amp_ipc_task(void *argument)
{
    /* 1. 循环收集异步任务结果并接收 Linux 请求。 */
    /* 2. 执行校验及命令分发，发送响应。 */
    /* 3. 周期发布心跳并上报 IPC 任务存活状态。 */

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
    configASSERT(amp_transport_ready(transport) == AMP_TRANSPORT_OK);

    last_heartbeat = xTaskGetTickCount();

    for (;;) {
        int result;
        TickType_t now;
        monitor_beat(MONITOR_IPC);
        /*
         * 控制任务完成后，才产生最终响应。
         * IpcTask 不阻塞等待队列。
         */
        if (amp_motor_ipc_pending()) {
            AMPMessage original;
            if (amp_motor_ipc_collect(&original, &pending_response)) {
                if (pending_response.status != AMP_RESULT_OK ||
                    (pending_response.payload_length == AMP_MOTOR_WIRE_STATUS_SIZE &&
                     amp_motor_wire_get32(pending_response.payload + 16) != 0))
                    ++rejected_requests;
                cache_result(&original, &pending_response);
                response_pending = 1;
            }
        }

        if (bus_pending) {
            if (collect_bus_result(&pending_response))
                response_pending = 1;
        }

        if (control_pending) {
            if (collect_control_result(&pending_response))
                response_pending = 1;
        }

        /*
         * 保留响应直到成功写入返回通道。
         * Linux 未消费旧消息时，不覆盖共享槽。
         */
        if (response_pending) {
            result = amp_channel_send(&transport->rtos_to_linux, &pending_response);

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
        if (!response_pending && !control_pending && !bus_pending && !amp_motor_ipc_pending()) {
            result = amp_channel_receive(&transport->linux_to_rtos, &request);

            if (result == AMP_TRANSPORT_OK) {
                if (request.type == AMP_MSG_REQUEST) {
                    ++received_requests;

                    handle_request(&request, &pending_response);

                    /*
                     * 普通命令立即产生响应。
                     * LED 命令等待 ControlTask 的执行结果。
                     */
                    if (!control_pending && !bus_pending && !amp_motor_ipc_pending())
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
        if (!response_pending && active_session != 0 &&
            (TickType_t)(now - last_heartbeat) >= heartbeat_period) {
            heartbeat.type = AMP_MSG_HEARTBEAT;
            heartbeat.session = active_session;
            heartbeat.sequence = heartbeat_sequence + 1U;
            heartbeat.opcode = AMP_OP_GET_STATUS;
            heartbeat.status = AMP_RESULT_OK;

            fill_status(&heartbeat);

            result = amp_channel_send(&transport->rtos_to_linux, &heartbeat);

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
