#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "../common/amp_ab_store.h"

/* Compile-time gate; default binary never opens the device for writing. */
#ifndef AMP_AB_CONFIRM_WRITE
#define AMP_AB_CONFIRM_WRITE 0
#endif
static unsigned char copies[2][AMP_AB_META_BYTES], encoded[AMP_AB_META_BYTES];
static unsigned char scratch[AMP_AB_META_BYTES];
static int io_read(void *ctx, uint64_t lba, void *buf, size_t n)
{
    ssize_t got;
    do { got = pread(*(int *)ctx, buf, n, (off_t)(lba * 512)); }
    while (got < 0 && errno == EINTR);
    return got == (ssize_t)n ? 0 : -1;
}
static int io_write(void *ctx, uint64_t lba, const void *buf, size_t n)
{
    ssize_t got;
    do { got = pwrite(*(int *)ctx, buf, n, (off_t)(lba * 512)); }
    while (got < 0 && errno == EINTR);
    return got == (ssize_t)n ? 0 : -1;
}
static int io_sync(void *ctx) { return fsync(*(int *)ctx); }
static void fail(const char *why)
{
    fprintf(stderr, "AB confirm refused: %s (errno=%d)\n", why, errno);
    exit(1);
}
static uint64_t number(const char *s)
{
    char *end;
    unsigned long long n;
    if (!s || !*s || strspn(s, "0123456789") != strlen(s)) fail("invalid number");
    errno = 0;
    n = strtoull(s, &end, 10);
    if (errno || *end) fail("number overflow");
    return n;
}
static void parent(dev_t dev, char out[4096])
{
    char link[128], *slash;
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(dev), minor(dev));
    if (!realpath(link, out)) fail("sysfs device missing");
    slash = strrchr(out, '/');
    if (!slash) fail("sysfs parent missing");
    *slash = 0;
}
static int same(const AMPABState *a, const AMPABState *b)
{
    return a->generation == b->generation && a->confirmed_slot == b->confirmed_slot &&
           a->pending_slot == b->pending_slot && a->tries_remaining == b->tries_remaining &&
           a->trial_slot == b->trial_slot;
}
int main(int argc, char **argv)
{
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
        fprintf(stderr, "usage: amp_ab_confirm --check|--commit\n"); return 2;
    }
    commit = !strcmp(argv[1], "--commit");
    if (commit && !AMP_AB_CONFIRM_WRITE) fail("write gate disabled at build time");
    fp = fopen("/proc/cmdline", "r");
    if (!fp || !fgets(line, sizeof(line), fp) || !strchr(line, '\n')) fail("cmdline read");
    fclose(fp);
    for (tok = strtok_r(line, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        char **dst = NULL, *value = NULL;
        if (!strncmp(tok, "amp_ab.slot=", 12)) { dst = &slot_s; value = tok + 12; }
        if (!strncmp(tok, "amp_ab.generation=", 18)) { dst = &gen_s; value = tok + 18; }
        if (!strncmp(tok, "amp_ab.trial=", 13)) { dst = &trial_s; value = tok + 13; }
        if (!strncmp(tok, "root=", 5)) { dst = &root_s; value = tok + 5; }
        if (dst) { if (*dst) fail("duplicate boot argument"); *dst = value; }
    }
    if (!slot_s || (strcmp(slot_s, "A") && strcmp(slot_s, "B")) ||
        !root_s || strncmp(root_s, "PARTUUID=", 9)) fail("not an A/B boot");
    slot = !strcmp(slot_s, "B");
    generation = number(gen_s);
    trial = number(trial_s);
    if (!generation || trial > 1 || strlen(root_s + 9) != 36 ||
        strspn(root_s + 9, "0123456789abcdefABCDEF-") != 36) fail("invalid boot identity");
    snprintf(path, sizeof(path), "/dev/disk/by-partuuid/%s", root_s + 9);
    if (stat(path, &byuuid) || !S_ISBLK(byuuid.st_mode)) fail("root PARTUUID cannot be resolved");
    snprintf(path, sizeof(path), "/dev/block/by-name/%s", slot ? "rootfs_b" : "rootfs");
    if (stat(path, &partition) || !S_ISBLK(partition.st_mode) || stat("/", &root) ||
        root.st_dev != partition.st_rdev || byuuid.st_rdev != partition.st_rdev)
        fail("mounted root is not the selected slot");
    lock = open("/run/amp-ab.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) fail("metadata writer is busy");
    fd = open("/dev/block/by-name/abmeta", (commit ? O_RDWR | O_SYNC : O_RDONLY) | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &meta) || !S_ISBLK(meta.st_mode) ||
        ioctl(fd, BLKSSZGET, &sector) || sector != 512 ||
        ioctl(fd, BLKGETSIZE64, &bytes) || bytes < 8192) fail("invalid abmeta device");
    parent(partition.st_rdev, par_root);
    parent(meta.st_rdev, par_meta);
    if (strcmp(par_root, par_meta) || partition.st_rdev == meta.st_rdev)
        fail("rootfs/abmeta not distinct partitions on the same device");
    memset(&store, 0, sizeof(store));
    store.ctx = &fd; store.partition_blocks = bytes / 512;
    store.block_size = 512; store.copy_lba[1] = 8;
    store.read_block = io_read; store.write_block = io_write; store.sync = io_sync;
    for (i = 0; i < 2; i++)
        if (amp_ab_store_read(&store, i, copies[i], sizeof(copies[i]))) fail("metadata read");
    if (amp_ab_meta_select(copies[0], copies[1], &selected)) fail("metadata invalid/conflicting");
    if (trial && selected.state.generation != generation) {
        /* Idempotency is accepted only if the retained previous record proves
         * this exact boot was confirmed, not merely because slot names match. */
        if (!amp_ab_meta_decode(copies[selected.selected_copy ^ 1U], &prior) &&
            !amp_ab_confirm(&prior, slot, generation, &expected) && same(&expected, &selected.state)) {
            puts("ALREADY_CONFIRMED"); close(fd); close(lock); return 0;
        }
        fail("stale boot generation");
    }
    if (!trial) {
        if (selected.state.generation != generation || selected.state.confirmed_slot != slot ||
            selected.state.pending_slot != AMP_AB_SLOT_NONE) fail("confirmed boot identity mismatch");
        puts("CONFIRMED_BOOT"); close(fd); close(lock); return 0;
    }
    if (amp_ab_confirm(&selected.state, slot, generation, &next)) fail("trial identity mismatch");
    if (!commit) {
        printf("TRIAL slot=%c generation=%" PRIu64 "\n", slot ? 'B' : 'A', generation);
        close(fd); close(lock); return 0;
    }
    if (amp_ab_meta_encode(&next, encoded) ||
        amp_ab_store_commit_other(&store, selected.selected_copy, encoded, scratch, sizeof(scratch)))
        fail("confirmation persistence failed; reread before any retry");
    for (i = 0; i < 2; i++)
        if (amp_ab_store_read(&store, i, copies[i], sizeof(copies[i]))) fail("confirmation reread");
    if (amp_ab_meta_select(copies[0], copies[1], &verified) || !same(&verified.state, &next))
        fail("confirmation verification");
    printf("CONFIRMED slot=%c generation=%" PRIu64 "\n", slot ? 'B' : 'A', next.generation);
    close(fd); close(lock); return 0;
}
