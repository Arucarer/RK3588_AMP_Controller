#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../common/amp_ab_boot.h"

#define BLOCK_BYTES 512U
#define DISK_BYTES (2U * AMP_AB_META_BYTES)

typedef struct {
    uint8_t data[DISK_BYTES];
    unsigned int writes;
    unsigned int syncs;
    int sync_error;
    int preflight_error;
} FakeDevice;

static FakeDevice device;
static AmpAbBootWorkspace workspace;

static int read_block(
    void *ctx, uint64_t lba, void *buf, size_t bytes)
{
    FakeDevice *d = ctx;

    if (bytes != BLOCK_BYTES ||
        lba >= DISK_BYTES / BLOCK_BYTES)
        return -1;

    memcpy(buf, d->data + (size_t)lba * BLOCK_BYTES, bytes);
    return 0;
}

static int write_block(
    void *ctx, uint64_t lba, const void *buf, size_t bytes)
{
    FakeDevice *d = ctx;

    if (bytes != BLOCK_BYTES ||
        lba >= DISK_BYTES / BLOCK_BYTES)
        return -1;

    memcpy(d->data + (size_t)lba * BLOCK_BYTES, buf, bytes);
    d->writes++;
    return 0;
}

static int sync_device(void *ctx)
{
    FakeDevice *d = ctx;

    d->syncs++;
    return d->sync_error;
}

static int preflight(void *ctx, uint32_t slot)
{
    FakeDevice *d = ctx;

    assert(slot == AMP_AB_SLOT_A || slot == AMP_AB_SLOT_B);
    return d->preflight_error;
}

/* 仅测试夹具初始化，不是生产环境元数据初始化接口。 */
static AmpAbStore fixture(const AMPABState *state)
{
    AmpAbStore store;

    memset(&device, 0, sizeof(device));
    memset(&store, 0, sizeof(store));

    assert(amp_ab_meta_encode(state, device.data) == AMP_AB_OK);
    memcpy(device.data + AMP_AB_META_BYTES,
           device.data, AMP_AB_META_BYTES);

    store.ctx = &device;
    store.partition_blocks = DISK_BYTES / BLOCK_BYTES;
    store.copy_lba[0] = 0;
    store.copy_lba[1] = AMP_AB_META_BYTES / BLOCK_BYTES;
    store.block_size = BLOCK_BYTES;
    store.read_block = read_block;
    store.write_block = write_block;
    store.sync = sync_device;

    return store;
}

static int boot_once(AmpAbStore *store, AmpAbBootResult *result)
{
    return amp_ab_boot_transaction(
        store, preflight, &device, &workspace, result);
}

int main(void)
{
    AMPABState initial;
    AMPABState pending;
    AmpAbStore store;
    AmpAbBootResult result;
    unsigned int attempt;

    amp_ab_init(&initial);

    /* 正常 A 槽：无需写元数据。 */
    store = fixture(&initial);
    assert(boot_once(&store, &result) == AMP_AB_BOOT_OK);
    assert(result.ready == 1);
    assert(result.slot == AMP_AB_SLOT_A);
    assert(result.generation == 1);
    assert(result.metadata_committed == 0);
    assert(device.writes == 0);
    assert(device.syncs == 0);

    assert(amp_ab_stage(
        &initial, AMP_AB_SLOT_B, &pending) == AMP_AB_OK);

    /*
     * 每次调用模拟下一次启动。
     * 三次未确认的 B 启动后，第四次持久化回退到 A。
     */
    store = fixture(&pending);

    for (attempt = 0; attempt < AMP_AB_MAX_TRIES; ++attempt) {
        assert(boot_once(&store, &result) == AMP_AB_BOOT_OK);
        assert(result.ready == 1);
        assert(result.slot == AMP_AB_SLOT_B);
        assert(result.metadata_committed == 1);
        assert(result.rolled_back == 0);
        assert(result.state.tries_remaining ==
               AMP_AB_MAX_TRIES - attempt - 1);
        assert(result.generation ==
               pending.generation + attempt + 1);
    }

    assert(boot_once(&store, &result) == AMP_AB_BOOT_OK);
    assert(result.ready == 1);
    assert(result.slot == AMP_AB_SLOT_A);
    assert(result.rolled_back == 1);
    assert(result.state.pending_slot == AMP_AB_SLOT_NONE);
    assert(device.syncs == 4);

    /* 槽资源检查失败：不扣次、不写元数据。 */
    store = fixture(&pending);
    device.preflight_error = -77;
    assert(boot_once(&store, &result) == AMP_AB_BOOT_EPREFLIGHT);
    assert(result.ready == 0);
    assert(result.detail == -77);
    assert(device.writes == 0);

    /* 缺少同步接口：不能允许候选槽启动。 */
    store = fixture(&pending);
    store.sync = NULL;
    assert(boot_once(&store, &result) == AMP_AB_BOOT_ECOMMIT);
    assert(result.ready == 0);
    assert(device.writes == 0);

    /* 同步失败：即使内存模拟盘已写入，也不得返回 ready。 */
    store = fixture(&pending);
    device.sync_error = -1;
    assert(boot_once(&store, &result) == AMP_AB_BOOT_ECOMMIT);
    assert(result.ready == 0);

    /* 元数据均无效：禁止自动初始化或默认启动 A。 */
    store = fixture(&initial);
    memset(device.data, 0, sizeof(device.data));
    assert(boot_once(&store, &result) == AMP_AB_BOOT_EMETA);
    assert(result.ready == 0);
    assert(device.writes == 0);

    /* 需要推进代次时遇到溢出：拒绝启动事务。 */
    pending.generation = UINT64_MAX;
    store = fixture(&pending);
    assert(boot_once(&store, &result) == AMP_AB_BOOT_ESTATE);
    assert(result.ready == 0);
    assert(device.writes == 0);

    puts("PASS: 启动事务、持久化扣次、三次失败回退及失败禁止启动");
    return 0;
}
