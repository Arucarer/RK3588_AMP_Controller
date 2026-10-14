#include "modbus_master.h" /* 引入 Modbus 主站事务及底层 I/O 回调接口 */
#include "modbus_rtu.h"    /* 引入 Modbus RTU 编解码和 CRC16 接口 */

static int quiet(const AmpMbIo *io, uint32_t gap, uint32_t budget)
{
    /* 1. 轮询接收并等待总线静默间隔。 */
    /* 2. 检查超时及硬件错误，避免在上一帧未结束时发送。 */

    uint64_t start = io->now_us(io->context), last = start, stamp;
    uint8_t byte;
    for (;;) {
        int rc = io->poll(io->context, &byte, &stamp);
        uint64_t now = io->now_us(io->context);
        if (rc < 0)
            return AMP_MB_IO_ERROR;
        /* Discard stale input, but always wait a full quiet period after drain. */
        if (rc > 0)
            last = now;
        if (now - start >= budget)
            return AMP_MB_BUS_BUSY;
        if (!rc && now - last >= gap)
            return 0;
        io->idle(io->context);
    }
}

int amp_mb_master_execute(const AmpMbIo *io, const AmpMbConfig *cfg, const uint8_t req[8],
                          AmpMbTransaction *out)
{
    /* 1. 构建请求并检查串口及截止时间。 */
    /* 2. 等待静默后发送请求，持续接收响应。 */
    /* 3. 判断帧间隔并解析结果，将超时或协议错误返回调用方。 */

    uint32_t t15, t35;
    uint8_t canonical[8];
    unsigned i;
    uint16_t registers[125];
    if (!io || !cfg || !req || !out || !io->now_us || !io->poll || !io->write || !io->idle ||
        cfg->baud < 1200 || cfg->baud > 115200 || cfg->bits_per_character < 10 ||
        cfg->bits_per_character > 12 || !cfg->response_timeout_us ||
        cfg->response_timeout_us > 500000 || !cfg->quiet_timeout_us ||
        cfg->quiet_timeout_us > 100000 || cfg->read_retries > 1)
        return AMP_MB_ARGUMENT;
    if (amp_mb_build_request(req[0], req[1], (uint16_t)((req[2] << 8) | req[3]),
                             (uint16_t)((req[4] << 8) | req[5]), canonical))
        return AMP_MB_ARGUMENT;
    for (i = 0; i < 8; ++i)
        if (req[i] != canonical[i])
            return AMP_MB_ARGUMENT;
    t15 =
        cfg->baud > 19200 ? 750 : (1500000U * cfg->bits_per_character + cfg->baud - 1) / cfg->baud;
    t35 =
        cfg->baud > 19200 ? 1750 : (3500000U * cfg->bits_per_character + cfg->baud - 1) / cfg->baud;
    if (cfg->quiet_timeout_us <= t35)
        return AMP_MB_ARGUMENT;
    out->attempts = 0;
    out->length = 0;
    out->transmitted = 0;
    out->exception = 0;
    for (;;) {
        uint64_t start, last = 0, stamp = 0;
        int rc = quiet(io, t35, cfg->quiet_timeout_us);
        if (rc)
            return rc;
        ++out->attempts;
        out->length = 0;
        out->transmitted = 0;
        rc = io->write(io->context, req, 8, cfg->response_timeout_us, &out->transmitted);
        if (rc || out->transmitted != 8)
            return req[1] == 6 && out->transmitted ? AMP_MB_WRITE_UNKNOWN : AMP_MB_IO_ERROR;
        start = io->now_us(io->context);
        for (;;) {
            uint8_t byte;
            uint64_t now;
            rc = io->poll(io->context, &byte, &stamp);
            now = io->now_us(io->context);
            if (rc < 0)
                return req[1] == 6 ? AMP_MB_WRITE_UNKNOWN : AMP_MB_IO_ERROR;
            if (rc > 0) {
                if (stamp > now || stamp < start || (out->length && stamp < last))
                    return AMP_MB_IO_ERROR;
                if (out->length && stamp - last > t15)
                    return AMP_MB_GAP;
                if (out->length == sizeof(out->response))
                    return AMP_MB_FORMAT;
                out->response[out->length++] = byte;
                last = stamp;
            } else if (out->length && now - last >= t35) {
                return amp_mb_parse_response(req, out->response, out->length, registers, 125,
                                             &out->exception);
            }
            if (now - start >= cfg->response_timeout_us)
                break;
            io->idle(io->context);
        }
        if (req[1] == 6)
            return AMP_MB_WRITE_UNKNOWN;
        if (out->length || out->attempts > cfg->read_retries)
            return AMP_MB_TIMEOUT;
    }
}
