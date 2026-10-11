#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "bus_task.h"
#include "drivers/amp_bus_handoff.h"
#include "drivers/can1.h"
#include "drivers/can_timing.h"
#include "drivers/uart3.h"
#include "protocol/modbus_mock.h"

static QueueHandle_t command_queue;
static QueueHandle_t result_queue;
static int outstanding;
static AmpModbusMock mock;

static void clear_result(AmpBusResult *result,
                           const AmpBusCommand *command)
{
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

static void can_loopback(const AmpBusCommand *command,
                          AmpBusResult *result)
{
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

static void uart_loopback(const AmpBusCommand *command,
                           AmpBusResult *result)
{
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

    if (ret == AMP_BUS_OK &&
        (done != 1U || received != command->data[0]))
        ret = AMP_BUS_HW_ERROR;

    result->status = ret;
    if (ret == AMP_BUS_OK) {
        result->length = 1U;
        result->data[0] = received;
    }
}

static void mock_request(const AmpBusCommand *command,
                           AmpBusResult *result)
{
    size_t length = 0U;
    int ret;

    if (command->length != 8U)
        return;

    ret = amp_mb_mock_reply(&mock,
                            command->data, command->length,
                            result->data, sizeof(result->data),
                            &length);

    if (ret == 1) {
        /*
         * 模拟从站决定“不响应”。
         * 此处不是实际等待超时，返回值不能作为计时验收证据。
         */
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
    AmpBusCommand command;
    AmpBusResult result;

    (void)argument;

    for (;;) {
        if (xQueueReceive(command_queue, &command,
                          portMAX_DELAY) != pdTRUE)
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
    if (command_queue != NULL || result_queue != NULL)
        return -1;

    if (amp_mb_mock_init(&mock, 1U))
        return -1;

    command_queue = xQueueCreate(1U, sizeof(AmpBusCommand));
    result_queue = xQueueCreate(1U, sizeof(AmpBusResult));

    if (command_queue == NULL || result_queue == NULL)
        goto fail;

    if (xTaskCreate(bus_task, "BusTask", 1024U, NULL, 2U, NULL)
        != pdPASS)
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
    if (command == NULL || command_queue == NULL ||
        result_queue == NULL || outstanding)
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
    if (result == NULL || result_queue == NULL)
        return -1;

    if (xQueueReceive(result_queue, result, 0) != pdTRUE)
        return 0;

    outstanding = 0;
    return 1;
}
