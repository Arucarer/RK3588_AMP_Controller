#ifndef AMP_BUS_HANDOFF_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_BUS_HANDOFF_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#ifndef AMP_BUS_HW_ACTIVATE /* 未由编译参数指定 AMP_BUS_HW_ACTIVATE 时，使用下面的默认配置 */
#define AMP_BUS_HW_ACTIVATE 1 /* 总线硬件访问开关；启用后仍须通过板级资源交接检查 */
#endif                        /* 结束 AMP_BUS_HW_ACTIVATE 对应的条件编译区段 */

#define AMP_BUS_HANDOFF_ADDR UINT64_C(0x20FE1000) /* Linux 与 RTOS 的总线资源交接记录物理地址 */
#define AMP_BUS_HANDOFF_MAGIC                                                                      \
    UINT32_C(0x42555331) /* 总线交接记录的有效标记，对应 ASCII 字符 BUS1 */

#define AMP_BUS_CAN1 0U  /* 板级交接表中的 CAN1 资源编号 */
#define AMP_BUS_UART3 1U /* 板级交接表中的 UART3 资源编号 */

#define AMP_BUS_OK 0        /* 总线操作结果：成功 */
#define AMP_BUS_ARGUMENT -1 /* 总线操作结果：输入参数无效 */
#define AMP_BUS_NOT_READY -2 /* 总线操作结果：资源未交接、驱动未初始化或已失效 */
#define AMP_BUS_TIMEOUT -3     /* 总线操作结果：等待硬件或收发完成超时 */
#define AMP_BUS_HW_ERROR -4    /* 总线操作结果：硬件状态或配置读回异常 */
#define AMP_BUS_UNSUPPORTED -5 /* 总线操作结果：不支持的操作、帧格式或硬件配置 */

/* 清除总线资源交接记录；通过内存屏障发布失效状态。 */
void amp_bus_handoff_clear(void);

/* 核对板级交接记录的标识、版本与资源状态；返回资源交接是否满足驱动使用条件。 */
int amp_bus_platform_ready(uint32_t bus);
/* 检查设备编号及交接记录；确认时钟、复位和引脚资源已就绪，返回检查结果。 */
int amp_bus_require_ready(uint32_t bus);

/* 核对总线资源交接状态；返回交接记录中的输入时钟频率。 */
uint32_t amp_bus_clock_hz(uint32_t bus);

/* 把寄存器地址转换为 volatile 指针并读取；返回读取值，由调用方解释状态位。 */
uint32_t amp_bus_read32(uint64_t address);
/* 定位目标 MMIO 寄存器；通过 volatile 访问写入指定值。 */
void amp_bus_write32(uint64_t address, uint32_t value);

/* 基于物理计数器的轮询截止时间，使用计数差判断超时。 */
typedef struct {
    uint64_t start; /* 开始等待时的物理计数器值 */
    uint64_t ticks; /* 允许等待的计数器周期数，不是 FreeRTOS Tick 数 */
} AmpBusDeadline;

/* 检查输出指针、超时及计数器频率；把微秒超时换算为计数周期；保存当前计数值和等待期限。 */
int amp_bus_deadline_start(AmpBusDeadline *deadline, uint32_t timeout_us);
/* 读取当前硬件计数值；用已消耗周期判断是否达到等待期限。 */
int amp_bus_deadline_expired(const AmpBusDeadline *deadline);

#endif /* 结束 AMP_BUS_HANDOFF_H 对应的条件编译区段 */
