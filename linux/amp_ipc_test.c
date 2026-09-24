#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../common/amp_protocol.h"

#if !defined(__aarch64__)
#error "Build this program for the RK3588 AArch64 Linux target."
#endif

#define TIMEOUT_MS 2000ULL

static inline void ipc_barrier(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

static int monotonic_ms(uint64_t *value)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    *value = (uint64_t)ts.tv_sec * 1000ULL +
             (uint64_t)ts.tv_nsec / 1000000ULL;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long long number;

    if (*text == '\0' || *text == '-')
        return -1;

    errno = 0;
    number = strtoull(text, &end, 0);

    if (errno != 0 || *end != '\0' || number > UINT32_MAX)
        return -1;

    *value = (uint32_t)number;
    return 0;
}

int main(int argc, char **argv)
{
    int lock_fd = -1;
    int mem_fd = -1;
    int exit_code = EXIT_FAILURE;
    void *mapping = MAP_FAILED;
    volatile AMPSharedIPC *ipc;
    uint32_t arg0, arg1, expected;
    uint32_t request_seq, response_seq, seq;
    uint32_t status, result;
    uint64_t begin_ms, now_ms;
    const struct timespec poll_delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000
    };

    if (argc != 3 ||
        parse_u32(argv[1], &arg0) != 0 ||
        parse_u32(argv[2], &arg1) != 0) {
        fprintf(stderr,
                "Usage: %s <uint32 arg0> <uint32 arg1>\n"
                "Example: %s 123 456\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    expected = arg0 + arg1;

    /*
     * Cooperative single-client lock.
     * All Linux clients using this protocol must take this lock.
     * Do not unlink the lock file when exiting.
     */
    lock_fd = open("/run/amp-ipc-test.lock",
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                   0600);
    if (lock_fd < 0) {
        perror("open lock");
        goto cleanup;
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        perror("lock: another client may be running");
        goto cleanup;
    }

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) {
        perror("open /dev/mem");
        goto cleanup;
    }

    mapping = mmap(NULL,
                   AMP_IPC_MAP_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   mem_fd,
                   (off_t)AMP_IPC_PHYS_ADDR);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }

    ipc = (volatile AMPSharedIPC *)mapping;

    if (ipc->magic != AMP_IPC_MAGIC) {
        fprintf(stderr,
                "IPC not ready: magic=0x%08" PRIx32 "\n"
                "Start FreeRTOS first.\n",
                ipc->magic);
        goto cleanup;
    }

    ipc_barrier();

    if (ipc->version != AMP_IPC_VERSION) {
        fprintf(stderr, "IPC version mismatch: %" PRIu32 "\n",
                ipc->version);
        goto cleanup;
    }

    request_seq = ipc->request_seq;
    response_seq = ipc->response_seq;

    if (request_seq != response_seq) {
        fprintf(stderr,
                "Previous request is still pending: request=%" PRIu32
                ", response=%" PRIu32 "\n",
                request_seq, response_seq);
        goto cleanup;
    }

    seq = request_seq + 1U;
    if (seq == 0)
        seq = 1;

    if (monotonic_ms(&begin_ms) != 0) {
        perror("clock_gettime");
        goto cleanup;
    }

    /* Write payload first, publish the sequence last. */
    ipc->command = AMP_CMD_ADD;
    ipc->arg0 = arg0;
    ipc->arg1 = arg1;

    ipc_barrier();
    ipc->request_seq = seq;
    ipc_barrier();

    for (;;) {
        if (ipc->response_seq == seq) {
            /* Acquire response payload after observing its sequence. */
            ipc_barrier();

            status = ipc->status;
            result = ipc->result;
            break;
        }

        if (monotonic_ms(&now_ms) != 0) {
            perror("clock_gettime");
            goto cleanup;
        }

        if (now_ms - begin_ms >= TIMEOUT_MS) {
            fprintf(stderr,
                    "Timeout: request=%" PRIu32
                    ", response=%" PRIu32 "\n",
                    seq, ipc->response_seq);
            goto cleanup;
        }

        /* Poll approximately every 1 ms. */
        nanosleep(&poll_delay, NULL);
    }

    if (monotonic_ms(&now_ms) != 0) {
        perror("clock_gettime");
        goto cleanup;
    }

    if (status != AMP_STATUS_OK || result != expected) {
        fprintf(stderr,
                "FAIL: seq=%" PRIu32 ", status=%" PRIu32
                ", result=%" PRIu32 ", expected=%" PRIu32 "\n",
                seq, status, result, expected);
        goto cleanup;
    }

    printf("PASS: seq=%" PRIu32
           ", %" PRIu32 " + %" PRIu32 " = %" PRIu32
           ", elapsed=%" PRIu64 " ms\n",
           seq, arg0, arg1, result, now_ms - begin_ms);

    exit_code = EXIT_SUCCESS;

cleanup:
    if (mapping != MAP_FAILED)
        munmap(mapping, AMP_IPC_MAP_SIZE);

    if (mem_fd >= 0)
        close(mem_fd);

    if (lock_fd >= 0)
        close(lock_fd);

    return exit_code;
}