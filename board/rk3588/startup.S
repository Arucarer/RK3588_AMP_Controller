.section .text.boot
.align 7

.global _start
.type _start, %function

.extern __stack_top
.extern __bss_start__
.extern __bss_end__
.extern amp_main

_start:
    /* 第一阶段关闭异常，避免尚未安装异常向量时进入IRQ/FIQ */
    msr     daifset, #0xf
    isb

    /* 初始化SP */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* 清零BSS */
    ldr     x0, =__bss_start__
    ldr     x1, =__bss_end__

1:
    cmp     x0, x1
    b.hs    2f

    str     xzr, [x0], #8
    b       1b

2:
    dsb     sy
    isb

    /* 进入C代码 */
    bl      amp_main

3:
    wfe
    b       3b

.size _start, . - _start