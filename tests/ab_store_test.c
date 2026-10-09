#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../common/amp_ab_store.h"

#define BLOCK_SIZE 512U
#define DISK_BYTES (2U * AMP_AB_META_BYTES)

typedef struct {
    uint8_t disk[DISK_BYTES];
    unsigned int writes;
    unsigned int syncs;
    int fail_write;
    int fail_sync;
    int corrupt_on_sync;
} FakeDisk;

static FakeDisk disk;
static uint8_t initial_record[AMP_AB_META_BYTES];
static uint8_t proposed_record[AMP_AB_META_BYTES];
static uint8_t scratch[AMP_AB_META_BYTES];

static int fake_read(void *ctx, uint64_t lba, void *buf, size_t bytes)
{
    FakeDisk *d = ctx;

    if (bytes != BLOCK_SIZE || lba >= DISK_BYTES / BLOCK_SIZE)
        return -1;

    memcpy(buf, d->disk + (size_t)lba * BLOCK_SIZE, bytes);
    return 0;
}

static int fake_write(
    void *ctx, uint64_t lba, const void *buf, size_t bytes)
{
    FakeDisk *d = ctx;

    if (bytes != BLOCK_SIZE || lba >= DISK_BYTES / BLOCK_SIZE)
        return -1;

    if (d->fail_write >= 0 &&
        d->writes == (unsigned int)d->fail_write)
        return -1;

    memcpy(d->disk + (size_t)lba * BLOCK_SIZE, buf, bytes);
    d->writes++;
    return 0;
}

static int fake_sync(void *ctx)
{
    FakeDisk *d = ctx;

    d->syncs++;

    if (d->fail_sync)
        return -1;

    if (d->corrupt_on_sync)
        d->disk[AMP_AB_META_BYTES + 100] ^= 1U;

    return 0;
}

static AmpAbStore reset_store(void)
{
    AMPABState initial;
    AMPABState next;
    AmpAbStore store;

    memset(&disk, 0, sizeof(disk));
    disk.fail_write = -1;

    amp_ab_init(&initial);
    assert(amp_ab_stage(&initial, AMP_AB_SLOT_B, &next) == AMP_AB_OK);

    assert(amp_ab_meta_encode(&initial, initial_record) == AMP_AB_OK);
    assert(amp_ab_meta_encode(&next, proposed_record) == AMP_AB_OK);

    memcpy(disk.disk, initial_record, AMP_AB_META_BYTES);
    memcpy(disk.disk + AMP_AB_META_BYTES,
           initial_record, AMP_AB_META_BYTES);

    memset(&store, 0, sizeof(store));
    store.ctx = &disk;
    store.partition_blocks = DISK_BYTES / BLOCK_SIZE;
    store.copy_lba[0] = 0;
    store.copy_lba[1] = AMP_AB_META_BYTES / BLOCK_SIZE;
    store.block_size = BLOCK_SIZE;
    store.read_block = fake_read;
    store.write_block = fake_write;
    store.sync = fake_sync;

    return store;
}

int main(void)
{
    AmpAbStore store;
    AMPABMetaSelection selection;
    unsigned int cut;

    store = reset_store();

    assert(amp_ab_store_read(&store, 0, scratch, sizeof(scratch)) == 0);
    assert(memcmp(scratch, initial_record, sizeof(scratch)) == 0);

    /* 512 字节块设备上，必须完整写入 8 块。 */
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch)) == 0);
    assert(disk.writes == 8);
    assert(disk.syncs == 1);
    assert(memcmp(disk.disk, initial_record, AMP_AB_META_BYTES) == 0);
    assert(memcmp(disk.disk + AMP_AB_META_BYTES,
                  proposed_record, AMP_AB_META_BYTES) == 0);

    assert(amp_ab_meta_select(
        disk.disk, disk.disk + AMP_AB_META_BYTES, &selection) == 0);
    assert(selection.selected_copy == 1);
    assert(selection.state.generation == 2);

    /* 旧调用方不能覆盖已经成为最新副本的目标。 */
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch))
        == AMP_AB_STORE_ESTALE);
    assert(disk.writes == 8);

    /* 缺少同步接口：一次写入都不能发生。 */
    store = reset_store();
    store.sync = NULL;
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch))
        == AMP_AB_STORE_ESYNC);
    assert(disk.writes == 0);

    /* 模拟每个块写入前发生失败，原有效副本必须保留。 */
    for (cut = 0; cut < 8; ++cut) {
        store = reset_store();
        disk.fail_write = (int)cut;

        assert(amp_ab_store_commit_other(
            &store, 0, proposed_record, scratch, sizeof(scratch))
            == AMP_AB_STORE_EIO);

        assert(memcmp(disk.disk, initial_record,
                      AMP_AB_META_BYTES) == 0);
        assert(disk.syncs == 0);

        assert(amp_ab_meta_select(
            disk.disk, disk.disk + AMP_AB_META_BYTES, &selection) == 0);
        assert(selection.selected_copy == 0);
        assert(selection.state.generation == 1);
    }

    /* 刷新失败：不得报告提交成功。 */
    store = reset_store();
    disk.fail_sync = 1;
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch))
        == AMP_AB_STORE_ESYNC);
    assert(memcmp(disk.disk, initial_record, AMP_AB_META_BYTES) == 0);

    /* 读回发生内容不一致：不得报告成功。 */
    store = reset_store();
    disk.corrupt_on_sync = 1;
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch))
        == AMP_AB_STORE_EVERIFY);

    /* 副本范围重叠。 */
    store = reset_store();
    store.copy_lba[1] = 1;
    assert(amp_ab_store_read(&store, 0, scratch, sizeof(scratch))
           == AMP_AB_STORE_EINVAL);

    /* 分区装不下第二份完整记录。 */
    store = reset_store();
    store.partition_blocks--;
    assert(amp_ab_store_read(&store, 0, scratch, sizeof(scratch))
           == AMP_AB_STORE_EINVAL);

    /* 两份均无效，不能通过提交接口自动初始化。 */
    store = reset_store();
    memset(disk.disk, 0, sizeof(disk.disk));
    assert(amp_ab_store_commit_other(
        &store, 0, proposed_record, scratch, sizeof(scratch))
        == AMP_AB_STORE_EVERIFY);
    assert(disk.writes == 0);

    puts("PASS: 完整记录读写、同步要求、旧版本拒绝、部分写失败及读回校验");
    return 0;
}