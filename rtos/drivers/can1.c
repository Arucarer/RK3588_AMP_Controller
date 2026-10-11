#include <stddef.h>
#include <stdint.h>

#include "amp_bus_resources.h"
#include "amp_bus_handoff.h"
#include "can1.h"

/* 对照当前 SDK rockchip_canfd.c。 */
#define REG_MODE      0x000U
#define REG_CMD       0x004U
#define REG_STATE     0x008U
#define REG_INT       0x00CU
#define REG_INT_MASK  0x010U
#define REG_RX_ERROR  0x034U
#define REG_TX_ERROR  0x038U
#define REG_IDCODE    0x03CU
#define REG_IDMASK    0x040U
#define REG_NBTP      0x100U
#define REG_RXFC      0x118U
#define REG_AFC       0x11CU
#define REG_TXFIC     0x200U
#define REG_TXID      0x204U
#define REG_TXDAT0    0x208U
#define REG_RXFRD     0x400U

#define MODE_WORK       (1U << 0)
#define MODE_SELF_TEST  (1U << 2)
#define MODE_LOOPBACK   (1U << 4)
#define MODE_SPACE_RX   (1U << 12)
#define MODE_FDOE       (1U << 15)

#define INT_RX_FINISH   (1U << 0)
#define INT_TX_FINISH   (1U << 1)
#define INT_ERRORS      ((1U << 2) | (1U << 3) | \
                         (1U << 4) | (1U << 6) | \
                         (1U << 8) | (1U << 9))
#define BUS_OFF         (1U << 5)

static int initialized;
static int poisoned;

static uint32_t rd(uint32_t offset)
{
    return amp_bus_read32(AMP_CAN1_BASE + offset);
}

static void wr(uint32_t offset, uint32_t value)
{
    amp_bus_write32(AMP_CAN1_BASE + offset, value);
}

static int available(void)
{
    if (!initialized || poisoned)
        return AMP_BUS_NOT_READY;

    return amp_bus_require_ready(AMP_BUS_CAN1);
}

int amp_can1_init(uint32_t nominal_timing, int loopback)
{
    uint32_t mode;
    unsigned int i;
    int result;

    initialized = 0;
    poisoned = 0;

    if (loopback != 0 && loopback != 1)
        return AMP_BUS_ARGUMENT;

    result = amp_bus_require_ready(AMP_BUS_CAN1);
    if (result != AMP_BUS_OK)
        return result;

    wr(REG_MODE, 0U);

    /* 本阶段轮询；屏蔽控制器中断。 */
    wr(REG_INT_MASK, UINT32_MAX);
    wr(REG_CMD, 0U);
    wr(REG_INT, UINT32_MAX);

    wr(REG_IDCODE, 0U);
    wr(REG_IDMASK, 0x1FFFFFFFU);

    for (i = 0; i < 5U; ++i) {
        wr(0x120U + i * 8U, 0U);
        wr(0x124U + i * 8U, 0x1FFFFFFFU);
    }

    wr(REG_AFC, 0U);
    wr(REG_RXFC, 1U);
    wr(REG_NBTP, nominal_timing);

    /*
     * SDK 在经典 CAN 匹配分支也设置 FDOE。
     * 此处保留对应初始化行为，但发送接口禁止 FD 帧。
     */
    mode = MODE_FDOE | MODE_WORK;
    if (loopback)
        mode |= MODE_SELF_TEST | MODE_LOOPBACK;

    wr(REG_MODE, mode);

    if ((rd(REG_MODE) & mode) != mode)
        return AMP_BUS_HW_ERROR;

    initialized = 1;
    return AMP_BUS_OK;
}

int amp_can1_send(const AmpCanFrame *frame, uint32_t timeout_us)
{
    AmpBusDeadline deadline;
    uint32_t words[2] = {0U, 0U};
    uint32_t pending;
    unsigned int i;
    int result;

    if (frame == NULL || frame->id > 0x7FFU ||
        frame->length > 8U)
        return AMP_BUS_ARGUMENT;

    result = available();
    if (result != AMP_BUS_OK)
        return result;

    result = amp_bus_deadline_start(&deadline, timeout_us);
    if (result != AMP_BUS_OK)
        return result;

    if (rd(REG_CMD) & 3U)
        return AMP_BUS_NOT_READY;

    if (rd(REG_STATE) & BUS_OFF)
        return AMP_BUS_HW_ERROR;

    for (i = 0; i < frame->length; ++i)
        words[i / 4U] |=
            (uint32_t)frame->data[i] << ((i % 4U) * 8U);

    wr(REG_INT, INT_TX_FINISH);
    wr(REG_TXID, frame->id);
    wr(REG_TXFIC, frame->length);
    wr(REG_TXDAT0, words[0]);
    wr(REG_TXDAT0 + 4U, words[1]);

    wr(REG_MODE, rd(REG_MODE) | MODE_SPACE_RX);
    wr(REG_CMD, 1U);
    wr(REG_MODE, rd(REG_MODE) & ~MODE_SPACE_RX);

    for (;;) {
        pending = rd(REG_INT);

        if ((pending & INT_ERRORS) || (rd(REG_STATE) & BUS_OFF)) {
            poisoned = 1;
            return AMP_BUS_HW_ERROR;
        }

        if ((pending & INT_TX_FINISH) && !(rd(REG_CMD) & 3U)) {
            wr(REG_CMD, 0U);
            wr(REG_INT, INT_TX_FINISH);
            return AMP_BUS_OK;
        }

        if (amp_bus_deadline_expired(&deadline)) {
            /*
             * 超时不等于硬件已取消发送。
             * 禁止继续复用，必须由上层受控重新初始化。
             */
            poisoned = 1;
            return AMP_BUS_TIMEOUT;
        }

        __asm__ volatile("yield");
    }
}

int amp_can1_receive(AmpCanFrame *frame, uint32_t timeout_us)
{
    AmpBusDeadline deadline;
    uint32_t fic, id, words[16];
    unsigned int i;
    int result;

    if (frame == NULL)
        return AMP_BUS_ARGUMENT;

    result = available();
    if (result != AMP_BUS_OK)
        return result;

    result = amp_bus_deadline_start(&deadline, timeout_us);
    if (result != AMP_BUS_OK)
        return result;

    /* rockchip,can-2.0 分支的 FIFO count 位于 [7:5]。 */
    while (((rd(REG_RXFC) >> 5) & 7U) == 0U) {
        if ((rd(REG_INT) & INT_ERRORS) || (rd(REG_STATE) & BUS_OFF))
            return AMP_BUS_HW_ERROR;

        if (amp_bus_deadline_expired(&deadline))
            return AMP_BUS_TIMEOUT;

        __asm__ volatile("yield");
    }

    /*
     * RXFRD 是连续弹出窗口：
     * FIC、ID、timestamp、16 个 data word。
     * 不读取普通 RX 寄存器来伪造 FIFO 消费。
     */
    fic = rd(REG_RXFRD);
    id = rd(REG_RXFRD);
    (void)rd(REG_RXFRD);

    for (i = 0; i < 16U; ++i)
        words[i] = rd(REG_RXFRD);

    wr(REG_INT, INT_RX_FINISH);

    if ((fic & ((1U << 7) | (1U << 6) | (1U << 5))) ||
        (fic & 0xFU) > 8U || id > 0x7FFU)
        return AMP_BUS_UNSUPPORTED;

    frame->id = id;
    frame->length = (uint8_t)(fic & 0xFU);

    for (i = 0; i < 8U; ++i)
        frame->data[i] = i < frame->length ?
            (uint8_t)(words[i / 4U] >> ((i % 4U) * 8U)) : 0U;

    return AMP_BUS_OK;
}

int amp_can1_status(AmpCanStatus *status)
{
    int result;

    if (status == NULL)
        return AMP_BUS_ARGUMENT;

    /* 即使 TX 超时，仍允许在交接有效时读取诊断状态。 */
    if (!initialized)
        return AMP_BUS_NOT_READY;

    result = amp_bus_require_ready(AMP_BUS_CAN1);
    if (result != AMP_BUS_OK)
        return result;

    status->mode = rd(REG_MODE);
    status->state = rd(REG_STATE);
    status->rx_errors = rd(REG_RX_ERROR);
    status->tx_errors = rd(REG_TX_ERROR);
    status->rx_fifo = (rd(REG_RXFC) >> 5) & 7U;

    return AMP_BUS_OK;
}