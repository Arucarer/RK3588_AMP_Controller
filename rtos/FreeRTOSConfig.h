#ifndef FREERTOS_CONFIG_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define FREERTOS_CONFIG_H /* 定义头文件保护标记，避免本文件被重复包含 */

/* Scheduler */
#define configUSE_PREEMPTION 1 /* 1 启用抢占式调度，高优先级就绪任务可抢占当前任务 */
#define configUSE_TIME_SLICING 1  /* 1 启用同优先级任务的时间片轮转 */
#define configUSE_TICKLESS_IDLE 0 /* 0 关闭无 Tick 空闲模式 */
#define configTICK_RATE_HZ 100    /* 系统 Tick 频率为 100 Hz，每个 Tick 为 10 ms */
#define configMAX_PRIORITIES 5    /* 任务优先级数量为 5，合法编号为 0～4 */
#define configMINIMAL_STACK_SIZE 512 /* 最小任务栈深度为 512 个 StackType_t 元素，不是字节数 */
#define configMAX_TASK_NAME_LEN 16 /* 任务名称缓冲区长度，包含结尾的空字符 */
#define configUSE_16_BIT_TICKS 0 /* 0 不使用 16 位 Tick 计数，具体类型由移植层定义 */
#define configIDLE_SHOULD_YIELD 1 /* 1 允许空闲任务向其他同优先级就绪任务让出 CPU */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1 /* 1 使用移植层提供的优先级位图快速选择任务 */

/* Memory: task stack sizes are in StackType_t units, not bytes. */
#define configSUPPORT_STATIC_ALLOCATION 0  /* 0 关闭静态分配的内核对象创建接口 */
#define configSUPPORT_DYNAMIC_ALLOCATION 1 /* 1 启用动态分配的任务和队列创建接口 */
#define configTOTAL_HEAP_SIZE (128U * 1024U) /* FreeRTOS 动态堆容量，128 KiB */

/* Hooks and diagnostics */
#define configUSE_IDLE_HOOK 1            /* 1 在空闲任务中调用 vApplicationIdleHook */
#define configUSE_TICK_HOOK 0            /* 0 不调用应用层 Tick 钩子 */
#define configUSE_MALLOC_FAILED_HOOK 1   /* 1 在内核动态分配失败时调用失败钩子 */
#define configCHECK_FOR_STACK_OVERFLOW 2 /* 使用第 2 级任务栈溢出检查 */

/* Optional facilities: not needed for the first two-task test. */
#define configUSE_MUTEXES 0             /* 0 关闭互斥锁功能 */
#define configUSE_RECURSIVE_MUTEXES 0   /* 0 关闭递归互斥锁功能 */
#define configUSE_COUNTING_SEMAPHORES 0 /* 0 关闭计数信号量功能 */
#define configUSE_TIMERS 0              /* 0 关闭 FreeRTOS 软件定时器功能 */
#define configUSE_CO_ROUTINES 0         /* 0 关闭协程功能 */
#define configUSE_TRACE_FACILITY 0      /* 0 关闭内核跟踪辅助功能 */
#define configGENERATE_RUN_TIME_STATS 0 /* 0 不生成任务运行时间统计 */

/* APIs used by the initial test. */
#define INCLUDE_vTaskDelay 1        /* 1 编译相对延时接口 vTaskDelay */
#define INCLUDE_vTaskDelayUntil 1   /* 1 编译周期任务使用的绝对延时接口 */
#define INCLUDE_vTaskDelete 0       /* 0 不编译任务删除接口 */
#define INCLUDE_vTaskSuspend 0      /* 0 不编译任务挂起及恢复接口 */
#define INCLUDE_vTaskPrioritySet 0  /* 0 不编译任务优先级修改接口 */
#define INCLUDE_uxTaskPriorityGet 0 /* 0 不编译任务优先级查询接口 */

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
#define configUNIQUE_INTERRUPT_PRIORITIES 16 /* GIC 当前优先级视图中的有效优先级数量 */
#define configMAX_API_CALL_INTERRUPT_PRIORITY                                                      \
    9 /* 可调用内核中断 API 的优先级阈值，移植层会换算为硬件值 */

#ifndef __ASSEMBLER__ /* 仅 C 编译时声明函数和断言接口，汇编预处理时跳过 */

/* 计算系统 Tick 周期并配置本核中断接口；设置定时器触发值并使能 Tick 中断；记录配置结果供启动诊断使用。 */
void amp_setup_tick(void);
/* 更新系统定时器下一次触发值；维护 Tick 中断诊断记录。 */
void amp_clear_tick(void);

/* 屏蔽异常，保存断言文件地址与行号；使用断言错误码进入停止路径。 */
void amp_assert_failed(const char *file, unsigned int line) __attribute__((noreturn));

#define configSETUP_TICK_INTERRUPT() amp_setup_tick() /* 调度器启动时调用板级 Tick 中断配置函数 */
#define configCLEAR_TICK_INTERRUPT() amp_clear_tick() /* 处理 Tick 时更新下一次定时器触发值 */

#define configASSERT(condition) /* 条件为假时记录源文件和行号，并进入断言失败路径 */ \
    do {                                                                                                    \
        if (!(condition))                                                                                   \
            amp_assert_failed(__FILE__, __LINE__);                                                          \
    } while (0)

#endif /* 结束 __ASSEMBLER__ 对应的条件编译区段 */

#endif /* 结束 FREERTOS_CONFIG_H 对应的条件编译区段 */
