/* 新建：linux/amp_ipc_bench.c
 *
 * 阶段九第一步：同一会话连续 PING 的往返耗时基线。
 *
 * 计时包含：
 * Linux 请求编解码、共享内存轮询、FreeRTOS 调度和响应处理。
 *
 * 不包含进程启动和首次 HELLO。
 * 不是单向 IPC 延迟，不是 GPIO IRQ 延迟，也不是 Tick jitter。
 *
 * stdout：逐请求 CSV。
 * stderr：统计摘要。
 *
 * 遇到通信失败立即结束，避免超时后盲目重试。
 * 分位数采用 nearest-rank。
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "amp_ipc.h"

#define MAX_SAMPLES 100000U

static int clock_ns(uint64_t *value)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    *value = (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
             (uint64_t)ts.tv_nsec;
    return 0;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static int parse_count(const char *text, uint32_t *count)
{
    char *end;
    const char *p;
    unsigned long value;

    if (*text == '\0')
        return -1;

    for (p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 10);

    if (errno || *end || value == 0 || value > MAX_SAMPLES)
        return -1;

    *count = (uint32_t)value;
    return 0;
}

static uint64_t percentile(const uint64_t *sorted,
                           uint32_t count,
                           uint32_t percent)
{
    size_t rank = ((size_t)count * percent + 99U) / 100U;

    return sorted[rank - 1U];
}

int main(int argc, char **argv)
{
    AMPClient client;
    AMPMessage response;
    uint64_t *samples;
    uint32_t requested;
    uint32_t attempted = 0;
    uint32_t success = 0;
    uint32_t failed = 0;
    long double sum = 0;
    int exit_code = EXIT_SUCCESS;

    if (argc != 2 || parse_count(argv[1], &requested) != 0) {
        fprintf(stderr, "Usage: %s <count:1..%u>\n",
                argv[0], MAX_SAMPLES);
        return EXIT_FAILURE;
    }

    samples = calloc(requested, sizeof(*samples));
    if (samples == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    /*
     * amp_client_open 持有全程客户端锁。
     * 测量期间 HMI 或其他 ampctl 不能同时使用 IPC。
     */
    if (amp_client_open(&client) != 0) {
        perror("HELLO/connect");
        free(samples);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "session=%016" PRIx64
            " requested=%" PRIu32
            " payload_bytes=16 warmup=0\n",
            client.session, requested);

    printf("sample,sequence,result,rtt_ns,remote_status\n");

    for (uint32_t i = 0; i < requested; ++i) {
        uint8_t payload[16] = {0};
        uint64_t begin, end;
        int call_result;
        int call_errno;
        int valid;

        payload[0] = (uint8_t)i;
        payload[1] = (uint8_t)(i >> 8);
        payload[2] = (uint8_t)(i >> 16);
        payload[3] = (uint8_t)(i >> 24);

        for (unsigned int j = 4; j < sizeof(payload); ++j)
            payload[j] = (uint8_t)(0xA0U + j);

        if (clock_ns(&begin) != 0) {
            perror("clock_gettime");
            exit_code = EXIT_FAILURE;
            break;
        }

        ++attempted;

        call_result = amp_client_call(
            &client, AMP_OP_PING,
            payload, sizeof(payload), &response);
        call_errno = errno;

        if (clock_ns(&end) != 0) {
            ++failed;
            perror("clock_gettime");
            printf("%" PRIu32 ",%" PRIu32
                   ",CLOCK_ERROR,,\n",
                   i + 1U, client.sequence);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (call_result != 0) {
            ++failed;
            printf("%" PRIu32 ",%" PRIu32
                   ",CALL_ERROR,%" PRIu64 ",\n",
                   i + 1U, client.sequence, end - begin);
            fprintf(stderr, "Request failed: %s\n",
                    strerror(call_errno));
            exit_code = EXIT_FAILURE;
            break;
        }

        valid = response.status == AMP_RESULT_OK &&
                response.payload_length == sizeof(payload) &&
                memcmp(response.payload, payload, sizeof(payload)) == 0;

        if (!valid) {
            ++failed;
            printf("%" PRIu32 ",%" PRIu32
                   ",BAD_RESPONSE,%" PRIu64 ",%" PRIu32 "\n",
                   i + 1U, response.sequence,
                   end - begin, response.status);
            exit_code = EXIT_FAILURE;
            break;
        }

        samples[success++] = end - begin;
        sum += (long double)(end - begin);

        printf("%" PRIu32 ",%" PRIu32
               ",OK,%" PRIu64 ",%" PRIu32 "\n",
               i + 1U, response.sequence,
               end - begin, response.status);

        if (ferror(stdout)) {
            fprintf(stderr, "CSV write failed\n");
            exit_code = EXIT_FAILURE;
            break;
        }
    }

    amp_client_close(&client);

    fprintf(stderr,
            "attempted=%" PRIu32
            " success=%" PRIu32
            " failed=%" PRIu32
            " unattempted=%" PRIu32 "\n",
            attempted, success, failed,
            requested - attempted);

    if (success != 0) {
        qsort(samples, success, sizeof(*samples), compare_u64);

        fprintf(stderr,
                "successful_samples_only:\n"
                "min_us=%.3Lf\n"
                "mean_us=%.3Lf\n"
                "p95_us=%.3Lf\n"
                "p99_us=%.3Lf\n"
                "max_us=%.3Lf\n",
                (long double)samples[0] / 1000.0L,
                sum / success / 1000.0L,
                (long double)percentile(samples, success, 95) / 1000.0L,
                (long double)percentile(samples, success, 99) / 1000.0L,
                (long double)samples[success - 1U] / 1000.0L);
    }

    if (fflush(stdout) != 0) {
        perror("flush CSV");
        exit_code = EXIT_FAILURE;
    }

    free(samples);
    return exit_code;
}