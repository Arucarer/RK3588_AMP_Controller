#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "shared_memory.h" /* 引入 RTOS 共享内存 IPC 初始化及任务入口 */
#include "FreeRTOS.h"      /* 引入 FreeRTOS 内核类型、配置及基础接口 */
#include "task.h"          /* 引入 FreeRTOS 任务、延时及调度接口 */
#include "irq_timer.h"     /* 引入系统 Tick 定时器及中断接口 */
#include "control_task.h"  /* 引入 LED 控制命令、结果及任务接口 */
#include "monitor_task.h"  /* 引入任务心跳、故障位及健康快照接口 */
#include "bus_task.h"      /* 引入总线命令、结果及任务队列接口 */
#include "motor/motor_task.h"

#define AMP_BOOT_MAGIC 0xA55A55A55AA55AA5ULL /* 启动握手记录的有效标记，数据写好后发布 */
#define AMP_BOOT_FLAG_ADDR 0x20FE0000ULL /* 启动握手记录的共享内存物理地址 */

#define RTOS_INFO_ADDR 0x20FE0300ULL /* RTOS 运行诊断记录的共享内存物理地址 */
#define RTOS_INFO_MAGIC 0x4652544F53303031ULL /* RTOS 运行诊断记录的有效标记 */

/* Linux 可读取的 RTOS 启动握手记录。 */
typedef struct {
    uint64_t magic;      /* 启动记录有效标记，取 AMP_BOOT_MAGIC */
    uint64_t current_el; /* RTOS 启动时的异常级别编号 */
    uint64_t mpidr;      /* 启动核心的 MPIDR_EL1 亲和标识 */
    uint64_t stage;      /* 启动握手阶段标记 */
} AMPBootInfo;

/* RTOS 调度及失败诊断共享记录，保留固定字段偏移供 Linux 读取。 */
typedef struct {
    uint64_t magic;        /* +0x00：诊断记录有效标记，取 RTOS_INFO_MAGIC */
    uint64_t stage;        /* +0x08：RTOS 初始化及任务运行阶段 */
    uint64_t error;        /* +0x10：停止原因码，具体含义见 rtos_stop 上方说明 */
    uint64_t task_a_count; /* +0x18：诊断任务 A 的累计循环次数 */
    uint64_t task_b_count; /* +0x20：诊断任务 B 的累计循环次数 */
    uint64_t task_a_tick;  /* +0x28：诊断任务 A 最近运行时的系统 Tick */
    uint64_t task_b_tick;  /* +0x30：诊断任务 B 最近运行时的系统 Tick */
    uint64_t assert_line;  /* +0x38：断言失败的源文件行号 */
    uint64_t assert_file;  /* +0x40：断言源文件名字符串在固件中的地址 */
    uint64_t fault_task;   /* +0x48：发生栈溢出的任务句柄 */
    uint64_t fault_name;   /* +0x50：发生栈溢出的任务名字符串地址 */
} RTOSInfo;

static volatile AMPBootInfo *const boot_info =
    (volatile AMPBootInfo *)(uintptr_t)AMP_BOOT_FLAG_ADDR; /* boot_info：启动握手信息共享区 */

static volatile RTOSInfo *const rtos_info =
    (volatile RTOSInfo *)(uintptr_t)RTOS_INFO_ADDR; /* rtos_info：RTOS 运行信息共享区 */

static inline void barrier(void)
{
    /* 1. 执行内存屏障，保证共享数据与状态标志的访问顺序。 */

    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

static inline uint64_t read_current_el(void)
{
    /* 1. 读取 CurrentEL 系统寄存器。 */
    /* 2. 提取异常级别字段，返回 EL 编号。 */

    uint64_t value;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3ULL;
}

static inline uint64_t read_mpidr(void)
{
    /* 1. 读取 MPIDR_EL1，返回当前 CPU 的亲和标识。 */

    uint64_t value;

    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(value));
    return value;
}

static void initialise_records(void)
{
    /* 1. 清除启动魔数，记录当前异常级别、CPU 标识与启动阶段。 */
    /* 2. 通过屏障发布启动握手记录。 */
    /* 3. 清零 RTOS 诊断区，再写入初始阶段和有效魔数。 */

    unsigned int i;
    volatile uint64_t *words = (volatile uint64_t *)(uintptr_t)RTOS_INFO_ADDR;

    boot_info->magic = 0;
    barrier();

    boot_info->current_el = read_current_el();
    boot_info->mpidr = read_mpidr();
    boot_info->stage = 1;

    barrier();
    boot_info->magic = AMP_BOOT_MAGIC;

    for (i = 0; i < sizeof(RTOSInfo) / sizeof(uint64_t); i++)
        words[i] = 0;

    rtos_info->stage = 1;
    barrier();
    rtos_info->magic = RTOS_INFO_MAGIC;
    barrier();
}

/*
 * Error codes:
 * 1: configASSERT
 * 2: heap allocation failure
 * 3: task stack overflow
 * 4: task creation failure
 * 5: scheduler unexpectedly returned
 */
static __attribute__((noreturn)) void rtos_stop(uint64_t error)
{
    /* 1. 屏蔽异常并关闭系统定时器。 */
    /* 2. 保存错误码，通过屏障发布后永久等待。 */

    uint64_t timer_disabled = 0;

    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    __asm__ volatile("msr cntp_ctl_el0, %0" ::"r"(timer_disabled) : "memory");

    rtos_info->error = error;
    barrier();

    for (;;)
        __asm__ volatile("wfe");
}

void amp_assert_failed(const char *file, unsigned int line)
{
    /* 1. 屏蔽异常，保存断言文件地址与行号。 */
    /* 2. 使用断言错误码进入停止路径。 */

    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    rtos_info->assert_file = (uint64_t)(uintptr_t)file;
    rtos_info->assert_line = line;

    rtos_stop(1);
}

void vApplicationMallocFailedHook(void)
{
    /* 1. 使用内存分配失败错误码进入停止路径。 */

    rtos_stop(2);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    /* 1. 屏蔽异常，保存发生栈溢出的任务句柄及名称地址。 */
    /* 2. 使用栈溢出错误码进入停止路径。 */

    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    rtos_info->fault_task = (uint64_t)(uintptr_t)task;
    rtos_info->fault_name = (uint64_t)(uintptr_t)name;

    rtos_stop(3);
}

static void task_a(void *argument)
{
    /* 1. 周期更新本任务运行计数。 */
    /* 2. 延时让出 CPU，供调度与诊断观察。 */

    (void)argument;

    for (;;) {
        rtos_info->task_a_count++;
        rtos_info->task_a_tick = (uint64_t)xTaskGetTickCount();
        rtos_info->stage = 4;
        barrier();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void task_b(void *argument)
{
    /* 1. 周期更新本任务运行计数。 */
    /* 2. 延时让出 CPU，供调度与诊断观察。 */

    (void)argument;

    for (;;) {
        rtos_info->task_b_count++;
        rtos_info->task_b_tick = (uint64_t)xTaskGetTickCount();
        rtos_info->stage = 4;
        barrier();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void amp_main(void)
{
    /* 1. 初始化诊断记录并核对启动环境。 */
    /* 2. 初始化 IPC、监控及业务任务。 */
    /* 3. 启动 FreeRTOS 调度器，意外返回时进入停止路径。 */

    BaseType_t result;

    initialise_records();

    /*
     * Wait for Linux to finish booting.
     * Existing start flag: 0x20FE0208.
     */
    amp_wait_for_start();

    rtos_info->stage = 2;
    barrier();

    if (monitor_init() != 0)
        rtos_stop(4);

    amp_ipc_init();
    /*
    * 在调度器启动前创建控制任务。
    * 实际 GPIO 初始化在 ControlTask 运行时执行。
    */
    if (control_init() != 0)
        rtos_stop(4);

    if (amp_bus_task_init() != 0)
        rtos_stop(4);

    /* SIM only, starts DISABLED. No PWM, ADC or physical motor access. */
    monitor_require_motor();
    if (amp_motor_task_init() != 0)
        rtos_stop(4);
    /*
     * Stack depth is in StackType_t units:
     * 512 entries = 4096 bytes on this AArch64 Port.
     */
    result = xTaskCreate(task_a, "TaskA", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    if (result != pdPASS)
        rtos_stop(4);

    result = xTaskCreate(task_b, "TaskB", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    if (result != pdPASS)
        rtos_stop(4);

    result = xTaskCreate(amp_ipc_task, "IPC", configMINIMAL_STACK_SIZE, NULL, 2, NULL);

    if (result != pdPASS)
        rtos_stop(4);

    rtos_info->stage = 3;
    boot_info->stage = 2;
    barrier();

    vTaskStartScheduler();

    /* A successful scheduler start does not return. */
    rtos_stop(5);
}

/*
 * Snapshot CPU-local state from the RTOS core.
 * No FreeRTOS API calls and no blocking operations.
 */
void vApplicationIdleHook(void)
{
    /* 1. 在空闲任务中更新运行记录，供外部诊断观察。 */

    volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)0x20FE0400ULL;
    uint64_t value;

    __asm__ volatile("mrs %0, DAIF" : "=r"(value));
    d[1] = value;

    /* ICC_PMR_EL1 */
    __asm__ volatile("mrs %0, S3_0_C4_C6_0" : "=r"(value));
    d[2] = value;

    /* ICC_IGRPEN1_EL1 */
    __asm__ volatile("mrs %0, S3_0_C12_C12_7" : "=r"(value));
    d[3] = value;

    /* ICC_HPPIR1_EL1: observe pending ID without acknowledging it. */
    __asm__ volatile("mrs %0, S3_0_C12_C12_2" : "=r"(value));
    d[4] = value;

    /* ICC_RPR_EL1 */
    __asm__ volatile("mrs %0, S3_0_C12_C11_3" : "=r"(value));
    d[5] = value;

    __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(value));
    d[6] = value;

    __asm__ volatile("mrs %0, VBAR_EL1" : "=r"(value));
    d[7] = value;

    __asm__ volatile("dsb sy" ::: "memory");
    d[0] = 0x49444C4530303031ULL;
    __asm__ volatile("dsb sy" ::: "memory");
}
