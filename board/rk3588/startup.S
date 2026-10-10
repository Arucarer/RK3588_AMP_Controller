/*
 * 文件名：startup.S
 * 作用：RK3588 AMP 模式下 Cortex-A55 的启动汇编代码。
 *
 * 主要功能：
 * 1. 屏蔽中断，防止初始化过程中意外进入异常处理。
 * 2. 检查 CPU 当前是否运行在 EL1 异常级别。
 * 3. 初始化栈指针 SP，为后续 C 函数调用准备栈空间。
 * 4. 设置异常向量表地址，指定中断和异常的处理入口。
 * 5. 清空 BSS 段，初始化未显式赋值的全局变量和静态变量。
 * 6. 跳转到 amp_main()，进入 FreeRTOS 的初始化流程。
 */

.section .text.boot, "ax"       // 将代码放入 .text.boot 段，a 表示可分配，x 表示可执行
.balign 128                     // 将当前位置按 128 字节对齐

.global _start                  // 声明 _start 为全局符号，供链接器识别
.type _start, %function         // 声明 _start 为函数类型

.extern __stack_top             // 栈顶地址，由 linker.ld 定义
.extern __bss_start__           // BSS 段起始地址，由 linker.ld 定义
.extern __bss_end__             // BSS 段结束地址，由 linker.ld 定义
.extern amp_main                // FreeRTOS 的 C 语言入口，定义于 rtos/main.c
.extern amp_vectors             // 异常向量表，定义于 board/rk3588/vector.S

/* ==================== 1. CPU 启动入口 ==================== */

_start:
    msr     daifset, #0xf       // 屏蔽 Debug、SError、IRQ、FIQ 等异常
    isb                         // 指令同步屏障，确保前面的系统寄存器修改生效

/* ==================== 2. 检查 CPU 异常级别 ==================== */

    mrs     x0, CurrentEL       // 将当前异常级别寄存器 CurrentEL 读入 x0
    cmp     x0, #4              // EL1 的 CurrentEL 编码为 0x4
    b.eq    .Lel1_entry         // 如果当前为 EL1，跳转到正常初始化入口

/* 如果不是 EL1，则停止后续初始化 */

.Lunexpected_el:
    wfe                         // 等待事件，使 CPU 进入低功耗等待状态
    b       .Lunexpected_el     // 循环等待，不继续执行 FreeRTOS

/* ==================== 3. 初始化 CPU 栈 ==================== */

.Lel1_entry:
    msr     spsel, #1           // 选择使用 EL1 专属栈指针 SP_EL1
    isb                         // 确保栈指针选择操作生效

    ldr     x0, =__stack_top    // 读取 linker.ld 中定义的栈顶地址
    mov     sp, x0              // 将栈顶地址写入 SP，完成初始栈设置

/* ==================== 4. 清除旧异常记录 ==================== */

    ldr     x0, =0x20FE0100     // 异常记录区域的物理地址
    str     xzr, [x0]           // 将该地址的 8 字节清零，使旧异常记录失效
    dsb     sy                  // 数据同步屏障，确保前面的内存写入完成

/* ==================== 5. 设置异常向量表 ==================== */

    ldr     x0, =amp_vectors    // 获取 vector.S 中异常向量表的起始地址
    msr     vbar_el1, x0        // 将异常向量表地址写入 VBAR_EL1
    isb                         // 确保异常向量表地址更新生效

/* ==================== 6. 清空 BSS 段 ==================== */

    ldr     x0, =__bss_start__  // 获取 BSS 段的起始地址
    ldr     x1, =__bss_end__    // 获取 BSS 段的结束地址

.Lclear_bss:
    cmp     x0, x1              // 比较当前清零位置和 BSS 结束位置
    b.hs    .Lbss_done          // 如果 x0 >= x1，说明清零完成
    str     xzr, [x0], #8       // 将 x0 指向的 8 字节清零，然后 x0 增加 8
    b       .Lclear_bss         // 继续循环清零

/* ==================== 7. 完成底层初始化 ==================== */

.Lbss_done:
    dsb     sy                  // 确保 BSS 清零等内存操作完成
    isb                         // 同步指令执行环境

/* ==================== 8. 进入 FreeRTOS C 语言入口 ==================== */

    bl      amp_main            // 调用 rtos/main.c 中的 amp_main() 函数

/* ==================== 9. 防止 C 入口意外返回 ==================== */

.Lreturned:
    wfe                         // 如果 amp_main 意外返回，则等待事件
    b       .Lreturned          // 无限循环，防止执行到未知代码

/* ==================== 10. 汇编文件结束 ==================== */

.size _start, . - _start        // 计算并记录 _start 函数的代码大小
.ltorg                          // 在当前位置生成汇编器需要的常量池