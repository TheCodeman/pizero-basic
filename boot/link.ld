/*
 * link.ld: kernel.img is loaded at 0x8000. After the program come the
 * stacks, then the heap (_sbrk in syscalls.c), which runs up to the top of
 * the ARM's memory as the firmware reports it (mailbox.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
ENTRY(_start)

SECTIONS
{
    . = 0x8000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    }

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    }

    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .data : ALIGN(8) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* Stacks: SVC (where BASIC runs; its evaluator recurses), then small
     * ones for the exception modes. */
    .stack (NOLOAD) : ALIGN(16) {
        __svc_stack_bottom = .;
        . += 0x80000;               /* 512 KB */
        __svc_stack_top = .;
        . += 0x4000;
        __irq_stack_top = .;
        . += 0x1000;
        __fiq_stack_top = .;
        . += 0x1000;
        __exc_stack_top = .;
    }

    . = ALIGN(0x100000);
    __heap_start = .;

    /DISCARD/ : { *(.comment) *(.note*) }
}
