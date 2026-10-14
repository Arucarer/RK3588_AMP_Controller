#ifndef AMP_CAN1_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_CAN1_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* CAN1 驱动支持的经典 CAN 标准帧。 */
typedef struct {
    uint32_t id;     /* 标准帧标识符，范围 0～0x7FF */
    uint8_t length;  /* 有效数据字节数，范围 0～8 */
    uint8_t data[8]; /* 帧数据，有效长度由 length 指定 */
} AmpCanFrame;

/* CAN 控制器状态寄存器的读取结果。 */
typedef struct {
    uint32_t mode;      /* 模式寄存器原始值 */
    uint32_t state;     /* 状态寄存器原始值，包含 BUS_OFF 等状态位 */
    uint32_t rx_errors; /* 接收错误计数寄存器原始值 */
    uint32_t tx_errors; /* 发送错误计数寄存器原始值 */
    uint32_t rx_fifo;   /* 接收 FIFO 控制及状态寄存器原始值 */
} AmpCanStatus;

/*
 * 经典 CAN 标准数据帧基础驱动，单任务调用。
 * 不支持 FD、RTR、扩展帧。init 使用轮询；open 使用独占 IRQ 队列。
 *
 * nominal_timing 是当前 SDK 定义的 NBTP 寄存器值，
 * 必须由后续板级时钟配置计算并提供，不能使用猜测值。
 */
int amp_can1_init(uint32_t nominal_timing, int loopback);

/* 检查帧格式、驱动状态和截止时间；等待可发送并写入标识符与数据；等待发送完成或错误，返回操作结果。 */
int amp_can1_send(const AmpCanFrame *frame, uint32_t timeout_us);
/* 检查输出指针、驱动状态和等待期限；从接收队列读取帧；无数据时等待至完成、故障或超时。 */
int amp_can1_receive(AmpCanFrame *frame, uint32_t timeout_us);
/* 检查输出参数并读取 CAN 硬件状态；返回控制器状态及错误信息。 */
int amp_can1_status(AmpCanStatus *status);

/* 在中断保护下采集的 CAN 软件统计和故障快照。 */
typedef struct {
    uint32_t rx_frames;  /* 中断路径累计入队的接收帧数 */
    uint32_t tx_frames;  /* 中断路径累计记录的发送完成帧数 */
    uint32_t error_bits; /* 锁存的 CAN 错误中断位图 */
    uint32_t rx_dropped; /* 丢弃的接收帧或接收积压事件计数 */
    uint32_t faulted;    /* 驱动是否因不可恢复错误而失效，非零表示已失效 */
} AmpCanDiagnostics;
/* 按传入参数初始化 CAN 控制器；使能 CAN 中断，失败时屏蔽中断并使驱动失效。 */
int amp_can1_open(uint32_t nominal_timing, int loopback);
/* 确认驱动已初始化且板级资源仍就绪；按保存的时序与回环参数重新打开控制器，不重放上一帧。 */
int amp_can1_recover(void);
/* 在中断保护下采集收发计数及故障标志；向调用方返回一致的诊断快照。 */
int amp_can1_diagnostics(AmpCanDiagnostics *out);
#endif /* 结束 AMP_CAN1_H 对应的条件编译区段 */
