/* 新建：common/amp_ab.h
 *
 * A/B 启动状态机，供 U-Boot 和 Linux 升级工具共用。
 *
 * 本文件不访问磁盘，不直接修改启动参数。
 * AMPABState 是内存结构，禁止直接写入 abmeta 分区。
 * 持久化编码和双副本校验由 amp_ab_meta 提供，存储访问由 amp_ab_store 提供。
 */

#ifndef AMP_AB_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_AB_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */

#define AMP_AB_SLOT_A 0U            /* A/B 启动状态机中的 A 槽编号 */
#define AMP_AB_SLOT_B 1U            /* A/B 启动状态机中的 B 槽编号 */
#define AMP_AB_SLOT_NONE UINT32_MAX /* 无候选槽或无试启动槽的占位值，不是实际分区编号 */

#define AMP_AB_MAX_TRIES 3U /* 每次候选升级最多允许的试启动次数 */

#define AMP_AB_OK 0          /* A/B 状态机结果：成功 */
#define AMP_AB_INVALID (-1)  /* A/B 状态机结果：状态或参数不合法 */
#define AMP_AB_BUSY (-2)     /* A/B 状态机结果：已有尚未结束的升级事务 */
#define AMP_AB_OVERFLOW (-3) /* A/B 状态机结果：代次达到上限，拒绝回绕 */

/* A/B 槽启动状态的内存表示；持久化时须使用元数据编码接口。 */
typedef struct {
    uint64_t generation; /* 状态代次，成功变更时递增，用于识别陈旧提交 */

    /* 已确认可正常启动的槽。 */
    uint32_t confirmed_slot; /* 最近一次确认成功的槽，取 AMP_AB_SLOT_A 或 AMP_AB_SLOT_B */

    /* 待验证槽，NONE 表示没有升级事务。 */
    uint32_t pending_slot;    /* 待验证的候选槽，无升级事务时为 AMP_AB_SLOT_NONE */
    uint32_t tries_remaining; /* 候选槽剩余试启动次数，启动前持久化扣减 */

    /*
     * 最近一次准备启动的候选槽。
     * Linux 成功确认必须匹配 trial_slot 和 generation。
     */
    uint32_t trial_slot; /* 最近准备试启动的槽，确认时必须与运行槽和代次匹配 */
} AMPABState;

/*
 * 决策函数输出的 next 必须持久化成功后，
 * 才能加载选定槽的 DTB、AMP 和 Linux。
 */
/* 启动选槽决策；要求持久化时，须先提交 next 再加载所选槽。 */
typedef struct {
    AMPABState next;       /* 本轮决策产生的下一状态 */
    uint32_t slot;         /* 本轮允许加载的槽编号 */
    uint32_t must_persist; /* 是否必须在加载镜像前持久化 next，1 是、0 否 */
    uint32_t rolled_back;  /* 本轮是否因尝试次数耗尽而回退到已确认槽 */
} AMPABDecision;

/* 仅用于首次受控初始化，不能在元数据损坏时自动调用。 */
void amp_ab_init(AMPABState *state);

/* 检查指针、代次及已确认槽；检查候选槽与试启动槽之间的一致性；校验剩余尝试次数与试启动阶段是否匹配。 */
int amp_ab_validate(const AMPABState *state);

/*
 * 候选槽镜像写入、读回校验及认证完成后才能调用。
 * 不允许把当前 confirmed_slot 当作升级目标。
 */
int amp_ab_stage(const AMPABState *current, uint32_t target_slot, AMPABState *next);

/* 启动前选槽；候选启动会先扣减一次尝试次数。 */
int amp_ab_prepare_boot(const AMPABState *current, AMPABDecision *decision);

/*
 * Linux/FreeRTOS 健康验证成功后确认候选槽。
 * boot_generation 来自本次启动实际采用的元数据版本。
 */
int amp_ab_confirm(const AMPABState *current, uint32_t running_slot, uint64_t boot_generation,
                   AMPABState *next);

#endif /* 结束 AMP_AB_H 对应的条件编译区段 */
