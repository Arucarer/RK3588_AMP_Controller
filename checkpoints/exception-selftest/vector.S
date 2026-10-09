.section .text.vectors, "ax"
.balign 2048

.global amp_vectors
.type amp_vectors, %object

/* Each AArch64 vector slot occupies 128 bytes. */
.macro VECTOR_SLOT number
    .balign 128
    mov     x0, #\number
    b       amp_exception_stop
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