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

#ifndef GPU3D_OPENGLTEXHEAP_H
#define GPU3D_OPENGLTEXHEAP_H

#include <memory>
#include <vector>
#include "types.h"

namespace melonDS
{

// the 512 byte texture VRAM chunks a frame changed (RenderFrame's textureDirty), as a count of the
// changed chunks below each one, so whether a range changed is one subtraction. built once a frame
// and shared by GLRenderer::InvalidateOpaqueTex (GPU::GLNoDiscard bit 1) and GLTexHeap::Invalidate
struct TexDirtyPrefix
{
    static constexpr u32 NumChunks = 512*1024 / 512;
    u16 Before[NumChunks + 1];

    template <class Bits>
    void Build(const Bits& dirty)
    {
        Before[0] = 0;
        for (u32 c = 0; c < NumChunks; c++)
            Before[c + 1] = Before[c] + (dirty[c] ? 1 : 0);
    }
    u32 Count() const { return Before[NumChunks]; }
    bool Changed(u32 first, u32 last) const { return first <= last && Before[last + 1] != Before[first]; }
};

// GPU::GLTexCache (experiment T2, docs/plan-t2-texcache.md): the CPU side of the OpenGL renderer's
// decoded texel heap. a texture (TexParam's address, size, format and color 0 bit, plus the palette
// base) is decoded once into texel records, the integers the render shader's texture fetches feed
// their float tails (palette coordinates, alpha, a kind tag; GPU3D_OpenGL_shaders.h,
// TextureLookup_Cached), and kept in a power-of-two block of one 1024 texel wide RGBA8UI texture
// until texture VRAM under it changes. palette RAM isn't in the records, so palette animation
// costs nothing here. no GL calls: the records go out through an UploadSink. emulator thread only
class GLTexHeap
{
public:
    // where decoded records go: x, y, w, h is a rectangle of the heap texture, data w x h records of
    // 4 bytes (R, G, B, A)
    struct UploadSink
    {
        virtual void Upload(u32 x, u32 y, u32 w, u32 h, const u8* data) = 0;
    };

    // what Use did with a polygon's texture: served from the heap (the first four), or why not
    enum Result
    {
        Hit,        // already decoded and current
        Kept,       // VRAM under it changed, but its bytes hash the same
        Decoded,    // decoded now (new, or after an eviction or a volatile spell)
        Redecoded,  // its bytes changed and it was decoded again into its block
        NumServed,
        FallRange = NumServed, // reads past TexMem or the palette's 48 rows (by TexParam)
        FallPalette, // a 4x4 block's palette reaches past the 48 rows (by its data, retried when it changes)
        FallVolatile, // changed too often lately (GPU::GLTexCache 8 and 2 turn the rule off)
        FallBudget, // this frame's decode budget is spent
        FallFull,   // no room in the heap or the entry table, even after evicting
        NumResults
    };

    // mode: GPU::GLTexCache's value (2 check, 4 tiny, 8 no budget or volatile rule); rows: how many
    // 1024 texel rows the heap texture has (a power of two). drops everything
    void Configure(int mode, u32 rows);
    void Clear();
    // at the start of every frame AssignTexCache runs
    void BeginFrame();
    // the texture of a polygon. offset is its block, for fPolygonAttr.y, when the result is one of
    // the served ones. vram is gpu.VRAMFlat_Texture after this frame's MakeVRAMFlat_TextureCoherent
    Result Use(const u8* vram, u32 texparam, u32 texpal, UploadSink& sink, u32& offset);
    // texture VRAM changed in these chunks this frame: what was decoded from them is checked again
    // at its next use
    void Invalidate(const TexDirtyPrefix& dirty);

    int NumEntries() const { return (int)Entries.size(); }
    u32 UsedTexels() const { return Used; }

    // counted whatever the stats switch, read and cleared by the debug stats (GLTexCacheStats).
    // Timing: the ns ones are measured (debug builds, while the stats run)
    struct Counters
    {
        u64 Texels = 0, Uploads = 0, UploadBytes = 0, HashBytes = 0;
        u64 Evictions = 0, Volatiles = 0, Removed = 0;
        s64 DecodeNs = 0, UploadNs = 0, HashNs = 0;
    };
    Counters Sec;
    bool Timing = false;

    static constexpr u32 DefaultBudget = 256*1024; // decoded texels per frame
    static constexpr u32 TinyBudget = 4*1024;      // GPU::GLTexCache 4
    static constexpr u32 TinyRows = 128;           // GPU::GLTexCache 4's heap, 128K texels
    static constexpr u32 StagingTexels = 64*1024;  // decoded and uploaded in strips of this many

private:
    enum State : u8
    {
        Valid,          // its block holds what VRAM decodes to now
        Stale,          // VRAM under it changed: hash it at the next use
        Changed,        // its bytes did change: decode again into its block
        Pending,        // no block: allocate and decode at the next use
        Uncacheable,    // FallRange, for good
        UncacheablePal, // FallPalette, until VRAM under it changes
        Volatile,       // FallVolatile until VolatileUntil, no block
    };

    struct Entry
    {
        u64 Key;
        u32 TexParam, TexPal;
        u32 Offset;
        u32 LastUsed;
        u32 VolatileUntil;
        u64 SrcHash;
        // the bytes the decode reads, first to last (inclusive): the texels, and for 4x4 the slot 1
        // index data its blocks' palette words come from (Lo1 > Hi1 for none), and their chunks
        u32 Lo0, Hi0, Lo1, Hi1;
        u16 First0, Last0, First1, Last1;
        u16 Slot;    // its Index slot
        u8 Order;    // log2 of its texel count
        u8 State;
        u8 History;  // the last 8 frames it was used in, bit set where its bytes had changed
        bool HasBlock;
    };

    // the entries, in no order, and an open-addressed index by key (-1 empty, half full at most)
    static constexpr int MaxEntries = 4096;
    static constexpr int IndexSlots = 8192;
    std::vector<Entry> Entries;
    s16 Index[IndexSlots];

    // the buddy allocator: power-of-two blocks from 64 texels to 1M, aligned to their size. Free[k]
    // has a bit per block of order k, set when that block is free
    static constexpr u32 MinOrder = 6, MaxOrder = 20;
    std::vector<u64> Free[MaxOrder + 1];
    u32 FreeCount[MaxOrder + 1] {};
    u32 TopOrder = 0;
    u32 Used = 0;
    // the smallest order that couldn't be allocated this frame even after evicting everything that
    // wasn't used in it (MaxOrder + 1 for none), so later polygons don't try again
    u32 FullOrder = MaxOrder + 1;

    int Mode = 0;
    u32 Rows = 0;
    u32 Frame = 0;
    u32 BudgetUsed = 0;
    std::unique_ptr<u8[]> Staging;

    static u32 HashSlot(u64 key) { return (u32)((key * 0x9E3779B97F4A7C15ull) >> 51); } // IndexSlots = 8192
    void RemoveEntry(int e);
    bool MakeRoomForEntry();
    bool AllocBlock(u32 order, u32& offset);
    void FreeBlock(u32 offset, u32 order);
    bool Evict(u32 order, u32& offset);
    void DropBlock(Entry& e);
    u64 HashSource(const u8* vram, const Entry& e);
    void Decode(const u8* vram, Entry& e, UploadSink& sink);
};

}

#endif
