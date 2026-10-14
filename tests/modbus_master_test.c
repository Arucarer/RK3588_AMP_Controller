#include <assert.h>                 /* 引入 assert 断言，用于验证测试预期 */
#include <stdio.h>                  /* 引入标准输入输出接口，如 printf、fprintf */
#include "protocol/modbus_master.h" /* 引入 Modbus 主站事务及底层 I/O 回调接口 */
#include "protocol/modbus_rtu.h"    /* 引入 Modbus RTU 编解码和 CRC16 接口 */
/* Modbus 主站测试的模拟时钟、响应和故障场景。 */
typedef struct {
    uint64_t now;    /* 模拟当前时间，单位 us */
    uint64_t sent;   /* 最近一次请求发送时的模拟时间，单位 us */
    unsigned writes; /* 发送回调累计调用次数 */
    unsigned index;  /* 下一个待返回的响应字节位置 */
    int mode; /* 测试场景编号，控制超时、字节间隔、部分发送及 I/O 失败 */
    uint8_t reply[8]; /* 模拟返回的 8 字节响应数据 */
} Fake;
static uint64_t now(void *p)
{
    /* 1. 读取模拟时钟，供主站截止时间与帧间隔判断。 */

    return ((Fake *)p)->now;
}

static void idle(void *p)
{
    /* 1. 推进模拟时间，驱动主站轮询进入下一次时间检查。 */

    ((Fake *)p)->now += 100;
}

static int write_bytes(void *p, const uint8_t *b, size_t n, uint32_t budget, size_t *done)
{
    /* 1. 记录主站发送的数据。 */
    /* 2. 根据测试场景提供模拟写入结果。 */

    Fake *f = p;
    (void)budget;
    ++f->writes;
    f->sent = f->now;
    f->index = 0;
    *done = n;
    if (f->mode == 4) {
        *done = 3;
        return -1;
    }
    for (unsigned i = 0; i < 8; ++i)
        f->reply[i] = b[i];
    return 0;
}

static int poll_byte(void *p, uint8_t *b, uint64_t *stamp)
{
    /* 1. 按测试场景返回一个接收字节及时间戳。 */
    /* 2. 无数据或故障时返回预设状态。 */

    Fake *f = p;
    if (!f->writes || f->mode == 0 || (f->mode == 2 && f->writes == 1) || f->index == 8)
        return 0;
    if (f->mode == 5)
        return -1;
    uint64_t due = f->sent + 1000 + f->index * 100;
    if (f->mode == 3 && f->index >= 4)
        due += 3000;
    if (f->now < due)
        return 0;
    *stamp = due;
    *b = f->reply[f->index++];
    return 1;
}

int main(void)
{
    /* 1. 准备当前测试使用的输入、模拟状态与预期结果。 */
    /* 2. 调用被测接口，断言返回值及状态变化符合预期。 */

    Fake f = {0};
    AmpMbIo io = {&f, now, poll_byte, write_bytes, idle};
    AmpMbConfig c = {9600, 20000, 10000, 11, 1};
    AmpMbTransaction result;
    uint8_t request[8];
    assert(!amp_mb_build_request(1, 6, 3, 42, request));
    f.mode = 1;
    assert(!amp_mb_master_execute(&io, &c, request, &result));
    assert(result.length == 8 && f.writes == 1);
    f = (Fake){0};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_WRITE_UNKNOWN);
    assert(f.writes == 1);
    f = (Fake){.mode = 4};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_WRITE_UNKNOWN);
    assert(result.transmitted == 3 && f.writes == 1);
    f = (Fake){.mode = 3};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_GAP);
    f = (Fake){.mode = 5};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_WRITE_UNKNOWN);
    assert(!amp_mb_build_request(1, 3, 3, 1, request));
    f = (Fake){0};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_TIMEOUT);
    assert(f.writes == 2);
    f = (Fake){.mode = 2};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_FORMAT);
    assert(f.writes == 2); /* Fake echoes read request: invalid response, no third replay. */
    request[7] ^= 1;
    f = (Fake){0};
    assert(amp_mb_master_execute(&io, &c, request, &result) == AMP_MB_ARGUMENT);
    assert(f.writes == 0);
    puts("PASS: master success, write ambiguity, partial TX, gap, I/O failure, bounded read retry, "
         "CRC rejection");
}
