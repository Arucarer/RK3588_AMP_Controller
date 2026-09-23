OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x08400000, LENGTH = 16M
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text :
    {
        KEEP(*(.text.boot))
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

    /*
     * 0x093E0000：第一次启动的调试信息区域
     *
     * 0x093F0000 ~ 0x093FFFFF：
     * 初始启动栈，64 KiB
     */
    __boot_flag   = 0x093E0000;
    __stack_bottom = 0x093F0000;
    __stack_top    = 0x09400000;

    ASSERT(__image_end__ < __boot_flag, "AMP image is too large")
}