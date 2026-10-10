#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/amp_ab.h"
#include "../common/amp_ab_meta.h"

/* 与当前候选布局中 4 MiB 的 abmeta 分区一致。 */
#define IMAGE_BYTES (4U * 1024U * 1024U)

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void write_full(int fd, const void *buffer, size_t bytes)
{
    const unsigned char *p = buffer;

    while (bytes != 0) {
        ssize_t n = write(fd, p, bytes);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            fail("write");
        }

        p += (size_t)n;
        bytes -= (size_t)n;
    }
}

static void read_full(int fd, void *buffer, size_t bytes)
{
    unsigned char *p = buffer;

    while (bytes != 0) {
        ssize_t n = read(fd, p, bytes);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            fail("read back");
        }

        p += (size_t)n;
        bytes -= (size_t)n;
    }
}

static int same_state(const AMPABState *a, const AMPABState *b)
{
    return a->generation == b->generation &&
           a->confirmed_slot == b->confirmed_slot &&
           a->pending_slot == b->pending_slot &&
           a->tries_remaining == b->tries_remaining &&
           a->trial_slot == b->trial_slot;
}

int main(int argc, char **argv)
{
    AMPABState initial, decoded;
    unsigned char record[AMP_AB_META_BYTES];
    unsigned char check[AMP_AB_META_BYTES];
    unsigned char zero[4096] = {0};
    struct stat info;
    size_t written;
    unsigned int copy;
    int fd;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s NEW_OUTPUT_IMAGE\n", argv[0]);
        return EXIT_FAILURE;
    }

    amp_ab_init(&initial);

    if (initial.generation != 1 ||
        initial.confirmed_slot != AMP_AB_SLOT_A ||
        initial.pending_slot != AMP_AB_SLOT_NONE ||
        initial.tries_remaining != 0 ||
        initial.trial_slot != AMP_AB_SLOT_NONE ||
        amp_ab_meta_encode(&initial, record) != 0) {
        fprintf(stderr, "FAIL: unexpected initial state or encoding\n");
        return EXIT_FAILURE;
    }

    /*
     * O_EXCL:
     * - 拒绝覆盖已有镜像；
     * - 拒绝已有块设备；
     * - 拒绝已有符号链接。
     */
    fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        fail("create new output");

    if (fstat(fd, &info) != 0)
        fail("stat output");

    if (!S_ISREG(info.st_mode)) {
        fprintf(stderr, "FAIL: output is not a regular file\n");
        close(fd);
        return EXIT_FAILURE;
    }

    /* 两份初始化记录相同，放在偏移 0 和 4096。 */
    write_full(fd, record, sizeof(record));
    write_full(fd, record, sizeof(record));

    /* 明确写零剩余空间，不依赖稀疏文件。 */
    written = sizeof(record) * 2U;
    while (written < IMAGE_BYTES) {
        size_t count = IMAGE_BYTES - written;

        if (count > sizeof(zero))
            count = sizeof(zero);

        write_full(fd, zero, count);
        written += count;
    }

    if (fsync(fd) != 0)
        fail("fsync output");

    if (fstat(fd, &info) != 0)
        fail("stat completed output");

    if (info.st_size != (off_t)IMAGE_BYTES) {
        fprintf(stderr, "FAIL: output size mismatch\n");
        close(fd);
        return EXIT_FAILURE;
    }

    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("seek");

    for (copy = 0; copy < 2; ++copy) {
        read_full(fd, check, sizeof(check));

        if (memcmp(record, check, sizeof(record)) != 0 ||
            amp_ab_meta_decode(check, &decoded) != 0 ||
            !same_state(&initial, &decoded)) {
            fprintf(stderr, "FAIL: copy %u verification\n", copy);
            close(fd);
            return EXIT_FAILURE;
        }
    }

    while (written > sizeof(record) * 2U) {
        size_t count = written - sizeof(record) * 2U;
        size_t i;

        if (count > sizeof(check))
            count = sizeof(check);

        read_full(fd, check, count);
        for (i = 0; i < count; ++i) {
            if (check[i] != 0) {
                fprintf(stderr, "FAIL: nonzero reserved image area\n");
                close(fd);
                return EXIT_FAILURE;
            }
        }
        written -= count;
    }

    if (close(fd) != 0)
        fail("close output");

    printf("PASS: %s\n", argv[1]);
    puts("4 MiB; two verified records; generation=1; confirmed=A");
    puts("Only a local file was created; no partition was written.");
    return EXIT_SUCCESS;
}
