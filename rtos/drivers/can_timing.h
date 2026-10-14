#ifndef AMP_CAN_TIMING_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_CAN_TIMING_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

/* CAN 标称位时序计算结果及对应的寄存器编码。 */
typedef struct {
    uint32_t nbtp;            /* 可直接写入 CAN 标称位时序寄存器的编码值 */
    uint32_t bitrate;         /* 对应的总线比特率，单位 bit/s */
    uint32_t sample_permille; /* 采样点位置，单位千分之一位周期 */
    uint32_t brp;             /* 输入时钟分频系数 */
    uint32_t tseg1; /* 同步段之后、采样点之前的时间段长度，单位时间量子 */
    uint32_t tseg2; /* 采样点之后的时间段长度，单位时间量子 */
} AmpCanTiming;

/*
 * 对照当前 SDK：
 * BRP 取偶数 2..256，TSEG1/TSEG2 为 1..128，SJW=1。
 * 只接受能精确生成目标 bitrate 的组合。
 */
int amp_can_timing(uint32_t clock_hz, uint32_t bitrate, AmpCanTiming *out);

#endif /* 结束 AMP_CAN_TIMING_H 对应的条件编译区段 */
