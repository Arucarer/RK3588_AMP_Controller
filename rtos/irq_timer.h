#ifndef AMP_IRQ_TIMER_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_IRQ_TIMER_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* 初始化当前核心的诊断信息；等待启动握手条件满足后继续。 */
void amp_wait_for_start(void);
/* 计算系统 Tick 周期并配置本核中断接口；设置定时器触发值并使能 Tick 中断；记录配置结果供启动诊断使用。 */
void amp_setup_tick(void);
/* 更新系统定时器下一次触发值；维护 Tick 中断诊断记录。 */
void amp_clear_tick(void);
/* 读取中断号并区分 Tick 和外设中断；处理 Tick 或分发已注册的总线回调；完成中断应答并更新诊断信息。 */
void vApplicationIRQHandler(uint64_t iar);

#endif /* 结束 AMP_IRQ_TIMER_H 对应的条件编译区段 */
