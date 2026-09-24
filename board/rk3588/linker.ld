OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16M
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text :
    {
        KEEP(*(.text.boot))
        . = ALIGN(2048);
        KEEP(*(.text.vectors))
        *(.text*)
    } > RAM

    .rodata :
    {
        *(.rodata*)
    } > RAM

    .data :
    {
        *(.data*)
    } > RAM

    .bss (NOLOAD) :
    {
        . = ALIGN(16);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end__ = .;
    } > RAM

    . = ALIGN(4096);
    __image_end__ = .;

    /* Boot flag and initial 64 KiB stack within RTOS RAM. */
    __boot_flag    = ORIGIN(RAM) + LENGTH(RAM) - 0x20000;
    __stack_bottom = ORIGIN(RAM) + LENGTH(RAM) - 0x10000;
    __stack_top    = ORIGIN(RAM) + LENGTH(RAM);

    __ipc_base = 0x20FD0000;
    ASSERT(__image_end__ <= __ipc_base, "AMP image overlaps IPC area")
}