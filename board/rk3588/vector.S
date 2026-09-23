.section .text.vectors, "ax"
.balign 2048

.global amp_vectors
.type amp_vectors, %object

/* Each AArch64 vector slot occupies 128 bytes. */
.macro VECTOR_SLOT number
    .balign 128
    .if \number == 5
        b       amp_irq_entry
    .else
        mov     x0, #\number
        b       amp_exception_stop
    .endif
.endm

amp_vectors:
    /* Current EL using SP_EL0 */
    VECTOR_SLOT 0
    VECTOR_SLOT 1
    VECTOR_SLOT 2
    VECTOR_SLOT 3

    /* Current EL using SP_ELx */
    VECTOR_SLOT 4
    VECTOR_SLOT 5
    VECTOR_SLOT 6
    VECTOR_SLOT 7

    /* Lower EL, AArch64 */
    VECTOR_SLOT 8
    VECTOR_SLOT 9
    VECTOR_SLOT 10
    VECTOR_SLOT 11

    /* Lower EL, AArch32 */
    VECTOR_SLOT 12
    VECTOR_SLOT 13
    VECTOR_SLOT 14
    VECTOR_SLOT 15

.balign 128
.size amp_vectors, . - amp_vectors

.type amp_exception_stop, %function
amp_exception_stop:
    msr     daifset, #0xf

    /* Terminal handler: no stack or C runtime required. */
    ldr     x1, =0x20FE0100

    /* Invalidate record before updating fields. */
    str     xzr, [x1]
    dsb     sy

    str     x0, [x1, #8]

    mrs     x2, CurrentEL
    lsr     x2, x2, #2
    str     x2, [x1, #16]

    mrs     x2, ESR_EL1
    str     x2, [x1, #24]

    mrs     x2, ELR_EL1
    str     x2, [x1, #32]

    mrs     x2, FAR_EL1
    str     x2, [x1, #40]

    mrs     x2, SPSR_EL1
    str     x2, [x1, #48]

    mrs     x2, MPIDR_EL1
    str     x2, [x1, #56]

    dsb     sy

    /* Write valid marker last. */
    ldr     x2, =0x4558434550543031
    str     x2, [x1]
    dsb     sy

1:
    wfe
    b       1b

.size amp_exception_stop, . - amp_exception_stop
.ltorg


.extern amp_irq_handler

.balign 16
.type amp_irq_entry, %function
amp_irq_entry:
    /* 31 general registers + ELR/SPSR, aligned to 16 bytes. */
    sub     sp, sp, #272

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]

    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    str     x0, [sp, #248]
    str     x1, [sp, #256]

    bl      amp_irq_handler

    ldr     x0, [sp, #248]
    ldr     x1, [sp, #256]
    msr     ELR_EL1, x0
    msr     SPSR_EL1, x1

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]

    add     sp, sp, #272
    eret

.size amp_irq_entry, . - amp_irq_entry