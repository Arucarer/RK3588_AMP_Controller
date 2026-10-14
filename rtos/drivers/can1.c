#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "amp_bus_resources.h" /* 引入 CAN1、UART3 的物理地址及中断资源定义 */
#include "amp_bus_handoff.h"   /* 引入总线资源交接、MMIO 访问及截止时间接口 */
#include "can1.h"              /* 引入 CAN1 帧结构、收发及诊断接口 */
#include "amp_bus_irq.h"       /* 引入总线中断回调注册及分发接口 */

/* 对照当前 SDK rockchip_canfd.c。 */
#define REG_MODE 0x000U     /* CAN 模式寄存器字节偏移 */
#define REG_CMD 0x004U      /* CAN 命令寄存器字节偏移 */
#define REG_STATE 0x008U    /* CAN 状态寄存器字节偏移 */
#define REG_INT 0x00CU      /* CAN 中断状态寄存器字节偏移 */
#define REG_INT_MASK 0x010U /* CAN 中断屏蔽寄存器字节偏移 */
#define REG_RX_ERROR 0x034U /* CAN 接收错误计数寄存器字节偏移 */
#define REG_TX_ERROR 0x038U /* CAN 发送错误计数寄存器字节偏移 */
#define REG_IDCODE 0x03CU   /* CAN 接收标识符过滤码寄存器字节偏移 */
#define REG_IDMASK 0x040U   /* CAN 接收标识符过滤掩码寄存器字节偏移 */
#define REG_NBTP 0x100U     /* CAN 标称位时序配置寄存器字节偏移 */
#define REG_RXFC 0x118U     /* CAN 接收 FIFO 控制寄存器字节偏移 */
#define REG_AFC 0x11CU      /* CAN 接收过滤控制寄存器字节偏移 */
#define REG_TXFIC 0x200U    /* CAN 发送帧信息寄存器字节偏移 */
#define REG_TXID 0x204U     /* CAN 发送帧标识符寄存器字节偏移 */
#define REG_TXDAT0 0x208U   /* CAN 发送数据区第一个寄存器的字节偏移 */
#define REG_RXFRD 0x400U    /* CAN 接收 FIFO 数据读取端口的字节偏移 */

#define MODE_WORK (1U << 0)      /* CAN 工作模式使能位 */
#define MODE_SELF_TEST (1U << 2) /* CAN 自测试模式使能位 */
#define MODE_LOOPBACK (1U << 4)  /* CAN 内部回环模式使能位 */
#define MODE_SPACE_RX (1U << 12) /* CAN 接收空间相关模式位，具体定义见对应 SDK 驱动 */
#define MODE_FDOE (1U << 15) /* SDK 初始化使用的 FD 模式位；本驱动发送接口仍限制为经典 CAN 帧 */

#define INT_RX_FINISH (1U << 0) /* CAN 接收完成中断标志位 */
#define INT_TX_FINISH (1U << 1) /* CAN 发送完成中断标志位 */
#define INT_ERRORS                                                                                 \
    ((1U << 2) | (1U << 3) | (1U << 4) | (1U << 6) | (1U << 8) |                                   \
     (1U << 9))           /* 驱动关注的 CAN 错误中断位集合 */
#define BUS_OFF (1U << 5) /* CAN 状态中的总线关闭标志位 */

static int initialized;       /* initialized：模块初始化完成标志 */
static volatile int poisoned; /* poisoned：不可恢复故障标志，置位后停止正常收发 */

static uint32_t rd(uint32_t offset)
{
    /* 1. 把寄存器地址转换为 volatile 指针并读取。 */
    /* 2. 返回读取值，由调用方解释状态位。 */

    return amp_bus_read32(AMP_CAN1_BASE + offset);
}

static void wr(uint32_t offset, uint32_t value)
{
    /* 1. 定位目标 MMIO 寄存器。 */
    /* 2. 通过 volatile 访问写入指定值。 */

    amp_bus_write32(AMP_CAN1_BASE + offset, value);
}

#define CAN_RX_CAP 16U /* CAN 接收环形队列容量，保留一个槽位区分队满和队空 */
#define CAN_IRQ_BITS                                                                               \
    (INT_RX_FINISH | INT_TX_FINISH | INT_ERRORS | (1U << 5) |                                      \
     (1U << 7))                          /* 中断模式允许处理的 CAN 中断位集合 */
static volatile unsigned rx_head;        /* 接收环形队列的写入位置 */
static volatile unsigned rx_tail;        /* 接收环形队列的读取位置 */
static AmpCanFrame rx_queue[CAN_RX_CAP]; /* rx_queue：接收帧环形缓存 */
static volatile uint32_t irq_tx_done;    /* 中断记录的发送完成标志 */
static volatile uint32_t irq_errors;     /* 中断记录的错误位图 */
static volatile uint32_t rx_dropped;     /* 接收丢弃帧数 */
static volatile uint32_t rx_total;       /* 累计接收帧数 */
static volatile uint32_t tx_total;       /* 累计发送帧数 */
static int irq_enabled;                  /* irq_enabled：中断模式使能标志 */
static uint32_t saved_timing; /* saved_timing：初始化时保存的 CAN 位时序配置 */
static int saved_loopback;    /* saved_loopback：初始化时保存的 CAN 回环配置 */
static uint64_t local_lock(void)
{
    /* 1. 保存当前中断屏蔽状态。 */
    /* 2. 屏蔽本核 IRQ，返回原状态供解锁恢复。 */

    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags)::"memory");
    return flags;
}

static void local_unlock(uint64_t flags)
{
    /* 1. 恢复调用方保存的中断屏蔽状态。 */

    __asm__ volatile("msr daif, %0" ::"r"(flags) : "memory");
}

static int pop_fifo(AmpCanFrame *frame)
{
    /* 1. 按硬件 FIFO 格式读出帧控制字、标识符和数据。 */
    /* 2. 拒绝当前驱动不支持的帧格式。 */
    /* 3. 提取经典 CAN 数据并填充输出帧。 */

    uint32_t fic = rd(REG_RXFRD), id = rd(REG_RXFRD), data[16];
    unsigned i;
    (void)rd(REG_RXFRD);
    for (i = 0; i < 16; i++)
        data[i] = rd(REG_RXFRD);
    if ((fic & 0xe0U) || (fic & 15U) > 8U || id > 0x7ffU)
        return AMP_BUS_UNSUPPORTED;
    frame->id = id;
    frame->length = (uint8_t)(fic & 15U);
    for (i = 0; i < 8; i++)
        frame->data[i] = i < frame->length ? (uint8_t)(data[i / 4] >> ((i % 4) * 8)) : 0;
    return 0;
}

static void can_irq(void *argument)
{
    /* 1. 读取中断状态并更新发送完成或错误标志。 */
    /* 2. 在配额内把接收 FIFO 搬入环形队列。 */
    /* 3. 处理溢出和剩余积压，再清除本次中断状态。 */

    uint32_t pending = rd(REG_INT);
    unsigned quota = 7U;
    (void)argument;
    if (pending & INT_TX_FINISH) {
        irq_tx_done = 1;
        ++tx_total;
    }
    if (pending & INT_ERRORS || rd(REG_STATE) & BUS_OFF) {
        irq_errors |= pending & INT_ERRORS;
        poisoned = 1;
        wr(REG_INT_MASK, UINT32_MAX);
    }
    while (!poisoned && quota-- && ((rd(REG_RXFC) >> 5) & 7U)) {
        AmpCanFrame frame;
        unsigned next = (rx_head + 1U) % CAN_RX_CAP;
        if (pop_fifo(&frame)) {
            ++rx_dropped;
            continue;
        }
        if (next == rx_tail) {
            ++rx_dropped;
            poisoned = 1;
            wr(REG_INT_MASK, UINT32_MAX);
            break;
        }
        rx_queue[rx_head] = frame;
        __asm__ volatile("dmb ish" ::: "memory");
        rx_head = next;
        ++rx_total;
    }
    if (!poisoned && ((rd(REG_RXFC) >> 5) & 7U)) {
        /* Do not assume that W1C RX will retrigger for an existing FIFO backlog. */
        ++rx_dropped;
        poisoned = 1;
        wr(REG_INT_MASK, UINT32_MAX);
    }
    wr(REG_INT, pending);
}

#define CAN_GICD UINT64_C(0xfe600000) /* GIC Distributor 的 MMIO 物理基地址 */
static int can_gic_wait(void)
{
    /* 1. 建立等待的时间边界。 */
    /* 2. 轮询硬件忙位。 */
    /* 3. 完成时返回成功，超时则返回错误。 */

    AmpBusDeadline deadline;
    int rc = amp_bus_deadline_start(&deadline, 10000U);
    if (rc)
        return rc;
    while (amp_bus_read32(CAN_GICD) & (1U << 31))
        if (amp_bus_deadline_expired(&deadline))
            return AMP_BUS_TIMEOUT;
    return 0;
}

static int can_irq_enable(void)
{
    /* 1. 检查 GIC 中断分组及路由配置。 */
    /* 2. 注册 CAN 回调并配置优先级、目标核心和使能位。 */
    /* 3. 读回检查配置，失败时返回错误。 */

    uint32_t id = AMP_CAN1_INTID, bit = 1U << (id % 32U);
    uint64_t flags, affinity;
    int rc;
    uint32_t ctl = amp_bus_read32(CAN_GICD);
    /* IGROUPR is RAZ/WI for NS accesses when security is enabled.
     * Only inspect group bits in the single-security-state view (DS=1).
     * Routing, priority and enable readback below still prove access. */
    if (!(ctl & (1U << 4)) ||
        ((ctl & (1U << 6)) && !(amp_bus_read32(CAN_GICD + 0x80U + (id / 32) * 4) & bit)) ||
        (amp_bus_read32(CAN_GICD + 0xc00U + (id / 16) * 4) & (2U << ((id % 16) * 2))))
        return AMP_BUS_UNSUPPORTED;
    flags = local_lock();
    wr(REG_INT_MASK, UINT32_MAX);
    amp_bus_write32(CAN_GICD + 0x180U + (id / 32) * 4, bit);
    rc = can_gic_wait();
    if (rc)
        goto done;
    if (amp_bus_read32(CAN_GICD + 0x300U + (id / 32) * 4) & bit) {
        rc = AMP_BUS_HW_ERROR;
        goto done;
    }
    (void)amp_bus_irq_unregister(id, can_irq);
    if (amp_bus_irq_register(id, can_irq, 0)) {
        rc = AMP_BUS_HW_ERROR;
        goto done;
    }
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(affinity));
    affinity &= UINT64_C(0xff00ffffff);
    *(volatile uint64_t *)(uintptr_t)(CAN_GICD + 0x6000U + id * 8) = affinity;
    *(volatile uint8_t *)(uintptr_t)(CAN_GICD + 0x400U + id) = 0xa0;
    __asm__ volatile("dsb sy; isb" ::: "memory");
    if (*(volatile uint64_t *)(uintptr_t)(CAN_GICD + 0x6000U + id * 8) != affinity ||
        *(volatile uint8_t *)(uintptr_t)(CAN_GICD + 0x400U + id) != 0xa0U) {
        rc = AMP_BUS_HW_ERROR;
        goto done;
    }
    rx_head = rx_tail = 0;
    irq_tx_done = 0;
    wr(REG_INT, UINT32_MAX);
    amp_bus_write32(CAN_GICD + 0x280U + (id / 32) * 4, bit);
    amp_bus_write32(CAN_GICD + 0x100U + (id / 32) * 4, bit);
    rc = can_gic_wait();
    if (!rc && !(amp_bus_read32(CAN_GICD + 0x100U + (id / 32) * 4) & bit))
        rc = AMP_BUS_UNSUPPORTED;
    if (!rc) {
        irq_enabled = 1;
        wr(REG_INT_MASK, ~CAN_IRQ_BITS);
    }
done:
    if (rc) {
        wr(REG_INT_MASK, UINT32_MAX);
        amp_bus_write32(CAN_GICD + 0x180U + (id / 32) * 4, bit);
        (void)can_gic_wait();
        irq_enabled = 0;
    }
    local_unlock(flags);
    return rc;
}

int amp_can1_open(uint32_t timing, int loopback)
{
    /* 1. 按传入参数初始化 CAN 控制器。 */
    /* 2. 使能 CAN 中断，失败时屏蔽中断并使驱动失效。 */

    int rc = amp_can1_init(timing, loopback);
    if (!rc) {
        rc = can_irq_enable();
        if (rc) {
            uint64_t flags = local_lock();
            wr(REG_INT_MASK, UINT32_MAX);
            wr(REG_MODE, 0);
            wr(REG_CMD, 0);
            poisoned = 1;
            initialized = 0;
            local_unlock(flags);
        }
    }
    return rc;
}

int amp_can1_recover(void)
{
    /* 1. 确认驱动已初始化且板级资源仍就绪。 */
    /* 2. 按保存的时序与回环参数重新打开控制器，不重放上一帧。 */

    if (!initialized || amp_bus_require_ready(AMP_BUS_CAN1))
        return AMP_BUS_NOT_READY;
    /* Explicit reset of controller state; never replay the previous frame. */
    return amp_can1_open(saved_timing, saved_loopback);
}

int amp_can1_diagnostics(AmpCanDiagnostics *out)
{
    /* 1. 在中断保护下采集收发计数及故障标志。 */
    /* 2. 向调用方返回一致的诊断快照。 */

    uint64_t flags;
    if (!out)
        return AMP_BUS_ARGUMENT;
    if (!initialized || amp_bus_require_ready(AMP_BUS_CAN1))
        return AMP_BUS_NOT_READY;
    flags = local_lock();
    out->rx_frames = rx_total;
    out->tx_frames = tx_total;
    out->error_bits = irq_errors;
    out->rx_dropped = rx_dropped;
    out->faulted = (uint32_t)poisoned;
    local_unlock(flags);
    return 0;
}

static int available(void)
{
    /* 1. 检查 CAN 初始化状态和故障标志。 */
    /* 2. 返回当前是否允许收发。 */

    if (!initialized || poisoned)
        return AMP_BUS_NOT_READY;

    return amp_bus_require_ready(AMP_BUS_CAN1);
}

static int can_init_masked(uint32_t nominal_timing, int loopback)
{
    /* 1. 检查回环参数及资源交接，复位软件初始化状态。 */
    /* 2. 屏蔽控制器中断，配置过滤规则及位时序。 */
    /* 3. 开启工作模式并读回检查，成功后标记已初始化。 */

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

    wr(REG_INT_MASK, UINT32_MAX);
    irq_enabled = 0;
    wr(REG_MODE, 0U);
    saved_timing = nominal_timing;
    saved_loopback = loopback;

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

    if (rd(REG_STATE) & BUS_OFF)
        return AMP_BUS_HW_ERROR;
    initialized = 1;
    return AMP_BUS_OK;
}

int amp_can1_send(const AmpCanFrame *frame, uint32_t timeout_us)
{
    /* 1. 检查帧格式、驱动状态和截止时间。 */
    /* 2. 等待可发送并写入标识符与数据。 */
    /* 3. 等待发送完成或错误，返回操作结果。 */

    AmpBusDeadline deadline;
    uint32_t words[2] = {0U, 0U};
    uint32_t pending;
    uint64_t tx_flags;
    unsigned int i;
    int result;

    if (frame == NULL || frame->id > 0x7FFU || frame->length > 8U)
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
        words[i / 4U] |= (uint32_t)frame->data[i] << ((i % 4U) * 8U);

    tx_flags = local_lock();
    irq_tx_done = 0;
    wr(REG_INT, INT_TX_FINISH);
    wr(REG_TXID, frame->id);
    wr(REG_TXFIC, frame->length);
    wr(REG_TXDAT0, words[0]);
    wr(REG_TXDAT0 + 4U, words[1]);

    wr(REG_MODE, rd(REG_MODE) | MODE_SPACE_RX);
    wr(REG_CMD, 1U);
    wr(REG_MODE, rd(REG_MODE) & ~MODE_SPACE_RX);
    local_unlock(tx_flags);

    for (;;) {
        pending = rd(REG_INT);

        if (poisoned || (pending & INT_ERRORS) || (rd(REG_STATE) & BUS_OFF)) {
            poisoned = 1;
            wr(REG_INT_MASK, UINT32_MAX);
            return AMP_BUS_HW_ERROR;
        }

        if ((irq_enabled ? irq_tx_done : (pending & INT_TX_FINISH)) && !(rd(REG_CMD) & 3U)) {
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
            wr(REG_INT_MASK, UINT32_MAX);
            return AMP_BUS_TIMEOUT;
        }

        __asm__ volatile("yield");
    }
}

int amp_can1_receive(AmpCanFrame *frame, uint32_t timeout_us)
{
    /* 1. 检查输出指针、驱动状态和等待期限。 */
    /* 2. 从接收队列读取帧。 */
    /* 3. 无数据时等待至完成、故障或超时。 */

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

    if (irq_enabled) {
        for (;;) {
            uint64_t flags = local_lock();
            if (poisoned) {
                local_unlock(flags);
                return AMP_BUS_HW_ERROR;
            }
            if (rx_tail != rx_head) {
                *frame = rx_queue[rx_tail];
                rx_tail = (rx_tail + 1U) % CAN_RX_CAP;
                local_unlock(flags);
                return 0;
            }
            local_unlock(flags);
            if (amp_bus_deadline_expired(&deadline))
                return AMP_BUS_TIMEOUT;
            __asm__ volatile("yield");
        }
    }
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

    if ((fic & ((1U << 7) | (1U << 6) | (1U << 5))) || (fic & 0xFU) > 8U || id > 0x7FFU)
        return AMP_BUS_UNSUPPORTED;

    frame->id = id;
    frame->length = (uint8_t)(fic & 0xFU);

    for (i = 0; i < 8U; ++i)
        frame->data[i] = i < frame->length ? (uint8_t)(words[i / 4U] >> ((i % 4U) * 8U)) : 0U;

    return AMP_BUS_OK;
}

int amp_can1_status(AmpCanStatus *status)
{
    /* 1. 检查输出参数并读取 CAN 硬件状态。 */
    /* 2. 返回控制器状态及错误信息。 */

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

int amp_can1_init(uint32_t timing, int loopback)
{
    /* 1. 保存并屏蔽本核中断。 */
    /* 2. 执行 CAN 配置，再恢复原中断状态。 */

    uint64_t flags = local_lock();
    int rc = can_init_masked(timing, loopback);
    local_unlock(flags);
    return rc;
}
