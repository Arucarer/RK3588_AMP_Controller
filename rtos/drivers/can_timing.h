#ifndef AMP_CAN_TIMING_H
#define AMP_CAN_TIMING_H

#include <stdint.h>

typedef struct {
    uint32_t nbtp;
    uint32_t bitrate;
    uint32_t sample_permille;
    uint32_t brp;
    uint32_t tseg1;
    uint32_t tseg2;
} AmpCanTiming;

/*
 * 对照当前 SDK：
 * BRP 取偶数 2..256，TSEG1/TSEG2 为 1..128，SJW=1。
 * 只接受能精确生成目标 bitrate 的组合。
 */
int amp_can_timing(uint32_t clock_hz,
                    uint32_t bitrate,
                    AmpCanTiming *out);

#endif
