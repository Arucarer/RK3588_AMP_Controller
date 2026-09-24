.section .text.boot, "ax"
.balign 128

.global _start
.type _start, %function

.extern __stack_top
.extern __bss_start__
.extern __bss_end__
.extern amp_main
.extern amp_vectors

_start:
    msr     daifset, #0xf
    isb

    /* This firmware expects EL1. */
    mrs     x0, CurrentEL
    cmp     x0, #4
    b.eq    .Lel1_entry

.Lunexpected_el:
    wfe
    b       .Lunexpected_el

.Lel1_entry:
    /* Initialize the exception/boot stack before installing vectors. */
    msr     spsel, #1
    isb

    ldr     x0, =__stack_top
    mov     sp, x0

    /* Invalidate the previous exception record. */
    ldr     x0, =0x20FE0100
    str     xzr, [x0]
    dsb     sy

    ldr     x0, =amp_vectors
    msr     vbar_el1, x0
    isb

    /* Clear BSS. */
    ldr     x0, =__bss_start__
    ldr     x1, =__bss_end__

.Lclear_bss:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lclear_bss

.Lbss_done:
    dsb     sy
    isb

    bl      amp_main

.Lreturned:
    wfe
    b       .Lreturned

.size _start, . - _start
.ltorg