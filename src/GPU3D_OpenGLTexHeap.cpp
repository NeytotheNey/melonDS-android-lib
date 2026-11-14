/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU3D_OpenGLTexHeap.h"

#include <string.h>
#include <algorithm>
#include <chrono>
#include "xxhash/xxhash.h"

namespace melonDS
{

static inline s64 HeapNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// a texel record's kind, in A's low 3 bits (TextureLookup_Cached in GPU3D_OpenGL_shaders.h)
enum
{
    Kind_Pal = 0,    // R, G: palette x, y in TexPalLinear; B: alpha 0 or 1
    Kind_PalA = 1,   // R, G: palette x, y; B: alpha 0..31
    Kind_Direct = 2, // R, G, B: the 5-bit channels; A bit 7: the alpha bit
    Kind_Half = 3,   // 4x4 mode 1 index 2: R, G: color 0's palette x, y (color 1 is the next one)
    Kind_W53 = 4,    // 4x4 mode 3 index 2, as Kind_Half
    Kind_W35 = 5,    // 4x4 mode 3 index 3, as Kind_Half
    Kind_Zero = 6,   // 4x4 index 3 in modes 0 and 1: vec4(0)
};

// TexPalMem's 1024 x 48 entries, which TexPalLinear holds 256 a row: a palette address the shader
// would fetch from row 48 or later reads out of range (undefined), so those textures aren't cached
static constexpr int PalEntries = 1024 * 48;
// TexMem's 1024 x 512 bytes, the same: a texel byte at or past this reads out of range
static constexpr u32 TexMemBytes = 512 * 1024;

static inline void PutRecord(u8* out, int r, int g, int b, int a)
{
    out[0] = (u8)r;
    out[1] = (u8)g;
    out[2] = (u8)b;
    out[3] = (u8)a;
}

// a palette address (TexPalMem's linear one, addr.y in the shader) as TexPalLinear's x, y
static inline void PutPalette(u8* out, int pal, int b, int kind)
{
    PutRecord(out, pal & 0xFF, pal >> 8, b, kind);
}

// the 4x4 format's slot 1 index data address of a texel's block byte (TextureFetch_Compressed)
static inline u32 Slot1Of(u32 addr)
{
    u32 slot1addr = 0x20000 + ((addr & 0x1FFFC) >> 1);
    if (addr >= 0x40000) slot1addr += 0x10000;
    return slot1addr;
}

// the texel records of rows y0 to y1 (not included) of a texture, at out (texel (x, y) at
// (y - y0) * w + x). the integer code of each TextureFetch_* in kRenderFSCommon, line by line, for
// st.x = x and st.y = y (what TexcoordWrap hands it, always in [0, w) x [0, h)), st.z = w, and
// vramaddr = (TexParam's offset << 3, palette): the record holds exactly the integers that fetch
// feeds its float tail. the caller has made sure nothing here reads past TexMem or the palette rows
static void DecodeRows(const u8* vram, u32 texparam, u32 texpal, u32 y0, u32 y1, u8* out)
{
    int tw = 8 << ((texparam >> 20) & 0x7);
    int type = (texparam >> 26) & 0x7;
    // alpha0, as a record's B: 0 with TexParam bit 29 (color 0 transparent), 1 without
    int alpha0 = (texparam & (1u << 29)) ? 0 : 1;
    int vramaddr_x = (int)(texparam & 0xFFFF) << 3;
    int vramaddr_y = (int)texpal;

    for (int y = (int)y0; y < (int)y1; y++)
    {
        for (int x = 0; x < tw; x++, out += 4)
        {
            int addr_x = vramaddr_x;
            int addr_y = vramaddr_y;

            if (type == 5)
            {
                // TextureFetch_Compressed
                addr_x += ((y & 0x3FC) * (tw>>2)) + (x & 0x3FC) + (y & 0x3);
                int p = vram[addr_x];
                int val = (p >> (2 * (x & 0x3))) & 0x3;

                int slot1addr = 0x20000 + ((addr_x & 0x1FFFC) >> 1);
                if (addr_x >= 0x40000) slot1addr += 0x10000;

                int palinfo;
                p = vram[slot1addr];
                palinfo = p;
                slot1addr++;
                p = vram[slot1addr];
                palinfo |= (p << 8);

                addr_y = (addr_y << 3) + ((palinfo & 0x3FFF) << 1);
                palinfo >>= 14;

                if (val == 0)
                    PutPalette(out, addr_y, 1, Kind_Pal);
                else if (val == 1)
                {
                    addr_y++;
                    PutPalette(out, addr_y, 1, Kind_Pal);
                }
                else if (val == 2)
                {
                    if (palinfo == 1)
                        PutPalette(out, addr_y, 0, Kind_Half);
                    else if (palinfo == 3)
                        PutPalette(out, addr_y, 0, Kind_W53);
                    else
                    {
                        addr_y += 2;
                        PutPalette(out, addr_y, 1, Kind_Pal);
                    }
                }
                else
                {
                    if (palinfo == 2)
                    {
                        addr_y += 3;
                        PutPalette(out, addr_y, 1, Kind_Pal);
                    }
                    else if (palinfo == 3)
                        PutPalette(out, addr_y, 0, Kind_W35);
                    else
                        PutRecord(out, 0, 0, 0, Kind_Zero);
                }
            }
            else if (type == 2)
            {
                // TextureFetch_I2
                addr_x += ((y * tw) + x) >> 2;
                int pixel_r = vram[addr_x];
                pixel_r >>= (2 * (x & 3));
                pixel_r &= 0x03;

                addr_y = (addr_y << 2) + pixel_r;
                PutPalette(out, addr_y, (pixel_r>0) ? 1 : alpha0, Kind_Pal);
            }
            else if (type == 3)
            {
                // TextureFetch_I4
                addr_x += ((y * tw) + x) >> 1;
                int pixel_r = vram[addr_x];
                if ((x & 1) != 0) pixel_r >>= 4;
                else              pixel_r &= 0x0F;

                addr_y = (addr_y << 3) + pixel_r;
                PutPalette(out, addr_y, (pixel_r>0) ? 1 : alpha0, Kind_Pal);
            }
            else if (type == 4)
            {
                // TextureFetch_I8
                addr_x += ((y * tw) + x);
                int pixel_r = vram[addr_x];

                addr_y = (addr_y << 3) + pixel_r;
                PutPalette(out, addr_y, (pixel_r>0) ? 1 : alpha0, Kind_Pal);
            }
            else if (type == 1)
            {
                // TextureFetch_A3I5
                addr_x += ((y * tw) + x);
                int pixel_r = vram[addr_x];

                int pixel_a = (pixel_r & 0xE0);
                pixel_a = (pixel_a >> 3) + (pixel_a >> 6);
                pixel_r &= 0x1F;

                addr_y = (addr_y << 3) + pixel_r;
                PutPalette(out, addr_y, pixel_a, Kind_PalA);
            }
            else if (type == 6)
            {
                // TextureFetch_A5I3
                addr_x += ((y * tw) + x);
                int pixel_r = vram[addr_x];

                int pixel_a = (pixel_r & 0xF8) >> 3;
                pixel_r &= 0x07;

                addr_y = (addr_y << 3) + pixel_r;
                PutPalette(out, addr_y, pixel_a, Kind_PalA);
            }
            else
            {
                // TextureFetch_Direct
                addr_x += ((y * tw) + x) << 1;
                int pixelL_r = vram[addr_x];
                addr_x++;
                int pixelH_r = vram[addr_x];

                int r = pixelL_r & 0x1F;
                int g = (pixelL_r >> 5) | ((pixelH_r & 0x03) << 3);
                int b = (pixelH_r & 0x7C) >> 2;
                int a = pixelH_r >> 7;
                PutRecord(out, r, g, b, Kind_Direct | (a << 7));
            }
        }
    }
}

// the 4x4 blocks' palette words: false when one of them puts the four colors a block can read
// (its base to base + 3) at or past TexPalMem's 48 rows. conservative: a block that only reads its
// first color there falls back too, which is always exact
static bool Check4x4Palette(const u8* vram, u32 addr, u32 bytes, u32 texpal)
{
    for (u32 a = addr; a < addr + bytes; a += 4)
    {
        u32 slot1 = Slot1Of(a);
        u32 palinfo = vram[slot1] | (vram[slot1 + 1] << 8);
        u32 base = (texpal << 3) + ((palinfo & 0x3FFF) << 1);
        if (base + 3 >= (u32)PalEntries)
            return false;
    }
    return true;
}

void GLTexHeap::Configure(int mode, u32 rows, const u16* palette)
{
    Mode = mode;
    Rows = (mode & 4) ? std::min(rows, TinyRows) : rows;
    Palette = palette;
    if (!Staging)
        Staging = std::make_unique<u8[]>(StagingTexels * 4);
    if (ColorPlane() && !StagingColor)
        StagingColor = std::make_unique<u16[]>(StagingTexels);
    Clear();
}

void GLTexHeap::Clear()
{
    Entries.clear();
    Entries.reserve(MaxEntries);
    memset(Index, 0xFF, sizeof(Index));

    // Rows is a power of two: the heap is one block of up to 1M texels, or several of them
    u32 texels = Rows * 1024;
    TopOrder = MinOrder;
    while (TopOrder < MaxOrder && (2u << TopOrder) <= texels)
        TopOrder++;
    for (u32 k = 0; k <= MaxOrder; k++)
    {
        Free[k].assign(k >= MinOrder && k <= TopOrder ? (((texels >> k) + 63) / 64) : 0, 0);
        FreeCount[k] = 0;
    }
    u32 top = texels >> TopOrder;
    for (u32 b = 0; b < top; b++)
        Free[TopOrder][b / 64] |= 1ull << (b & 63);
    FreeCount[TopOrder] = top;
    Used = 0;
    FullOrder = MaxOrder + 1;
}

void GLTexHeap::BeginFrame()
{
    Frame++;
    BudgetUsed = 0;
    FullOrder = MaxOrder + 1;
}

bool GLTexHeap::AllocBlock(u32 order, u32& offset)
{
    if (order > TopOrder)
        return false;
    u32 j = order;
    while (j <= TopOrder && FreeCount[j] == 0)
        j++;
    if (j > TopOrder)
        return false;

    std::vector<u64>& bits = Free[j];
    u32 w = 0;
    while (bits[w] == 0)
        w++;
    u32 b = w * 64 + __builtin_ctzll(bits[w]);
    bits[w] &= ~(1ull << (b & 63));
    FreeCount[j]--;
    offset = b << j;

    // split down to the order asked for, freeing the upper halves
    while (j > order)
    {
        j--;
        u32 buddy = (offset + (1u << j)) >> j;
        Free[j][buddy / 64] |= 1ull << (buddy & 63);
        FreeCount[j]++;
    }
    Used += 1u << order;
    return true;
}

void GLTexHeap::FreeBlock(u32 offset, u32 order)
{
    Used -= 1u << order;
    // merge with free buddies as far up as they go
    while (order < TopOrder)
    {
        u32 buddy = (offset ^ (1u << order)) >> order;
        u64& word = Free[order][buddy / 64];
        u64 bit = 1ull << (buddy & 63);
        if (!(word & bit))
            break;
        word &= ~bit;
        FreeCount[order]--;
        offset &= ~(1u << order);
        order++;
    }
    u32 b = offset >> order;
    Free[order][b / 64] |= 1ull << (b & 63);
    FreeCount[order]++;
}

void GLTexHeap::DropBlock(Entry& e)
{
    if (!e.HasBlock)
        return;
    FreeBlock(e.Offset, e.Order);
    e.HasBlock = false;
    e.PlaneValid = false;
    e.PalStale = false;
}

// frees the blocks of entries not used this frame, least recently used first, until a block of
// this order can be had
bool GLTexHeap::Evict(u32 order, u32& offset)
{
    if (order > TopOrder)
        return false;
    std::vector<int> cand;
    cand.reserve(Entries.size());
    for (int i = 0; i < (int)Entries.size(); i++)
    {
        if (Entries[i].HasBlock && Entries[i].LastUsed != Frame)
            cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [this](int a, int b) { return Entries[a].LastUsed < Entries[b].LastUsed; });
    for (int i : cand)
    {
        Entry& e = Entries[i];
        DropBlock(e);
        if (e.State != Uncacheable && e.State != UncacheablePal && e.State != Volatile)
            e.State = Pending;
        Sec.Evictions++;
        if (AllocBlock(order, offset))
            return true;
    }
    return false;
}

// takes an entry out of the index (backward shift, so lookups never need tombstones) and the list
void GLTexHeap::RemoveEntry(int e)
{
    constexpr u32 mask = IndexSlots - 1;
    u32 i = Entries[e].Slot;
    for (;;)
    {
        Index[i] = -1;
        u32 j = i;
        for (;;)
        {
            j = (j + 1) & mask;
            if (Index[j] < 0)
                goto removed;
            u32 home = HashSlot(Entries[Index[j]].Key);
            // the entry at j can move to i when its home slot isn't cyclically in (i, j]
            bool between = (i <= j) ? (home > i && home <= j) : (home > i || home <= j);
            if (!between)
            {
                Index[i] = Index[j];
                Entries[Index[i]].Slot = (u16)i;
                i = j;
                break;
            }
        }
    }
removed:
    int last = (int)Entries.size() - 1;
    if (e != last)
    {
        Entries[e] = Entries[last];
        Index[Entries[e].Slot] = (s16)e;
    }
    Entries.pop_back();
}

// the table is full: drops the entry used longest ago (not this frame), with its block
bool GLTexHeap::MakeRoomForEntry()
{
    int oldest = -1;
    for (int i = 0; i < (int)Entries.size(); i++)
    {
        if (Entries[i].LastUsed == Frame)
            continue;
        if (oldest < 0 || Entries[i].LastUsed < Entries[oldest].LastUsed)
            oldest = i;
    }
    if (oldest < 0)
        return false;
    DropBlock(Entries[oldest]);
    RemoveEntry(oldest);
    Sec.Removed++;
    return true;
}

u64 GLTexHeap::HashSource(const u8* vram, const Entry& e)
{
    s64 start = Timing ? HeapNowNs() : 0;
    u64 hash = XXH3_64bits(vram + e.Lo0, e.Hi0 - e.Lo0 + 1);
    Sec.HashBytes += e.Hi0 - e.Lo0 + 1;
    if (e.Lo1 <= e.Hi1)
    {
        hash = XXH3_64bits_withSeed(vram + e.Lo1, e.Hi1 - e.Lo1 + 1, hash);
        Sec.HashBytes += e.Hi1 - e.Lo1 + 1;
    }
    if (Timing)
        Sec.HashNs += HeapNowNs() - start;
    return hash;
}

// GPU::GLTexCache 128 (T2 step 4b): the palette words an entry's color plane was resolved from,
// TexPalConvBuffer's entries PalLo to PalHi (the same buffer TexPalLinear is uploaded from)
u64 GLTexHeap::HashPalette(const Entry& e)
{
    s64 start = Timing ? HeapNowNs() : 0;
    u32 bytes = (e.PalHi - e.PalLo + 1) * 2;
    u64 hash = XXH3_64bits(Palette + e.PalLo, bytes);
    Sec.HashBytes += bytes;
    if (Timing)
        Sec.HashNs += HeapNowNs() - start;
    return hash;
}

// into its block, in strips of up to StagingTexels: a block of 1024 texels or more is whole heap
// rows (strips too, as both are powers of two and the strip is a multiple of the row), a smaller one
// part of a row. records: decode and upload the records; color: resolve and upload the color plane
// (GPU::GLTexCache 128, color formats). color alone (step 4b's re-resolve after a palette change)
// decodes the records into the staging buffer again only to read their palette coordinates: the
// caller has made sure VRAM under the entry still holds what its records were decoded from, so
// they come out the same, and only the plane is uploaded
void GLTexHeap::Decode(const u8* vram, Entry& e, UploadSink& sink, bool records, bool color)
{
    u32 w = 8u << ((e.TexParam >> 20) & 0x7);
    u32 h = 8u << ((e.TexParam >> 23) & 0x7);
    u32 rowsPerStrip = std::min(h, StagingTexels / w);
    u32 lo = 0xFFFF, hi = 0;
    for (u32 y0 = 0; y0 < h; y0 += rowsPerStrip)
    {
        u32 y1 = std::min(h, y0 + rowsPerStrip);
        s64 start = Timing ? HeapNowNs() : 0;
        DecodeRows(vram, e.TexParam, e.TexPal, y0, y1, Staging.get());
        u32 first = e.Offset + y0 * w;
        u32 count = (y1 - y0) * w;
        if (color)
        {
            // GPU::GLTexCache 128 (T2 step 4a): each record's palette word as TexPalLinear holds it at
            // (R, G) (palette entry R + 256 G, the same 16 bits, so a fetch of it gives the same rgb as
            // the record path's TexPalLinear fetch), with A (bit 0 of RGB5_A1) set to kind 0's alpha,
            // B != 0. kind 1 (A3I5, A5I3) takes its alpha from the record's B instead. resolved from
            // this frame's palette; step 4b keeps the range of entries it read to see later changes
            const u8* rec = Staging.get();
            u16* out = StagingColor.get();
            for (u32 i = 0; i < count; i++, rec += 4)
            {
                u32 p = rec[0] | (rec[1] << 8);
                lo = std::min(lo, p);
                hi = std::max(hi, p);
                out[i] = (u16)((Palette[p] & ~1u) | (rec[2] != 0 ? 1u : 0u));
            }
        }
        s64 mid = Timing ? HeapNowNs() : 0;

        if (count >= 1024)
        {
            if (records)
                sink.Upload(0, first >> 10, 1024, count >> 10, Staging.get());
            if (color)
                sink.UploadColor(0, first >> 10, 1024, count >> 10, StagingColor.get());
        }
        else
        {
            if (records)
                sink.Upload(first & 0x3FF, first >> 10, count, 1, Staging.get());
            if (color)
                sink.UploadColor(first & 0x3FF, first >> 10, count, 1, StagingColor.get());
        }

        if (records)
            Sec.Texels += count;
        else
            Sec.ResolveTexels += count;
        Sec.Uploads++;
        Sec.UploadBytes += count * ((records ? 4 : 0) + (color ? 2 : 0));
        if (Timing)
        {
            s64 end = HeapNowNs();
            Sec.DecodeNs += mid - start;
            Sec.UploadNs += end - mid;
        }
    }

    e.PlaneValid = color;
    e.PalStale = false;
    if (color)
    {
        // step 4b: the palette entries the plane was resolved from (every texel reads one, so
        // lo <= hi), their chunks (256 entries, 512 bytes each), and their words now
        e.PalLo = (u16)lo;
        e.PalHi = (u16)hi;
        e.PalFirst = (u8)(lo >> 8);
        e.PalLast = (u8)(hi >> 8);
        e.PalHash = HashPalette(e);
    }
}

// GPU::GLTexCache 128 (T2 step 4b), for a served entry of a color format after its records are
// current: makes its color plane match this frame's palette, or says why the polygon reads the
// records instead (which is always exact: TexPalLinear holds this frame's palette)
GLTexHeap::Plane GLTexHeap::UsePlane(const u8* vram, Entry& e, UploadSink& sink, bool limits)
{
    if (e.PalVolatile)
        return PlaneVolatile;

    if (e.PlaneValid && e.PalStale)
    {
        // palette RAM changed under its entries: the same words keep the plane
        e.PalStale = false;
        Sec.PalRehashes++;
        if (HashPalette(e) == e.PalHash)
        {
            Sec.PalKept++;
            return PlaneOk;
        }
        e.PlaneValid = false;
        e.PalHistory |= 1;
        // its words changed in 3 of the last 8 frames it was used in: records only for a while,
        // no re-resolves (GPU::GLTexCache 8 and 2 turn the rule off)
        if (limits && __builtin_popcount(e.PalHistory) >= 3)
        {
            e.PalVolatile = true;
            e.PalVolatileUntil = Frame + 120;
            Sec.PalVolatiles++;
            return PlaneVolatile;
        }
    }
    if (e.PlaneValid)
        return PlaneOk;

    // re-resolve the plane only, within the budget like a decode (it's a decode on the CPU, and
    // a third of its upload)
    u32 n = 1u << e.Order;
    u32 budget = (Mode & 4) ? TinyBudget : DefaultBudget;
    if (limits && BudgetUsed && BudgetUsed + n > budget)
        return PlaneBudget;
    Decode(vram, e, sink, false, true);
    BudgetUsed += n;
    Sec.Resolves++;
    return PlaneOk;
}

int GLTexHeap::NumPalVolatile() const
{
    int n = 0;
    for (const Entry& e : Entries)
        n += (e.PalVolatile && (s32)(Frame - e.PalVolatileUntil) < 0) ? 1 : 0;
    return n;
}

GLTexHeap::Result GLTexHeap::Use(const u8* vram, u32 texparam, u32 texpal, UploadSink& sink, u32& offset, Plane& plane)
{
    constexpr u32 mask = IndexSlots - 1;
    u32 format = (texparam >> 26) & 0x7;
    plane = PlaneNone;
    // what decides the records: wrap, flip (16-19) and texgen (30-31) don't, color 0 (29) only for
    // I2/I4/I8, the palette not for direct color
    u32 tp = texparam & 0x3FF0FFFF;
    if (format == 1 || format >= 5)
        tp &= ~(1u << 29);
    u64 key = tp | (format != 7 ? (u64)texpal << 32 : 0);

    int ei = -1;
    u32 slot = HashSlot(key);
    for (; Index[slot] >= 0; slot = (slot + 1) & mask)
    {
        if (Entries[Index[slot]].Key == key)
        {
            ei = Index[slot];
            break;
        }
    }

    if (ei < 0)
    {
        if ((int)Entries.size() >= MaxEntries)
        {
            if (!MakeRoomForEntry())
                return FallFull;
            slot = HashSlot(key);
            while (Index[slot] >= 0)
                slot = (slot + 1) & mask;
        }

        Entry e {};
        e.Key = key;
        e.TexParam = tp;
        e.TexPal = texpal;
        e.Slot = (u16)slot;
        u32 texels = (8u << ((tp >> 20) & 0x7)) * (8u << ((tp >> 23) & 0x7));
        e.Order = (u8)__builtin_ctz(texels);
        e.State = Pending;
        e.Lo1 = 1;
        e.Hi1 = 0;
        e.First1 = 1;
        e.Last1 = 0;

        // the bytes the fetch reads (ScanOpaqueTexture's sizes), and the highest palette entry it can
        // reach without looking at the data (4x4's comes from its data, Check4x4Palette)
        u32 addr = (tp & 0xFFFF) << 3;
        u32 bytes = 0;
        u32 palTop = 0;
        switch (format)
        {
        case 1: bytes = texels; palTop = (texpal << 3) + 31; break;      // A3I5
        case 2: bytes = texels / 4; palTop = (texpal << 2) + 3; break;   // I2
        case 3: bytes = texels / 2; palTop = (texpal << 3) + 15; break;  // I4
        case 4: bytes = texels; palTop = (texpal << 3) + 255; break;     // I8
        case 5: bytes = texels / 4; break;                               // 4x4
        case 6: bytes = texels; palTop = (texpal << 3) + 7; break;       // A5I3
        default: bytes = texels * 2; break;                              // direct
        }
        if (addr + bytes > TexMemBytes || palTop >= (u32)PalEntries)
            e.State = Uncacheable;
        else
        {
            e.Lo0 = addr;
            e.Hi0 = addr + bytes - 1;
            if (format == 5)
            {
                // the slot 1 address grows with the block's inside a 128 KB slot, so it's the first
                // block's word to the last one's in each slot the texture covers
                u32 last = addr + bytes - 4;
                e.Lo1 = ~0u;
                e.Hi1 = 0;
                for (u32 s = addr >> 17; s <= last >> 17; s++)
                {
                    u32 b0 = std::max(addr, s << 17);
                    u32 b1 = std::min(last, (s << 17) + 0x1FFFC);
                    e.Lo1 = std::min(e.Lo1, Slot1Of(b0));
                    e.Hi1 = std::max(e.Hi1, Slot1Of(b1) + 1);
                }
                e.First1 = (u16)(e.Lo1 / 512);
                e.Last1 = (u16)(e.Hi1 / 512);
            }
            e.First0 = (u16)(e.Lo0 / 512);
            e.Last0 = (u16)(e.Hi0 / 512);
        }

        ei = (int)Entries.size();
        Entries.push_back(e);
        Index[slot] = (s16)ei;
    }

    Entry& e = Entries[ei];
    bool check = (Mode & 2) != 0;
    bool limits = !check && !(Mode & 8); // the budget and the volatile rules
    bool firstUse = e.LastUsed != Frame;
    e.LastUsed = Frame;
    if (firstUse)
    {
        e.History <<= 1;
        e.PalHistory <<= 1;
    }
    // GPU::GLTexCache 128 (T2 step 4b): the color formats' plane, checked against this frame's
    // palette after the records (a decode below resolves it too, unless it's palette-volatile)
    bool color = ColorPlane() && Palette && ColorFormat(format);
    if (color && e.PalVolatile && (s32)(Frame - e.PalVolatileUntil) >= 0)
    {
        e.PalVolatile = false;
        e.PalHistory = 0;
    }

    switch (e.State)
    {
    case Uncacheable:
        return FallRange;
    case UncacheablePal:
        return FallPalette;
    case Volatile:
        if ((s32)(Frame - e.VolatileUntil) < 0)
            return FallVolatile;
        e.State = Pending;
        e.History = 0;
        break;
    case Valid:
        // GPU::GLTexCache 2 (check): decoded again the first time it's used in every frame, the
        // color plane with it
        if (check && firstUse)
            e.State = Changed;
        break;
    case Stale:
        if (check)
        {
            e.State = Changed;
            break;
        }
        if (HashSource(vram, e) == e.SrcHash)
        {
            e.State = Valid;
            offset = e.Offset;
            if (color)
                plane = UsePlane(vram, e, sink, limits);
            return Kept;
        }
        e.State = Changed;
        e.History |= 1;
        // changed in 3 of the last 8 frames it was used in: the old path for a while, no decodes
        if (limits && __builtin_popcount(e.History) >= 3)
        {
            DropBlock(e);
            e.State = Volatile;
            e.VolatileUntil = Frame + 120;
            Sec.Volatiles++;
            return FallVolatile;
        }
        break;
    default:
        break;
    }

    if (e.State == Valid)
    {
        offset = e.Offset;
        if (color)
            plane = UsePlane(vram, e, sink, limits);
        return Hit;
    }

    // Changed or Pending: decode, when the budget allows. one texture per frame may go over it, so
    // one bigger than the whole budget still gets in
    u32 n = 1u << e.Order;
    u32 budget = (Mode & 4) ? TinyBudget : DefaultBudget;
    if (limits && BudgetUsed && BudgetUsed + n > budget)
        return FallBudget;

    bool format5 = ((e.TexParam >> 26) & 0x7) == 5;
    if (format5 && !Check4x4Palette(vram, e.Lo0, e.Hi0 - e.Lo0 + 1, e.TexPal))
    {
        DropBlock(e);
        e.State = UncacheablePal;
        return FallPalette;
    }

    bool fresh = !e.HasBlock;
    if (fresh)
    {
        if (e.Order >= FullOrder)
            return FallFull;
        if (!AllocBlock(e.Order, e.Offset) && !Evict(e.Order, e.Offset))
        {
            FullOrder = std::min<u32>(FullOrder, e.Order);
            return FallFull;
        }
        e.HasBlock = true;
    }

    // the color plane with the records, from this frame's palette, unless the entry is in a
    // palette-volatile spell (then the plane stays invalid until it ends)
    bool resolve = color && !e.PalVolatile;
    Decode(vram, e, sink, true, resolve);
    BudgetUsed += n;
    e.SrcHash = HashSource(vram, e);
    e.State = Valid;
    offset = e.Offset;
    if (color)
        plane = resolve ? PlaneOk : PlaneVolatile;
    return fresh ? Decoded : Redecoded;
}

void GLTexHeap::Invalidate(const TexDirtyPrefix& dirty)
{
    if (dirty.Count() == 0)
        return;
    for (Entry& e : Entries)
    {
        if (e.State != Valid && e.State != UncacheablePal)
            continue;
        if (!dirty.Changed(e.First0, e.Last0) && !dirty.Changed(e.First1, e.Last1))
            continue;
        e.State = (e.State == Valid) ? Stale : Pending;
    }
}

void GLTexHeap::InvalidatePalette(const TexPalDirtyPrefix& dirty)
{
    if (dirty.Count() == 0)
        return;
    for (Entry& e : Entries)
    {
        if (e.PlaneValid && dirty.Changed(e.PalFirst, e.PalLast))
            e.PalStale = true;
    }
}

}
