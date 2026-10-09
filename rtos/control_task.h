#ifndef AMP_CONTROL_TASK_H
#define AMP_CONTROL_TASK_H

#include <stdint.h>

#define CONTROL_LED_OFF      0U
#define CONTROL_LED_ON       1U
#define CONTROL_LED_BLINK    2U
#define CONTROL_LED_QUERY    3U

#define CONTROL_OK           0U
#define CONTROL_BAD_ARGUMENT 1U
#define CONTROL_HW_ERROR     2U

typedef struct {
    uint64_t session;
    uint32_t sequence;
    uint32_t operation;

    /* BLINK 使用：完整亮灭周期，单位 ms。 */
    uint32_t period_ms;
} ControlCommand;

typedef struct {
    uint64_t session;
    uint32_t sequence;
    uint32_t status;

    uint32_t mode;
    uint32_t output;
    uint32_t period_ms;
} ControlResult;

/* 创建队列和任务。成功返回 0，失败返回 -1。 */
int control_init(void);

/*
 * 仅供单个 IpcTask 调用。
 * 同时只允许一条未取回结果的控制命令。
 *
 * submit：0 已入队，-1 参数错误或忙。
 * receive：1 已取回，0 暂无结果，-1 参数错误。
 */
int control_submit(const ControlCommand *command);
int control_receive(ControlResult *result);

#endif