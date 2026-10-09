#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "monitor_task.h"
#include "board_led.h"
#include "control_task.h"

static QueueHandle_t command_queue;
static QueueHandle_t result_queue;

/* 只由调用接口的 IpcTask 使用。 */
static int command_in_flight;

/* 只由 ControlTask 使用。 */
static uint32_t led_mode;
static uint32_t led_output;
static uint32_t blink_period_ms;

static TickType_t half_period;
static TickType_t last_edge;
static int hardware_ok;

static void execute_command(const ControlCommand *command,
                            ControlResult *result)
{
    uint32_t status = CONTROL_OK;

    result->session = command->session;
    result->sequence = command->sequence;

    if (!hardware_ok) {
        status = CONTROL_HW_ERROR;
    } else {
        switch (command->operation) {
        case CONTROL_LED_OFF:
        case CONTROL_LED_ON:
            if (command->period_ms != 0) {
                status = CONTROL_BAD_ARGUMENT;
                break;
            }

            if (board_led_set(
                    command->operation == CONTROL_LED_ON) != 0) {
                hardware_ok = 0;
                status = CONTROL_HW_ERROR;
                break;
            }

            led_mode = command->operation;
            led_output = board_led_get();
            blink_period_ms = 0;
            break;

        case CONTROL_LED_BLINK:
            /*
             * 第一版只支持 100～10000 ms 的完整周期，
             * 且为 20 ms 的整数倍。
             * 当前 100 Hz Tick 下，半周期可以精确表示。
             */
            if (command->period_ms < 100U ||
                command->period_ms > 10000U ||
                command->period_ms % 20U != 0) {
                status = CONTROL_BAD_ARGUMENT;
                break;
            }

            if (board_led_set(1) != 0) {
                hardware_ok = 0;
                status = CONTROL_HW_ERROR;
                break;
            }

            led_mode = CONTROL_LED_BLINK;
            led_output = 1;
            blink_period_ms = command->period_ms;
            half_period = pdMS_TO_TICKS(blink_period_ms / 2U);
            last_edge = xTaskGetTickCount();
            break;

        case CONTROL_LED_QUERY:
            if (command->period_ms != 0)
                status = CONTROL_BAD_ARGUMENT;
            break;

        default:
            status = CONTROL_BAD_ARGUMENT;
            break;
        }
    }

    if (hardware_ok)
        led_output = board_led_get();

    result->status = status;
    result->mode = led_mode;
    result->output = led_output;
    result->period_ms = blink_period_ms;
}

static void control_task(void *argument)
{
    ControlCommand command;
    ControlResult result;
    const TickType_t idle_wait = pdMS_TO_TICKS(100);

    (void)argument;

    configASSERT(configTICK_RATE_HZ == 100);
    configASSERT(idle_wait > 0);

    led_mode = CONTROL_LED_OFF;
    led_output = 0;
    blink_period_ms = 0;
    hardware_ok = (board_led_init() == 0);

    for (;;) {
        TickType_t wait_ticks = idle_wait;

        monitor_beat(MONITOR_CONTROL);

        if (!hardware_ok)
            monitor_report_led_fault();

        if (hardware_ok && led_mode == CONTROL_LED_BLINK) {
            TickType_t now = xTaskGetTickCount();
            TickType_t elapsed = now - last_edge;

            if (elapsed >= half_period) {
                /*
                 * 发生调度延迟时，只更新到当前应有状态，
                 * 不连续补发多个快速翻转。
                 */
                TickType_t edges = elapsed / half_period;

                if (edges & 1U) {
                    if (board_led_set(led_output ^ 1U) != 0) {
                        hardware_ok = 0;
                    } else {
                        led_output ^= 1U;
                    }
                }

                last_edge += edges * half_period;
            }

            if (hardware_ok) {
                now = xTaskGetTickCount();
                elapsed = now - last_edge;

                wait_ticks = elapsed >= half_period ?
                             0 : half_period - elapsed;
            }
        }
        /*
         * 慢速闪烁时也定期醒来报告运行进度。
         * last_edge 仍负责维持原闪烁时间基准。
         */
        if (wait_ticks > idle_wait)
            wait_ticks = idle_wait;

        if (!hardware_ok)
            monitor_report_led_fault();

        /*
         * 等待期间可被新命令唤醒，
         * OFF/ON 命令不必等到一个闪烁周期结束。
         */
        if (xQueueReceive(command_queue,
                          &command,
                          wait_ticks) == pdPASS) {
            execute_command(&command, &result);
            if (!hardware_ok)
                monitor_report_led_fault();

            /*
             * 单请求约束保证这里有一个空结果槽。
             * 不等待 Linux 读取响应，不阻塞后续闪烁。
             */
            configASSERT(
                xQueueSend(result_queue, &result, 0) == pdPASS);
        }
    }
}

int control_init(void)
{
    BaseType_t created;

    if (command_queue != NULL || result_queue != NULL)
        return -1;

    command_queue = xQueueCreate(1, sizeof(ControlCommand));
    if (command_queue == NULL)
        return -1;

    result_queue = xQueueCreate(1, sizeof(ControlResult));
    if (result_queue == NULL) {
        vQueueDelete(command_queue);
        command_queue = NULL;
        return -1;
    }

    command_in_flight = 0;

    created = xTaskCreate(control_task,
                          "Control",
                          configMINIMAL_STACK_SIZE,
                          NULL,
                          3,
                          NULL);

    if (created != pdPASS) {
        vQueueDelete(command_queue);
        vQueueDelete(result_queue);
        command_queue = NULL;
        result_queue = NULL;
        return -1;
    }

    return 0;
}

int control_submit(const ControlCommand *command)
{
    if (command == NULL ||
        command_queue == NULL ||
        command_in_flight) {
        return -1;
    }

    if (xQueueSend(command_queue, command, 0) != pdPASS)
        return -1;

    command_in_flight = 1;
    return 0;
}

int control_receive(ControlResult *result)
{
    if (result == NULL || result_queue == NULL)
        return -1;

    if (!command_in_flight)
        return 0;

    if (xQueueReceive(result_queue, result, 0) != pdPASS)
        return 0;

    command_in_flight = 0;
    return 1;
}