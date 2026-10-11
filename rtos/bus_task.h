#ifndef AMP_BUS_TASK_H
#define AMP_BUS_TASK_H

#include <stdint.h>

#define AMP_BUS_OP_CAN_LOOPBACK  1U
#define AMP_BUS_OP_UART_LOOPBACK 2U
#define AMP_BUS_OP_MODBUS_MOCK   3U

typedef struct {
    uint64_t session;
    uint32_t sequence;
    uint32_t operation;

    /* CAN: standard ID, length 0..8, data. */
    uint32_t can_id;
    uint32_t length;
    uint8_t data[8];

    /* UART: data[0]. Modbus mock: complete 8-byte request. */
} AmpBusCommand;

typedef struct {
    uint64_t session;
    uint32_t sequence;
    uint32_t operation;
    int32_t status;

    uint32_t length;
    uint8_t data[16];

    /* CAN: actual clock/sample point, UART: actual clock. */
    uint32_t clock_hz;
    uint32_t sample_permille;
} AmpBusResult;

/*
 * 单个 IpcTask 调用 submit/receive。
 * 一次只允许一个未取回结果的请求。
 * submit: 0成功，-1参数错误或忙。
 * receive: 1有结果，0无结果，-1参数错误。
 */
int amp_bus_task_init(void);
int amp_bus_submit(const AmpBusCommand *command);
int amp_bus_receive(AmpBusResult *result);

#endif
