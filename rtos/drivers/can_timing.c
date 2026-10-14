#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */
#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#include "can_timing.h" /* 引入 CAN 位时序计算接口 */

int amp_can_timing(uint32_t clock_hz, uint32_t bitrate, AmpCanTiming *out)
{
    /* 1. 校验输入时钟和目标波特率。 */
    /* 2. 枚举分频及采样段组合，筛选可行时序。 */
    /* 3. 编码最佳寄存器值并返回实际时序。 */

    uint32_t brp, tseg2;
    uint32_t best_error = UINT32_MAX;
    AmpCanTiming best = {0};

    if (out == NULL || clock_hz == 0U || bitrate < 10000U || bitrate > 1000000U)
        return -1;

    for (brp = 2U; brp <= 256U; brp += 2U) {
        uint64_t divisor = (uint64_t)brp * bitrate;
        uint32_t total;

        if ((uint64_t)clock_hz % divisor != 0U)
            continue;

        total = (uint32_t)((uint64_t)clock_hz / divisor);
        if (total < 3U || total > 257U)
            continue;

        for (tseg2 = 1U; tseg2 <= 128U; ++tseg2) {
            uint32_t tseg1, sample, error;

            if (total <= 1U + tseg2)
                continue;

            tseg1 = total - 1U - tseg2;
            if (tseg1 == 0U || tseg1 > 128U)
                continue;

            sample = (1U + tseg1) * 1000U / total;
            error = sample > 875U ? sample - 875U : 875U - sample;

            if (error >= best_error)
                continue;

            best_error = error;
            best.brp = brp;
            best.tseg1 = tseg1;
            best.tseg2 = tseg2;
            best.bitrate = bitrate;
            best.sample_permille = sample;

            /*
             * SDK: BRP field=(brp>>1)-1,
             * TSEG1/TSEG2 fields are segment length minus 1.
             * SJW=1 means SJW register field is zero.
             */
            best.nbtp = (((brp >> 1) - 1U) << 16) | ((tseg2 - 1U) << 8) | (tseg1 - 1U);
        }
    }

    if (best_error == UINT32_MAX)
        return -1;

    *out = best;
    return 0;
}
