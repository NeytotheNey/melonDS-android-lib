/*
    Memory layout of the SWI test ROM, shared by crt.S, runner.c and (by
    hand) gen_tests.py / report.py. Everything the harness reads back is in
    main RAM; the ARM9 binary loads at 02000000h, the ARM7 binary at
    02380000h.

    -DTWL builds the DSi-mode ROM: bigger results areas, the DSi addresses of
    the ARM9 debug vector and the ARM7 IF2 check bits, and an ARM7 svc stack
    below them.
*/

#ifndef SWITESTROM_LAYOUT_H
#define SWITESTROM_LAYOUT_H

/* control block, poked by the harness before the first frame */
#define CTRL_BASE       0x02300000
#define CTRL_MODE       0x00        /* 0 = normal, 1 = isolated test, 2 = SoftReset */
#define CTRL_PARAM      0x04        /* test id (mode 1) or SoftReset variant (mode 2) */
#define CTRL_ARM9READY  0x10        /* set by the ARM9 once VRAM is mapped */

#define MODE_NORMAL     0
#define MODE_ISOLATED   1
#define MODE_SOFTRESET  2

/* results: a 100h-byte header, then 100h-byte records */
#define RES9_BASE       0x02200000
#if defined(TWL)
#define RES7_BASE       0x02220000
#define RES_SIZE        0x20000
#else
#define RES7_BASE       0x02210000
#define RES_SIZE        0x10000
#endif
#define REC_SIZE        0x100
#define RES_MAGIC       0x52545753  /* "SWTR" */
#define RES_DONE        0x454E4F44  /* "DONE" */

/* header words */
#define HDR_MAGIC       0
#define HDR_VERSION     1
#define HDR_MODE        2
#define HDR_PARAM       3
#define HDR_COUNT       4
#define HDR_CURRENT     5   /* id of the test that is running, FFFFFFFFh between tests */
#define HDR_DONE        6
#define HDR_CPU         7
#define HDR_TRAP        8   /* 0 = none, 1 = jumped to 0 (ARM9 ITCM trap), 2 = exception (debug vector) */
#define HDR_TRAP_CPSR   9
#define HDR_TRAP_LR     10
#define HDR_TRAP_R0     11  /* r0-r3 */

/* work buffers (per CPU) */
#define BUF9_SRC        0x02100000
#define BUF9_DST        0x02110000
#define BUF9_AUX        0x02120000
#define BUF9_TMP        0x02121000
#define BUF9_VRAM       0x06800000  /* VRAM bank A, LCDC */
#define BUF7_SRC        0x02140000
#define BUF7_DST        0x02150000
#define BUF7_AUX        0x02160000
#define BUF7_TMP        0x02161000
#define BUF7_VRAM       0x06000000  /* VRAM bank C, mapped to the ARM7 */
#define BUF_SRC_SIZE    0x10000
#define BUF_DST_SIZE    0x10000

/* stacks */
#define STACK9_SYS      0x020FF000  /* main RAM, so it survives DTCM moves */
#define STACK7_SYS      0x0237F000
/* svc at GBATEK's SoftReset value; irq lower than GBATEK's so the
   callback decompressors (which keep variables on the svc stack) have room */
#if defined(TWL)
#define STACK7_SVC      0x0380FFC0  /* GBATEK's NDS value; the DSi7 IF2 check bits are at 0380FFC0h */
#else
#define STACK7_SVC      0x0380FFDC
#endif
#define STACK7_IRQ      0x0380FE00
#define DTCM_SVC_SP     0x3FC0      /* ARM9: DTCM-relative */
#define DTCM_IRQ_SP     0x3D00

/* BIOS RAM locations (GBATEK "BIOS RAM Usage" / "DS Interrupts") */
#define ARM7_IRQ_FLAGS  0x0380FFF8
#define ARM7_IRQ_VECTOR 0x0380FFFC
#define ARM7_IRQ_FLAGS2 0x0380FFC0  /* DSi7 IF2 check bits */
#define ARM7_DEBUG_VEC  0x0380FFDC
#if defined(TWL)
#define ARM9_DEBUG_VEC  0x02FFFD9C
#else
#define ARM9_DEBUG_VEC  0x027FFD9C
#endif
#define ENTRY9_ADDR     0x027FFE24
#define ENTRY7_ADDR     0x027FFE34

#endif
