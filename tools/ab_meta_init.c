#define _POSIX_C_SOURCE 200809L /* 启用 POSIX.1-2008 接口声明，如单调时钟和纳秒休眠 */

#include <errno.h>    /* 引入 errno 和系统调用错误码 */
#include <fcntl.h>    /* 引入文件打开选项与文件控制接口 */
#include <stdint.h>   /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include <stdio.h>    /* 引入标准输入输出接口，如 printf、fprintf */
#include <stdlib.h>   /* 引入数值转换、内存分配及进程退出等标准库接口 */
#include <string.h>   /* 引入内存复制、清零及字符串处理接口 */
#include <sys/stat.h> /* 引入文件属性查询及文件类型判断接口 */
#include <unistd.h>   /* 引入 POSIX 文件读写、关闭及其他系统接口 */

#include "../common/amp_ab.h"      /* 引入 A/B 启动槽状态机接口 */
#include "../common/amp_ab_meta.h" /* 引入启动元数据编码、校验及双副本选择接口 */

/* 与当前候选布局中 4 MiB 的 abmeta 分区一致。 */
#define IMAGE_BYTES (4U * 1024U * 1024U) /* 初始化工具生成的元数据镜像总容量，单位字节 */

static void fail(const char *message)
{
    /* 1. 输出错误上下文及系统错误信息。 */
    /* 2. 以失败状态结束当前工具。 */

    fprintf(stderr, "FAIL: %s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void write_full(int fd, const void *buffer, size_t bytes)
{
    /* 1. 循环读写剩余字节，处理短传输。 */
    /* 2. 遇到系统调用失败或不完整传输时报错。 */

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
    /* 1. 循环读写剩余字节，处理短传输。 */
    /* 2. 遇到系统调用失败或不完整传输时报错。 */

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
    /* 1. 比较启动状态或身份信息的各个字段。 */
    /* 2. 返回是否完全一致，用于检测状态漂移。 */

    return a->generation == b->generation && a->confirmed_slot == b->confirmed_slot &&
           a->pending_slot == b->pending_slot && a->tries_remaining == b->tries_remaining &&
           a->trial_slot == b->trial_slot;
}

int main(int argc, char **argv)
{
    /* 1. 解析输出路径并初始化默认 A 槽状态。 */
    /* 2. 编码并写入双副本元数据。 */
    /* 3. 同步后读回校验，输出初始化结果。 */

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

    if (initial.generation != 1 || initial.confirmed_slot != AMP_AB_SLOT_A ||
        initial.pending_slot != AMP_AB_SLOT_NONE || initial.tries_remaining != 0 ||
        initial.trial_slot != AMP_AB_SLOT_NONE || amp_ab_meta_encode(&initial, record) != 0) {
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
            amp_ab_meta_decode(check, &decoded) != 0 || !same_state(&initial, &decoded)) {
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
