#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Scheduler */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_TICKLESS_IDLE                 0
#define configTICK_RATE_HZ                      100
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                512
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1

/* Memory: task stack sizes are in StackType_t units, not bytes. */
#define configSUPPORT_STATIC_ALLOCATION        0
#define configSUPPORT_DYNAMIC_ALLOCATION       1
#define configTOTAL_HEAP_SIZE                  (128U * 1024U)

/* Hooks and diagnostics */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2

/* Optional facilities: not needed for the first two-task test. */
#define configUSE_MUTEXES                       0
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0
#define configUSE_TRACE_FACILITY                0
#define configGENERATE_RUN_TIME_STATS           0

/* APIs used by the initial test. */
#define INCLUDE_vTaskDelay                     1
#define INCLUDE_vTaskDelayUntil                1
#define INCLUDE_vTaskDelete                    0
#define INCLUDE_vTaskSuspend                   0
#define INCLUDE_vTaskPrioritySet               0
#define INCLUDE_uxTaskPriorityGet              0

/*
 * Current non-secure priority view:
 * 16 effective levels -> portPRIORITY_SHIFT = 4.
 *
 * API-call threshold: 9 << 4 = 0x90.
 * Tick priority: (16 - 2) << 4 = 0xE0.
 *
 * The maximum unmask value reads back as 0xF0.
 * Tick must have a numerically lower value than that threshold.
 */
#define configUNIQUE_INTERRUPT_PRIORITIES      16
#define configMAX_API_CALL_INTERRUPT_PRIORITY   9

#ifndef __ASSEMBLER__

void amp_setup_tick(void);
void amp_clear_tick(void);

void amp_assert_failed(const char *file, unsigned int line)
    __attribute__((noreturn));

#define configSETUP_TICK_INTERRUPT() amp_setup_tick()
#define configCLEAR_TICK_INTERRUPT() amp_clear_tick()

#define configASSERT(condition)                           \
    do {                                                  \
        if (!(condition))                                 \
            amp_assert_failed(__FILE__, __LINE__);         \
    } while (0)

#endif

#endif