/*
 * Linker script for TI MSP432E401Y on XDS110 / LP-XDS110ET
 *
 * Memory Map:
 *   0x00000000 - 0x00003FFF (16 KB)  : Bootloader (DFU ROM/Flash bootloader)
 *   0x00004000 - 0x000FBFFF (992 KB) : Application Flash (this binary)
 *   0x000FC000 - 0x000FFFFF (16 KB)  : Serial / Config Block (preserved across updates)
 *   0x20000000 - 0x2003FFFF (256 KB) : Internal SRAM
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00004000, LENGTH = 0x000F8000 /* 992 KB */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00040000 /* 256 KB */
}

/* Initial stack pointer at top of SRAM (0x20040000) */
__stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

/* Preserved TI USB DFU bootloader forced-update entry vector (SVCall slot @ 0x2C) */
PROVIDE(__dfu_bootloader_entry_vec = 0x0000002C);

ENTRY(reset_isr)

SECTIONS
{
    /* The interrupt vector table is placed at the start of application flash */
    .intvecs :
    {
        KEEP(*(.intvecs))
        KEEP(*(.vectors))
    } > FLASH

    /* Program code and read-only data */
    .text :
    {
        *(.text*)
        KEEP(*(.rodata.version*))
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP (*(.init))
        KEEP (*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* Initialized data section loaded from flash into SRAM */
    .data :
    {
        . = ALIGN(4);
        __data_start__ = .;
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
        _edata = .;
    } > SRAM AT> FLASH

    __data_load__ = LOADADDR(.data);
    _sidata = LOADADDR(.data);

    /* Zero-initialized data section (.bss) */
    .bss :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
        _ebss = .;
    } > SRAM

    /* Uninitialized section preserved across warm resets (e.g., DFU double-reset cookie) */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        __noinit_start__ = .;
        *(.noinit*)
        . = ALIGN(4);
        __noinit_end__ = .;
    } > SRAM

    /* User heap and stack space (reserve 16 KiB stack at top of 256 KiB SRAM) */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        PROVIDE(__heap_start__ = .);
        . = . + 0x1000; /* Minimum 4 KB heap */
        . = ALIGN(8);
    } > SRAM

    PROVIDE(__heap_end__ = ORIGIN(SRAM) + LENGTH(SRAM) - 0x4000);
}
