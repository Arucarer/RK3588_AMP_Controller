#include <assert.h> /* 引入 assert 断言，用于验证测试预期 */
#include <stdio.h>  /* 引入标准输入输出接口，如 printf、fprintf */
#include <string.h> /* 引入内存复制、清零及字符串处理接口 */

#include "../common/amp_ab_boot.h" /* 引入 A/B 启动事务及槽预检接口 */

#define BLOCK_BYTES 512U                    /* 模拟设备的逻辑块大小，单位字节 */
#define DISK_BYTES (2U * AMP_AB_META_BYTES) /* 测试内存磁盘总容量，恰好容纳两份元数据记录 */

/* 启动事务测试的模拟块设备及失败开关。 */
typedef struct {
    uint8_t data[DISK_BYTES]; /* 模拟分区字节数据，包含两份元数据记录 */
    unsigned int writes;      /* 累计成功写入的逻辑块数 */
    unsigned int syncs;       /* 累计同步回调调用次数 */
    int sync_error;           /* 同步回调返回的预设错误码 */
    int preflight_error;      /* 槽资源预检回调返回的预设错误码 */
} FakeDevice;

static FakeDevice device;            /* device：测试使用的模拟设备状态 */
static AmpAbBootWorkspace workspace; /* workspace：启动流程使用的元数据工作区 */

static int read_block(void *ctx, uint64_t lba, void *buf, size_t bytes)
{
    /* 1. 检查模拟设备读取边界。 */
    /* 2. 复制指定块内容到输出缓冲区。 */

    FakeDevice *d = ctx;

    if (bytes != BLOCK_BYTES || lba >= DISK_BYTES / BLOCK_BYTES)
        return -1;

    memcpy(buf, d->data + (size_t)lba * BLOCK_BYTES, bytes);
    return 0;
}

static int write_block(void *ctx, uint64_t lba, const void *buf, size_t bytes)
{
    /* 1. 检查模拟设备写入边界。 */
    /* 2. 更新内存设备内容并累计写入次数。 */

    FakeDevice *d = ctx;

    if (bytes != BLOCK_BYTES || lba >= DISK_BYTES / BLOCK_BYTES)
        return -1;

    memcpy(d->data + (size_t)lba * BLOCK_BYTES, buf, bytes);
    d->writes++;
    return 0;
}

static int sync_device(void *ctx)
{
    /* 1. 累计模拟同步次数。 */
    /* 2. 返回预设的同步错误码。 */

    FakeDevice *d = ctx;

    d->syncs++;
    return d->sync_error;
}

static int preflight(void *ctx, uint32_t slot)
{
    /* 1. 断言待启动槽有效。 */
    /* 2. 返回测试注入的预检结果。 */

    FakeDevice *d = ctx;

    assert(slot == AMP_AB_SLOT_A || slot == AMP_AB_SLOT_B);
    return d->preflight_error;
}

/* 仅测试夹具初始化，不是生产环境元数据初始化接口。 */
static AmpAbStore fixture(const AMPABState *state)
{
    /* 1. 清空模拟设备并编码两份初始元数据。 */
    /* 2. 设置块大小、副本位置和模拟读写回调。 */

    AmpAbStore store;

    memset(&device, 0, sizeof(device));
    memset(&store, 0, sizeof(store));

    assert(amp_ab_meta_encode(state, device.data) == AMP_AB_OK);
    memcpy(device.data + AMP_AB_META_BYTES, device.data, AMP_AB_META_BYTES);

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
    /* 1. 使用模拟设备、预检回调和工作区执行一次启动事务。 */
    /* 2. 返回事务结果供测试断言。 */

    return amp_ab_boot_transaction(store, preflight, &device, &workspace, result);
}

int main(void)
{
    /* 1. 准备当前测试使用的输入、模拟状态与预期结果。 */
    /* 2. 调用被测接口，断言返回值及状态变化符合预期。 */

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

    assert(amp_ab_stage(&initial, AMP_AB_SLOT_B, &pending) == AMP_AB_OK);

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
        assert(result.state.tries_remaining == AMP_AB_MAX_TRIES - attempt - 1);
        assert(result.generation == pending.generation + attempt + 1);
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
