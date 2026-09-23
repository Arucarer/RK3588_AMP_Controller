typedef unsigned long long uint64_t;

#define AMP_BOOT_MAGIC      0xA55A55A55AA55AA5ULL
#define AMP_BOOT_FLAG_ADDR  0x093E0000ULL

typedef struct
{
    uint64_t magic;       /* 启动成功标志 */
    uint64_t current_el;  /* 当前异常等级 */
    uint64_t mpidr;       /* 当前CPU MPIDR */
    uint64_t stage;       /* 当前启动阶段 */
} AMPBootInfo;

static inline uint64_t read_current_el(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, CurrentEL"
        : "=r"(value)
    );

    return value;
}

static inline uint64_t read_mpidr(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, MPIDR_EL1"
        : "=r"(value)
    );

    return value;
}

void amp_main(void)
{
    volatile AMPBootInfo *info = (volatile AMPBootInfo *)AMP_BOOT_FLAG_ADDR;

    uint64_t current_el = read_current_el();
    uint64_t mpidr = read_mpidr();

    /* 先清除有效标志 */
    info->magic = 0;

    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * CurrentEL[3:2]
     *
     * EL0 = 0
     * EL1 = 1
     * EL2 = 2
     * EL3 = 3
     */
    info->current_el = (current_el >> 2) & 0x3ULL;

    /* 保存完整MPIDR_EL1 */
    info->mpidr = mpidr;

    /* 已经成功执行到amp_main */
    info->stage = 1;

    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Magic最后写入。
     * 后续Linux端只要看到Magic正确，
     * 就可以认为前面的字段已经写完。
     */
    info->magic = AMP_BOOT_MAGIC;

    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb");

    while(1)
    {
        __asm__ volatile("wfe");
    }
}