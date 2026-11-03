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


# ---------------------------------------------------------------------------
# SHA-1 (DSi SWIs 24h-29h). The hash itself is FIPS 180-4; the context layout
# and the Finish/29h details are GBATEK's ("BIOS SHA1 Functions (DSi only)"):
#   +00h H0-H4 (32-bit words), +14h/+18h total length in bits (low/high),
#   +1Ch 40h-byte fragment buffer, +5Ch fragment size, +60h callback.
# Finish appends 80h, zeros up to fragment size 38h and the big-endian bit
# length, then restores the bit length (the padding does not count).

SHA1_IV = (0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0)


def _rol(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def sha1_block(h, block):
    w = list(struct.unpack(">16I", block))
    for t in range(16, 80):
        w.append(_rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1))
    a, b, c, d, e = h
    for t in range(80):
        if t < 20:
            f, k = (b & c) | (~b & d), 0x5A827999
        elif t < 40:
            f, k = b ^ c ^ d, 0x6ED9EBA1
        elif t < 60:
            f, k = (b & c) | (b & d) | (c & d), 0x8F1BBCDC
        else:
            f, k = b ^ c ^ d, 0xCA62C1D6
        a, b, c, d, e = (_rol(a, 5) + (f & 0xFFFFFFFF) + e + k + w[t]) & 0xFFFFFFFF, a, _rol(b, 30), c, d
    return tuple((x + y) & 0xFFFFFFFF for x, y in zip(h, (a, b, c, d, e)))


class Sha1Ctx:
    """The 64h-byte DSi SHA-1 context as GBATEK describes it."""

    def __init__(self, buf=None, callback=0):
        self.h = SHA1_IV
        self.bits = 0
        self.buf = bytearray(buf if buf is not None else bytes(0x40))   # Init leaves it as it was
        self.frag = 0
        self.callback = callback

    def update(self, data):
        """GBATEK: a pending fragment is topped up and processed, whole blocks
        go to the callback straight from the source (the buffer keeps its old
        bytes), and only the rest is copied to the buffer."""
        self.bits = (self.bits + len(data) * 8) & 0xFFFFFFFFFFFFFFFF
        data = bytes(data)
        if self.frag:
            n = min(0x40 - self.frag, len(data))
            self.buf[self.frag:self.frag + n] = data[:n]
            self.frag += n
            data = data[n:]
            if self.frag < 0x40:
                return
            self.h = sha1_block(self.h, bytes(self.buf))
            self.frag = 0
        while len(data) >= 0x40:
            self.h = sha1_block(self.h, data[:0x40])
            data = data[0x40:]
        self.buf[:len(data)] = data
        self.frag = len(data)

    def finish(self):
        bits = self.bits
        self.update(b"\x80")
        while self.frag != 0x38:
            self.update(b"\0")
        self.update(struct.pack(">Q", bits))
        self.bits = bits
        return struct.pack(">5I", *self.h)

    def raw(self):
        return (struct.pack("<5I", *self.h) + struct.pack("<II", self.bits & 0xFFFFFFFF, self.bits >> 32)
                + bytes(self.buf) + struct.pack("<II", self.frag, self.callback))


def sha1(data):
    c = Sha1Ctx()
    c.update(data)
    return c.finish()


def sha1_mess(dstlen, src):
    """SWI 29h as GBATEK's pseudocode: hash src, then per round increment the
    20-byte value (carry only while the byte just incremented is 01h), hash it
    in the same context without Init, and copy up to 14h bytes."""
    c = Sha1Ctx()
    c.update(src)
    first = bytearray(c.finish())
    out = bytearray()
    while dstlen:
        i = 0x13
        while True:
            first[i] = (first[i] + 1) & 0xFF
            i -= 1
            if not (i >= 0 and first[i + 1] == 0x01):
                break
        c.update(bytes(first))
        second = c.finish()
        n = min(0x14, dstlen)
        out += second[:n]
        dstlen -= n
    return bytes(out)


# ---------------------------------------------------------------------------
# RSA (DSi SWIs 20h-23h): textbook RSA with our own 1024-bit keys, generated
# here from a fixed seed (Miller-Rabin primes, e = 10001h, the usual public
# exponent). Numbers are big-endian byte strings of 80h bytes, as in the
# signature layouts GBATEK shows (00h, 01h, FFh..., 00h, payload).

RSA_E = 0x10001


def _is_probable_prime(n, rng):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(40):
        a = 2 + rng.big(n.bit_length()) % (n - 3)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


class BigRng(Rng):
    def big(self, bits):
        v = 0
        for _ in range((bits + 15) // 16):
            v = (v << 16) | self.next()
        return v & ((1 << bits) - 1)


def rsa_key(seed, top_bit0=None):
    """(n, d) for a 1024-bit modulus. top_bit0 forces bit 0 of the first
    (most significant) modulus byte, which GBATEK says selects a mode."""
    rng = BigRng(seed)
    while True:
        ps = []
        while len(ps) < 2:
            p = rng.big(512) | (3 << 510) | 1
            if (p - 1) % RSA_E and _is_probable_prime(p, rng):
                ps.append(p)
        n = ps[0] * ps[1]
        if n.bit_length() != 1024:
            continue
        if top_bit0 is not None and ((n >> 1016) & 1) != top_bit0:
            continue
        phi = (ps[0] - 1) * (ps[1] - 1)
        return n, pow(RSA_E, -1, phi)


def rsa_sign(m_bytes, n, d):
    """Raw RSA of a 80h-byte big-endian block: the 'signature' the SWIs decrypt."""
    m = int.from_bytes(m_bytes, "big")
    assert m < n
    return pow(m, d, n).to_bytes(0x80, "big")


def pkcs1_type1(payload, nff=None, btype=1, sep=True):
    """00h, block type, FFh padding, 00h, payload (80h bytes in all)."""
    if nff is None:
        nff = 0x80 - 3 - len(payload)
    body = bytes([0, btype]) + b"\xFF" * nff + (b"\0" if sep else b"") + payload
    assert len(body) <= 0x80
    return body + bytes(0x80 - len(body)) if len(body) < 0x80 else body
