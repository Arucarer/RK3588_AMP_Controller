/* 新建：common/amp_ab.h
 *
 * A/B 启动状态机，供后续 U-Boot 和 Linux 升级工具共用。
 *
 * 本文件不访问磁盘，不直接修改启动参数。
 * AMPABState 是内存结构，禁止直接写入 abmeta 分区。
 * 持久化编码、双副本校验和磁盘写入将在下一步接入。
 */

#ifndef AMP_AB_H
#define AMP_AB_H

#include <stdint.h>

#define AMP_AB_SLOT_A       0U
#define AMP_AB_SLOT_B       1U
#define AMP_AB_SLOT_NONE    UINT32_MAX

#define AMP_AB_MAX_TRIES    3U

#define AMP_AB_OK           0
#define AMP_AB_INVALID     (-1)
#define AMP_AB_BUSY        (-2)
#define AMP_AB_OVERFLOW    (-3)

typedef struct {
    uint64_t generation;

    /* 已确认可正常启动的槽。 */
    uint32_t confirmed_slot;

    /* 待验证槽，NONE 表示没有升级事务。 */
    uint32_t pending_slot;
    uint32_t tries_remaining;

    /*
     * 最近一次准备启动的候选槽。
     * Linux 成功确认必须匹配 trial_slot 和 generation。
     */
    uint32_t trial_slot;
} AMPABState;

/*
 * 决策函数输出的 next 必须持久化成功后，
 * 才能加载选定槽的 DTB、AMP 和 Linux。
 */
typedef struct {
    AMPABState next;
    uint32_t slot;
    uint32_t must_persist;
    uint32_t rolled_back;
} AMPABDecision;

/* 仅用于首次受控初始化，不能在元数据损坏时自动调用。 */
void amp_ab_init(AMPABState *state);

int amp_ab_validate(const AMPABState *state);

/*
 * 候选槽镜像写入、读回校验及认证完成后才能调用。
 * 不允许把当前 confirmed_slot 当作升级目标。
 */
int amp_ab_stage(const AMPABState *current,
                 uint32_t target_slot,
                 AMPABState *next);

/* 启动前选槽；候选启动会先扣减一次尝试次数。 */
int amp_ab_prepare_boot(const AMPABState *current,
                        AMPABDecision *decision);

/*
 * Linux/FreeRTOS 健康验证成功后确认候选槽。
 * boot_generation 来自本次启动实际采用的元数据版本。
 */
int amp_ab_confirm(const AMPABState *current,
                   uint32_t running_slot,
                   uint64_t boot_generation,
                   AMPABState *next);

#endif