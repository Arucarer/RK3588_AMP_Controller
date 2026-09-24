.section .text.vectors, "ax"
.balign 2048

.global amp_vectors
.global _freertos_vector_table

.extern FreeRTOS_IRQ_Handler
.extern FreeRTOS_SWI_Handler

.type amp_vectors, %object
.type _freertos_vector_table, %object

.macro VECTOR_SLOT number
    .balign 128
    .if \number == 0
        b       amp_sync_sp0
    .elseif \number == 4
        b       amp_sync_spx
    .elseif \number == 1
        b       FreeRTOS_IRQ_Handler
    .elseif \number == 5
        b       FreeRTOS_IRQ_Handler
    .else
        mov     x0, #\number
        b       amp_exception_stop
    .endif
.endm

amp_vectors:
_freertos_vector_table:
    VECTOR_SLOT 0
    VECTOR_SLOT 1
    VECTOR_SLOT 2
    VECTOR_SLOT 3

    VECTOR_SLOT 4
    VECTOR_SLOT 5
    VECTOR_SLOT 6
    VECTOR_SLOT 7

    VECTOR_SLOT 8
    VECTOR_SLOT 9
    VECTOR_SLOT 10
    VECTOR_SLOT 11

    VECTOR_SLOT 12
    VECTOR_SLOT 13
    VECTOR_SLOT 14
    VECTOR_SLOT 15

.balign 128
.size amp_vectors, . - amp_vectors
.size _freertos_vector_table, . - _freertos_vector_table

/* Route SVC to FreeRTOS; record other synchronous exceptions. */
.macro SYNC_ENTRY name, number
    .balign 16
    .type \name, %function
\name:
    stp     x0, x1, [sp, #-16]!

    mrs     x0, ESR_EL1
    lsr     x1, x0, #26
    cmp     x1, #0x15
    b.eq    .Lsvc_\name

    ldp     x0, x1, [sp], #16
    mov     x0, #\number
    b       amp_exception_stop

.Lsvc_\name:
    ldp     x0, x1, [sp], #16
    b       FreeRTOS_SWI_Handler

    .size \name, . - \name
.endm

SYNC_ENTRY amp_sync_sp0, 0
SYNC_ENTRY amp_sync_spx, 4

.balign 16
.type amp_exception_stop, %function
amp_exception_stop:
    msr     daifset, #0xf

    ldr     x1, =0x20FE0100
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

    ldr     x2, =0x4558434550543031
    str     x2, [x1]
    dsb     sy

.Lexception_wait:
    wfe
    b       .Lexception_wait

.size amp_exception_stop, . - amp_exception_stop
.ltorg