#include <stdint.h>

#include "shared_memory.h"
#include "FreeRTOS.h"
#include "task.h"
#include "irq_timer.h"
#include "control_task.h"
#include "monitor_task.h"

#define AMP_BOOT_MAGIC      0xA55A55A55AA55AA5ULL
#define AMP_BOOT_FLAG_ADDR  0x20FE0000ULL

#define RTOS_INFO_ADDR      0x20FE0300ULL
#define RTOS_INFO_MAGIC     0x4652544F53303031ULL

typedef struct {
    uint64_t magic;
    uint64_t current_el;
    uint64_t mpidr;
    uint64_t stage;
} AMPBootInfo;

typedef struct {
    uint64_t magic;          /* +0x00 */
    uint64_t stage;          /* +0x08 */
    uint64_t error;          /* +0x10 */
    uint64_t task_a_count;   /* +0x18 */
    uint64_t task_b_count;   /* +0x20 */
    uint64_t task_a_tick;    /* +0x28 */
    uint64_t task_b_tick;    /* +0x30 */
    uint64_t assert_line;    /* +0x38 */
    uint64_t assert_file;    /* +0x40: firmware string address */
    uint64_t fault_task;     /* +0x48: task handle */
    uint64_t fault_name;     /* +0x50: task name address */
} RTOSInfo;

static volatile AMPBootInfo *const boot_info =
    (volatile AMPBootInfo *)(uintptr_t)AMP_BOOT_FLAG_ADDR;

static volatile RTOSInfo *const rtos_info =
    (volatile RTOSInfo *)(uintptr_t)RTOS_INFO_ADDR;

static inline void barrier(void)
{
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

static inline uint64_t read_current_el(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3ULL;
}

static inline uint64_t read_mpidr(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(value));
    return value;
}

static void initialise_records(void)
{
    unsigned int i;
    volatile uint64_t *words =
        (volatile uint64_t *)(uintptr_t)RTOS_INFO_ADDR;

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
    uint64_t timer_disabled = 0;

    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    __asm__ volatile("msr cntp_ctl_el0, %0"
                     :: "r"(timer_disabled) : "memory");

    rtos_info->error = error;
    barrier();

    for (;;)
        __asm__ volatile("wfe");
}

void amp_assert_failed(const char *file, unsigned int line)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    rtos_info->assert_file = (uint64_t)(uintptr_t)file;
    rtos_info->assert_line = line;

    rtos_stop(1);
}

void vApplicationMallocFailedHook(void)
{
    rtos_stop(2);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    rtos_info->fault_task = (uint64_t)(uintptr_t)task;
    rtos_info->fault_name = (uint64_t)(uintptr_t)name;

    rtos_stop(3);
}

static void task_a(void *argument)
{
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
    /*
     * Stack depth is in StackType_t units:
     * 512 entries = 4096 bytes on this AArch64 Port.
     */
    result = xTaskCreate(task_a,
                         "TaskA",
                         configMINIMAL_STACK_SIZE,
                         NULL,
                         1,
                         NULL);

    if (result != pdPASS)
        rtos_stop(4);

    result = xTaskCreate(task_b,
                         "TaskB",
                         configMINIMAL_STACK_SIZE,
                         NULL,
                         1,
                         NULL);

    if (result != pdPASS)
        rtos_stop(4);

    result = xTaskCreate(amp_ipc_task,
                        "IPC",
                        configMINIMAL_STACK_SIZE,
                        NULL,
                        2,
                        NULL);

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
    volatile uint64_t *d =
        (volatile uint64_t *)(uintptr_t)0x20FE0400ULL;
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