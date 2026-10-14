#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "FreeRTOS.h" /* 引入 FreeRTOS 内核类型、配置及基础接口 */
#include "task.h"     /* 引入 FreeRTOS 任务、延时及调度接口 */
#include "queue.h"    /* 引入 FreeRTOS 队列接口 */

#include "bus_task.h"                /* 引入总线命令、结果及任务队列接口 */
#include "drivers/amp_bus_handoff.h" /* 引入总线资源交接、MMIO 访问及截止时间接口 */
#include "drivers/can1.h"            /* 引入 CAN1 帧结构、收发及诊断接口 */
#include "drivers/can_timing.h"      /* 引入 CAN 位时序计算接口 */
#include "drivers/uart3.h"           /* 引入 UART3 初始化、收发及接收时间戳接口 */
#include "protocol/modbus_mock.h"    /* 引入 Modbus 模拟从站接口 */

static QueueHandle_t
    command_queue; /* command_queue：待执行命令队列，由提交接口写入、工作任务读取 */
static QueueHandle_t result_queue; /* result_queue：已完成结果队列，由工作任务写入、IPC 接口读取 */
static int outstanding;    /* outstanding：总线请求正在执行的标志 */
static AmpModbusMock mock; /* mock：Modbus 模拟从站状态 */

static void clear_result(AmpBusResult *result, const AmpBusCommand *command)
{
    /* 1. 清零总线结果，避免残留上一次请求数据。 */
    /* 2. 填入当前请求的会话、序号和操作码。 */

    unsigned int i;

    result->session = command->session;
    result->sequence = command->sequence;
    result->operation = command->operation;
    result->status = AMP_BUS_ARGUMENT;
    result->length = 0U;
    result->clock_hz = 0U;
    result->sample_permille = 0U;

    for (i = 0; i < sizeof(result->data); ++i)
        result->data[i] = 0U;
}

static void can_loopback(const AmpBusCommand *command, AmpBusResult *result)
{
    /* 1. 初始化 CAN 回环测试参数。 */
    /* 2. 发送测试帧并接收回环帧。 */
    /* 3. 比较标识符、长度与数据，填入执行结果。 */

    AmpCanTiming timing;
    AmpCanFrame tx = {0}, rx = {0};
    unsigned int i;
    int ret;

    if (command->can_id > 0x7FFU || command->length > 8U)
        return;

    result->clock_hz = amp_bus_clock_hz(AMP_BUS_CAN1);
    if (result->clock_hz == 0U) {
        result->status = AMP_BUS_NOT_READY;
        return;
    }

    if (amp_can_timing(result->clock_hz, 500000U, &timing)) {
        result->status = AMP_BUS_UNSUPPORTED;
        return;
    }

    result->sample_permille = timing.sample_permille;

    ret = amp_can1_init(timing.nbtp, 1);
    if (ret != AMP_BUS_OK) {
        result->status = ret;
        return;
    }

    tx.id = command->can_id;
    tx.length = (uint8_t)command->length;
    for (i = 0; i < tx.length; ++i)
        tx.data[i] = command->data[i];

    ret = amp_can1_send(&tx, 10000U);
    if (ret == AMP_BUS_OK)
        ret = amp_can1_receive(&rx, 10000U);

    if (ret != AMP_BUS_OK) {
        result->status = ret;
        return;
    }

    if (rx.id != tx.id || rx.length != tx.length) {
        result->status = AMP_BUS_HW_ERROR;
        return;
    }

    for (i = 0; i < tx.length; ++i) {
        if (rx.data[i] != tx.data[i]) {
            result->status = AMP_BUS_HW_ERROR;
            return;
        }
    }

    result->length = rx.length;
    for (i = 0; i < rx.length; ++i)
        result->data[i] = rx.data[i];

    result->status = AMP_BUS_OK;
}

static void uart_loopback(const AmpBusCommand *command, AmpBusResult *result)
{
    /* 1. 初始化 UART 回环参数。 */
    /* 2. 发送测试字节并按超时接收。 */
    /* 3. 比较收发数据，填入执行结果。 */

    uint8_t received = 0U;
    size_t done = 0U;
    int ret;

    if (command->length != 1U)
        return;

    result->clock_hz = amp_bus_clock_hz(AMP_BUS_UART3);
    if (result->clock_hz == 0U) {
        result->status = AMP_BUS_NOT_READY;
        return;
    }

    ret = amp_uart3_init(result->clock_hz, 115200U, 0x03U, 1);
    if (ret == AMP_BUS_OK)
        ret = amp_uart3_write(command->data, 1U, 10000U, &done);

    if (ret == AMP_BUS_OK && done != 1U)
        ret = AMP_BUS_HW_ERROR;

    if (ret == AMP_BUS_OK)
        ret = amp_uart3_read(&received, 1U, 10000U, &done);

    if (ret == AMP_BUS_OK && (done != 1U || received != command->data[0]))
        ret = AMP_BUS_HW_ERROR;

    result->status = ret;
    if (ret == AMP_BUS_OK) {
        result->length = 1U;
        result->data[0] = received;
    }
}

static void mock_request(const AmpBusCommand *command, AmpBusResult *result)
{
    /* 1. 构建 Modbus 请求帧。 */
    /* 2. 交给模拟从站生成响应。 */
    /* 3. 解析响应并保存协议结果。 */

    size_t length = 0U;
    int ret;

    if (command->length != 8U)
        return;

    ret = amp_mb_mock_reply(&mock, command->data, command->length, result->data,
                            sizeof(result->data), &length);

    if (ret == 1) {
        result->status = AMP_BUS_TIMEOUT;
        return;
    }

    if (ret != 0) {
        result->status = AMP_BUS_ARGUMENT;
        return;
    }

    result->length = (uint32_t)length;
    result->status = AMP_BUS_OK;
}

static void bus_task(void *argument)
{
    /* 1. 等待总线命令队列。 */
    /* 2. 按操作码执行 CAN、UART 或 Modbus 请求。 */
    /* 3. 将执行结果送回结果队列。 */

    AmpBusCommand command;
    AmpBusResult result;

    (void)argument;

    for (;;) {
        if (xQueueReceive(command_queue, &command, portMAX_DELAY) != pdTRUE)
            continue;

        clear_result(&result, &command);

        switch (command.operation) {
        case AMP_BUS_OP_CAN_LOOPBACK:
            can_loopback(&command, &result);
            break;

        case AMP_BUS_OP_UART_LOOPBACK:
            uart_loopback(&command, &result);
            break;

        case AMP_BUS_OP_MODBUS_MOCK:
            mock_request(&command, &result);
            break;

        default:
            result.status = AMP_BUS_UNSUPPORTED;
            break;
        }

        /*
         * Single outstanding command guarantees one free result slot.
         * If this invariant is broken, fail through configASSERT.
         */
        {
            BaseType_t sent = xQueueSend(result_queue, &result, 0);
            configASSERT(sent == pdTRUE);
        }
    }
}




int amp_bus_task_init(void)
{
    /* 1. 创建命令队列和结果队列并检查分配结果。 */
    /* 2. 创建工作任务，交由 FreeRTOS 调度。 */

    if (command_queue != NULL || result_queue != NULL)
        return -1;

    if (amp_mb_mock_init(&mock, 1U))
        return -1;

    command_queue = xQueueCreate(1U, sizeof(AmpBusCommand));
    result_queue = xQueueCreate(1U, sizeof(AmpBusResult));

    if (command_queue == NULL || result_queue == NULL)
        goto fail;

    if (xTaskCreate(bus_task, "BusTask", 1024U, NULL, 2U, NULL) != pdPASS)
        goto fail;

    outstanding = 0;
    return 0;

fail:
    if (command_queue != NULL)
        vQueueDelete(command_queue);
    if (result_queue != NULL)
        vQueueDelete(result_queue);

    command_queue = NULL;
    result_queue = NULL;
    return -1;
}

int amp_bus_submit(const AmpBusCommand *command)
{
    /* 1. 检查参数及是否已有未完成命令。 */
    /* 2. 将命令放入队列。 */
    /* 3. 成功后标记请求在途，防止覆盖未取回结果。 */

    if (command == NULL || command_queue == NULL || result_queue == NULL || outstanding)
        return -1;

    outstanding = 1;
    if (xQueueSend(command_queue, command, 0) != pdTRUE) {
        outstanding = 0;
        return -1;
    }

    return 0;
}

int amp_bus_receive(AmpBusResult *result)
{
    /* 1. 检查结果指针并尝试读取结果队列。 */
    /* 2. 取回结果后清除在途标志，允许提交下一条命令。 */

    if (result == NULL || result_queue == NULL)
        return -1;

    if (xQueueReceive(result_queue, result, 0) != pdTRUE)
        return 0;

    outstanding = 0;
    return 1;
}
