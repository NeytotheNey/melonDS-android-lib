"""Generates the test table and test data of the SWI test ROM.

    python -I gen_tests.py <outdir>

Writes tests_arm9.h and tests_arm7.h (the C tables plus a data blob per
CPU) and tests.json (the manifest the report uses: names, inputs and the
GBATEK model of each result).

Test inputs come from GBATEK and the edge cases in freebios/UPGRADE_PLAN.md.
Each test runs on the CPU(s) where GBATEK lists the SWI ("BIOS Function
Summary"). Tests that may hang or overwrite memory on real hardware are
"isolated": the normal run skips them, and the run script starts one
harness run per isolated test (mode 1).

Clean room: no test reads BIOS memory into the results. The table SWIs
(GetSineTable, GetPitchTable, GetVolumeTable) and GetBootProcs are only
stored as a hash of everything they return, and no test uses a BIOS address
as a source.

Record layout (100h bytes, see src/runner.c):
  0 id, 1 status (bit 0 returned, bit 1 hash only), 2-5 r0-r3 after,
  6 CPSR after, 7 changed mask (bit n = rn changed for r4-r11, bit 13 sp,
  bit 14 lr), 8 cycles (33MHz ticks), 9 FNV-1a hash of the output region,
  10 output region length, 11 offset of the first byte that differs from
  the GBATEK model (FFFFFFFFh = same, FFFFFFFEh = no model), 12 hash of the
  temp buffer (table SWIs: hash of r1-r3, while 9 is the hash of r0),
  status bits 8-11: r0-r3 held a BIOS-range value (ARM9 FFFF0000h+, ARM7
  1-3FFFh) and were not stored (only for registers GBATEK gives no result for),
  13-31 extras (see EXTRAS), 32-47 first 64 bytes of the output
  region, 48 window offset, 49-63 60 bytes from the window offset.
"""

import json
import os
import struct
import sys

sys.dont_write_bytecode = True  # keep the source tree clean
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import encoders as E  # noqa: E402

F_THUMB, F_HASHONLY, F_VRAM, F_ISOLATED = 0x01, 0x02, 0x04, 0x08
K_REGS, K_MEM, K_CB, K_TABLE, K_IRQ = range(5)
R_RAW, R_SRC, R_DST, R_AUX, R_TMP, R_CB = range(6)
KIND_NAMES = ["regs", "mem", "callback", "table", "irq"]

EXTRAS = {
    K_REGS: ["io_after", "word_after"],
    K_MEM: [],
    K_CB: ["open_calls", "close_calls", "get8_calls", "get16_calls", "get32_calls",
           "open_r0", "open_r1", "open_r2", "close_r0"],
    K_TABLE: ["calls"],
    K_IRQ: ["flags_after", "IF_after", "IE_after", "IME_after", "irqs_taken", "irq_bits_seen"],
}

BUFS = {
    9: dict(src=0x02100000, dst=0x02110000, aux=0x02120000, tmp=0x02121000, vram=0x06800000),
    7: dict(src=0x02140000, dst=0x02150000, aux=0x02160000, tmp=0x02161000, vram=0x06000000),
}

SWI_NAMES = {
    0x00: "SoftReset", 0x03: "WaitByLoop", 0x04: "IntrWait", 0x05: "VBlankIntrWait", 0x06: "Halt",
    0x07: "Sleep", 0x08: "SoundBias", 0x09: "Div", 0x0B: "CpuSet", 0x0C: "CpuFastSet", 0x0D: "Sqrt",
    0x0E: "GetCRC16", 0x0F: "IsDebugger", 0x10: "BitUnPack", 0x11: "LZ77UnCompWram",
    0x12: "LZ77UnCompCallbackVram", 0x13: "HuffUnCompCallback", 0x14: "RLUnCompWram",
    0x15: "RLUnCompCallbackVram", 0x16: "Diff8bitUnFilterWram", 0x18: "Diff16bitUnFilter",
    0x1A: "GetSineTable", 0x1B: "GetPitchTable", 0x1C: "GetVolumeTable", 0x1D: "GetBootProcs",
    0x1F: "CustomPost/CustomHalt",
}

# GBATEK section names, cited in the manifest and the report
G_ARITH = "GBATEK BIOS Arithmetic Functions"
G_COPY = "GBATEK BIOS Memory Copy"
G_DECOMP = "GBATEK BIOS Decompression Functions"
G_CALLBACK = "GBATEK BIOS Decompression Functions (NDS/DSi Decompression Callbacks)"
G_HALT = "GBATEK BIOS Halt Functions"
G_MISC = "GBATEK BIOS Misc Functions"
G_SOUND = "GBATEK BIOS Sound Functions"
G_SUMMARY = "GBATEK BIOS Function Summary"

DST_OFS = 0x10      # the destination pointer starts this far into the output region
MARGIN = 0x20       # bytes after the expected end that are hashed too (overruns)
TMP_LEN = 0x240     # Huffman temp buffer region; r2 points 20h into it
CB_PARAM = 0x12340000

T2, T3, VB = 1 << 5, 1 << 6, 1 << 0
BOTH = (9, 7)


# result registers GBATEK documents per SWI; the ROM never masks these, and the
# report treats the other r0-r3 as scratch
DOC_REGS = {0x09: {0, 1, 3}, 0x0D: {0}, 0x0E: {0, 3}, 0x0F: {0}, 0x12: {0}, 0x13: {0}, 0x15: {0}}


def documented_regs(t):
    regs = set(DOC_REGS.get(t.swi, set()))
    if t.swi == 0x06 and not t.name.startswith("invalid"):   # Halt: all kept, NDS9 destroys r0
        regs |= {1, 2, 3} if t.cpu == 9 else {0, 1, 2, 3}
    if t.name.startswith("invalid"):
        regs = set()
    regs |= set(t.expect.get("regs", {}))
    return regs


def prefill(n, xor=0xA5):
    return bytes(((i & 0xFF) ^ xor) for i in range(n))


def round4(n):
    return (n + 3) & ~3


class Test:
    pass


TESTS = []


def add(name, cpus, swi, kind=K_REGS, ins=(), flags=0, src=b"", src_at=0, aux=b"",
        out_len=0, tmp_len=0, p=(), expect=None, isolated=False, note=""):
    """ins: up to four (reloc, value) pairs or plain values. expect: dict or
    a function of the CPU returning one, with any of regs {n: value}, out
    (bytes of the whole output region), extra {name: value}, basis, note."""
    for cpu in cpus:
        t = Test()
        t.name, t.cpu, t.swi, t.kind = name, cpu, swi, kind
        t.ins = [(i if isinstance(i, tuple) else (R_RAW, i)) for i in ins]
        while len(t.ins) < 4:
            t.ins.append((R_RAW, [0, 0, 0x22222222, 0x33333333][len(t.ins)]))
        t.flags = flags | (F_ISOLATED if isolated else 0)
        t.src, t.src_at, t.aux = src, src_at, aux
        t.out_len, t.tmp_len = out_len, tmp_len
        t.p = list(p) + [0] * (8 - len(p))
        e = expect(cpu) if callable(expect) else expect
        t.expect = dict(e) if e else {}
        t.note = note
        TESTS.append(t)


def region(out_len, dst_ofs=DST_OFS, data=None):
    r = bytearray(prefill(out_len))
    if data is not None:
        r[dst_ofs:dst_ofs + len(data)] = data
    return bytes(r)


# ---------------------------------------------------------------------------
# Div / Sqrt

def div_tests():
    cases = [(-1234, 10), (1234, -10), (-1234, -10), (1234, 10), (7, -2), (-7, 2), (-7, -2), (7, 2),
             (0, 5), (5, 7), (-1, 1), (1, -1), (100, 3), (0x7FFFFFFF, 1), (0x7FFFFFFF, -1),
             (0x80000000, 1), (0x80000000, 2), (0x80000000, -1), (0x80000000, 0x80000000),
             (1, 0x80000000), (0x7FFFFFFF, 0x80000000)]
    for n, d in cases:
        n, d = E.u32(n), E.u32(d)
        q, r, a = E.div_model(n, d)
        basis = G_ARITH + " (Div)"
        note = "INT_MIN / -1 overflows; GBATEK does not say, the model wraps" if (n, d) == (0x80000000, 0xFFFFFFFF) else ""
        add("Div %08X / %08X" % (n, d), BOTH, 0x09, ins=(n, d),
            expect=dict(regs={0: q, 1: r, 3: a}, basis=basis, note=note))
    q, r, a = E.div_model(E.u32(-1234), 10)
    add("Div -1234 / 10 (Thumb SWI)", BOTH, 0x09, ins=(E.u32(-1234), 10), flags=F_THUMB,
        expect=dict(regs={0: q, 1: r, 3: a}, basis=G_ARITH + " (Div)"))
    for n, d in [(5, 0), (-5, 0), (0, 0), (0x80000000, 0)]:
        n = E.u32(n)
        add("Div %08X / 0" % n, BOTH, 0x09, ins=(n, d), isolated=True,
            expect=dict(basis=G_ARITH + " (Div)", note="GBATEK: usually caught in an endless loop upon division by zero"))


def sqrt_tests():
    for n in [0, 1, 2, 3, 4, 15, 16, 17, 0xFFFF, 0x10000, 0x40000000, 0x7FFFFFFF, 2 << 30, 0xC0000000,
              0xFFFE0001, 0xFFFFFFFF]:
        add("Sqrt %08X" % n, BOTH, 0x0D, ins=(n,), expect=dict(regs={0: E.isqrt(n)}, basis=G_ARITH + " (Sqrt)"))
    add("Sqrt 16 (Thumb SWI)", BOTH, 0x0D, ins=(16,), flags=F_THUMB, expect=dict(regs={0: 4}, basis=G_ARITH + " (Sqrt)"))


def waitbyloop_tests():
    for n in [1, 0x10, 0x100, 0x1000]:
        add("WaitByLoop %X" % n, BOTH, 0x03, ins=(n,),
            expect=dict(regs={0: 0}, basis=G_MISC + " (WaitByLoop: SUB R0,1 / BGT loop)",
                        note="cycles are the point of this test"))
    for n, after in [(0, 0xFFFFFFFF), (0xFFFFFFFF, 0xFFFFFFFE), (0x80000000, 0x7FFFFFFF)]:
        add("WaitByLoop %08X" % n, BOTH, 0x03, ins=(n,), isolated=True,
            expect=dict(regs={0: after}, basis=G_MISC + " (WaitByLoop: SUB R0,1 / BGT loop; GBATEK range 1..7FFFFFFFh)",
                        note="outside GBATEK's range; the model runs the SUB/BGT loop once"))


# ---------------------------------------------------------------------------
# CpuSet / CpuFastSet

COPY_SRC = E.Rng(0xC0FFEE).bytes(0x1100)


def cpuset_model(out_len, r2, src_off=None, dst_off=DST_OFS, fast=False):
    """Destination region after CpuSet/CpuFastSet as GBATEK describes them.
    src_off None: the source is COPY_SRC (SRC buffer); otherwise an offset
    inside the destination region (overlap tests), read live."""
    count = r2 & 0x1FFFFF
    fill = (r2 >> 24) & 1
    size = 4 if (fast or (r2 >> 26) & 1) else 2
    reg = bytearray(prefill(out_len))

    def rd(off):
        if src_off is None:
            return COPY_SRC[off:off + size]
        return bytes(reg[src_off + off:src_off + off + size])

    if fill:
        v = rd(0)
        for k in range(count):
            reg[dst_off + k * size:dst_off + (k + 1) * size] = v
    else:
        for k in range(count):
            reg[dst_off + k * size:dst_off + (k + 1) * size] = rd(k * size)
    return bytes(reg)


def cpuset_tests():
    src = COPY_SRC
    cases = [
        ("32-bit copy 16 words", (1 << 26) | 16),
        ("32-bit copy 1 word", (1 << 26) | 1),
        ("32-bit copy 400h words", (1 << 26) | 0x400),
        ("32-bit fill 9 words", (1 << 26) | (1 << 24) | 9),
        ("16-bit copy 7 halfwords", 7),
        ("16-bit copy 1 halfword", 1),
        ("16-bit fill 5 halfwords", (1 << 24) | 5),
        ("16-bit copy 6, junk in bits 21-23/25/27-31", 0xFAE00000 | 6),
        ("32-bit copy 6, junk in bits 21-23/25/27-31", 0xFEE00000 | 6),
        ("32-bit fill 5, junk in bits 21-23/25/27-31", 0xFFE00000 | 5),
    ]
    for name, r2 in cases:
        count = r2 & 0x1FFFFF
        size = 4 if (r2 >> 26) & 1 else 2
        n = (1 if (r2 >> 24) & 1 else count) * size
        out_len = round4(DST_OFS + count * size + MARGIN)
        junk = r2 & ~0x051FFFFF
        add("CpuSet " + name, BOTH, 0x0B, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS), r2),
            src=src[:max(n, 4)], out_len=out_len,
            expect=dict(out=cpuset_model(out_len, r2), basis=G_COPY + " (CpuSet)",
                        note="GBATEK documents bits 0-20, 24, 26 only; the model ignores the rest" if junk else ""))
    # 16-bit copy from/to halfword (not word) aligned addresses
    out_len = round4(DST_OFS + 2 + 8 * 2 + MARGIN)
    reg = bytearray(prefill(out_len))
    reg[DST_OFS + 2:DST_OFS + 2 + 16] = src[2:18]
    add("CpuSet 16-bit copy 8, halfword-aligned src/dst", BOTH, 0x0B, K_MEM,
        ins=((R_SRC, 2), (R_DST, DST_OFS + 2), 8), src=src[:32], out_len=out_len,
        expect=dict(out=bytes(reg), basis=G_COPY + " (CpuSet)"))
    # overlapping copies (dst = src + one unit), source read live
    for name, r2, step in [("32-bit copy 8 words, dst = src+4", (1 << 26) | 8, 4),
                           ("16-bit copy 8 halfwords, dst = src+2", 8, 2)]:
        out_len = round4(DST_OFS + step + 8 * step + MARGIN)
        add("CpuSet " + name, BOTH, 0x0B, K_MEM, ins=((R_DST, DST_OFS), (R_DST, DST_OFS + step), r2),
            out_len=out_len,
            expect=dict(out=cpuset_model(out_len, r2, src_off=DST_OFS, dst_off=DST_OFS + step),
                        basis=G_COPY + " (CpuSet: LDMIA/STMIA or LDRH/STRH one unit at a time)"))
    for name, r2 in [("32-bit copy, count 0", 1 << 26), ("16-bit fill, count 0", 1 << 24)]:
        out_len = round4(DST_OFS + MARGIN)
        add("CpuSet " + name, BOTH, 0x0B, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS), r2), src=src[:16],
            out_len=out_len, isolated=True,
            expect=dict(basis=G_COPY + " (CpuSet)", note="count 0 is not described by GBATEK"))


def cpufastset_tests():
    src = COPY_SRC
    cases = [("copy %d words" % n, n) for n in [1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 0x400]]
    cases += [("fill %d words" % n, (1 << 24) | n) for n in [1, 7, 8, 9, 0x400]]
    cases += [("copy 10, junk in bits 21-23/25-31", 0xFEE00000 | 10),
              ("fill 10, junk in bits 21-23/25-31", 0xFFE00000 | 10)]
    for name, r2 in cases:
        count = r2 & 0x1FFFFF
        out_len = round4(DST_OFS + count * 4 + MARGIN)
        junk = r2 & ~0x011FFFFF
        add("CpuFastSet " + name, BOTH, 0x0C, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS), r2),
            src=src[:max(count * 4, 4)], out_len=out_len,
            expect=dict(out=cpuset_model(out_len, r2, fast=True),
                        basis=G_COPY + " (CpuFastSet: NDS processes the remaining words as 4-byte blocks)",
                        note="GBATEK documents bits 0-20 and 24 only; the model ignores the rest" if junk else ""))
    out_len = round4(DST_OFS + 4 + 12 * 4 + MARGIN)
    add("CpuFastSet copy 12 words, dst = src+4", BOTH, 0x0C, K_MEM, ins=((R_DST, DST_OFS), (R_DST, DST_OFS + 4), 12),
        out_len=out_len, expect=dict(basis=G_COPY + " (CpuFastSet)",
                                      note="overlap result depends on the 8-register block size; no model"))
    for name, r2 in [("copy, count 0", 0), ("fill, count 0", 1 << 24)]:
        out_len = round4(DST_OFS + MARGIN)
        add("CpuFastSet " + name, BOTH, 0x0C, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS), r2), src=src[:16],
            out_len=out_len, isolated=True,
            expect=dict(basis=G_COPY + " (CpuFastSet)", note="count 0 is not described by GBATEK"))


# ---------------------------------------------------------------------------
# BitUnPack

def bitunpack_tests():
    rng = E.Rng(0xB17)
    cases = [
        ("1->4bpp", bytes([0xAA, 0x55]), 1, 4, 0, 0),
        ("1->4bpp, offset 1 with the zero-data flag", bytes([0xAA, 0x55]), 1, 4, 1, 1),
        ("1->4bpp, offset 3", bytes([0xAA, 0x55]), 1, 4, 3, 0),
        ("2->8bpp, offset 10h", rng.bytes(4), 2, 8, 0x10, 0),
        ("4->8bpp", rng.bytes(4), 4, 8, 0, 0),
        ("8->16bpp, offset 100h", rng.bytes(4), 8, 16, 0x100, 0),
        ("1->32bpp, offset 5 with the zero-data flag", bytes([0x5A]), 1, 32, 5, 1),
        ("8->32bpp, offset 12345678h", rng.bytes(3), 8, 32, 0x12345678, 0),
        ("4->4bpp", rng.bytes(8), 4, 4, 0, 0),
        ("8->8bpp, zero-data flag with offset 0", bytes([0, 1, 0, 2, 0, 3, 0, 4]), 8, 8, 0, 1),
        ("1->2bpp, 1 byte (2 bytes unpacked, not a multiple of 4)", bytes([0x96]), 1, 2, 0, 0),
        ("1->2bpp, 3 bytes (6 bytes unpacked)", bytes([0x96, 0x3C, 0xF0]), 1, 2, 0, 0),
        ("4->4bpp, offset 8 overflows the unit", bytes([0xF1, 0x7F, 0x08, 0x80]), 4, 4, 8, 0),
    ]
    for name, data, sw, dw, off, zf in cases:
        for src_at in ([0, 1] if name == "1->4bpp" else [0]):
            info = struct.pack("<HBBI", len(data), sw, dw, off | (zf << 31))
            exp, clean = E.bitunpack_model(data, sw, dw, off, zf)
            nbits = len(data) * 8 // sw * dw
            out_len = round4(DST_OFS + max(nbits // 8, 4) + MARGIN)
            overflow = off and any(((b >> k) & ((1 << sw) - 1)) + off >= (1 << dw)
                                   for b in data for k in range(0, 8, sw))
            e = dict(basis=G_DECOMP + " (BitUnPack)")
            if clean and not overflow:
                e["out"] = region(out_len, data=exp)
            else:
                e["note"] = ("GBATEK: the unpacked size must be a multiple of 4" if not clean
                             else "GBATEK: unit plus offset should not exceed the destination width")
            add("BitUnPack " + name + (", unaligned source" if src_at else ""), BOTH, 0x10, K_MEM,
                ins=((R_SRC, src_at), (R_DST, DST_OFS), (R_AUX, 0)), src=data, src_at=src_at, aux=info,
                out_len=out_len, expect=e)
    info = struct.pack("<HBBI", 0, 1, 4, 0)
    add("BitUnPack source length 0", BOTH, 0x10, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS), (R_AUX, 0)),
        src=b"\xAA\x55", aux=info, out_len=round4(DST_OFS + MARGIN), isolated=True,
        expect=dict(basis=G_DECOMP + " (BitUnPack)", note="GBATEK lists the length as 0-FFFFh but not what 0 does"))


# ---------------------------------------------------------------------------
# LZ77

def lz77_sets():
    sets = {}
    d = E.text(300, 1)
    sets["text 300 bytes"] = (E.lz77_tokens(d), None)
    toks = [("lit", 0x41), ("ref", 5, 0), ("lit", 0x42), ("lit", 0x43), ("ref", 6, 1), ("ref", 3, 0),
            ("lit", 0x44), ("ref", 4, 2), ("lit", 0x45)]
    sets["disp 0 blocks"] = (toks, None)
    sets["odd size 37"] = (E.lz77_tokens(E.text(37, 2)), None)
    toks = E.lz77_tokens(E.text(200, 4))
    k = (len(toks) // 8) * 8 - 5           # 8n+3 tokens: the stream ends in the middle of a flag group
    sets["ends mid flag group"] = (toks[:k], None)
    toks = E.lz77_tokens(E.text(80, 5))
    j = max(i for i, t in enumerate(toks) if t[0] == "ref" and t[1] >= 5)
    toks = toks[:j + 1]
    sets["last block overshoots the size"] = (toks, "overshoot")
    big = E.Rng(77).bytes(0x1000)
    big = big + big[:0x80]                  # repeats at distance 1000h (disp FFFh)
    sets["4224 bytes, disp FFFh"] = (E.lz77_tokens(big), None)
    sets["long runs (18-byte blocks)"] = (E.lz77_tokens(b"x" * 100 + b"yz" * 40 + b"x" * 50), None)
    return sets


def lz77_decoded_len(toks):
    n = 0
    for t in toks:
        n += 1 if t[0] == "lit" else t[1]
    return n


def lz77_case(toks, kind):
    n = lz77_decoded_len(toks)
    size = n - 2 if kind == "overshoot" else n
    stream = E.lz77_stream(toks, size)
    out, clean = E.lz77_model(stream)
    return stream, size, out, clean


def lz77_tests():
    sets = lz77_sets()
    for name, (toks, kind) in sets.items():
        stream, size, out, clean = lz77_case(toks, kind)
        out_len = round4(DST_OFS + size + MARGIN)
        e = dict(basis=G_DECOMP + " (LZ77UnCompReadNormalWrite8bit)")
        if clean:
            e["out"] = region(out_len, data=out)
        else:
            e["note"] = "the last block runs past the header size; GBATEK does not say where the BIOS stops"
        add("LZ77 Wram " + name, BOTH, 0x11, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=stream,
            out_len=out_len, expect=e)
    add("LZ77 Wram size 0", BOTH, 0x11, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=E.lz77_stream([], 0),
        out_len=round4(DST_OFS + MARGIN), isolated=True,
        expect=dict(basis=G_DECOMP + " (LZ77UnCompReadNormalWrite8bit)", note="size 0 is not described by GBATEK"))

    # SWI 12h: read by callback, write 16-bit units
    runs = [("text 300 bytes", 0, 0), ("text 300 bytes", 0, F_VRAM), ("odd size 37", 0, 0),
            ("odd size 37", 0, F_VRAM), ("disp 0 blocks", 0, 0), ("ends mid flag group", 0, F_VRAM),
            ("last block overshoots the size", 0, 0), ("4224 bytes, disp FFFh", 0, F_VRAM),
            ("long runs (18-byte blocks)", 0, 0)]
    runs += [("text 300 bytes", cb, 0) for cb in (1, 2, 3, 4, 5)]
    for name, cb, vram in runs:
        toks, kind = sets[name]
        stream, size, out, clean = lz77_case(toks, kind)
        add_callback_test("LZ77 Callback16", 0x12, name, cb, vram, stream, size, out, clean,
                          disp0=E.lz77_has_disp0(toks))
    add("LZ77 Callback16 size 0", BOTH, 0x12, K_CB,
        ins=((R_SRC, 0), (R_DST, DST_OFS), CB_PARAM, (R_CB, 0)), src=E.lz77_stream([], 0),
        out_len=round4(DST_OFS + MARGIN), isolated=True,
        expect=dict(basis=G_CALLBACK, note="size 0 is not described by GBATEK"))


CB_NAMES = {0: "", 1: ", no Close callback", 2: ", Open returns -3", 3: ", Close returns -1",
            4: ", Thumb callbacks", 5: ", Open returns size 8"}


def add_callback_test(prefix, swi, name, cb, vram, stream, size, out, clean, disp0=False, huff=False):
    out_len = round4(DST_OFS + max(size, 8) + MARGIN)
    if huff:
        out_len = round4(DST_OFS + round4(size) + MARGIN)
    e = dict(basis=G_CALLBACK, extra={"open_calls": 1})
    r0 = size
    data = out
    if cb == 2:
        r0, data = 0xFFFFFFFD, b""
    elif cb == 3:
        r0 = 0xFFFFFFFF
    elif cb == 5:
        r0 = 8
        if swi == 0x13:
            data, ok = E.huff_model(stream[:1] + bytes([8, 0, 0]) + stream[4:])
            clean = clean and ok
        elif swi == 0x12:
            data, ok = E.lz77_model(stream[:1] + bytes([8, 0, 0]) + stream[4:])
            clean = clean and ok
        else:
            data, ok = E.rle_model(stream[:1] + bytes([8, 0, 0]) + stream[4:])
            clean = clean and ok
    e["regs"] = {0: r0}
    notes = []
    if not clean:
        notes.append("the output size is not covered by GBATEK (overshoot or partial word)")
    if swi in (0x12, 0x15) and size % 2 and cb != 2:
        notes.append("odd size with 16-bit writes: GBATEK does not say how the last byte is written")
        clean = False
    if disp0:
        notes.append("GBATEK: the Vram function does not work with disp 0")
        clean = False
    if clean or cb == 2:
        e["out"] = region(out_len, data=data)
    if notes:
        e["note"] = "; ".join(notes)
    ins = [(R_SRC, 0), (R_DST, DST_OFS), CB_PARAM, (R_CB, cb)]
    tmp = 0
    if huff:
        ins[2] = (R_TMP, 0x20)
        tmp = TMP_LEN
    add("%s %s%s%s" % (prefix, name, CB_NAMES[cb], ", VRAM destination" if vram else ""), BOTH, swi, K_CB,
        ins=tuple(ins), flags=vram, src=stream, out_len=out_len, tmp_len=tmp, expect=e)


# ---------------------------------------------------------------------------
# Huffman

def fib_units(nsym, base=0):
    a, b = 1, 1
    units = []
    for k in range(nsym):
        units += [base + k] * a
        a, b = b, a + b
    return units


def huff_tests():
    sets = []
    d = E.text(200, 3)
    sets.append(("8-bit text 200 bytes", 8, E.huff_units(d, 8), 200, None))
    d = E.Rng(44).bytes(64)
    sets.append(("4-bit 64 bytes", 4, E.huff_units(d, 4), 64, None))
    d = E.text(40, 6)
    sets.append(("8-bit size 37, stream padded to 40", 8, E.huff_units(d, 8), 37, None))
    sets.append(("8-bit size 37, no padding symbols", 8, E.huff_units(d[:37], 8), 37, None))
    d = E.Rng(45).bytes(5) + b"\0\0\0"
    sets.append(("4-bit size 5", 4, E.huff_units(d, 4), 5, None))
    sets.append(("8-bit single-symbol tree", 8, [0x5A] * 64, 64, None))
    tree = fib_units(24, 0x20)
    data = [0x20 + (k % 24) for k in range(256)]
    sets.append(("8-bit deep tree (24 symbols, Fibonacci weights)", 8, data, 256, tree))
    tree = fib_units(16)
    data = [k % 16 for k in range(128)]
    sets.append(("4-bit deep tree (16 symbols, Fibonacci weights)", 4, data, 64, tree))
    data = list(E.Rng(46).bytes(256, 64))
    sets.append(("8-bit 64 symbols", 8, data, 256, None))
    built = {}
    for name, bits, units, size, tree in sets:
        stream, depth = E.huff_stream(units, bits, size, tree)
        out, clean = E.huff_model(stream)
        built[name] = (stream, size, out[:round4(size)], clean, depth)
    runs = [(n, 0, 0) for n, *_ in sets] + [("8-bit text 200 bytes", 0, F_VRAM)]
    runs += [("8-bit text 200 bytes", cb, 0) for cb in (1, 2, 3, 4, 5)]
    for name, cb, vram in runs:
        stream, size, out, clean, depth = built[name]
        add_callback_test("Huffman", 0x13, "%s (max code %d bits)" % (name, depth) if cb == 0 and not vram else name,
                          cb, vram, stream, size, out, clean, huff=True)
    stream, _ = E.huff_stream([1, 2, 3, 4], 8, 0)
    add("Huffman size 0", BOTH, 0x13, K_CB, ins=((R_SRC, 0), (R_DST, DST_OFS), (R_TMP, 0x20), (R_CB, 0)),
        src=stream, out_len=round4(DST_OFS + MARGIN), tmp_len=TMP_LEN, isolated=True,
        expect=dict(basis=G_CALLBACK, note="size 0 is not described by GBATEK"))


# ---------------------------------------------------------------------------
# RLE

def rle_sets():
    rng = E.Rng(0x41E)
    d = bytearray()
    while len(d) < 200:
        if rng.next() & 1:
            d += bytes([rng.next() & 0xFF]) * (3 + rng.next() % 12)
        else:
            d += rng.bytes(1 + rng.next() % 6)
    d = bytes(d[:200])
    sets = {"mixed 200 bytes": (E.rle_tokens(d), len(d))}
    sets["run of 10 with size 5"] = ([("run", 10, 0x58)], 5)
    sets["literal block of 8 with size 5"] = ([("lit", b"ABCDEFGH")], 5)
    sets["odd size 33"] = (E.rle_tokens(d[:33]), 33)
    mx = b"r" * 130 + bytes(range(0x30, 0x30 + 128)) + b"ssss"   # even size, so only the lengths are tested
    sets["max run 130, max literal 128"] = ([("run", 130, ord("r")), ("lit", mx[130:258]), ("run", 4, ord("s"))], len(mx))
    return sets


def rle_tests():
    sets = rle_sets()
    for name, (toks, size) in sets.items():
        stream = E.rle_stream(toks, size)
        out, clean = E.rle_model(stream)
        out_len = round4(DST_OFS + size + MARGIN)
        e = dict(basis=G_DECOMP + " (RLUnCompReadNormalWrite8bit)")
        if clean:
            e["out"] = region(out_len, data=out)
        else:
            e["note"] = "the last block runs past the header size; GBATEK does not say where the BIOS stops"
        add("RLE Wram " + name, BOTH, 0x14, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=stream,
            out_len=out_len, expect=e)
    add("RLE Wram size 0", BOTH, 0x14, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=E.rle_stream([], 0),
        out_len=round4(DST_OFS + MARGIN), isolated=True,
        expect=dict(basis=G_DECOMP + " (RLUnCompReadNormalWrite8bit)", note="size 0 is not described by GBATEK"))

    runs = [("mixed 200 bytes", 0, 0, 0), ("odd size 33", 0, 0, 0), ("run of 10 with size 5", 0, 0, 0),
            ("literal block of 8 with size 5", 0, 0, 0), ("max run 130, max literal 128", 0, 0, 0),
            ("mixed 200 bytes", 0, F_VRAM, 0), ("odd size 33", 0, F_VRAM, 0),
            ("mixed 200 bytes", 0, F_VRAM, 1), ("odd size 33", 0, F_VRAM, 1), ("mixed 200 bytes", 0, 0, 1)]
    runs += [("mixed 200 bytes", cb, 0, 0) for cb in (1, 2, 3, 4, 5)]
    for name, cb, vram, odd in runs:
        toks, size = sets[name]
        stream = E.rle_stream(toks, size)
        out, clean = E.rle_model(stream)
        if odd:
            # destination at an odd address: GBATEK says it must be halfword aligned
            out_len = round4(DST_OFS + 1 + size + MARGIN)
            e = dict(basis=G_CALLBACK + "; RLUnCompReadByCallbackWrite16bit needs a halfword-aligned destination",
                     regs={0: size}, extra={"open_calls": 1},
                     note="odd destination is outside GBATEK's rules; no output model")
            add("RLE Callback16 %s, odd destination%s" % (name, ", VRAM destination" if vram else ""), BOTH, 0x15, K_CB,
                ins=((R_SRC, 0), (R_DST, DST_OFS + 1), CB_PARAM, (R_CB, 0)), flags=vram, src=stream,
                out_len=out_len, expect=e)
            continue
        add_callback_test("RLE Callback16", 0x15, name, cb, vram, stream, size, out, clean)
    add("RLE Callback16 size 0", BOTH, 0x15, K_CB, ins=((R_SRC, 0), (R_DST, DST_OFS), CB_PARAM, (R_CB, 0)),
        src=E.rle_stream([], 0), out_len=round4(DST_OFS + MARGIN), isolated=True,
        expect=dict(basis=G_CALLBACK, note="size 0 is not described by GBATEK"))


# ---------------------------------------------------------------------------
# Diff filters (ARM9 only)

def diff_tests():
    d8 = bytes(((i * 37 + 250) & 0xFF) for i in range(64))
    d16 = b"".join(struct.pack("<H", (i * 0x3001 + 0xFF00) & 0xFFFF) for i in range(32))
    cases = [("Diff8 64 bytes with wrap-around", 0x16, d8, 1, None),
             ("Diff8 odd size 13", 0x16, d8[:13], 1, None),
             ("Diff16 32 halfwords with wrap-around", 0x18, d16, 2, None),
             ("Diff16 size 7 (not a multiple of 2)", 0x18, d16[:8], 2, 7)]
    for name, swi, data, unit, size in cases:
        stream = E.diff_stream(data, unit, size)
        size = len(data) if size is None else size
        out_len = round4(DST_OFS + size + MARGIN)
        e = dict(basis=G_DECOMP + " (Diff8bitUnFilter / Diff16bitUnFilter)")
        if unit == 2 and size % 2:
            e["note"] = "size is not a multiple of the unit size; no model"
        else:
            e["out"] = region(out_len, data=data[:size])
        add(name, (9,), swi, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=stream, out_len=out_len, expect=e)
    for name, swi, unit in [("Diff8 size 0", 0x16, 1), ("Diff16 size 0", 0x18, 2)]:
        add(name, (9,), swi, K_MEM, ins=((R_SRC, 0), (R_DST, DST_OFS)), src=E.diff_stream(b"", unit, 0),
            out_len=round4(DST_OFS + MARGIN), isolated=True,
            expect=dict(basis=G_DECOMP, note="size 0 is not described by GBATEK"))


# ---------------------------------------------------------------------------
# GetCRC16 (RAM only)

def crc_tests():
    data = E.Rng(0xC12C).bytes(0x100)
    for seed, at, n in [(0xFFFF, 0, 0), (0xFFFF, 0, 2), (0xFFFF, 0, 7), (0xFFFF, 0, 1), (0xFFFF, 0, 0x100),
                        (0x0000, 0, 0x100), (0x12345678, 0, 0x10), (0xFFFF, 1, 8)]:
        e = dict(basis=G_MISC + " (GetCRC16)")
        if n % 2 == 0 and at % 2 == 0:
            regs = {0: E.crc16_model(seed, data[at:at + n])}
            if n:
                regs[3] = struct.unpack_from("<H", data, at + n - 2)[0]
            e["regs"] = regs
        else:
            e["note"] = "GBATEK: start and length must be aligned by 2"
        add("GetCRC16 seed %X, %s, length %d" % (seed, "odd start" if at else "aligned", n), BOTH, 0x0E, K_MEM,
            ins=(seed, (R_SRC, at), n), src=data, expect=e)


# ---------------------------------------------------------------------------
# small misc SWIs

def misc_tests():
    # isolated, so the ARM9 and ARM7 calls never touch the scratch word at the same time.
    # word_after is the word at 27FFFF8h: GBATEK says the NDS9 destroys the halfword at
    # 27FFFF8h (low half) and the NDS7 the one at 27FFFFAh (high half); the model checks
    # that the other half is kept and the destroyed half changed.
    for cpu, half, keep in [(9, 0x027FFFF8, 0xFFFF0000), (7, 0x027FFFFA, 0x0000FFFF)]:
        add("IsDebugger", (cpu,), 0x0F, K_REGS, ins=(), p=[0x027FFFF8, 0x12345678, 4, 0, 0, 0x027FFFF8],
            isolated=True,
            expect=dict(regs={0: 0}, extra_changed={"word_after": [keep, 0x12345678]},
                        basis=G_MISC + " (IsDebugger: 0 = 4MB console; destroys the halfword at %07Xh)" % (half & 0xFFFFFFF)))
    for r0 in [1, 3, 0]:
        # POSTFLG is set to 1 before each call (bit 0 is already set by the direct boot and
        # cannot be cleared), so only bit 1 tells anything and the order does not matter
        add("CustomPost r0=%d" % r0, (9,), 0x1F, K_REGS, ins=(r0,), p=[0x04000300, 1, 1, 0x04000300, 1],
            expect=dict(basis=G_MISC + " (CustomPost writes r0 to POSTFLG)",
                        note="POSTFLG bit 0 is set before the call and stays set; only bit 1 can differ"))
    for r0, r1, pre, after in [(1, 8, 0x000, 0x200), (0, 8, 0x200, 0x000), (1, 8, 0x100, 0x200),
                               (1, 8, 0x200, 0x200), (0x80, 8, 0x000, 0x200), (0, 8, 0x000, 0x000)]:
        add("SoundBias r0=%X r1=%X from %03X" % (r0, r1, pre), (7,), 0x08, K_REGS, ins=(r0, r1),
            p=[0x04000504, pre, 2, 0x04000504, 2],
            expect=dict(extra={"io_after": after}, basis=G_SOUND + " (SoundBias)"))
    add("SoundBias r0=1 r1=0 from 000", (7,), 0x08, K_REGS, ins=(1, 0), p=[0x04000504, 0, 2, 0x04000504, 2],
        isolated=True, expect=dict(extra={"io_after": 0x200}, basis=G_SOUND + " (SoundBias)",
                                   note="delay 0 is not described by GBATEK"))
    for r2 in [0, 0xDEADBEEF]:
        add("GetBootProcs r2=%X (hash only)" % r2, (7,), 0x1D, K_REGS, ins=(0, 0, r2), flags=F_HASHONLY,
            expect=dict(basis=G_MISC + " (GetBootProcs returns BIOS addresses: compared by hash only)"))


def table_tests():
    for swi, name, n in [(0x1A, "GetSineTable", 0x40), (0x1B, "GetPitchTable", 0x300), (0x1C, "GetVolumeTable", 0x2D4)]:
        for label, start, count, step in [("all %d entries" % n, 0, n, 1), ("16 past the end", n, 16, 1),
                                          ("-16..-1", 0xFFFFFFF0, 16, 1), ("far indices", 0x1000, 8, 0x800)]:
            add("%s %s (hash only)" % (name, label), (7,), swi, K_TABLE, flags=F_HASHONLY, p=[start, count, step],
                expect=dict(basis=G_MISC + " (%s; table SWIs are compared by hash only)" % name))


# ---------------------------------------------------------------------------
# IntrWait / VBlankIntrWait / Halt / CustomHalt

def irq_tests():
    def iw(name, r0, r1, flags, ie, ime, d2, d3, expect, thumb=False, cpus=BOTH):
        add("IntrWait " + name + (" (Thumb SWI)" if thumb else ""), cpus, 0x04, K_IRQ, ins=(r0, r1),
            flags=F_THUMB if thumb else 0, p=[flags, ie, ime, d2, d3, 0], expect=expect)

    def ex(cpu, arm7, arm9, note=""):
        e = dict(basis=G_HALT + " (IntrWait)", extra=dict(arm7 if cpu == 7 else arm9))
        if note:
            e["note"] = note
        return e

    iw("r0=0, wanted flag already set", 0, T2, T2, T2, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 0, "flags_after": 0, "IME_after": 1}, {"irqs_taken": 1, "flags_after": 0, "IME_after": 1},
                    "GBATEK: NDS9 always waits for at least one IRQ even if the flag is set; NDS7 returns at once"))
    iw("r0=0, flag not set", 0, T2, 0, T2, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 1, "flags_after": 0}, {"irqs_taken": 1, "flags_after": 0}))
    iw("r0=1, wanted flag already set (discarded)", 1, T2, T2, T2, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 1, "flags_after": 0}, {"irqs_taken": 1, "flags_after": 0}))
    iw("r0=0, another flag pending", 0, T2, T3, T2, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 1, "flags_after": T3}, {"irqs_taken": 1, "flags_after": T3}))
    iw("r0=0, one of two wanted flags set", 0, T2 | T3, T3, T2 | T3, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 0, "flags_after": 0}, {"irqs_taken": 1, "flags_after": 0},
                    "GBATEK: NDS9 waits for one IRQ first"))
    iw("r0=1, unwanted IRQs arrive first", 1, T2, 0, T2 | T3, 1, 0x100, 0x30,
       lambda c: ex(c, {"flags_after": T3}, {"flags_after": T3},
                    "timer 3 fires every 30h ticks, timer 2 after 100h; T3 is not wanted, so its flag stays"))
    iw("r0=0, flag set, an unwanted IRQ arrives first", 0, T2, T2, T2 | T3, 1, 0x100, 0x30,
       lambda c: ex(c, {"irqs_taken": 0, "flags_after": 0}, {"irqs_taken": 1},
                    "GBATEK: NDS9 waits for at least one IRQ, including IRQs not selected in r1"))
    iw("r0=1 with IME=0 before the call", 1, T2, 0, T2, 0, 0x100, 0,
       lambda c: ex(c, {"IME_after": 1, "irqs_taken": 1}, {"IME_after": 1, "irqs_taken": 1},
                    "GBATEK: IntrWait forcefully sets IME=1"))
    iw("r0=0, flag not set", 0, T2, 0, T2, 1, 0x100, 0,
       lambda c: ex(c, {"irqs_taken": 1, "flags_after": 0}, {"irqs_taken": 1, "flags_after": 0}), thumb=True)

    def vb(name, r0, r1, flags, ie, d2, extra):
        add("VBlankIntrWait " + name, BOTH, 0x05, K_IRQ, ins=(r0, r1), p=[flags, ie, 1, d2, 0, 1],
            expect=dict(basis=G_HALT + " (VBlankIntrWait: no parameters, no return value; it sets r0=1, r1=1"
                        " and runs IntrWait, so r0/r1 after are not documented results)", extra=extra))
    vb("basic", 0x12345678, 0x9ABCDEF0, 0, VB, 0, {"flags_after": 0})
    vb("VBlank flag already set (discarded)", 0, 0, VB, VB, 0, {"flags_after": 0})
    vb("timer 2 IRQs while waiting", 0, 0, 0, VB | T2, 0x100, {"flags_after": T2})

    def halt(name, cpus, ime, isolated=False, note=""):
        add("Halt " + name, cpus, 0x06, K_IRQ, ins=(0x12345678, 0x11111111), p=[0, T2, ime, 0x100, 0, 0],
            isolated=isolated, expect=lambda c: dict(
                basis=G_HALT + " (Halt: NDS9 destroys r0; the CP15 halt hangs if IME=0)",
                regs={} if c == 9 else {0: 0x12345678, 1: 0x11111111}, note=note))
    halt("IME=1", BOTH, 1)
    halt("IME=0", (7,), 0, note="GBATEK: Halt passes through with IME=0 (IRQ not taken, IF stays set)")
    halt("IME=0", (9,), 0, isolated=True, note="GBATEK: on NDS9 the CP15 halt opcode hangs if IME=0")

    for r2, name in [(0x80, "r2=80h (halt)"), (0x00, "r2=00h")]:
        # isolated: a wrong HALTCNT value (40h = GBA mode) stops the emulator
        add("CustomHalt " + name, (7,), 0x1F, K_IRQ, ins=(0x12345678, 0x11111111, r2), p=[0, T2, 1, 0x100, 0, 0],
            isolated=True, expect=dict(basis=G_HALT + " (CustomHalt writes the 8-bit r2 to HALTCNT)"))


# ---------------------------------------------------------------------------
# SWI numbers GBATEK lists as invalid (isolated: they jump to 0 or crash)

def invalid_tests():
    lists = {9: [0x01, 0x02, 0x07, 0x08, 0x0A, 0x17, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0x2A, 0x3F, 0x49, 0xFF],
             7: [0x01, 0x02, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1E, 0x20, 0x2A, 0x3F, 0x49, 0xFF]}
    for cpu, nums in lists.items():
        for n in nums:
            note = ("GBATEK: SWI 20h+ crash (jump to garbage addresses); the 40h+ mirrors are DSi only"
                    if n >= 0x20 else "GBATEK: invalid NDS SWIs jump to 0")
            add("invalid SWI %02Xh" % n, (cpu,), n, K_REGS, ins=(100, 7), isolated=True,
                expect=dict(basis=G_SUMMARY + " (Invalid Functions)", note=note))


SOFTRESET = [
    dict(variant=0, cpu=9, desc="ARM9, DTCM at 03000000h (direct boot default), ARM entry"),
    dict(variant=1, cpu=9, desc="ARM9, DTCM at 00800000h (GBATEK default, ITCM shrunk to 32KB), ARM entry"),
    dict(variant=2, cpu=9, desc="ARM9, DTCM at 027E0000h (moved, like SDK games), ARM entry"),
    dict(variant=3, cpu=9, desc="ARM9, DTCM at 0B000000h (moved, like libnds), ARM entry"),
    dict(variant=4, cpu=9, desc="ARM9, DTCM at 03000000h, Thumb entry (bit 0 set)"),
    dict(variant=5, cpu=7, desc="ARM7, ARM entry"),
    dict(variant=6, cpu=7, desc="ARM7, Thumb entry (bit 0 set)"),
]


# ---------------------------------------------------------------------------
# output

def build():
    div_tests()
    sqrt_tests()
    waitbyloop_tests()
    cpuset_tests()
    cpufastset_tests()
    bitunpack_tests()
    lz77_tests()
    huff_tests()
    rle_tests()
    diff_tests()
    crc_tests()
    misc_tests()
    table_tests()
    irq_tests()
    invalid_tests()
    for n, t in enumerate(TESTS):
        t.id = n + 1


class Blob:
    def __init__(self):
        self.data = bytearray()
        self.index = {}

    def put(self, b):
        b = bytes(b)
        if not b:
            return 0
        if b in self.index:
            return self.index[b]
        while len(self.data) % 4:
            self.data.append(0)
        off = len(self.data)
        self.data += b
        self.index[b] = off
        return off


def c_table(cpu):
    blob = Blob()
    rows = []
    for t in TESTS:
        if t.cpu != cpu:
            continue
        src_off = blob.put(t.src)
        aux_off = blob.put(t.aux)
        exp = t.expect.get("out")
        if exp is not None:
            assert len(exp) == t.out_len, t.name
            exp_off = blob.put(exp)
        else:
            exp_off = 0xFFFFFFFF
        ins = ", ".join("0x%08X" % (v & 0xFFFFFFFF) for _, v in t.ins)
        rel = ", ".join(str(r) for r, _ in t.ins)
        p = ", ".join("0x%08X" % (v & 0xFFFFFFFF) for v in t.p)
        docmask = sum(1 << r for r in documented_regs(t))
        rows.append("    { %d, 0x%02X, %d, 0x%02X, 0x%X, { %s }, { %s }, 0x%X, 0x%X, 0x%X, 0x%X, 0x%X, 0x%X, 0x%X, 0x%08X, { %s } }, /* %s */"
                    % (t.id, t.swi, t.kind, t.flags, docmask, ins, rel, src_off, len(t.src), t.src_at, aux_off, len(t.aux),
                       t.out_len, t.tmp_len, exp_off, p, t.name.replace("*/", "")))
    lines = ["/* generated by gen_tests.py, do not edit */", "#define NUM_TESTS %d" % len(rows),
             "static const u8 testblob[] __attribute__((aligned(4))) = {"]
    d = blob.data
    for i in range(0, len(d), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in d[i:i + 16]) + ",")
    if not d:
        lines.append("    0")
    lines.append("};")
    lines.append("static const test_t tests[] = {")
    lines += rows
    lines.append("};")
    return "\n".join(lines) + "\n"


def manifest():
    out = []
    for t in TESTS:
        bufs = BUFS[t.cpu]
        ins = []
        for r, v in t.ins:
            if r == R_SRC:
                ins.append(bufs["src"] + v)
            elif r == R_DST:
                ins.append((bufs["vram"] if t.flags & F_VRAM else bufs["dst"]) + v)
            elif r == R_AUX:
                ins.append(bufs["aux"] + v)
            elif r == R_TMP:
                ins.append(bufs["tmp"] + v)
            elif r == R_CB:
                ins.append("callbacks %d" % v)
            else:
                ins.append(v & 0xFFFFFFFF)
        e = dict(t.expect)
        if "out" in e:
            e["out"] = e["out"].hex()
        if "regs" in e:
            e["regs"] = {str(k): v for k, v in e["regs"].items()}
        out.append(dict(id=t.id, cpu=t.cpu, name=t.name, swi=t.swi, swi_name="invalid" if t.name.startswith("invalid SWI") else SWI_NAMES.get(t.swi, "invalid"),
                        kind=KIND_NAMES[t.kind], flags=t.flags, isolated=bool(t.flags & F_ISOLATED),
                        thumb=bool(t.flags & F_THUMB), hash_only=bool(t.flags & F_HASHONLY),
                        vram=bool(t.flags & F_VRAM), inputs=ins, out_len=t.out_len, dst_ofs=DST_OFS,
                        extras=EXTRAS[t.kind], expect=e, note=t.note,
                        doc_regs=sorted(documented_regs(t))))
    return dict(version=1, tests=out, softreset=SOFTRESET)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    build()
    for cpu in (9, 7):
        with open(os.path.join(outdir, "tests_arm%d.h" % cpu), "w", newline="\n") as f:
            f.write(c_table(cpu))
    with open(os.path.join(outdir, "tests.json"), "w", newline="\n") as f:
        json.dump(manifest(), f, indent=1)
    n9 = sum(1 for t in TESTS if t.cpu == 9)
    n7 = len(TESTS) - n9
    iso = sum(1 for t in TESTS if t.flags & F_ISOLATED)
    print("tests: %d ARM9, %d ARM7 (%d isolated), %d SoftReset variants" % (n9, n7, iso, len(SOFTRESET)))


if __name__ == "__main__":
    main()
