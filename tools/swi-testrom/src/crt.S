/*
    Startup, IRQ handler, SWI trampoline, decompression callbacks, traps and
    the SoftReset probe of the SWI test ROM. Assembled twice, with -DARM9 and
    -DARM7. Written from GBATEK ("DS Interrupts", "BIOS RAM Usage", "BIOS
    Reset Functions", "ARM CP15 Tightly Coupled Memory").

    All code here is ARM state (ARMv4T compatible) unless marked Thumb.
*/

#include "layout.h"

#if defined(ARM9)
#define RES_BASE    RES9_BASE
#define SYS_STACK   STACK9_SYS
#elif defined(ARM7)
#define RES_BASE    RES7_BASE
#define SYS_STACK   STACK7_SYS
#else
#error "build with -DARM9 or -DARM7"
#endif

    .syntax unified
    .arm

/* ------------------------------------------------------------------ */
/* startup                                                            */
/* ------------------------------------------------------------------ */

    .section .text.start, "ax"
    .global _start
_start:
    msr     cpsr_c, #0xD3               @ svc, IRQ/FIQ off
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]            @ IME = 0
    str     r1, [r0, #0x210]            @ IE = 0
    mvn     r1, #0
    str     r1, [r0, #0x214]            @ acknowledge all IF

#if defined(ARM9)
    @ DTCM placement: the direct boot's 0300000Ah unless SoftReset mode picks another
    ldr     r4, =0x0300000A
    ldr     r1, =CTRL_BASE
    ldr     r2, [r1, #CTRL_MODE]
    ldr     r3, [r1, #CTRL_PARAM]
    cmp     r2, #MODE_SOFTRESET
    bne     1f
    cmp     r3, #4
    bhi     1f
    adr     r5, dtcm_table
    ldr     r4, [r5, r3, lsl #2]
    cmp     r3, #1                      @ DTCM at 00800000h: shrink the ITCM to 32KB so it
    moveq   r5, #0x0C                   @ does not cover it (ITCM has priority, GBATEK)
    mcreq   p15, 0, r5, c9, c1, 1
1:  mcr     p15, 0, r4, c9, c1, 0
    mrc     p15, 0, r5, c1, c0, 0
    orr     r5, r5, #(1 << 16)          @ DTCM on
    orr     r5, r5, #(1 << 18)          @ ITCM on
    mcr     p15, 0, r5, c1, c0, 0
    mov     r4, r4, lsr #12
    mov     r4, r4, lsl #12             @ DTCM base
    add     sp, r4, #DTCM_SVC_SP
    msr     cpsr_c, #0xD2
    add     sp, r4, #DTCM_IRQ_SP
    msr     cpsr_c, #0xDF
    ldr     sp, =SYS_STACK
    @ BIOS IRQ vector and flags (DTCM+3FFCh / +3FF8h)
    add     r0, r4, #0x3F00
    ldr     r1, =irq_handler
    str     r1, [r0, #0xFC]
    mov     r1, #0
    str     r1, [r0, #0xF8]
    @ trap for SWIs that jump to 0 (GBATEK: invalid NDS9 SWIs jump to 0, which is ITCM)
    mov     r0, #0
    ldr     r1, =0xE51FF004             @ ldr pc, [pc, #-4]
    str     r1, [r0]
    ldr     r1, =trap_zero
    str     r1, [r0, #4]
    ldr     r0, =ARM9_DEBUG_VEC
    ldr     r1, =trap_exception
    str     r1, [r0]
#else
    ldr     sp, =STACK7_SVC
    msr     cpsr_c, #0xD2
    ldr     sp, =STACK7_IRQ
    msr     cpsr_c, #0xDF
    ldr     sp, =SYS_STACK
    ldr     r0, =ARM7_IRQ_VECTOR
    ldr     r1, =irq_handler
    str     r1, [r0]
    ldr     r0, =ARM7_IRQ_FLAGS
    mov     r1, #0
    str     r1, [r0]
    ldr     r0, =ARM7_DEBUG_VEC
    ldr     r1, =trap_exception
    str     r1, [r0]
#endif

    msr     cpsr_c, #0x1F               @ system mode, IRQs on in CPSR (IME gates them)
    bl      main
    b       .

#if defined(ARM9)
    @ SoftReset variants 0-4: direct boot default, GBATEK default, game-like (SDK), libnds-like, default
dtcm_table:
    .word   0x0300000A, 0x0080000A, 0x027E000A, 0x0B00000A, 0x0300000A
#endif
    .ltorg

/* ------------------------------------------------------------------ */
/* IRQ handler, called by the BIOS IRQ vector in IRQ mode with r0-r3,  */
/* r12 and lr saved. Acknowledges IE&IF and ORs it into the BIOS flags */
/* ------------------------------------------------------------------ */

    .text
    .global irq_handler
irq_handler:
    mov     r12, #0x04000000
    add     r12, r12, #0x200
    ldr     r1, [r12, #0x10]            @ IE
    ldr     r2, [r12, #0x14]            @ IF
    and     r1, r1, r2
    str     r1, [r12, #0x14]            @ acknowledge
#if defined(ARM9)
    mrc     p15, 0, r0, c9, c1, 0
    mov     r0, r0, lsr #12
    mov     r0, r0, lsl #12
    add     r0, r0, #0x3F00
    add     r0, r0, #0xF8               @ DTCM+3FF8h
#else
    ldr     r0, =ARM7_IRQ_FLAGS
#endif
    ldr     r2, [r0]
    orr     r2, r2, r1
    str     r2, [r0]
    ldr     r0, =irq_stats
    ldr     r2, [r0]
    add     r2, r2, #1
    str     r2, [r0]                    @ IRQs taken
    ldr     r2, [r0, #4]
    orr     r2, r2, r1
    str     r2, [r0, #4]                @ IRQ bits seen
    bx      lr
    .ltorg

/* ------------------------------------------------------------------ */
/* run_swi(struct swi_io *io)                                         */
/*   io: 0-3 r0-r3 in, 4 stub address, 5-8 r0-r3 out, 9 CPSR,         */
/*       10 changed mask (bit n = rn for r4-r11, 13 = sp, 14 = lr),    */
/*       11 cycles (cascaded timer 0/1, 33MHz ticks)                   */
/* r4-r11 are set to a pattern, so the mask shows what the SWI kept.   */
/* ------------------------------------------------------------------ */

    .global run_swi
run_swi:
    push    {r4-r11, lr}
    ldr     r1, =cur_io
    str     r0, [r1]
    str     sp, [r1, #4]
    ldr     r12, [r0, #16]
    ldr     r1, =hi_pattern
    ldmia   r1, {r4-r11}
    mov     lr, r0
    mov     r1, #0x04000000
    mov     r2, #0x00840000
    str     r2, [r1, #0x104]            @ timer 1: count-up, on
    mov     r2, #0x00800000
    str     r2, [r1, #0x100]            @ timer 0: F/1, on
    ldmia   lr, {r0-r3}
    adr     lr, swi_back
    bx      r12
swi_back:
    mov     r12, #0x04000000
    str     r12, [r12, #0x100]          @ timer 0 control 0400h = off (timer 1 stops with it)
    mrs     r12, cpsr
    str     r12, swi_cpsr_slot
    ldr     r12, =cur_io
    ldr     r12, [r12]
    str     r0, [r12, #20]
    str     r1, [r12, #24]
    str     r2, [r12, #28]
    str     r3, [r12, #32]
    ldr     r0, swi_cpsr_slot
    str     r0, [r12, #36]
    ldr     r1, =hi_pattern
    mov     r0, #0
    ldr     r2, [r1, #0]
    cmp     r4, r2
    orrne   r0, r0, #(1 << 4)
    ldr     r2, [r1, #4]
    cmp     r5, r2
    orrne   r0, r0, #(1 << 5)
    ldr     r2, [r1, #8]
    cmp     r6, r2
    orrne   r0, r0, #(1 << 6)
    ldr     r2, [r1, #12]
    cmp     r7, r2
    orrne   r0, r0, #(1 << 7)
    ldr     r2, [r1, #16]
    cmp     r8, r2
    orrne   r0, r0, #(1 << 8)
    ldr     r2, [r1, #20]
    cmp     r9, r2
    orrne   r0, r0, #(1 << 9)
    ldr     r2, [r1, #24]
    cmp     r10, r2
    orrne   r0, r0, #(1 << 10)
    ldr     r2, [r1, #28]
    cmp     r11, r2
    orrne   r0, r0, #(1 << 11)
    ldr     r2, =cur_io
    ldr     r2, [r2, #4]
    cmp     sp, r2
    orrne   r0, r0, #(1 << 13)
    adr     r2, swi_back
    cmp     lr, r2
    orrne   r0, r0, #(1 << 14)
    str     r0, [r12, #40]
    mov     r1, #0x04000000
    add     r1, r1, #0x100
    ldrh    r0, [r1]
    ldrh    r2, [r1, #4]
    orr     r0, r0, r2, lsl #16
    str     r0, [r12, #44]
    mov     r0, #0
    str     r0, [r1, #4]                @ timer 1 off
    ldr     r1, =cur_io
    ldr     sp, [r1, #4]
    pop     {r4-r11, lr}
    bx      lr
swi_cpsr_slot:
    .word   0
    .ltorg

hi_pattern:
    .word   0x44444444, 0x55555555, 0x66666666, 0x77777777
    .word   0x88888888, 0x99999999, 0xAAAAAAAA, 0xBBBBBBBB

/* SWI stubs, one per number: ARM "svc #n<<16; bx lr" (8 bytes each) and */
/* Thumb "svc #n; bx lr" (4 bytes each)                                  */

    .balign 4
    .global swi_stubs_arm
swi_stubs_arm:
    .set    n, 0
    .rept   256
    svc     #(n << 16)
    bx      lr
    .set    n, n + 1
    .endr

    .thumb
    .balign 4
    .global swi_stubs_thumb
swi_stubs_thumb:
    .set    n, 0
    .rept   256
    svc     #n
    bx      lr
    .set    n, n + 1
    .endr
    .arm

/* ------------------------------------------------------------------ */
/* decompression callbacks (GBATEK "NDS/DSi Decompression Callbacks")  */
/* cb_state: 0 open, 4 close, 8 get8, 12 get16, 16 get32 call counts,  */
/*           20/24/28 r0/r1/r2 seen by Open, 32 r0 seen by Close       */
/* Each callback keeps r1-r3.                                          */
/* ------------------------------------------------------------------ */

.macro CB_COUNT ofs
    ldr     r3, =cb_state
    ldr     r1, [r3, #\ofs]
    add     r1, r1, #1
    str     r1, [r3, #\ofs]
.endm

cb_open:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #20]
    str     r1, [r3, #24]
    str     r2, [r3, #28]
    CB_COUNT 0
    ldr     r0, [r0]
    pop     {r1-r3}
    bx      lr

cb_open_err:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #20]
    str     r1, [r3, #24]
    str     r2, [r3, #28]
    CB_COUNT 0
    mvn     r0, #2                      @ -3
    pop     {r1-r3}
    bx      lr

cb_open_short:                          @ returns the header with the size cut to 8 bytes
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #20]
    str     r1, [r3, #24]
    str     r2, [r3, #28]
    CB_COUNT 0
    ldr     r0, [r0]
    and     r0, r0, #0xFF
    orr     r0, r0, #0x800
    pop     {r1-r3}
    bx      lr

cb_close:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #32]
    CB_COUNT 4
    mov     r0, #0
    pop     {r1-r3}
    bx      lr

cb_close_err:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #32]
    CB_COUNT 4
    mvn     r0, #0                      @ -1
    pop     {r1-r3}
    bx      lr

cb_get8:
    push    {r1-r3}
    CB_COUNT 8
    ldrb    r0, [r0]
    pop     {r1-r3}
    bx      lr

cb_get16:
    push    {r1-r3}
    CB_COUNT 12
    ldrh    r0, [r0]
    pop     {r1-r3}
    bx      lr

cb_get32:
    push    {r1-r3}
    CB_COUNT 16
    ldr     r0, [r0]
    pop     {r1-r3}
    bx      lr
    .ltorg

    .thumb
.macro TCB_COUNT ofs
    ldr     r3, =cb_state
    ldr     r1, [r3, #\ofs]
    adds    r1, r1, #1
    str     r1, [r3, #\ofs]
.endm

    .thumb_func
tcb_open:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #20]
    str     r1, [r3, #24]
    str     r2, [r3, #28]
    TCB_COUNT 0
    ldr     r0, [r0]
    pop     {r1-r3}
    bx      lr

    .thumb_func
tcb_close:
    push    {r1-r3}
    ldr     r3, =cb_state
    str     r0, [r3, #32]
    TCB_COUNT 4
    movs    r0, #0
    pop     {r1-r3}
    bx      lr

    .thumb_func
tcb_get8:
    push    {r1-r3}
    TCB_COUNT 8
    ldrb    r0, [r0]
    pop     {r1-r3}
    bx      lr

    .thumb_func
tcb_get16:
    push    {r1-r3}
    TCB_COUNT 12
    ldrh    r0, [r0]
    pop     {r1-r3}
    bx      lr

    .thumb_func
tcb_get32:
    push    {r1-r3}
    TCB_COUNT 16
    ldr     r0, [r0]
    pop     {r1-r3}
    bx      lr
    .balign 4
    .ltorg
    .arm

/* callback structures: Open, Close, Get8, Get16, Get32 */
    .section .rodata
    .balign 4
    .global cb_structs
cb_structs:
    .word   cb_open, cb_close, cb_get8, cb_get16, cb_get32         @ 0 normal
    .word   cb_open, 0, cb_get8, cb_get16, cb_get32                @ 1 no Close
    .word   cb_open_err, cb_close, cb_get8, cb_get16, cb_get32     @ 2 Open returns -3
    .word   cb_open, cb_close_err, cb_get8, cb_get16, cb_get32     @ 3 Close returns -1
    .word   tcb_open, tcb_close, tcb_get8, tcb_get16, tcb_get32    @ 4 Thumb callbacks
    .word   cb_open_short, cb_close, cb_get8, cb_get16, cb_get32   @ 5 Open returns size 8
    .text

/* ------------------------------------------------------------------ */
/* traps: write what we know to the results header and stop            */
/* ------------------------------------------------------------------ */

    .global trap_zero
trap_zero:
    str     r0, trap_r0_slot
    mov     r0, #1
    b       trap_body
    .global trap_exception
trap_exception:
    str     r0, trap_r0_slot
    mov     r0, #2
trap_body:
    @ keep the raw values out of the results area: trap_finish (runner.c)
    @ masks BIOS-range values before writing the header
    ldr     r12, =trap_save
    str     r0, [r12]
    mrs     r0, cpsr
    str     r0, [r12, #4]
    str     lr, [r12, #8]
    ldr     r0, trap_r0_slot
    str     r0, [r12, #12]
    str     r1, [r12, #16]
    str     r2, [r12, #20]
    str     r3, [r12, #24]
    msr     cpsr_c, #0xDF
    ldr     sp, =SYS_STACK
    bl      trap_finish
    b       .
trap_r0_slot:
    .word   0
    .ltorg

/* halt with nothing enabled: the CPU sleeps for the rest of the run */
    .global halt_forever
halt_forever:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]            @ IME = 0
    str     r1, [r0, #0x210]            @ IE = 0
1:
#if defined(ARM9)
    mcr     p15, 0, r1, c7, c0, 4       @ halt (GBATEK: hangs with IME = 0, which is the point)
#else
    add     r2, r0, #0x300
    mov     r1, #0x80
    strb    r1, [r2, #1]                @ HALTCNT = 80h
#endif
    b       1b

/* ------------------------------------------------------------------ */
/* SoftReset: load a pattern into r0-r12 and lr, then SWI 0.           */
/* The entry address at 027FFE24h/027FFE34h points at sr_probe_*.      */
/* ------------------------------------------------------------------ */

    .global do_softreset
do_softreset:
    ldr     lr, =0x0E0E0E0E
    ldr     r12, =sr_pattern
    ldmia   r12, {r0-r12}
    svc     #0
    b       .
    .ltorg
sr_pattern:
    .word   0x10101010, 0x11111111, 0x12121212, 0x13131313, 0x14141414, 0x15151515, 0x16161616
    .word   0x17171717, 0x18181818, 0x19191919, 0x1A1A1A1A, 0x1B1B1B1B, 0x1C1C1C1C

/* sr_save: 0 r0, 4-48 r1-r12, 52 sp_sys, 56 lr_sys, 60 sp_svc, 64 lr_svc, */
/* 68 spsr_svc, 72 sp_irq, 76 lr_irq, 80 spsr_irq, 84 CPSR at entry,       */
/* 88 CP15 control, 92 DTCM region, 96 ITCM region, 100 Thumb entry flag   */
    .global sr_probe_arm
sr_probe_arm:
    str     r0, sr_slot_r0
    mrs     r0, cpsr
    str     r0, sr_slot_cpsr
    mov     r0, #0
    str     r0, sr_slot_thumb
    b       sr_probe_body

    .thumb
    .balign 4
    .thumb_func
    .global sr_probe_thumb
sr_probe_thumb:
    bx      pc                          @ to ARM at the next word
    nop
    .arm
    str     r0, sr_slot_r0
    mrs     r0, cpsr
    str     r0, sr_slot_cpsr
    mov     r0, #1
    str     r0, sr_slot_thumb

sr_probe_body:
    ldr     r0, =sr_save
    add     r0, r0, #4
    stmia   r0, {r1-r12}
    ldr     r0, =sr_save
    str     sp, [r0, #52]
    str     lr, [r0, #56]
    msr     cpsr_c, #0xD3
    str     sp, [r0, #60]
    str     lr, [r0, #64]
    mrs     r1, spsr
    str     r1, [r0, #68]
    msr     cpsr_c, #0xD2
    str     sp, [r0, #72]
    str     lr, [r0, #76]
    mrs     r1, spsr
    str     r1, [r0, #80]
    msr     cpsr_c, #0xDF
    ldr     r1, sr_slot_r0
    str     r1, [r0, #0]
    ldr     r1, sr_slot_cpsr
    str     r1, [r0, #84]
    ldr     r1, sr_slot_thumb
    str     r1, [r0, #100]
#if defined(ARM9)
    mrc     p15, 0, r1, c1, c0, 0
    str     r1, [r0, #88]
    mrc     p15, 0, r1, c9, c1, 0
    str     r1, [r0, #92]
    mrc     p15, 0, r1, c9, c1, 1
    str     r1, [r0, #96]
#endif
    ldr     sp, =SYS_STACK
    bl      sr_finish
    b       .
sr_slot_r0:
    .word   0
sr_slot_cpsr:
    .word   0
sr_slot_thumb:
    .word   0
    .ltorg

/* ------------------------------------------------------------------ */
/* CP15 helpers (ARM9)                                                 */
/* ------------------------------------------------------------------ */

#if defined(ARM9)
    .global cp15_get_control
cp15_get_control:
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr
    .global cp15_set_control
cp15_set_control:
    mcr     p15, 0, r0, c1, c0, 0
    bx      lr
    .global cp15_get_dtcm
cp15_get_dtcm:
    mrc     p15, 0, r0, c9, c1, 0
    bx      lr
#endif

/* ------------------------------------------------------------------ */
    .data
    .balign 4
    .global cur_io, irq_stats, cb_state, sr_save, trap_save
trap_save:
    .space  32
cur_io:
    .word   0, 0
irq_stats:
    .word   0, 0
cb_state:
    .space  64
sr_save:
    .space  128
