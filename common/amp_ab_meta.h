/* 新建：common/amp_ab_meta.h
 *
 * 本轮实现固定格式、CRC 和双副本选择，不访问磁盘。
 *
 * abmeta 分区内候选位置：
 * 副本 0：偏移 0x0000，4096 字节
 * 副本 1：偏移 0x1000，4096 字节
 *
 * 全部整数使用小端编码，禁止直接把 C 结构写入磁盘。
 * CRC 用于检查意外损坏，不提供签名认证或防回放能力。
 */

#ifndef AMP_AB_META_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_AB_META_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include "amp_ab.h" /* 引入 A/B 启动槽状态机接口 */

#define AMP_AB_META_BYTES 4096U  /* 单份持久化启动元数据记录的大小，单位字节 */
#define AMP_AB_META_COPY0_OFF 0U /* 元数据副本 0 相对存储区域起点的字节偏移 */
#define AMP_AB_META_COPY1_OFF 4096U /* 元数据副本 1 相对存储区域起点的字节偏移 */

#define AMP_AB_META_INVALID (-10)  /* 元数据结果：格式、CRC 或状态校验失败 */
#define AMP_AB_META_CONFLICT (-11) /* 元数据结果：相同代次的有效副本内容冲突 */

/* 双副本元数据校验和选择结果。 */
typedef struct {
    AMPABState state; /* 选中副本解码后的启动状态 */

    /* 当前选中的副本编号：0 或 1。 */
    uint32_t selected_copy; /* 选中副本的编号，0 或 1 */

    /* 位 0、位 1 分别表示对应副本通过校验。 */
    uint32_t valid_mask; /* 有效副本位图，位 0 和位 1 分别对应两份副本 */
} AMPABMetaSelection;

/*
 * 固定格式：
 *  0..7     magic："AMPAB001"
 *  8..11    格式版本：1
 * 12..15    记录长度：4096
 * 16..23    generation
 * 24..27    confirmed_slot
 * 28..31    pending_slot
 * 32..35    tries_remaining
 * 36..39    trial_slot
 * 40..4091  保留，必须为 0
 * 4092..4095 CRC32，覆盖前 4092 字节
 */
int amp_ab_meta_encode(const AMPABState *state, uint8_t record[AMP_AB_META_BYTES]);

/* 核对记录魔数、版本、大小和 CRC；解码启动状态字段；检查状态一致性后返回结果。 */
int amp_ab_meta_decode(const uint8_t record[AMP_AB_META_BYTES], AMPABState *state);

/*
 * 两份都有效：选择 generation 较大者。
 * 同代次但内容不同：拒绝选择。
 * 仅一份有效：选择有效副本。
 * 两份都无效：返回错误，不能自动初始化为槽 A。
 *
 * 输入数组必须完整包含 4096 字节。
 * 后续磁盘读取失败或短读，不能当作成功读取。
 */
int amp_ab_meta_select(const uint8_t copy0[AMP_AB_META_BYTES],
                       const uint8_t copy1[AMP_AB_META_BYTES], AMPABMetaSelection *selection);

#endif /* 结束 AMP_AB_META_H 对应的条件编译区段 */
