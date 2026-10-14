#define _GNU_SOURCE        /* 启用 GNU 扩展接口及其依赖的标准接口声明 */
#include <errno.h>         /* 引入 errno 和系统调用错误码 */
#include <fcntl.h>         /* 引入文件打开选项与文件控制接口 */
#include <inttypes.h>      /* 引入固定宽度整数的 printf 格式宏 */
#include <linux/fs.h>      /* 引入 Linux 块设备控制命令 */
#include <stdio.h>         /* 引入标准输入输出接口，如 printf、fprintf */
#include <stdlib.h>        /* 引入数值转换、内存分配及进程退出等标准库接口 */
#include <string.h>        /* 引入内存复制、清零及字符串处理接口 */
#include <sys/file.h>      /* 引入 flock 文件锁接口 */
#include <sys/ioctl.h>     /* 引入 ioctl 设备控制接口 */
#include <sys/stat.h>      /* 引入文件属性查询及文件类型判断接口 */
#include <sys/sysmacros.h> /* 引入设备号的 major、minor 等辅助宏 */
#include <unistd.h>        /* 引入 POSIX 文件读写、关闭及其他系统接口 */
#include "../common/amp_ab_store.h" /* 引入启动元数据块存储及提交接口 */

/* Compile-time gate; default binary never opens the device for writing. */
#ifndef AMP_AB_CONFIRM_WRITE /* 未由编译参数指定 AMP_AB_CONFIRM_WRITE 时，使用下面的默认配置 */
#define AMP_AB_CONFIRM_WRITE 0 /* 编译期开关：0 禁止写入确认元数据，可由编译参数覆盖 */
#endif                         /* 结束 AMP_AB_CONFIRM_WRITE 对应的条件编译区段 */
static unsigned char copies[2][AMP_AB_META_BYTES]; /* 从磁盘读取的两份元数据 */
static unsigned char encoded[AMP_AB_META_BYTES];   /* 待提交的编码后元数据 */
static unsigned char scratch[AMP_AB_META_BYTES]; /* scratch：元数据读写校验用临时缓冲区 */
static int io_read(void *ctx, uint64_t lba, void *buf, size_t n)
{
    /* 1. 根据存储接口请求访问元数据设备。 */
    /* 2. 检查系统调用结果并向上层返回错误。 */

    ssize_t got;
    do {
        got = pread(*(int *)ctx, buf, n, (off_t)(lba * 512));
    } while (got < 0 && errno == EINTR);
    return got == (ssize_t)n ? 0 : -1;
}

static int io_write(void *ctx, uint64_t lba, const void *buf, size_t n)
{
    /* 1. 根据存储接口请求访问元数据设备。 */
    /* 2. 检查系统调用结果并向上层返回错误。 */

    ssize_t got;
    do {
        got = pwrite(*(int *)ctx, buf, n, (off_t)(lba * 512));
    } while (got < 0 && errno == EINTR);
    return got == (ssize_t)n ? 0 : -1;
}

static int io_sync(void *ctx)
{
    /* 1. 根据存储接口请求访问元数据设备。 */
    /* 2. 检查系统调用结果并向上层返回错误。 */

    return fsync(*(int *)ctx);
}

static void fail(const char *why)
{
    /* 1. 输出错误上下文及系统错误信息。 */
    /* 2. 以失败状态结束当前工具。 */

    fprintf(stderr, "AB confirm refused: %s (errno=%d)\n", why, errno);
    exit(1);
}

static uint64_t number(const char *s)
{
    /* 1. 把文本转换为整数。 */
    /* 2. 检查解析终点、溢出及允许范围。 */
    /* 3. 返回合法数值，非法输入进入错误路径。 */

    char *end;
    unsigned long long n;
    if (!s || !*s || strspn(s, "0123456789") != strlen(s))
        fail("invalid number");
    errno = 0;
    n = strtoull(s, &end, 10);
    if (errno || *end)
        fail("number overflow");
    return n;
}

static void parent(dev_t dev, char out[4096])
{
    /* 1. 从设备标识或路径推导父设备信息。 */
    /* 2. 检查路径有效性后返回结果。 */

    char link[128], *slash;
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(dev), minor(dev));
    if (!realpath(link, out))
        fail("sysfs device missing");
    slash = strrchr(out, '/');
    if (!slash)
        fail("sysfs parent missing");
    *slash = 0;
}

static int same(const AMPABState *a, const AMPABState *b)
{
    /* 1. 比较启动状态或身份信息的各个字段。 */
    /* 2. 返回是否完全一致，用于检测状态漂移。 */

    return a->generation == b->generation && a->confirmed_slot == b->confirmed_slot &&
           a->pending_slot == b->pending_slot && a->tries_remaining == b->tries_remaining &&
           a->trial_slot == b->trial_slot;
}

int main(int argc, char **argv)
{
    /* 1. 解析检查或确认模式，读取启动身份及元数据。 */
    /* 2. 核对运行槽、代次及确认前置条件。 */
    /* 3. 按模式检查或持久化启动确认，输出结果。 */

    char line[8192], *save, *tok, *slot_s = NULL, *gen_s = NULL, *trial_s = NULL, *root_s = NULL;
    char path[256], par_root[4096], par_meta[4096];
    struct stat root, partition, byuuid, meta;
    FILE *fp;
    uint64_t generation, trial, bytes;
    uint32_t slot;
    int fd, lock, sector, commit, i;
    AMPABMetaSelection selected, verified;
    AMPABState next, prior, expected;
    AmpAbStore store;

    if (argc != 2 || (strcmp(argv[1], "--check") && strcmp(argv[1], "--commit"))) {
        fprintf(stderr, "usage: amp_ab_confirm --check|--commit\n");
        return 2;
    }
    commit = !strcmp(argv[1], "--commit");
    if (commit && !AMP_AB_CONFIRM_WRITE)
        fail("write gate disabled at build time");
    fp = fopen("/proc/cmdline", "r");
    if (!fp || !fgets(line, sizeof(line), fp) || !strchr(line, '\n'))
        fail("cmdline read");
    fclose(fp);
    for (tok = strtok_r(line, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        char **dst = NULL, *value = NULL;
        if (!strncmp(tok, "amp_ab.slot=", 12)) {
            dst = &slot_s;
            value = tok + 12;
        }
        if (!strncmp(tok, "amp_ab.generation=", 18)) {
            dst = &gen_s;
            value = tok + 18;
        }
        if (!strncmp(tok, "amp_ab.trial=", 13)) {
            dst = &trial_s;
            value = tok + 13;
        }
        if (!strncmp(tok, "root=", 5)) {
            dst = &root_s;
            value = tok + 5;
        }
        if (dst) {
            if (*dst)
                fail("duplicate boot argument");
            *dst = value;
        }
    }
    if (!slot_s || (strcmp(slot_s, "A") && strcmp(slot_s, "B")) || !root_s ||
        strncmp(root_s, "PARTUUID=", 9))
        fail("not an A/B boot");
    slot = !strcmp(slot_s, "B");
    generation = number(gen_s);
    trial = number(trial_s);
    if (!generation || trial > 1 || strlen(root_s + 9) != 36 ||
        strspn(root_s + 9, "0123456789abcdefABCDEF-") != 36)
        fail("invalid boot identity");
    snprintf(path, sizeof(path), "/dev/disk/by-partuuid/%s", root_s + 9);
    if (stat(path, &byuuid) || !S_ISBLK(byuuid.st_mode))
        fail("root PARTUUID cannot be resolved");
    snprintf(path, sizeof(path), "/dev/block/by-name/%s", slot ? "rootfs_b" : "rootfs");
    if (stat(path, &partition) || !S_ISBLK(partition.st_mode) || stat("/", &root) ||
        root.st_dev != partition.st_rdev || byuuid.st_rdev != partition.st_rdev)
        fail("mounted root is not the selected slot");
    lock = open("/run/amp-ab.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        fail("metadata writer is busy");
    fd = open("/dev/block/by-name/abmeta", (commit ? O_RDWR | O_SYNC : O_RDONLY) | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &meta) || !S_ISBLK(meta.st_mode) || ioctl(fd, BLKSSZGET, &sector) ||
        sector != 512 || ioctl(fd, BLKGETSIZE64, &bytes) || bytes < 8192)
        fail("invalid abmeta device");
    parent(partition.st_rdev, par_root);
    parent(meta.st_rdev, par_meta);
    if (strcmp(par_root, par_meta) || partition.st_rdev == meta.st_rdev)
        fail("rootfs/abmeta not distinct partitions on the same device");
    memset(&store, 0, sizeof(store));
    store.ctx = &fd;
    store.partition_blocks = bytes / 512;
    store.block_size = 512;
    store.copy_lba[1] = 8;
    store.read_block = io_read;
    store.write_block = io_write;
    store.sync = io_sync;
    for (i = 0; i < 2; i++)
        if (amp_ab_store_read(&store, i, copies[i], sizeof(copies[i])))
            fail("metadata read");
    if (amp_ab_meta_select(copies[0], copies[1], &selected))
        fail("metadata invalid/conflicting");
    if (trial && selected.state.generation != generation) {
        /* Idempotency is accepted only if the retained previous record proves
         * this exact boot was confirmed, not merely because slot names match. */
        if (!amp_ab_meta_decode(copies[selected.selected_copy ^ 1U], &prior) &&
            !amp_ab_confirm(&prior, slot, generation, &expected) &&
            same(&expected, &selected.state)) {
            puts("ALREADY_CONFIRMED");
            close(fd);
            close(lock);
            return 0;
        }
        fail("stale boot generation");
    }
    if (!trial) {
        if (selected.state.generation != generation || selected.state.confirmed_slot != slot ||
            selected.state.pending_slot != AMP_AB_SLOT_NONE)
            fail("confirmed boot identity mismatch");
        puts("CONFIRMED_BOOT");
        close(fd);
        close(lock);
        return 0;
    }
    if (amp_ab_confirm(&selected.state, slot, generation, &next))
        fail("trial identity mismatch");
    if (!commit) {
        printf("TRIAL slot=%c generation=%" PRIu64 "\n", slot ? 'B' : 'A', generation);
        close(fd);
        close(lock);
        return 0;
    }
    if (amp_ab_meta_encode(&next, encoded) ||
        amp_ab_store_commit_other(&store, selected.selected_copy, encoded, scratch,
                                  sizeof(scratch)))
        fail("confirmation persistence failed; reread before any retry");
    for (i = 0; i < 2; i++)
        if (amp_ab_store_read(&store, i, copies[i], sizeof(copies[i])))
            fail("confirmation reread");
    if (amp_ab_meta_select(copies[0], copies[1], &verified) || !same(&verified.state, &next))
        fail("confirmation verification");
    printf("CONFIRMED slot=%c generation=%" PRIu64 "\n", slot ? 'B' : 'A', next.generation);
    close(fd);
    close(lock);
    return 0;
}
