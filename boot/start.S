/*
 * start.S: the first code to run. The Pi's firmware loads kernel.img at
 * 0x8000 and jumps here on the ARM1176 in SVC mode (QEMU's raspi0 enters
 * at the ELF's entry, in SVC mode too).
 *
 * Sets up the stacks for SVC, IRQ, FIQ, abort and undefined modes, points
 * the vector base at our table, turns on the VFP, clears the BSS and calls
 * main().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .section .text.boot, "ax"
    .global _start
    .arm

_start:
    /* Interrupts off while we set things up. */
    cpsid   if

    /* A stack for each mode we can end up in. */
    cps     #0x12               /* IRQ */
    ldr     sp, =__irq_stack_top
    cps     #0x11               /* FIQ */
    ldr     sp, =__fiq_stack_top
    cps     #0x17               /* abort */
    ldr     sp, =__exc_stack_top
    cps     #0x1b               /* undefined */
    ldr     sp, =__exc_stack_top
    cps     #0x13               /* back to SVC, where everything else runs */
    ldr     sp, =__svc_stack_top

    /* Vector base (VBAR, which the ARM1176 has) to our table. */
    ldr     r0, =vectors
    mcr     p15, 0, r0, c12, c0, 0

    /* VFP: full access to coprocessors 10 and 11, then FPEXC.EN. */
    mrc     p15, 0, r0, c1, c0, 2
    orr     r0, r0, #(0xf << 20)
    mcr     p15, 0, r0, c1, c0, 2
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4   /* flush prefetch buffer (ISB) */
    mov     r0, #0x40000000
    fmxr    fpexc, r0
    /* RunFast mode: flush-to-zero and default NaN on, exception traps off.
     * Without it the ARM1176's VFP11 hands some operations (NaN or
     * denormal operands) to support code we don't have, as an undefined
     * instruction on the next VFP instruction. QEMU doesn't do that. */
    mov     r0, #(3 << 24)
    fmxr    fpscr, r0

    /* Clear the BSS. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    bl      main
halt:
    wfi
    b       halt

/* ---- exception vectors ----------------------------------------------------- */

    .balign 32
vectors:
    ldr     pc, =_start             /* reset */
    ldr     pc, =undef_entry        /* undefined instruction */
    ldr     pc, =svc_entry          /* SVC */
    ldr     pc, =pabort_entry       /* prefetch abort */
    ldr     pc, =dabort_entry       /* data abort */
    nop
    ldr     pc, =irq_entry          /* IRQ */
    ldr     pc, =fiq_entry          /* FIQ */
    .ltorg

/* A fault: save the registers and hand them to C (fault.c), which prints
 * them on the serial console and stops. r0 = kind, r1 = saved registers,
 * r2 = the faulting address (lr adjusted). */
.macro FAULT kind, adjust
    sub     lr, lr, #\adjust
    stmfd   sp!, {r0-r12, lr}
    mov     r0, #\kind
    mov     r1, sp
    mov     r2, lr
    bl      fault_report
1:  b       1b
.endm

undef_entry:    FAULT 0, 4
svc_entry:      FAULT 1, 0
pabort_entry:   FAULT 2, 4
dabort_entry:   FAULT 3, 8
fiq_entry:      FAULT 5, 4

/* IRQ: save what C may clobber (the VFP's caller-saved registers too:
 * the interrupted code may be in the middle of floating point), call
 * irq_handler(), return. 6 + 16 + 2 words keep the stack 8-byte aligned. */
irq_entry:
    sub     lr, lr, #4
    stmfd   sp!, {r0-r3, r12, lr}
    vpush   {d0-d7}
    vmrs    r0, fpscr
    sub     sp, sp, #4
    push    {r0}
    bl      irq_handler
    pop     {r0}
    add     sp, sp, #4
    vmsr    fpscr, r0
    vpop    {d0-d7}
    ldmfd   sp!, {r0-r3, r12, pc}^
