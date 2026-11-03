"""Encoders and reference models for the SWI test ROM.

Everything here is written from GBATEK ("BIOS Decompression Functions",
"BIOS Memory Copy", "BIOS Arithmetic Functions", "BIOS Misc Functions"):
the encoders produce GBATEK-format streams, and the models compute what
GBATEK says the BIOS returns. The models are what the report calls "the
GBATEK model"; where GBATEK leaves the result open they return None.
"""

import heapq
import itertools
import math
import struct
from collections import Counter, deque


def u32(x):
    return x & 0xFFFFFFFF


def pad4(b):
    b = bytearray(b)
    while len(b) % 4:
        b.append(0)
    return bytes(b)


class Rng:
    """Small deterministic LCG, so the test data never depends on Python's random."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0xFFFFFFFF
        return self.s >> 16

    def bytes(self, n, mod=256):
        return bytes(self.next() % mod for _ in range(n))


WORDS = ("sun moon tree leaf rock river cloud stone bird road hill lamp door "
         "window paper music garden yellow seven quiet basket").split()


def text(n, seed):
    rng = Rng(seed)
    out = bytearray()
    while len(out) < n:
        out += WORDS[rng.next() % len(WORDS)].encode() + b" "
    return bytes(out[:n])


# ---------------------------------------------------------------------------
# LZ77 (type 1): header, then flag bytes (MSB first) each followed by 8 blocks.
# Literal: 1 byte. Reference: 2 bytes, ((len-3) << 4 | disp >> 8), disp & FFh,
# copies len bytes from dest - disp - 1.

def lz77_tokens(data, allow_disp0=False, max_cands=512):
    """Greedy token list for data: ('lit', b) or ('ref', length, disp).
    Matches may overlap the current position, as GBATEK's byte-by-byte copy
    allows. disp 0 (copy from dest-1) is only used when allowed."""
    toks = []
    chains = {}
    lo = 0 if allow_disp0 else 1

    def insert(pos):
        if pos + 3 <= len(data):
            chains.setdefault(data[pos:pos + 3], []).append(pos)

    i = 0
    while i < len(data):
        best_len, best_disp = 0, 0
        for p in reversed(chains.get(data[i:i + 3], [])[-max_cands:]):
            dist = i - p
            if dist > 0x1000:
                break
            if dist - 1 < lo:
                continue
            n = 0
            while n < 18 and i + n < len(data) and data[i + n] == data[i + n - dist]:
                n += 1
            if n > best_len:
                best_len, best_disp = n, dist - 1
                if n == 18:
                    break
        if best_len >= 3:
            toks.append(("ref", best_len, best_disp))
            for k in range(best_len):
                insert(i + k)
            i += best_len
        else:
            toks.append(("lit", data[i]))
            insert(i)
            i += 1
    return toks


def lz77_stream(tokens, size):
    out = bytearray(struct.pack("<I", (size << 8) | 0x10))
    for g in range(0, len(tokens), 8):
        group = tokens[g:g + 8]
        flags = 0
        body = bytearray()
        for k, t in enumerate(group):
            if t[0] == "ref":
                _, n, disp = t
                assert 3 <= n <= 18 and 0 <= disp <= 0xFFF
                flags |= 0x80 >> k
                body += bytes([((n - 3) << 4) | (disp >> 8), disp & 0xFF])
            else:
                body.append(t[1])
        out.append(flags)
        out += body
    return pad4(out)


def lz77_model(stream):
    """GBATEK LZ77 decode; returns (bytes, clean). clean is False when a block
    runs past the header size (GBATEK does not say where the BIOS stops)."""
    size = struct.unpack_from("<I", stream)[0] >> 8
    out = bytearray()
    i = 4
    while len(out) < size:
        flags = stream[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0, b1 = stream[i], stream[i + 1]
                i += 2
                n = (b0 >> 4) + 3
                disp = ((b0 & 0xF) << 8) | b1
                for _ in range(n):
                    out.append(out[-disp - 1])
            else:
                out.append(stream[i])
                i += 1
    return bytes(out[:size]), len(out) == size


def lz77_has_disp0(tokens):
    return any(t[0] == "ref" and t[2] == 0 for t in tokens)


# ---------------------------------------------------------------------------
# Huffman (type 2): header (data size 4/8 in bits 0-3), tree size byte, tree
# table (root at tree+1; children of the node at A are at (A & ~1) + off*2 + 2
# and +1; bit 7/6 = child 0/1 is data), bitstream as 32-bit words, bit 31 first.

class _Leaf:
    def __init__(self, sym):
        self.sym = sym


class _Node:
    def __init__(self, a, b):
        self.kids = (a, b)


def _huff_tree(units):
    freq = Counter(units)
    if len(freq) == 1:
        s = next(iter(freq))
        return _Node(_Leaf(s), _Leaf(s))
    tie = itertools.count()
    heap = [(f, next(tie), _Leaf(s)) for s, f in sorted(freq.items())]
    heapq.heapify(heap)
    while len(heap) > 1:
        fa, _, a = heapq.heappop(heap)
        fb, _, b = heapq.heappop(heap)
        heapq.heappush(heap, (fa + fb, next(tie), _Node(a, b)))
    return heap[0][2]


def _huff_codes(node, prefix="", codes=None):
    if codes is None:
        codes = {}
    if isinstance(node, _Leaf):
        codes.setdefault(node.sym, prefix or "0")
    else:
        _huff_codes(node.kids[0], prefix + "0", codes)
        _huff_codes(node.kids[1], prefix + "1", codes)
    return codes


def huff_units(data, bits):
    if bits == 8:
        return list(data)
    units = []
    for b in data:
        units += [b & 0xF, b >> 4]
    return units


def huff_stream(units, bits, size, tree_units=None):
    """units: the symbols to encode, in output order. size: header size in
    bytes. tree_units: symbols to build the tree from (default: units)."""
    root = _huff_tree(tree_units if tree_units is not None else units)
    codes = _huff_codes(root)
    table = bytearray([0, 0])  # [0] tree size byte, [1] root
    queue = deque([(root, 1)])
    next_pair = 2
    while queue:
        node, idx = queue.popleft()
        pair = next_pair
        next_pair += 2
        while len(table) < pair + 2:
            table.append(0)
        off = (pair - (idx & ~1) - 2) // 2
        if not 0 <= off <= 63:
            raise ValueError("huffman tree too wide for 6-bit offsets")
        val = off
        for c, kid in enumerate(node.kids):
            if isinstance(kid, _Leaf):
                table[pair + c] = kid.sym
                val |= 0x80 if c == 0 else 0x40
            else:
                queue.append((kid, pair + c))
        table[idx] = val
    while len(table) % 4:
        table.append(0)
    table[0] = len(table) // 2 - 1
    bitstr = "".join(codes[u] for u in units)
    while len(bitstr) % 32:
        bitstr += "0"
    words = b"".join(struct.pack("<I", int(bitstr[i:i + 32], 2)) for i in range(0, len(bitstr), 32))
    hdr = struct.pack("<I", (size << 8) | 0x20 | bits)
    depth = max(len(c) for c in codes.values())
    return hdr + bytes(table) + words, depth


def huff_model(stream):
    """GBATEK Huffman decode. Output is collected LSB first into 32-bit words
    and written a word at a time. Returns (bytes, clean); clean is False for
    sizes that are not a multiple of 4 (GBATEK only says to pad)."""
    hdr = struct.unpack_from("<I", stream)[0]
    bits = hdr & 0xF
    size = hdr >> 8
    T = 4
    ts = stream[T]
    pos = T + (ts + 1) * 2
    out = bytearray()
    acc = 0
    nacc = 0
    A = T + 1
    while len(out) < size:
        word = struct.unpack_from("<I", stream, pos)[0]
        pos += 4
        for k in range(31, -1, -1):
            bit = (word >> k) & 1
            val = stream[A]
            child = (A & ~1) + (val & 0x3F) * 2 + 2 + bit
            if val & (0x80 >> bit):
                acc |= stream[child] << nacc
                nacc += bits
                A = T + 1
                if nacc == 32:
                    out += struct.pack("<I", acc)
                    acc = 0
                    nacc = 0
                    if len(out) >= size:
                        break
            else:
                A = child
    return bytes(out), size % 4 == 0


# ---------------------------------------------------------------------------
# RLE (type 3): flag byte, bit 7 = run (length = (f & 7Fh) + 3, one data
# byte) or literal (length = (f & 7Fh) + 1, that many bytes).

def rle_tokens(data):
    toks = []
    i = 0
    lit = bytearray()
    while i < len(data):
        n = 1
        while i + n < len(data) and data[i + n] == data[i] and n < 130:
            n += 1
        if n >= 3:
            if lit:
                toks.append(("lit", bytes(lit)))
                lit = bytearray()
            toks.append(("run", n, data[i]))
            i += n
        else:
            lit.append(data[i])
            i += 1
            if len(lit) == 128:
                toks.append(("lit", bytes(lit)))
                lit = bytearray()
    if lit:
        toks.append(("lit", bytes(lit)))
    return toks


def rle_stream(tokens, size):
    out = bytearray(struct.pack("<I", (size << 8) | 0x30))
    for t in tokens:
        if t[0] == "run":
            assert 3 <= t[1] <= 130
            out += bytes([0x80 | (t[1] - 3), t[2]])
        else:
            assert 1 <= len(t[1]) <= 128
            out.append(len(t[1]) - 1)
            out += t[1]
    return pad4(out)


def rle_model(stream):
    size = struct.unpack_from("<I", stream)[0] >> 8
    out = bytearray()
    i = 4
    while len(out) < size:
        f = stream[i]
        i += 1
        if f & 0x80:
            out += bytes([stream[i]]) * ((f & 0x7F) + 3)
            i += 1
        else:
            n = (f & 0x7F) + 1
            out += stream[i:i + n]
            i += n
    return bytes(out[:size]), len(out) == size


# ---------------------------------------------------------------------------
# Diff filters (type 8, unit size 1 or 2 in bits 0-3)

def diff_stream(data, unit, size=None):
    size = len(data) if size is None else size
    out = bytearray(struct.pack("<I", (size << 8) | 0x80 | unit))
    if unit == 1:
        prev = 0
        for k, b in enumerate(data):
            out.append(b if k == 0 else (b - prev) & 0xFF)
            prev = b
    else:
        prev = 0
        for k in range(0, len(data), 2):
            v = struct.unpack_from("<H", data, k)[0]
            out += struct.pack("<H", v if k == 0 else (v - prev) & 0xFFFF)
            prev = v
    return pad4(out)


# ---------------------------------------------------------------------------
# other models

def div_model(n, d):
    """Signed division; GBATEK example -1234/10 = -123, -4, +123."""
    n = n - (1 << 32) if n & 0x80000000 else n
    d = d - (1 << 32) if d & 0x80000000 else d
    q = abs(n) // abs(d)
    if (n < 0) != (d < 0):
        q = -q
    r = n - q * d
    return u32(q), u32(r), u32(abs(q))


def crc16_model(seed, data):
    vals = [0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001]
    crc = seed & 0xFFFF
    for b in data:
        crc ^= b
        for j in range(8):
            carry = crc & 1
            crc >>= 1
            if carry:
                crc ^= vals[j] << (7 - j)
    return crc


def bitunpack_model(src, srcw, dstw, offset, zeroflag):
    mask_s = (1 << srcw) - 1
    mask_d = (1 << dstw) - 1
    out = bytearray()
    acc = 0
    n = 0
    for byte in src:
        for k in range(0, 8, srcw):
            v = (byte >> k) & mask_s
            if v or zeroflag:
                v += offset
            acc |= (v & mask_d) << n
            n += dstw
            if n == 32:
                out += struct.pack("<I", acc & 0xFFFFFFFF)
                acc = 0
                n = 0
    return bytes(out), n == 0


def isqrt(n):
    return math.isqrt(n)
