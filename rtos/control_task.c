#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "FreeRTOS.h"     /* 引入 FreeRTOS 内核类型、配置及基础接口 */
#include "task.h"         /* 引入 FreeRTOS 任务、延时及调度接口 */
#include "queue.h"        /* 引入 FreeRTOS 队列接口 */
#include "monitor_task.h" /* 引入任务心跳、故障位及健康快照接口 */
#include "board_led.h"    /* 引入板载 LED 驱动接口 */
#include "control_task.h" /* 引入 LED 控制命令、结果及任务接口 */

static QueueHandle_t
    command_queue; /* command_queue：待执行命令队列，由提交接口写入、工作任务读取 */
static QueueHandle_t result_queue; /* result_queue：已完成结果队列，由工作任务写入、IPC 接口读取 */

/* 只由调用接口的 IpcTask 使用。 */
static int command_in_flight; /* command_in_flight：IPC 已提交但尚未取回结果的命令标志 */

/* 只由 ControlTask 使用。 */
static uint32_t led_mode;        /* led_mode：当前 LED 工作模式 */
static uint32_t led_output;      /* led_output：当前 LED 输出状态 */
static uint32_t blink_period_ms; /* blink_period_ms：LED 完整闪烁周期，单位 ms */

static TickType_t half_period; /* half_period：LED 两次翻转之间的 Tick 数 */
static TickType_t last_edge;   /* last_edge：LED 上一次翻转的 Tick 时间 */
static int hardware_ok;        /* hardware_ok：LED 硬件初始化和操作是否正常 */

static void execute_command(const ControlCommand *command, ControlResult *result)
{
    /* 1. 继承请求的会话和序号。 */
    /* 2. 校验 LED 操作及周期参数并执行硬件操作。 */
    /* 3. 更新 LED 状态，将状态码和当前模式写入结果。 */

    uint32_t status = CONTROL_OK;

    result->session = command->session;
    result->sequence = command->sequence;

    //检查LED是否正常
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

            if (board_led_set(command->operation == CONTROL_LED_ON) != 0) {
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
            if (command->period_ms < 100U || command->period_ms > 10000U ||
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
    /* 1. 初始化 LED 状态及硬件。 */
    /* 2. 循环接收命令，同时按半周期维护闪烁输出。 */
    /* 3. 上报心跳和硬件故障，将命令结果放入返回队列。 */

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

                wait_ticks = elapsed >= half_period ? 0 : half_period - elapsed;
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


        if (xQueueReceive(command_queue, &command, wait_ticks) == pdPASS) {
            execute_command(&command, &result);
            if (!hardware_ok)
                monitor_report_led_fault();
            configASSERT(xQueueSend(result_queue, &result, 0) == pdPASS);
        }
    }
}

int control_init(void)
{
    /* 1. 创建命令队列和结果队列并检查分配结果。 */
    /* 2. 创建工作任务，交由 FreeRTOS 调度。 */

    BaseType_t created;
    //检查是否初始化
    if (command_queue != NULL || result_queue != NULL)
        return -1;
    //创建命令队列
    command_queue = xQueueCreate(1, sizeof(ControlCommand));
    if (command_queue == NULL)
        return -1;
    //创建结果队列
    result_queue = xQueueCreate(1, sizeof(ControlResult));
    if (result_queue == NULL) {
        vQueueDelete(command_queue);
        command_queue = NULL;
        return -1;
    }
    //初始化命令状态
    command_in_flight = 0;
    //创建任务
    created = xTaskCreate(control_task, "Control", configMINIMAL_STACK_SIZE, NULL, 3, NULL);

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
    /* 1. 检查参数及是否已有未完成命令。 */
    /* 2. 将命令放入队列。 */
    /* 3. 成功后标记请求在途，防止覆盖未取回结果。 */

    if (command == NULL || command_queue == NULL || command_in_flight) {
        return -1;
    }

    //把 LED 控制命令发送到 FreeRTOS 的命令队列中
    if (xQueueSend(command_queue, command, 0) != pdPASS)
        return -1;

    command_in_flight = 1;
    return 0;
}

int control_receive(ControlResult *result)
{
    /* 1. 检查结果指针并尝试读取结果队列。 */
    /* 2. 取回结果后清除在途标志，允许提交下一条命令。 */

    if (result == NULL || result_queue == NULL)
        return -1;

    if (!command_in_flight)
        return 0;

    if (xQueueReceive(result_queue, result, 0) != pdPASS)
        return 0;

    command_in_flight = 0;
    return 1;
}
