/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 Linker Script
 *
 * The kernel payload is concatenated to the BareMetal kernel binary.
 * During boot, init_sys copies it to physical address 0x1E0000 and the
 * BSP executes it via the SMP work queue (call 0x1E0000).
 *
 * Output is a flat binary (objcopy -O binary produces the raw code).
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PAYLOAD_BASE = 0x1000000;  /* 16MB — loaded here by boot loader stub */

SECTIONS
{
    . = PAYLOAD_BASE;

    /* ---- Code ---- */
    .text ALIGN(16) :
    {
        *(.text.entry)      /* Entry point first */
        *(.text .text.*)
    }

    /* ---- Read-only data ---- */
    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
    }

    /* ---- GOT (needed by some code models) ---- */
    .got ALIGN(16) :
    {
        *(.got .got.plt)
    }

    /* ---- Initialized data ---- */
    .data ALIGN(16) :
    {
        *(.data .data.*)
    }

    /* ---- Zero-initialized data ---- */
    . = ALIGN(16);
    _bss_start = .;
    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;

    _end = .;

    /* ---- Discarded sections ---- */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.rel*)
        *(.plt*)
        *(.interp)
    }
}
