/*
    Test runner of the SWI test ROM, built once per CPU (-DARM9 / -DARM7).
    Runs the generated test table (tests_arm9.h / tests_arm7.h from
    gen_tests.py) and writes one fixed-size record per test to the results
    area in main RAM (layout.h). See gen_tests.py for the record layout.

    -DTWL (DSi-mode ROM) adds two test kinds (SWI sequences for SHA1/RSA,
    flag-only source checks), the DSi7 IF2 check bits in the IRQ tests, and a
    stricter rule for registers GBATEK gives no result for: they are stored
    only when they are a small value (up to FFh), equal to an input or a
    RAM/IO/VRAM address (02000000h-06FFFFFFh);
    anything else is only flagged, so no word the DSi BIOS loaded from itself
    can reach the results.
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
#define REG_IE2      0x04000218  /* DSi7 */
#define REG_IF2      0x0400021C

/* test flags, kinds and input relocations: keep in sync with gen_tests.py */
#define F_THUMB     0x01
#define F_HASHONLY  0x02
#define F_VRAM      0x04
#define F_ISOLATED  0x08
#define F_FLAGSONLY 0x10    /* DSi mode: keep only flags (output unchanged, r0-r3 kept, r0 = 0/1) */

#define K_REGS  0
#define K_MEM   1
#define K_CB    2
#define K_TABLE 3
#define K_IRQ   4
#define K_SEQ   5   /* DSi mode */
#define K_SRCCHK 6  /* DSi mode */
#define K_PROCS 7   /* GetBootProcs: behavior flags of two calls, no values */

#define R_RAW 0
#define R_SRC 1
#define R_DST 2
#define R_AUX 3
#define R_TMP 4
#define R_CB  5
#define R_FN  6     /* DSi mode: fn_table index */

#define SEQ_THUMB 0x100
#define SEQ_STORE 0x200

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
#if defined(TWL)
extern volatile u32 irq_stats[4];
extern const u32 fn_table[];
#else
extern volatile u32 irq_stats[2];
#endif
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
#elif defined(TWL)
    return v != 0 && v < 0x10000;   /* the DSi7 BIOS is 64KB */
#else
    return v != 0 && v < 0x4000;
#endif
}

/* may a register GBATEK gives no result for be stored raw? in: the inputs (n of them) */
#if defined(TWL)
static int store_ok(u32 v, const u32* in, int n)
{
    /* small values up to FFh (a byte at most; on the ARM7 that range is the BIOS
       exception vectors), an input value, or an address in RAM/IO/VRAM */
    if (v <= 0xFF) return 1;
    for (int i = 0; i < n; i++)
        if (v == in[i]) return 1;
    return v >= 0x02000000 && v < 0x07000000;
}
#else
#define store_ok(v, in, n) (!bios_range(v))
#endif

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

/* every hash starts from the platform and the test id, so equal data in two
   tests (or in DS and DSi mode) gives different hashes: only the same test
   on the two BIOSes can be compared */
#if defined(TWL)
#define HASH_PLATFORM 0x004C5754    /* "TWL" */
#else
#define HASH_PLATFORM 0x0052544E    /* "NTR" */
#endif

static u32 hash_seed(const test_t* t, u32 what)
{
    return fnv_word(fnv_word(fnv_word(0x811C9DC5, HASH_PLATFORM), t->id), what);
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
#if defined(TWL) && defined(ARM7)
    REG32(REG_IE2) = 0;
    REG32(REG_IF2) = 0xFFFFFFFF;
    REG32(ARM7_IRQ_FLAGS2) = 0;
    irq_stats[2] = 0;
#endif
}

#if defined(TWL) && defined(ARM7)
/* a real IF2 IRQ for the IntrWait tests: the AES engine (GBATEK "DSi AES I/O
   Ports") with our own key encrypts one block, started by the IRQ handler on
   the first timer 3 IRQ (aes_kick), and raises IF2 bit 12 when done */
extern volatile u32 aes_kick;
#define REG_AES_CNT   0x04004400
#define REG_AES_BLK   0x04004404
#define REG_AES_OUT   0x0400440C
#define REG_AES_IV    0x04004420
#define REG_AES_KEY0  0x04004440

static void aes_setup(void)
{
    static const u32 key[4] = { 0x5357544B, 0x45593031, 0x00112233, 0x44556677 };
    static const u32 iv[4] = { 0x49563031, 0x8899AABB, 0xCCDDEEFF, 0x01020304 };
    REG32(REG_AES_CNT) = 0;
    for (int i = 0; i < 4; i++)
    {
        REG32(REG_AES_KEY0 + i * 4) = key[i];
        REG32(REG_AES_IV + i * 4) = iv[i];
    }
    REG32(REG_AES_BLK) = 1 << 16;                                   /* one block */
    REG32(REG_AES_CNT) = (1u << 31) | (1 << 30) | (2 << 28) | (1 << 24); /* start, IRQ, CTR, key slot 0 */
    aes_kick = 1;
}

static void aes_stop(void)
{
    aes_kick = 0;
    while ((REG32(REG_AES_CNT) >> 5) & 0x1F) (void)REG32(REG_AES_OUT);   /* drop the output */
    REG32(REG_AES_CNT) = 0;
}
#endif

#if defined(TWL)
/* flags-only record (source checks, calls that get address 0 as an input):
   bit 0 output region unchanged, bits 1-4 r0-r3 unchanged, bit 5 r0 = 0,
   bit 6 r0 = 1 (F_FLAGSONLY only); no bytes, hash, registers or cycles */
static void flags_only(const test_t* t, swi_io* io, volatile u32* rec, u32 base)
{
    u32 f = 1;
    for (u32 i = 0; i < t->out_len; i += 4)
    {
        u32 w = 0;
        for (u32 b = 0; b < 4; b++)
            w |= (((i + b) & 0xFF) ^ 0xA5) << (b * 8);
        if (REG32(base + i) != w) f = 0;
    }
    for (int r = 0; r < 4; r++)
        if (io->out[r] == io->in[r]) f |= 2 << r;
    if (t->flags & F_FLAGSONLY)
    {
        if (io->out[0] == 0) f |= 0x20;
        if (io->out[0] == 1) f |= 0x40;
    }
    if (t->out_len) prefill(base, t->out_len, 0xA5);
    if (t->tmp_len) prefill(BUF_TMP, t->tmp_len, 0x3C);
    for (int r = 0; r < 4; r++) io->out[r] = 0;
    rec[W_STATUS] = ST_RETURNED | ST_HASHONLY;
    rec[W_CPSR] = io->cpsr & 0x0FFFFFFF;    /* mode/state only, no condition flags */
    rec[W_CHANGED] = io->changed;
    rec[W_EXTRA] = f;
    Hdr[HDR_COUNT] = Hdr[HDR_COUNT] + 1;
    Hdr[HDR_CURRENT] = 0xFFFFFFFF;
}
#endif

static u32 stub_num(u32 swi, int thumb)
{
    if (thumb)
        return ((u32)swi_stubs_thumb & ~1) + swi * 4 + 1;
    return (u32)swi_stubs_arm + swi * 8;
}

static u32 stub_for(const test_t* t)
{
    return stub_num(t->swi, t->flags & F_THUMB);
}

static u32 dst_base(const test_t* t)
{
    return (t->flags & F_VRAM) ? BUF_VRAM : BUF_DST;
}

static u32 reloc_v(const test_t* t, u32 v, u32 rel)
{
    switch (rel)
    {
    case R_SRC: return BUF_SRC + v;
    case R_DST: return dst_base(t) + v;
    case R_AUX: return BUF_AUX + v;
    case R_TMP: return BUF_TMP + v;
    case R_CB:  return (u32)&cb_structs[v * 5];
#if defined(TWL)
    case R_FN:  return fn_table[v];
#endif
    default:    return v;
    }
}

static u32 reloc(const test_t* t, int n)
{
    return reloc_v(t, t->in[n], t->rel[n]);
}

static void run_test(const test_t* t, volatile u32* rec)
{
    swi_io io;
    u32 st_extra = 0;   /* DSi mode: status bits 16+ (r0 after a sequence call not stored) */
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

    u32 hash = hash_seed(t, 0);
    u32 scratch = 0;
    if (t->kind == K_TABLE)
    {
        /* GBATEK table SWIs: never store what they return, only a salted hash of
           r0 (the result) and, for r1-r3, whether they stayed as passed in */
        u32 cycles = 0;
        scratch = 0xE;
        for (u32 n = 0; n < t->p[1]; n++)
        {
            io.in[0] = t->p[0] + n * t->p[2];
            run_swi(&io);
            hash = fnv_word(hash, io.out[0]);
            for (int r = 1; r < 4; r++)
                if (io.out[r] != io.in[r]) scratch &= ~(1u << r);
            cycles += io.cycles;
        }
        io.cycles = cycles;
        extra[0] = t->p[1];
    }
    else if (t->kind == K_PROCS)
    {
        /* GetBootProcs returns BIOS addresses: two calls with r2 = p[0] and p[1],
           and only these flags per register n (bits 4n+0..3): zero in both calls,
           equal to the r2 passed in both calls, the same in both calls, equal to
           the register passed in (both calls) */
        u32 o[2][4], cycles = 0, f = 0;
        for (int c = 0; c < 2; c++)
        {
            io.in[2] = t->p[c];
            run_swi(&io);
            cycles += io.cycles;
            for (int r = 0; r < 4; r++) o[c][r] = io.out[r];
        }
        for (int r = 0; r < 4; r++)
        {
            u32 in0 = r == 2 ? t->p[0] : io.in[r], in1 = r == 2 ? t->p[1] : io.in[r];
            if (!o[0][r] && !o[1][r]) f |= 1u << (r * 4);
            if (o[0][r] == t->p[0] && o[1][r] == t->p[1]) f |= 2u << (r * 4);
            if (o[0][r] == o[1][r]) f |= 4u << (r * 4);
            if (o[0][r] == in0 && o[1][r] == in1) f |= 8u << (r * 4);
            io.out[r] = 0;
        }
        io.cycles = cycles;
        extra[0] = f;
    }
#if defined(TWL)
    else if (t->kind == K_SEQ)
    {
        /* SWI sequence from the script at testblob[aux_off] (see gen_tests.py) */
        const u32* s = (const u32*)&testblob[t->aux_off];
        u32 n = s[0];
        u32 cycles = 0, calls = 0;
        for (int i = 0; i < 16; i++) cb_state[i] = 0;
        for (u32 c = 0; c < n; c++)
        {
            const u32* e = &s[1 + c * 8];
            u32 r[4];
            for (int i = 0; i < 4; i++) r[i] = reloc_v(t, e[1 + i], (e[5] >> (i * 8)) & 0xFF);
            if (e[0] & SEQ_STORE)
            {
                REG32(r[0]) = r[1];
                continue;
            }
            for (int i = 0; i < 4; i++) io.in[i] = r[i];
            io.stub = stub_num(e[0] & 0xFF, e[0] & SEQ_THUMB);
            run_swi(&io);
            cycles += io.cycles;
            if (calls < 14)
            {
                if ((e[6] & 1) || store_ok(io.out[0], io.in, 4))
                    extra[5 + calls] = io.out[0];
                else
                    st_extra |= 1u << (16 + calls);
            }
            calls++;
        }
        io.cycles = cycles;
        extra[0] = calls;
        /* SHA1 context callback field (p[1] = its offset in the output region + 1):
           BIOS addresses are stored as 0, our own callback as a marker */
        if (t->p[1])
        {
            u32 a = dst_base(t) + t->p[1] - 1;
            u32 v = REG32(a);
            if (v == 0) extra[1] = 0;
            else if (bios_range(v)) { extra[1] = 1; REG32(a) = 0; }
            else if (v == fn_table[0]) { extra[1] = 2; REG32(a) = 0x5AC0C0B0; }
            else { extra[1] = 3; REG32(a) = 0; }
        }
        extra[2] = cb_state[10];
        /* what the BIOS passed to our callback: only address-like or small values */
        if (store_ok(cb_state[12], io.in, 0)) extra[3] = cb_state[12]; else st_extra |= 1u << 30;
        if (store_ok(cb_state[11], io.in, 0)) extra[4] = cb_state[11]; else st_extra |= 1u << 31;
    }
    else if (t->kind == K_SRCCHK)
    {
        /* flags only: the source may be the BIOS */
        run_swi(&io);
        flags_only(t, &io, rec, dst_base(t));
        return;
    }
#endif
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
#if defined(TWL) && defined(ARM7)
        REG32(ARM7_IRQ_FLAGS2) = t->p[6];
        if (t->p[7] & 0x80000000) aes_setup();
        REG32(REG_IE2) = t->p[7] & 0x7FFFFFFF;
#endif
        REG32(REG_IE) = t->p[1];
        REG32(REG_IME) = t->p[2];
        run_swi(&io);
        u32 ime = REG32(REG_IME);
        REG32(REG_IME) = 0;
#if defined(TWL) && defined(ARM7)
        extra[6] = REG32(ARM7_IRQ_FLAGS2);
        extra[7] = irq_stats[2];
        if (t->p[7] & 0x80000000) aes_stop();
#endif
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

#if defined(TWL)
    if (t->flags & F_FLAGSONLY)
    {
        flags_only(t, &io, rec, dst_base(t));
        return;
    }
#endif
    rec[W_STATUS] = ST_RETURNED | st_extra;
    rec[W_CPSR] = io.cpsr;
    rec[W_CHANGED] = io.changed;
    rec[W_CYCLES] = io.cycles;
    if (t->kind == K_PROCS)
        rec[W_STATUS] |= ST_HASHONLY;   /* flags only, in extra 0 */
    else if ((t->flags & F_HASHONLY) && t->kind != K_TABLE)
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
            if (!(t->docmask & (1 << r)) && !store_ok(io.out[r], io.in, 4))
                rec[W_STATUS] |= ST_MASKED << r;
            else
                rec[W_R0 + r] = io.out[r];
        }
        u32 base = dst_base(t);
        rec[W_HASH] = fnv_bytes(hash_seed(t, 1), base, t->out_len);
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
        rec[W_TMPHASH] = fnv_bytes(hash_seed(t, 2), BUF_TMP, t->tmp_len);
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
    if (!store_ok(trap_save[2], 0, 0)) kind |= 1 << 8;
    else Hdr[HDR_TRAP_LR] = trap_save[2];
    for (int r = 0; r < 4; r++)
    {
        if (!store_ok(trap_save[3 + r], 0, 0)) kind |= 1 << (9 + r);
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

#if !defined(TWL)
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
#endif

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

#if defined(TWL)
    /* the whole results area, so nothing the direct boot left there is dumped */
    for (u32 i = 0; i < RES_SIZE / 4; i++) Hdr[i] = 0;
#else
    for (u32 i = 0; i < REC_SIZE / 4; i++) Hdr[i] = 0;
#endif
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
    {
#if !defined(TWL)
        softreset_mode(param);   /* the DSi BIOS has no SoftReset (GBATEK) */
#endif
    }
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
