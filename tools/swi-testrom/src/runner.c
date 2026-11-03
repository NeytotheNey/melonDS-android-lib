/*
    Test runner of the SWI test ROM, built once per CPU (-DARM9 / -DARM7).
    Runs the generated test table (tests_arm9.h / tests_arm7.h from
    gen_tests.py) and writes one fixed-size record per test to the results
    area in main RAM (layout.h). See gen_tests.py for the record layout.
*/

#include "layout.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))
#define REG8(a)  (*(volatile u8*)(a))

#define REG_DISPSTAT 0x04000004
#define REG_VCOUNT   0x04000006
#define REG_TM2CNT   0x04000108
#define REG_TM3CNT   0x0400010C
#define REG_IME      0x04000208
#define REG_IE       0x04000210
#define REG_IF       0x04000214

/* test flags, kinds and input relocations: keep in sync with gen_tests.py */
#define F_THUMB     0x01
#define F_HASHONLY  0x02
#define F_VRAM      0x04
#define F_ISOLATED  0x08

#define K_REGS  0
#define K_MEM   1
#define K_CB    2
#define K_TABLE 3
#define K_IRQ   4

#define R_RAW 0
#define R_SRC 1
#define R_DST 2
#define R_AUX 3
#define R_TMP 4
#define R_CB  5

typedef struct
{
    u16 id;
    u8 swi;
    u8 kind;
    u16 flags;
    u16 docmask;    /* bit n: rn is a result GBATEK documents (never masked) */
    u32 in[4];
    u8 rel[4];
    u32 src_off, src_len, src_at;
    u32 aux_off, aux_len;
    u32 out_len;
    u32 tmp_len;
    u32 exp_off;
    u32 p[8];
} test_t;

/* record words */
#define W_ID       0
#define W_STATUS   1
#define W_R0       2
#define W_CPSR     6
#define W_CHANGED  7
#define W_CYCLES   8
#define W_HASH     9
#define W_OUTLEN   10
#define W_MODELOFS 11
#define W_TMPHASH  12
#define W_EXTRA    13   /* 19 words */
#define W_START    32   /* 64 bytes from the start of the output region */
#define W_WINOFS   48
#define W_WIN      49   /* 60 bytes around the first byte that differs from the GBATEK model */

#define ST_RETURNED 0x01
#define ST_HASHONLY 0x02
#define ST_MASKED   0x100   /* bit 8+n: rn held a BIOS-range value and was not stored */

#if defined(ARM9)
#include "tests_arm9.h"
#define RES_BASE  RES9_BASE
#define BUF_SRC   BUF9_SRC
#define BUF_DST   BUF9_DST
#define BUF_AUX   BUF9_AUX
#define BUF_TMP   BUF9_TMP
#define BUF_VRAM  BUF9_VRAM
#define CPU_NUM   9
#else
#include "tests_arm7.h"
#define RES_BASE  RES7_BASE
#define BUF_SRC   BUF7_SRC
#define BUF_DST   BUF7_DST
#define BUF_AUX   BUF7_AUX
#define BUF_TMP   BUF7_TMP
#define BUF_VRAM  BUF7_VRAM
#define CPU_NUM   7
#endif

typedef struct
{
    u32 in[4];
    u32 stub;
    u32 out[4];
    u32 cpsr;
    u32 changed;
    u32 cycles;
} swi_io;

extern void run_swi(swi_io* io);
extern void halt_forever(void);
extern void do_softreset(void);
extern void sr_probe_arm(void);
extern void sr_probe_thumb(void);
extern const u8 swi_stubs_arm[];
extern const u8 swi_stubs_thumb[];
extern const u32 cb_structs[];
extern volatile u32 irq_stats[2];
extern volatile u32 cb_state[16];
extern volatile u32 sr_save[32];
#if defined(ARM9)
extern u32 cp15_get_control(void);
extern void cp15_set_control(u32 val);
extern u32 cp15_get_dtcm(void);
#endif

/* the compiler may emit calls to these */
void* memset(void* dst, int val, unsigned int len)
{
    u8* d = dst;
    while (len--) *d++ = (u8)val;
    return dst;
}

void* memcpy(void* dst, const void* src, unsigned int len)
{
    u8* d = dst;
    const u8* s = src;
    while (len--) *d++ = *s++;
    return dst;
}

static volatile u32* const Hdr = (volatile u32*)RES_BASE;

static volatile u32* irq_flags(void)
{
#if defined(ARM9)
    return (volatile u32*)((cp15_get_dtcm() & ~0xFFF) + 0x3FF8);
#else
    return (volatile u32*)ARM7_IRQ_FLAGS;
#endif
}

/* clean room: values that look like BIOS addresses (or data loaded from the
   BIOS) are never stored raw, only flagged */
static int bios_range(u32 v)
{
#if defined(ARM9)
    return v >= 0xFFFF0000;
#else
    return v != 0 && v < 0x4000;
#endif
}

static u32 fnv_bytes(u32 h, u32 addr, u32 len)
{
    for (u32 i = 0; i < len; i++)
    {
        h ^= REG8(addr + i);
        h *= 0x01000193;
    }
    return h;
}

static u32 fnv_word(u32 h, u32 val)
{
    for (int i = 0; i < 4; i++)
    {
        h ^= (val >> (i * 8)) & 0xFF;
        h *= 0x01000193;
    }
    return h;
}

/* byte i of a prefilled region is (i & FFh) ^ xor, written as words (VRAM ignores 8-bit writes) */
static void prefill(u32 base, u32 len, u32 xor)
{
    for (u32 i = 0; i < len; i += 4)
    {
        u32 w = 0;
        for (u32 b = 0; b < 4; b++)
            w |= (((i + b) & 0xFF) ^ xor) << (b * 8);
        REG32(base + i) = w;
    }
}

static void copy_bytes(u32 dst, const u8* src, u32 len)
{
    for (u32 i = 0; i < len; i++) REG8(dst + i) = src[i];
}

static void zero_words(u32 base, u32 len)
{
    for (u32 i = 0; i < len; i += 4) REG32(base + i) = 0;
}

static void irq_reset(void)
{
    REG32(REG_IME) = 0;
    REG32(REG_IE) = 0;
    REG32(REG_TM2CNT) = 0;
    REG32(REG_TM3CNT) = 0;
    REG16(REG_DISPSTAT) &= ~(1 << 3);
    REG32(REG_IF) = 0xFFFFFFFF;
    *irq_flags() = 0;
    irq_stats[0] = 0;
    irq_stats[1] = 0;
}

static u32 stub_for(const test_t* t)
{
    if (t->flags & F_THUMB)
        return ((u32)swi_stubs_thumb & ~1) + t->swi * 4 + 1;
    return (u32)swi_stubs_arm + t->swi * 8;
}

static u32 dst_base(const test_t* t)
{
    return (t->flags & F_VRAM) ? BUF_VRAM : BUF_DST;
}

static u32 reloc(const test_t* t, int n)
{
    u32 v = t->in[n];
    switch (t->rel[n])
    {
    case R_SRC: return BUF_SRC + v;
    case R_DST: return dst_base(t) + v;
    case R_AUX: return BUF_AUX + v;
    case R_TMP: return BUF_TMP + v;
    case R_CB:  return (u32)&cb_structs[v * 5];
    default:    return v;
    }
}

static void run_test(const test_t* t, volatile u32* rec)
{
    swi_io io;
    u32 extra[19];
    for (int i = 0; i < 19; i++) extra[i] = 0;
    for (int i = 0; i < 64; i++) rec[i] = 0;

    Hdr[HDR_CURRENT] = t->id;
    rec[W_ID] = t->id;

    /* buffers */
    if (t->src_len)
    {
        zero_words(BUF_SRC, (t->src_at + t->src_len + 64 + 3) & ~3);
        copy_bytes(BUF_SRC + t->src_at, &testblob[t->src_off], t->src_len);
    }
    if (t->aux_len)
        copy_bytes(BUF_AUX, &testblob[t->aux_off], t->aux_len);
    if (t->out_len)
        prefill(dst_base(t), t->out_len, 0xA5);
    if (t->tmp_len)
        prefill(BUF_TMP, t->tmp_len, 0x3C);

    for (int i = 0; i < 4; i++) io.in[i] = reloc(t, i);
    io.stub = stub_for(t);

    u32 hash = 0x811C9DC5;
    u32 scratch = 0x811C9DC5;
    if (t->kind == K_TABLE)
    {
        /* GBATEK table SWIs: never store what they return, only a hash of r0 (the
           result) and a separate one of r1-r3 (scratch) */
        u32 cycles = 0;
        for (u32 n = 0; n < t->p[1]; n++)
        {
            io.in[0] = t->p[0] + n * t->p[2];
            run_swi(&io);
            hash = fnv_word(hash, io.out[0]);
            for (int r = 1; r < 4; r++) scratch = fnv_word(scratch, io.out[r]);
            cycles += io.cycles;
        }
        io.cycles = cycles;
        extra[0] = t->p[1];
    }
    else if (t->kind == K_IRQ)
    {
        irq_reset();
        /* start every IRQ test at the same point of the frame (VCOUNT 100) so
           VBlank timing does not depend on how long the earlier tests took */
        while (REG16(REG_VCOUNT) == 100) {}
        while (REG16(REG_VCOUNT) != 100) {}
        volatile u32* flags = irq_flags();
        *flags = t->p[0];
        if (t->p[5]) REG16(REG_DISPSTAT) |= (1 << 3);
        /* timers at F/64 with IRQ: reload = 10000h - delay */
        if (t->p[4]) REG32(REG_TM3CNT) = (0x10000 - t->p[4]) | (0xC1 << 16);
        if (t->p[3]) REG32(REG_TM2CNT) = (0x10000 - t->p[3]) | (0xC1 << 16);
        REG32(REG_IE) = t->p[1];
        REG32(REG_IME) = t->p[2];
        run_swi(&io);
        u32 ime = REG32(REG_IME);
        REG32(REG_IME) = 0;
        extra[0] = *flags;
        extra[1] = REG32(REG_IF);
        extra[2] = REG32(REG_IE);
        extra[3] = ime;
        extra[4] = irq_stats[0];
        extra[5] = irq_stats[1];
        irq_reset();
    }
    else
    {
        if (t->kind == K_REGS && t->p[0])
        {
            if (t->p[2] == 1) REG8(t->p[0]) = t->p[1];
            else if (t->p[2] == 2) REG16(t->p[0]) = t->p[1];
            else REG32(t->p[0]) = t->p[1];
        }
        if (t->kind == K_CB)
            for (int i = 0; i < 16; i++) cb_state[i] = 0;
        run_swi(&io);
        if (t->kind == K_REGS)
        {
            if (t->p[3]) extra[0] = (t->p[4] == 2) ? REG16(t->p[3]) : (t->p[4] == 1) ? REG8(t->p[3]) : REG32(t->p[3]);
            if (t->p[5]) extra[1] = REG32(t->p[5]);
        }
        if (t->kind == K_CB)
            for (int i = 0; i < 9; i++) extra[i] = cb_state[i];
    }

    rec[W_STATUS] = ST_RETURNED;
    rec[W_CPSR] = io.cpsr;
    rec[W_CHANGED] = io.changed;
    rec[W_CYCLES] = io.cycles;
    if ((t->flags & F_HASHONLY) && t->kind != K_TABLE)
    {
        for (int r = 0; r < 4; r++) hash = fnv_word(hash, io.out[r]);
        rec[W_STATUS] |= ST_HASHONLY;
        rec[W_HASH] = hash;
    }
    else if (t->kind == K_TABLE)
    {
        rec[W_STATUS] |= ST_HASHONLY;
        rec[W_HASH] = hash;
        rec[W_TMPHASH] = scratch;
    }
    else
    {
        for (int r = 0; r < 4; r++)
        {
            if (!(t->docmask & (1 << r)) && bios_range(io.out[r]))
                rec[W_STATUS] |= ST_MASKED << r;
            else
                rec[W_R0 + r] = io.out[r];
        }
        u32 base = dst_base(t);
        rec[W_HASH] = fnv_bytes(0x811C9DC5, base, t->out_len);
        rec[W_OUTLEN] = t->out_len;
        for (u32 i = 0; i < 64 && i < t->out_len; i += 4)
            rec[W_START + i / 4] = REG32(base + i);

        /* first byte that differs from the GBATEK model, and a window around it */
        rec[W_MODELOFS] = 0xFFFFFFFE;
        if (t->exp_off != 0xFFFFFFFF)
        {
            u32 ofs = 0xFFFFFFFF;
            for (u32 i = 0; i < t->out_len; i++)
                if (REG8(base + i) != testblob[t->exp_off + i]) { ofs = i; break; }
            rec[W_MODELOFS] = ofs;
            if (ofs != 0xFFFFFFFF)
            {
                u32 win = (ofs & ~3) >= 16 ? (ofs & ~3) - 16 : 0;
                rec[W_WINOFS] = win;
                for (u32 i = 0; i < 60 && win + i < t->out_len; i += 4)
                    rec[W_WIN + i / 4] = REG32(base + win + i);
            }
        }
    }
    if (t->tmp_len)
        rec[W_TMPHASH] = fnv_bytes(0x811C9DC5, BUF_TMP, t->tmp_len);
    for (int i = 0; i < 19; i++) rec[W_EXTRA + i] = extra[i];

    /* don't leave anything in the work buffers between tests */
    if (t->out_len) prefill(dst_base(t), t->out_len, 0xA5);

    Hdr[HDR_COUNT] = Hdr[HDR_COUNT] + 1;
    Hdr[HDR_CURRENT] = 0xFFFFFFFF;
}

/* called by the traps in crt.S (jump to 0, CPU exception) with what they
   saved: kind, CPSR, lr, r0-r3. BIOS-range values are only flagged in
   HDR_TRAP bits 8 (lr) and 9-12 (r0-r3). */
extern volatile u32 trap_save[8];

void trap_finish(void)
{
    u32 kind = trap_save[0];
    if (bios_range(trap_save[2])) kind |= 1 << 8;
    else Hdr[HDR_TRAP_LR] = trap_save[2];
    for (int r = 0; r < 4; r++)
    {
        if (bios_range(trap_save[3 + r])) kind |= 1 << (9 + r);
        else Hdr[HDR_TRAP_R0 + r] = trap_save[3 + r];
    }
    Hdr[HDR_TRAP_CPSR] = trap_save[1];
    Hdr[HDR_TRAP] = kind;
    halt_forever();
}

/* ------------------------------------------------------------------ */
/* SoftReset (mode 2). Variants 0-4 are ARM9, 5-6 ARM7; see gen_tests.py */
/* ------------------------------------------------------------------ */

#define SR_ID_BASE 0xF000

#if defined(ARM9)
#define SR_AREA_LO  0x3C00   /* DTCM-relative */
#else
#define SR_AREA_LO  0x0380FC00
#endif

static u32 count_words(u32 base, u32 len, u32 pat_xor, u32* zeros)
{
    u32 intact = 0;
    *zeros = 0;
    for (u32 i = 0; i < len; i += 4)
    {
        u32 v = REG32(base + i);
        if (v == 0) (*zeros)++;
        if (v == (pat_xor | ((base + i) & 0xFFFF))) intact++;
    }
    return intact;
}

static void sr_prefill(u32 base, u32 len, u32 pat_xor)
{
    for (u32 i = 0; i < len; i += 4)
        REG32(base + i) = pat_xor | ((base + i) & 0xFFFF);
}

static void softreset_mode(u32 variant)
{
#if defined(ARM9)
    int mine = variant <= 4;
    int thumb = variant == 4;
#else
    int mine = variant == 5 || variant == 6;
    int thumb = variant == 6;
#endif
    if (!mine) return;

    volatile u32* rec = (volatile u32*)(RES_BASE + REC_SIZE);
    for (int i = 0; i < 64; i++) rec[i] = 0;
    irq_reset();

#if defined(ARM9)
    u32 dtcm = cp15_get_dtcm();
    u32 base = dtcm & ~0xFFF;
    sr_prefill(base + SR_AREA_LO, 0x400, 0xA5000000);
    sr_prefill(SR_AREA_LO, 0x400, 0x5A000000);      /* ITCM, where 00803C00h lands while ITCM covers it */
    rec[W_START + 8] = dtcm;
    rec[W_START + 9] = cp15_get_control();
#else
    sr_prefill(SR_AREA_LO, 0x400, 0xA5000000);
#endif

    u32 entry = thumb ? (u32)sr_probe_thumb : (u32)sr_probe_arm;
#if defined(ARM9)
    REG32(ENTRY9_ADDR) = entry;
#else
    REG32(ENTRY7_ADDR) = entry;
#endif
    rec[W_START + 10] = entry;
    Hdr[HDR_CURRENT] = SR_ID_BASE + variant;
    do_softreset();
}

/* called by the probe, on the main RAM stack, after the BIOS jumped to the entry address */
void sr_finish(void)
{
    u32 variant = REG32(CTRL_BASE + CTRL_PARAM);
    volatile u32* rec = (volatile u32*)(RES_BASE + REC_SIZE);
    rec[W_ID] = SR_ID_BASE + variant;
    rec[W_STATUS] = ST_RETURNED;
    /* r0-r12 and the banked sp/lr: BIOS-range values are only flagged (bit n
       of W_START+11 = sr_save word n) */
    u32 masked = 0;
    u32 v[21];
    for (int i = 0; i < 21; i++)
    {
        v[i] = sr_save[i];
        if (i != 17 && i != 20 && bios_range(v[i])) { masked |= 1 << i; v[i] = 0; }
    }
    for (int r = 0; r < 4; r++) rec[W_R0 + r] = v[r];
    rec[W_CPSR] = sr_save[21];
    for (int r = 4; r <= 12; r++) rec[W_EXTRA + r - 4] = v[r];  /* e0-e8 r4-r12 */
    for (int i = 0; i < 8; i++) rec[W_EXTRA + 9 + i] = v[13 + i]; /* e9-e16 banked sp/lr/spsr */
    rec[W_START + 11] = masked;
    rec[W_EXTRA + 17] = sr_save[22];    /* CP15 control */
    rec[W_EXTRA + 18] = sr_save[23];    /* DTCM region */
    rec[W_START + 0] = sr_save[24];     /* ITCM region */
    rec[W_START + 1] = sr_save[25];     /* entered in Thumb */

    u32 zlo, zhi;
#if defined(ARM9)
    /* turn both TCMs on again so both areas can be read */
    cp15_set_control(cp15_get_control() | (1 << 16) | (1 << 18));
    u32 base = sr_save[23] & ~0xFFF;
    rec[W_START + 4] = count_words(base + SR_AREA_LO, 0x200, 0xA5000000, &zlo);
    rec[W_START + 5] = count_words(base + SR_AREA_LO + 0x200, 0x200, 0xA5000000, &zhi);
    rec[W_START + 2] = zlo;
    rec[W_START + 3] = zhi;
    rec[W_START + 6] = (count_words(SR_AREA_LO, 0x200, 0x5A000000, &zlo), zlo);
    rec[W_START + 7] = (count_words(SR_AREA_LO + 0x200, 0x200, 0x5A000000, &zhi), zhi);
#else
    rec[W_START + 4] = count_words(SR_AREA_LO, 0x200, 0xA5000000, &zlo);
    rec[W_START + 5] = count_words(SR_AREA_LO + 0x200, 0x200, 0xA5000000, &zhi);
    rec[W_START + 2] = zlo;
    rec[W_START + 3] = zhi;
#endif

    Hdr[HDR_COUNT] = 1;
    Hdr[HDR_CURRENT] = 0xFFFFFFFF;
    Hdr[HDR_DONE] = RES_DONE;
    halt_forever();
}

int main(void)
{
    u32 mode = REG32(CTRL_BASE + CTRL_MODE);
    u32 param = REG32(CTRL_BASE + CTRL_PARAM);

    for (u32 i = 0; i < REC_SIZE / 4; i++) Hdr[i] = 0;
    Hdr[HDR_MAGIC] = RES_MAGIC;
    Hdr[HDR_VERSION] = 1;
    Hdr[HDR_MODE] = mode;
    Hdr[HDR_PARAM] = param;
    Hdr[HDR_CURRENT] = 0xFFFFFFFF;
    Hdr[HDR_CPU] = CPU_NUM;

#if defined(ARM9)
    REG8(0x04000240) = 0x80;    /* VRAM A: LCDC (06800000h) */
    REG8(0x04000242) = 0x82;    /* VRAM C: ARM7 (06000000h) */
    REG32(CTRL_BASE + CTRL_ARM9READY) = 1;
#else
    while (REG32(CTRL_BASE + CTRL_ARM9READY) != 1) {}
#endif

    if (mode == MODE_SOFTRESET)
        softreset_mode(param);
    else
    {
        volatile u32* rec = (volatile u32*)(RES_BASE + REC_SIZE);
        for (u32 i = 0; i < NUM_TESTS; i++)
        {
            const test_t* t = &tests[i];
            if (mode == MODE_NORMAL && (t->flags & F_ISOLATED)) continue;
            if (mode == MODE_ISOLATED && t->id != param) continue;
            if ((u32)(rec + 64) > RES_BASE + RES_SIZE) break;
            run_test(t, rec);
            rec += 64;
        }
    }

    Hdr[HDR_DONE] = RES_DONE;
    halt_forever();
    return 0;
}
