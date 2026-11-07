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

#ifndef GPU_H
#define GPU_H

#include <atomic>
#include <memory>

#include "GPU2D.h"
#include "GPU3D.h"
#include "NonStupidBitfield.h"

namespace melonDS
{
class GPU3D;
class ARMJIT;
namespace Platform { struct Thread; }

static constexpr u32 VRAMDirtyGranularity = 512;
class GPU;
struct CaptureReadCounter;
struct CaptureFlatDirty;

// debug only (GPU_CaptureReads.h): the flat VRAM copies' trackers, as GPU::MakeVRAMFlat_2DBG/OBJCoherent check them
enum : u32
{
    CapTrack_ABG = 0,
    CapTrack_ABGExtPal,
    CapTrack_AOBJExtPal,
    CapTrack_BBG,
    CapTrack_BBGExtPal,
    CapTrack_BOBJExtPal,
    CapTrack_AOBJ,
    CapTrack_BOBJ,
    CapTrack_Count,
};

// debug only (GPU_CaptureReads.h): what asked for the lines held for display capture to be drawn.
// passed along with the barriers, and only looked at while the counter is on
enum : u8
{
    CapFlush_Other = 0,     // anything not named below
    CapFlush_Read,          // ReadVRAM_LCDC, split into the next two when it's counted
    CapFlush_ReadARM9,
    CapFlush_ReadDMA,
    CapFlush_Write,         // WriteVRAM_LCDC
    CapFlush_VRAMCNT,       // MapVRAM_*
    CapFlush_Palette,       // WritePalette
    CapFlush_OAM,           // WriteOAM
    CapFlush_FlatBG,        // a flat VRAM copy is about to change: BG or extended palettes (MakeVRAMFlat_2DBGCoherent)
    CapFlush_FlatOBJ,       // or OBJ (MakeVRAMFlat_2DOBJCoherent)
    CapFlush_Line192,       // the usual one: the frame-end barrier before the Blit
    CapFlush_Line215,       // lines still held at the 3D render (a VCount write skipped line 192)
    CapFlush_LateBlit,      // a late Blit is due (Sync2D with BlitPending)
    CapFlush_Savestate,     // GPU::DoSavestate
    CapFlush_NotHeld,       // a line that isn't held comes next (GPU::Begin2DJob)
    CapFlush_Full,          // the held line buffer is full (a VCount write)
    CapFlush_FrameEnd,      // GPU::FinishFrame (a VCount write skipped lines 192 and 215)
    CapFlush_PowCnt,        // GPU::SetPowerCnt
    CapFlush_HalfCam,       // HalfCamera's real scene redraw at line 0
    CapFlush_Count,
};

// hi-res display capture (see GPU::HiresCapture): why a frame's capture isn't redone at the 3D
// scale. the 2D renderer sets the first four while it records, the compositor the others
enum : u32
{
    HiresCapSkip_Layout   = 1 << 0, // the destination, its offset or the size changed between lines
    HiresCapSkip_VCount   = 1 << 1, // a VCount write: a captured line isn't the line it was drawn as,
                                    // or lines held from an earlier frame go into this record
    HiresCapSkip_Abort    = 1 << 2, // the 3D frame was aborted, so the capture read no 3D
    HiresCapSkip_NoLCDC   = 1 << 3, // the destination bank isn't in LCDC mode
    HiresCapSkip_Size     = 1 << 4, // 128x128 (two lines per bank row)
    HiresCapSkip_Wrap     = 1 << 5, // the rectangle wraps past row 255
    HiresCapSkip_Missing  = 1 << 6, // not every line of the rectangle was captured
    HiresCapSkip_Written  = 1 << 7, // the CPU wrote or remapped the destination during the capture
    HiresCapSkip_Native1x = 1 << 8, // at 1x the native capture is already what the pass would make
    HiresCapSkip_Shader   = 1 << 9, // the capture program didn't build
    HiresCapSkip_Count    = 10,
};

// hi-res display capture: what DoCapture saw on each capture line of a frame, for the GL
// compositor's capture pass (GLCompositor::RunHiresCapture), which redoes the frame's capture at
// the 3D scale from it. lines are indexed by the capture line. the 2D renderer writes it on the
// emulator thread (capture lines are never drawn on the 2D thread), the compositor reads it at the
// line-192 Blit, and GPU clears it right after (see GPU::HiresCaptureBlitDone)
struct HiresCaptureRecord
{
    u32 Skip;               // HiresCapSkip_* bits
    u32 LineMask[6];        // the capture lines recorded
    u32 Layout;             // the first line's destination bank, offset and size (DISPCAPCNT bits 16-21)
    u8 DstMask;             // the banks this frame captured into (see GPU::NativeCapturedSincePass)
    u8 OffsetMask;          // the ones of them captured at a non-zero offset
    u8 StaleMask;           // GPU::NativeCapturedSincePass before this frame's lines: banks an earlier
                            // capture wrote that no pass redid

    struct Line
    {
        u32 CaptureCnt;
        u16 RenderXPos;     // the 3D X scroll the line's 3D was fetched with
        u8 SrcBBank;        // source B's LCDC bank and the row DoCapture read
        u8 SrcBRow;
        bool SrcBPresent;   // DoCapture's srcB != NULL: without it, it takes its A-only branch
        bool SrcBFIFO;      // source B is the display FIFO
    };
    Line Lines[192];

    // the three accelerated 2D planes (val1, val2, val3) before DoCapture composites plane 0
    u32 Planes[192][256*3];
    // source B as DoCapture read it
    u16 SrcB[192][256];

    void Clear() noexcept
    {
        Skip = 0;
        for (u32& mask : LineMask) mask = 0;
        Layout = 0;
        DstMask = 0;
        OffsetMask = 0;
    }

    bool HasLine(u32 line) const noexcept { return (LineMask[line >> 5] >> (line & 31)) & 1; }
};

template <u32 Size, u32 MappingGranularity>
struct VRAMTrackingSet
{
    u16 Mapping[Size / MappingGranularity];

    const u32 VRAMBitsPerMapping = MappingGranularity / VRAMDirtyGranularity;

    void Reset()
    {
        for (u32 i = 0; i < Size / MappingGranularity; i++)
        {
            // this is not a real VRAM bank
            // so it will always be a mismatch => the bank will be completely invalidated
            Mapping[i] = 0x8000;
        }
    }
    NonStupidBitField<Size/VRAMDirtyGranularity> DeriveState(const u32* currentMappings, GPU& gpu);
};

class GPU
{
public:
    explicit GPU(melonDS::NDS& nds, std::unique_ptr<Renderer3D>&& renderer3d = nullptr, std::unique_ptr<GPU2D::Renderer2D>&& renderer2d = nullptr) noexcept;
    ~GPU() noexcept;
    void Reset() noexcept;
    void Stop() noexcept;

    void DoSavestate(Savestate* file) noexcept;

    /// Sets the active renderer to the renderer given in the provided pointer.
    /// The pointer is moved-from, so it will be \c nullptr after this method is called.
    /// If the pointer is \c nullptr, the renderer is reset to the default renderer.
    void SetRenderer3D(std::unique_ptr<Renderer3D>&& renderer) noexcept;
    [[nodiscard]] const Renderer3D& GetRenderer3D() const noexcept { return GPU3D.GetCurrentRenderer(); }
    [[nodiscard]] Renderer3D& GetRenderer3D() noexcept { return GPU3D.GetCurrentRenderer(); }

    u8* GetUniqueBankPtr(u32 mask, u32 offset) noexcept;
    const u8* GetUniqueBankPtr(u32 mask, u32 offset) const noexcept;

    void SetRenderer2D(std::unique_ptr<GPU2D::Renderer2D>&& renderer) noexcept { Sync2D(); GPU2D_Renderer = std::move(renderer); }
    [[nodiscard]] const GPU2D::Renderer2D& GetRenderer2D() const noexcept { return *GPU2D_Renderer; }
    [[nodiscard]] GPU2D::Renderer2D& GetRenderer2D() noexcept { return *GPU2D_Renderer; }

    /// Draws the 2D layers on a thread of their own ("GPU2DThread") when true. Call it between frames.
    /// It's off by default, and then no thread exists and each line is drawn at its HBlank.
    void SetThreaded2D(bool threaded) noexcept;
    [[nodiscard]] bool IsThreaded2D() const noexcept { return Threaded2D != nullptr; }
    /// The 2D thread while threaded 2D is on, otherwise null (so the frontend can find it, e.g. for CPU hints)
    [[nodiscard]] Platform::Thread* Get2DThread() const noexcept;

    /// The 2D barrier: waits until the 2D thread has drawn every line it was given, draws the
    /// lines held back for display capture (see Begin2DJob) and runs a Blit left for later (see
    /// LateBlit). Call it before changing anything a queued line reads: LCDC VRAM banks, VRAM
    /// mappings, the flat VRAM copies, the framebuffers or the 2D renderer. It's only a check while
    /// no line is waiting
    void Sync2D(u8 flushCause = CapFlush_Other) noexcept
    {
        if (Is2DRingBusy())
            Wait2DJobs(Jobs2DQueued.load(std::memory_order_relaxed));
        if (Deferred2DPending)
            Flush2DDeferred(flushCause);
        if (BlitPending)
            RunLateBlit();
    }

    /// The barrier for writes to LCDC banks. A queued line only reads them in VRAM display mode,
    /// and a line held for display capture writes one, so it only syncs while such a line may
    /// still be waiting
    void Sync2DForLCDC(u8 flushCause) noexcept
    {
        if (Queued2DReadsLCDC)
        {
            // debug only, see GPU_CaptureReads.h
            if (CapReads)
                NoteCaptureBarrier();

            Sync2D(flushCause);
            Queued2DReadsLCDC = false;
        }
    }

    void MapVRAM_AB(u32 bank, u8 cnt) noexcept;
    void MapVRAM_CD(u32 bank, u8 cnt) noexcept;
    void MapVRAM_E(u32 bank, u8 cnt) noexcept;
    void MapVRAM_FG(u32 bank, u8 cnt) noexcept;
    void MapVRAM_H(u32 bank, u8 cnt) noexcept;
    void MapVRAM_I(u32 bank, u8 cnt) noexcept;

    template<typename T>
    T ReadVRAM_LCDC(u32 addr) noexcept
    {
        // display capture writes an LCDC bank, so the lines held back for it are drawn first
        if (Deferred2DPending)
            Flush2DDeferred(CapFlush_Read);

        int bank;

        switch (addr & 0xFF8FC000)
        {
        case 0x06800000: case 0x06804000: case 0x06808000: case 0x0680C000:
        case 0x06810000: case 0x06814000: case 0x06818000: case 0x0681C000:
            bank = 0;
            addr &= 0x1FFFF;
            break;

        case 0x06820000: case 0x06824000: case 0x06828000: case 0x0682C000:
        case 0x06830000: case 0x06834000: case 0x06838000: case 0x0683C000:
            bank = 1;
            addr &= 0x1FFFF;
            break;

        case 0x06840000: case 0x06844000: case 0x06848000: case 0x0684C000:
        case 0x06850000: case 0x06854000: case 0x06858000: case 0x0685C000:
            bank = 2;
            addr &= 0x1FFFF;
            break;

        case 0x06860000: case 0x06864000: case 0x06868000: case 0x0686C000:
        case 0x06870000: case 0x06874000: case 0x06878000: case 0x0687C000:
            bank = 3;
            addr &= 0x1FFFF;
            break;

        case 0x06880000: case 0x06884000: case 0x06888000: case 0x0688C000:
            bank = 4;
            addr &= 0xFFFF;
            break;

        case 0x06890000:
            bank = 5;
            addr &= 0x3FFF;
            break;

        case 0x06894000:
            bank = 6;
            addr &= 0x3FFF;
            break;

        case 0x06898000:
        case 0x0689C000:
            bank = 7;
            addr &= 0x7FFF;
            break;

        case 0x068A0000:
            bank = 8;
            addr &= 0x3FFF;
            break;

        default: return 0;
        }

        if (VRAMMap_LCDC & (1<<bank))
        {
            // debug only, see GPU_CaptureReads.h
            if (CapReads && bank < 4)
                NoteCaptureRead(bank, addr);

            return *(T*)&VRAM[bank][addr];
        }

        return 0;
    }

    template<typename T>
    void WriteVRAM_LCDC(u32 addr, T val)
    {
        // VRAM display reads LCDC banks directly. the BG/OBJ writers below don't need the barrier:
        // the 2D renderer reads those banks only through the flat copies (see MakeVRAMFlat_2DBGCoherent)
        Sync2DForLCDC(CapFlush_Write);

        int bank;

        switch (addr & 0xFF8FC000)
        {
        case 0x06800000: case 0x06804000: case 0x06808000: case 0x0680C000:
        case 0x06810000: case 0x06814000: case 0x06818000: case 0x0681C000:
            bank = 0;
            addr &= 0x1FFFF;
            break;

        case 0x06820000: case 0x06824000: case 0x06828000: case 0x0682C000:
        case 0x06830000: case 0x06834000: case 0x06838000: case 0x0683C000:
            bank = 1;
            addr &= 0x1FFFF;
            break;

        case 0x06840000: case 0x06844000: case 0x06848000: case 0x0684C000:
        case 0x06850000: case 0x06854000: case 0x06858000: case 0x0685C000:
            bank = 2;
            addr &= 0x1FFFF;
            break;

        case 0x06860000: case 0x06864000: case 0x06868000: case 0x0686C000:
        case 0x06870000: case 0x06874000: case 0x06878000: case 0x0687C000:
            bank = 3;
            addr &= 0x1FFFF;
            break;

        case 0x06880000: case 0x06884000: case 0x06888000: case 0x0688C000:
            bank = 4;
            addr &= 0xFFFF;
            break;

        case 0x06890000:
            bank = 5;
            addr &= 0x3FFF;
            break;

        case 0x06894000:
            bank = 6;
            addr &= 0x3FFF;
            break;

        case 0x06898000:
        case 0x0689C000:
            bank = 7;
            addr &= 0x7FFF;
            break;

        case 0x068A0000:
            bank = 8;
            addr &= 0x3FFF;
            break;

        default: return;
        }

        if (VRAMMap_LCDC & (1<<bank))
        {
            *(T*)&VRAM[bank][addr] = val;
            VRAMDirty[bank][addr / VRAMDirtyGranularity] = true;

            // banks A-D can hold a hi-res capture copy, which this write makes stale
            if (bank < 4)
            {
                NoteHiresCaptureWrite(bank);

                // debug only, see GPU_CaptureReads.h
                if (CapReads)
                    NoteCaptureWrite(bank, addr);
            }
        }
    }


    template<typename T>
    T ReadVRAM_ABG(u32 addr) const noexcept
    {
        u8* ptr = VRAMPtr_ABG[(addr >> 14) & 0x1F];
        if (ptr) return *(T*)&ptr[addr & 0x3FFF];

        T ret = 0;
        u32 mask = VRAMMap_ABG[(addr >> 14) & 0x1F];

        if (mask & (1<<0)) ret |= *(T*)&VRAM_A[addr & 0x1FFFF];
        if (mask & (1<<1)) ret |= *(T*)&VRAM_B[addr & 0x1FFFF];
        if (mask & (1<<2)) ret |= *(T*)&VRAM_C[addr & 0x1FFFF];
        if (mask & (1<<3)) ret |= *(T*)&VRAM_D[addr & 0x1FFFF];
        if (mask & (1<<4)) ret |= *(T*)&VRAM_E[addr & 0xFFFF];
        if (mask & (1<<5)) ret |= *(T*)&VRAM_F[addr & 0x3FFF];
        if (mask & (1<<6)) ret |= *(T*)&VRAM_G[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    void WriteVRAM_ABG(u32 addr, T val)
    {
        u32 mask = VRAMMap_ABG[(addr >> 14) & 0x1F];

        // debug only, see GPU_CaptureReads.h. what's there now, before this write replaces it
        if (CapReads && mask)
            NoteCaptureFlatWrite(CapTrack_ABG, addr & 0x7FFFF, sizeof(T), ReadVRAM_ABG<T>(addr) != val);

        if (mask & (1<<0))
        {
            VRAMDirty[0][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_A[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<1))
        {
            VRAMDirty[1][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_B[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<2))
        {
            VRAMDirty[2][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_C[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<3))
        {
            VRAMDirty[3][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_D[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<4))
        {
            VRAMDirty[4][(addr & 0xFFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_E[addr & 0xFFFF] = val;
        }
        if (mask & (1<<5))
        {
            VRAMDirty[5][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_F[addr & 0x3FFF] = val;
        }
        if (mask & (1<<6))
        {
            VRAMDirty[6][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_G[addr & 0x3FFF] = val;
        }
    }


    template<typename T>
    T ReadVRAM_AOBJ(u32 addr) const noexcept
    {
        u8* ptr = VRAMPtr_AOBJ[(addr >> 14) & 0xF];
        if (ptr) return *(T*)&ptr[addr & 0x3FFF];

        T ret = 0;
        u32 mask = VRAMMap_AOBJ[(addr >> 14) & 0xF];

        if (mask & (1<<0)) ret |= *(T*)&VRAM_A[addr & 0x1FFFF];
        if (mask & (1<<1)) ret |= *(T*)&VRAM_B[addr & 0x1FFFF];
        if (mask & (1<<4)) ret |= *(T*)&VRAM_E[addr & 0xFFFF];
        if (mask & (1<<5)) ret |= *(T*)&VRAM_F[addr & 0x3FFF];
        if (mask & (1<<6)) ret |= *(T*)&VRAM_G[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    void WriteVRAM_AOBJ(u32 addr, T val)
    {
        u32 mask = VRAMMap_AOBJ[(addr >> 14) & 0xF];

        // debug only, see GPU_CaptureReads.h. what's there now, before this write replaces it
        if (CapReads && mask)
            NoteCaptureFlatWrite(CapTrack_AOBJ, addr & 0x3FFFF, sizeof(T), ReadVRAM_AOBJ<T>(addr) != val);

        if (mask & (1<<0))
        {
            VRAMDirty[0][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_A[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<1))
        {
            VRAMDirty[1][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_B[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<4))
        {
            VRAMDirty[4][(addr & 0xFFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_E[addr & 0xFFFF] = val;
        }
        if (mask & (1<<5))
        {
            VRAMDirty[5][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_F[addr & 0x3FFF] = val;
        }
        if (mask & (1<<6))
        {
            VRAMDirty[6][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_G[addr & 0x3FFF] = val;
        }
    }


    template<typename T>
    T ReadVRAM_BBG(u32 addr) const noexcept
    {
        u8* ptr = VRAMPtr_BBG[(addr >> 14) & 0x7];
        if (ptr) return *(T*)&ptr[addr & 0x3FFF];

        T ret = 0;
        u32 mask = VRAMMap_BBG[(addr >> 14) & 0x7];

        if (mask & (1<<2)) ret |= *(T*)&VRAM_C[addr & 0x1FFFF];
        if (mask & (1<<7)) ret |= *(T*)&VRAM_H[addr & 0x7FFF];
        if (mask & (1<<8)) ret |= *(T*)&VRAM_I[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    void WriteVRAM_BBG(u32 addr, T val)
    {
        u32 mask = VRAMMap_BBG[(addr >> 14) & 0x7];

        // debug only, see GPU_CaptureReads.h. what's there now, before this write replaces it
        if (CapReads && mask)
            NoteCaptureFlatWrite(CapTrack_BBG, addr & 0x1FFFF, sizeof(T), ReadVRAM_BBG<T>(addr) != val);

        if (mask & (1<<2))
        {
            VRAMDirty[2][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_C[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<7))
        {
            VRAMDirty[7][(addr & 0x7FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_H[addr & 0x7FFF] = val;
        }
        if (mask & (1<<8))
        {
            VRAMDirty[8][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_I[addr & 0x3FFF] = val;
        }
    }


    template<typename T>
    T ReadVRAM_BOBJ(u32 addr) const noexcept
    {
        u8* ptr = VRAMPtr_BOBJ[(addr >> 14) & 0x7];
        if (ptr) return *(T*)&ptr[addr & 0x3FFF];

        T ret = 0;
        u32 mask = VRAMMap_BOBJ[(addr >> 14) & 0x7];

        if (mask & (1<<3)) ret |= *(T*)&VRAM_D[addr & 0x1FFFF];
        if (mask & (1<<8)) ret |= *(T*)&VRAM_I[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    void WriteVRAM_BOBJ(u32 addr, T val)
    {
        u32 mask = VRAMMap_BOBJ[(addr >> 14) & 0x7];

        // debug only, see GPU_CaptureReads.h. what's there now, before this write replaces it
        if (CapReads && mask)
            NoteCaptureFlatWrite(CapTrack_BOBJ, addr & 0x1FFFF, sizeof(T), ReadVRAM_BOBJ<T>(addr) != val);

        if (mask & (1<<3))
        {
            VRAMDirty[3][(addr & 0x1FFFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_D[addr & 0x1FFFF] = val;
        }
        if (mask & (1<<8))
        {
            VRAMDirty[8][(addr & 0x3FFF) / VRAMDirtyGranularity] = true;
            *(T*)&VRAM_I[addr & 0x3FFF] = val;
        }
    }

    template<typename T>
    T ReadVRAM_ARM7(u32 addr) const noexcept
    {
        T ret = 0;
        u32 mask = VRAMMap_ARM7[(addr >> 17) & 0x1];

        if (mask & (1<<2)) ret |= *(T*)&VRAM_C[addr & 0x1FFFF];
        if (mask & (1<<3)) ret |= *(T*)&VRAM_D[addr & 0x1FFFF];

        return ret;
    }

    template<typename T>
    void WriteVRAM_ARM7(u32 addr, T val)
    {
        u32 mask = VRAMMap_ARM7[(addr >> 17) & 0x1];

        if (mask & (1<<2)) *(T*)&VRAM_C[addr & 0x1FFFF] = val;
        if (mask & (1<<3)) *(T*)&VRAM_D[addr & 0x1FFFF] = val;
    }


    template<typename T>
    T ReadVRAM_BG(u32 addr) const noexcept
    {
        if ((addr & 0xFFE00000) == 0x06000000)
            return ReadVRAM_ABG<T>(addr);
        else
            return ReadVRAM_BBG<T>(addr);
    }

    template<typename T>
    T ReadVRAM_OBJ(u32 addr) const noexcept
    {
        if ((addr & 0xFFE00000) == 0x06400000)
            return ReadVRAM_AOBJ<T>(addr);
        else
            return ReadVRAM_BOBJ<T>(addr);
    }


    template<typename T>
    T ReadVRAM_Texture(u32 addr) const noexcept
    {
        T ret = 0;
        u32 mask = VRAMMap_Texture[(addr >> 17) & 0x3];

        if (mask & (1<<0)) ret |= *(T*)&VRAM_A[addr & 0x1FFFF];
        if (mask & (1<<1)) ret |= *(T*)&VRAM_B[addr & 0x1FFFF];
        if (mask & (1<<2)) ret |= *(T*)&VRAM_C[addr & 0x1FFFF];
        if (mask & (1<<3)) ret |= *(T*)&VRAM_D[addr & 0x1FFFF];

        return ret;
    }

    template<typename T>
    T ReadVRAM_TexPal(u32 addr) const noexcept
    {
        T ret = 0;
        u32 mask = VRAMMap_TexPal[(addr >> 14) & 0x7];

        if (mask & (1<<4)) ret |= *(T*)&VRAM_E[addr & 0xFFFF];
        if (mask & (1<<5)) ret |= *(T*)&VRAM_F[addr & 0x3FFF];
        if (mask & (1<<6)) ret |= *(T*)&VRAM_G[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    T ReadPalette(u32 addr) const noexcept
    {
        return *(T*)&Palette[addr & 0x7FF];
    }

    template<typename T>
    void WritePalette(u32 addr, T val)
    {
        // queued lines read a copy (see Start2DJob), so only the held ones are drawn first. with
        // EngineBNoHold they only have engine A's part left, which doesn't read engine B's half
        if (!(EngineBNoHold && (addr & 0x400)))
        {
            if (CapReads)
                NoteCaptureBarrier();
            if (Deferred2DPending)
                Flush2DDeferred(CapFlush_Palette);
        }
        addr &= 0x7FF;

        *(T*)&Palette[addr] = val;
        PaletteDirty |= 1 << (addr / VRAMDirtyGranularity);
        PalOAMWritten = true;
    }

    template<typename T>
    T ReadOAM(u32 addr) const noexcept
    {
        return *(T*)&OAM[addr & 0x7FF];
    }

    template<typename T>
    void WriteOAM(u32 addr, T val)
    {
        // as WritePalette
        if (!(EngineBNoHold && (addr & 0x400)))
        {
            if (CapReads)
                NoteCaptureBarrier();
            if (Deferred2DPending)
                Flush2DDeferred(CapFlush_OAM);
        }
        addr &= 0x7FF;

        *(T*)&OAM[addr] = val;
        OAMDirty |= 1 << (addr / 1024);
        PalOAMWritten = true;
    }

    template <typename T>
    inline T ReadVRAMFlat_Texture(u32 addr) const
    {
        return *(T*)&VRAMFlat_Texture[addr & 0x7FFFF];
    }
    template <typename T>
    inline T ReadVRAMFlat_TexPal(u32 addr) const
    {
        return *(T*)&VRAMFlat_TexPal[addr & 0x1FFFF];
    }

    void SetPowerCnt(u32 val) noexcept;

    void StartFrame() noexcept;
    void FinishFrame(u32 lines) noexcept;
    void BlankFrame() noexcept;
    void StartScanline(u32 line) noexcept;
    void StartHBlank(u32 line) noexcept;

    void DisplayFIFO(u32 x) noexcept;

    void SetDispStat(u32 cpu, u16 val, u16 mask = 0xFFFF) noexcept;

    void SetVCount(u16 val, u16 mask = 0xFFFF) noexcept;
    bool MakeVRAMFlat_ABGCoherent(NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_BBGCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept;

    bool MakeVRAMFlat_AOBJCoherent(NonStupidBitField<256*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_BOBJCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept;

    bool MakeVRAMFlat_ABGExtPalCoherent(NonStupidBitField<32*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_BBGExtPalCoherent(NonStupidBitField<32*1024/VRAMDirtyGranularity>& dirty) noexcept;

    bool MakeVRAMFlat_AOBJExtPalCoherent(NonStupidBitField<8*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_BOBJExtPalCoherent(NonStupidBitField<8*1024/VRAMDirtyGranularity>& dirty) noexcept;

    bool MakeVRAMFlat_TextureCoherent(NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_TexPalCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept;

    void SyncDirtyFlags() noexcept;

    melonDS::NDS& NDS;
    u16 VCount = 0;
    u16 TotalScanlines = 0;
    u16 DispStat[2] {};
    u8 VRAMCNT[9] {};
    u8 VRAMSTAT = 0;

    alignas(u64) u8 Palette[2*1024] {};
    alignas(u64) u8 OAM[2*1024] {};

    alignas(u64) u8 VRAM_A[128*1024] {};
    alignas(u64) u8 VRAM_B[128*1024] {};
    alignas(u64) u8 VRAM_C[128*1024] {};
    alignas(u64) u8 VRAM_D[128*1024] {};
    alignas(u64) u8 VRAM_E[ 64*1024] {};
    alignas(u64) u8 VRAM_F[ 16*1024] {};
    alignas(u64) u8 VRAM_G[ 16*1024] {};
    alignas(u64) u8 VRAM_H[ 32*1024] {};
    alignas(u64) u8 VRAM_I[ 16*1024] {};

    u8* const VRAM[9]     = {VRAM_A,  VRAM_B,  VRAM_C,  VRAM_D,  VRAM_E, VRAM_F, VRAM_G, VRAM_H, VRAM_I};
    u32 const VRAMMask[9] = {0x1FFFF, 0x1FFFF, 0x1FFFF, 0x1FFFF, 0xFFFF, 0x3FFF, 0x3FFF, 0x7FFF, 0x3FFF};

    u32 VRAMMap_LCDC = 0;
    u32 VRAMMap_ABG[0x20] {};
    u32 VRAMMap_AOBJ[0x10] {};
    u32 VRAMMap_BBG[0x8] {};
    u32 VRAMMap_BOBJ[0x8] {};
    u32 VRAMMap_ABGExtPal[4] {};
    u32 VRAMMap_AOBJExtPal {};
    u32 VRAMMap_BBGExtPal[4] {};
    u32 VRAMMap_BOBJExtPal {};
    u32 VRAMMap_Texture[4] {};
    u32 VRAMMap_TexPal[8] {};
    u32 VRAMMap_ARM7[2] {};

    u8* VRAMPtr_ABG[0x20] {};
    u8* VRAMPtr_AOBJ[0x10] {};
    u8* VRAMPtr_BBG[0x8] {};
    u8* VRAMPtr_BOBJ[0x8] {};

    int FrontBuffer = 0;
    std::unique_ptr<u32[]> Framebuffer[2][2] {};

    // debug only: log a checksum of both 2D framebuffers at the end of every frame (see LogHash2D)
    bool Hash2DLogging = false;
    // hold display capture lines back until line 192 with an accelerated 3D renderer (see
    // Begin2DJob). only turned off by a debug switch, to compare against drawing them at HBlank
    bool DeferCapture2D = true;
    // while display capture lines are held, draw engine B's part of each line right away and hold
    // only engine A's, which is all that display capture reads and all that needs the 3D readback.
    // then a game writing engine B's VRAM, palette or OAM doesn't draw the held lines early. off by
    // default here; debug builds turn it on (DebugProperties) to compare. set between games only
    bool EngineBNoHold = false;
    // with DeferCapture2D, submit the GPU work held lines wait for (the 3D frame at line 215, its
    // readback at line 0) right away instead of at the frontend's frame-end flush or the map at
    // line 192. only turned off by a debug switch, to compare
    bool EarlyFlushCapture = true;
    // OpenGL renderer: clear the shadow mask per scanline like the software renderer does,
    // instead of over the whole screen for every mask. only turned off by a debug switch, to compare
    bool ShadowMaskLineClear = true;
    // OpenGL renderer: depth-equal shadow masks and shadows (polygon attr bit 14) get the DS
    // margin instead of a plain GL_LESS/GL_LEQUAL. only turned off by a debug switch, to compare
    bool ShadowMaskDepthTest = true;
    // OpenGL renderer: all other depth-equal polygons get the far side of the DS margin too
    // (see GLRenderer::SetPolygonDepthBias). only turned off by a debug switch, to compare
    bool EqualDepthMargin = true;
    // debug only: the OpenGL renderer logs the masks and shadows of this many frames (that have any)
    int ShadowLogFrames = 0;
    // compute renderer: size the SortWork and Rasterise dispatches on the CPU instead of having the
    // GPU write indirect arguments, which Adreno's driver waits on (see ComputeRenderer::RenderFrame).
    // only turned off by a debug switch, to compare
    bool ComputeDirectDispatch = true;
    // compute renderer (Renderer3D::FlushAfterCaptureBlit): in frames that hold capture lines,
    // flush right after the line-192 Blit, so the GPU runs the compositor while it would otherwise
    // idle (after the capture map) until the line-215 flush, instead of ahead of the 3D there.
    // only turned off by a debug switch, to compare
    bool BlitFlushCapture = true;
    // threaded 2D with an accelerated 3D renderer: the frame-end 2D barrier and the Blit move from
    // line 192 to just before the 3D render at line 215, which is the first thing after them that
    // changes what the compositor reads, so the 2D thread's last lines overlap VBlank emulation.
    // not in frames that hold capture lines or record any for hi-res capture. only turned off by a
    // debug switch, to compare
    bool LateBlit = true;
    // compute renderer: before mapping the capture readback, poll a fence for up to this many µs
    // (yielding in between) instead of sleeping in the driver's map, which keeps the emulator
    // thread's clock up. 0 = no fence, no poll. only changed by a debug switch
    int CaptureSpinUs = 6000;

    // hi-res display capture, OpenGL and compute renderers: the GL compositor keeps an upscaled
    // shadow copy of the banks (A-D) display capture writes, redone each capture frame from the
    // 2D planes and the hi-res 3D (GLCompositor::RunHiresCapture), and VRAM display shows it, as
    // do regular display lines that are pixel for pixel a captured row (PrepareHiresStream).
    // native capture and what the game reads stay as they were. set with SetHiresCapture
    bool HiresCapture = false;
    // debug only: 1 logs a HIRESCAP line once a second, 2 also runs the pass at 1x and compares
    // the copy with the native capture every 30 passes
    int HiresCaptureDebug = 0;
    // banks A-D written (by the CPU, DMA or a VRAMCNT change) since the last Blit, and of those the
    // ones written while this frame's capture was armed. both only live one frame: the compositor
    // folds them into its bank state at every Blit, then they're cleared (HiresCaptureBlitDone)
    u8 HiresCapWrites = 0;
    u8 HiresCapWritesInCapture = 0;
    // banks native capture wrote (DoCapture, set on every capture line) that no capture pass has
    // redone yet. only clearing them consumes a record, so a record no Blit reached keeps its
    // banks native until a later pass covers them
    u8 NativeCapturedSincePass = 0;
    // this frame's capture lines, made the first time one is recorded (~680 KB)
    std::unique_ptr<HiresCaptureRecord> HiresCapRecord;
    // POWCNT1 bit 15 (screen swap), latched at the start of each frame like ScreensEnabled: the
    // compositor compares it between frames (GLCompositor::PrepareHiresDisplay), and the register
    // itself can have the next frame's value by then. not savestated, the compositor starts over
    // after a load
    bool ScreenSwap = false;

    // debug only: counts how often games read what display capture wrote and logs a CAPREADS line
    // once a second (GPU_CaptureReads.h). null while it's off, the default, so that's all it costs.
    // emulator thread only, and nothing in it goes into emulation or savestates
    std::unique_ptr<CaptureReadCounter> CapReads;
    void SetCaptureReadLogging(bool enable) noexcept;
    void NoteCaptureRead(u32 bank, u32 addr) noexcept;
    void NoteCaptureWrite(u32 bank, u32 addr) noexcept;
    void NoteCaptureBarrier() noexcept;
    void NoteCaptureDMA(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat) noexcept;
    void NoteCaptureDMARun(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat) noexcept;
    void NoteCaptureFlatWrite(u32 tracker, u32 offset, u32 size, bool changed) noexcept;

    void SetHiresCapture(bool enable) noexcept;
    // DoCapture, on every capture line it draws with HiresCapture on: the line's record entry
    HiresCaptureRecord& RecordHiresCaptureLine(u32 line, u32 captureCnt) noexcept;

    void NoteHiresCaptureWrite(u32 bank) noexcept
    {
        HiresCapWrites |= 1 << bank;
        if (GPU2D_A.CaptureLatch)
            HiresCapWritesInCapture |= 1 << bank;
    }

    GPU2D::Unit GPU2D_A;
    GPU2D::Unit GPU2D_B;
    melonDS::GPU3D GPU3D;

    NonStupidBitField<128*1024/VRAMDirtyGranularity> VRAMDirty[9] {};
    VRAMTrackingSet<512*1024, 16*1024> VRAMDirty_ABG {};
    VRAMTrackingSet<256*1024, 16*1024> VRAMDirty_AOBJ {};
    VRAMTrackingSet<128*1024, 16*1024> VRAMDirty_BBG {};
    VRAMTrackingSet<128*1024, 16*1024> VRAMDirty_BOBJ {};

    VRAMTrackingSet<32*1024, 8*1024> VRAMDirty_ABGExtPal {};
    VRAMTrackingSet<32*1024, 8*1024> VRAMDirty_BBGExtPal {};
    VRAMTrackingSet<8*1024, 8*1024> VRAMDirty_AOBJExtPal {};
    VRAMTrackingSet<8*1024, 8*1024> VRAMDirty_BOBJExtPal {};

    VRAMTrackingSet<512*1024, 128*1024> VRAMDirty_Texture {};
    VRAMTrackingSet<128*1024, 16*1024> VRAMDirty_TexPal {};

    u8 VRAMFlat_ABG[512*1024] {};
    u8 VRAMFlat_BBG[128*1024] {};
    u8 VRAMFlat_AOBJ[256*1024] {};
    u8 VRAMFlat_BOBJ[128*1024] {};

    alignas(u16) u8 VRAMFlat_ABGExtPal[32*1024] {};
    alignas(u16) u8 VRAMFlat_BBGExtPal[32*1024] {};

    alignas(u16) u8 VRAMFlat_AOBJExtPal[8*1024] {};
    alignas(u16) u8 VRAMFlat_BOBJExtPal[8*1024] {};

    alignas(u64) u8 VRAMFlat_Texture[512*1024] {};
    alignas(u64) u8 VRAMFlat_TexPal[128*1024] {};
private:
    void ResetVRAMCache() noexcept;
    void ResetHiresCapture() noexcept;
    void HiresCaptureBlitDone() noexcept;
    void RunLateBlit() noexcept;
    void AssignFramebuffers() noexcept;
    void InitFramebuffers() noexcept;
    void LogHash2D() noexcept;

    // building the 2D renderer's line jobs (see GPU2D::LineJob), on the emulator thread
    void Start2DJob(GPU2D::LineJob& job) noexcept;
    void Snapshot2DScanline(GPU2D::LineJob& job, u32 line) noexcept;
    void Snapshot2DSprites(GPU2D::LineJob& job, u32 line) noexcept;
    void MakeVRAMFlat_2DBGCoherent() noexcept;
    void MakeVRAMFlat_2DOBJCoherent() noexcept;

    // a job is either queued for the 2D thread (a ring slot), held for display capture (a
    // Deferred2DJobs slot) or drawn right away (Job2D)
    GPU2D::LineJob& Begin2DJob() noexcept;
    void End2DJob(GPU2D::LineJob& job) noexcept;
    bool Is2DJobQueued(const GPU2D::LineJob& job) const noexcept { return &job != &Job2D; }
    // only true for the job being built, between Begin2DJob and End2DJob
    bool Is2DJobDeferred(const GPU2D::LineJob& job) const noexcept
    {
        const GPU2D::LineJob* deferred = Deferred2DJobs.get();
        return deferred && &job == deferred + Deferred2DEnd;
    }
    GPU2D::LineJob* BeginDeferred2DJob() noexcept;
    void Flush2DDeferred(u8 flushCause = CapFlush_Other) noexcept;
    void NoteCaptureFlush(u8 flushCause) noexcept;
    void NoteCaptureMap(bool end) noexcept;
    void NoteCaptureFlatCheck(const CaptureFlatDirty* dirty, u32 count) noexcept;

    // true while some line isn't drawn yet: queued for the 2D thread or held for display capture
    bool Is2DThreadBusy() const noexcept
    {
        return Deferred2DPending || Is2DRingBusy();
    }
    // threaded 2D, see SetThreaded2D in GPU.cpp
    bool Is2DRingBusy() const noexcept
    {
        return Jobs2DQueued.load(std::memory_order_relaxed) != Jobs2DDone.load(std::memory_order_acquire);
    }
    void Wait2DJobs(u32 count) noexcept;
    void Start2DThread() noexcept;
    void Stop2DThread() noexcept;
    void Run2DThread() noexcept;
    bool Wait2DWork(u32 done, u32 spinUs) noexcept;

    template<typename T>
    T ReadVRAM_ABGExtPal(u32 addr) const noexcept
    {
        u32 mask = VRAMMap_ABGExtPal[(addr >> 13) & 0x3];

        T ret = 0;
        if (mask & (1<<4)) ret |= *(T*)&VRAM_E[addr & 0x7FFF];
        if (mask & (1<<5)) ret |= *(T*)&VRAM_F[addr & 0x3FFF];
        if (mask & (1<<6)) ret |= *(T*)&VRAM_G[addr & 0x3FFF];

        return ret;
    }

    template<typename T>
    T ReadVRAM_BBGExtPal(u32 addr) const noexcept
    {
        u32 mask = VRAMMap_BBGExtPal[(addr >> 13) & 0x3];

        T ret = 0;
        if (mask & (1<<7)) ret |= *(T*)&VRAM_H[addr & 0x7FFF];

        return ret;
    }

    template<typename T>
    T ReadVRAM_AOBJExtPal(u32 addr) const noexcept
    {
        u32 mask = VRAMMap_AOBJExtPal;

        T ret = 0;
        if (mask & (1<<4)) ret |= *(T*)&VRAM_F[addr & 0x1FFF];
        if (mask & (1<<5)) ret |= *(T*)&VRAM_G[addr & 0x1FFF];

        return ret;
    }

    template<typename T>
    T ReadVRAM_BOBJExtPal(u32 addr) const noexcept
    {
        u32 mask = VRAMMap_BOBJExtPal;

        T ret = 0;
        if (mask & (1<<8)) ret |= *(T*)&VRAM_I[addr & 0x1FFF];

        return ret;
    }

    template <u32 MappingGranularity, u32 Size>
    constexpr bool CopyLinearVRAM(u8* flat, const u32* mappings, NonStupidBitField<Size>& dirty, u64 (GPU::* const slowAccess)(u32) const noexcept) noexcept
    {
        const u32 VRAMBitsPerMapping = MappingGranularity / VRAMDirtyGranularity;

        bool change = false;

        typename NonStupidBitField<Size>::Iterator it = dirty.Begin();
        while (it != dirty.End())
        {
            u32 offset = *it * VRAMDirtyGranularity;
            u8* dst = flat + offset;
            u8* fastAccess = GetUniqueBankPtr(mappings[*it / VRAMBitsPerMapping], offset);
            if (fastAccess)
            {
                memcpy(dst, fastAccess, VRAMDirtyGranularity);
            }
            else
            {
                for (u32 i = 0; i < VRAMDirtyGranularity; i += 8)
                    *(u64*)&dst[i] = (this->*slowAccess)(offset + i);
            }
            change = true;
            it++;
        }
        return change;
    }

    u32 NextVCount = 0;

    bool RunFIFO = false;

    // POWCNT1 bit 0 (LCD enable), latched at the start of each frame. not savestated: a frame of
    // wrong output after a load is acceptable
    bool ScreensEnabled = true;
    bool LCDPowerLogged = false;

    u16 VMatch[2] {};

    // only lives for one HBlank, so it isn't savestated
    GPU2D::LineJob Job2D {};

    // emulator thread only: set when a queued job uses VRAM display or a job is held for display
    // capture, cleared by Sync2DForLCDC once both are drawn (a drain elsewhere leaves it set, which
    // only costs one idle check)
    bool Queued2DReadsLCDC = false;

    // display capture lines held back until line 192 (see Begin2DJob), on the emulator thread only.
    // the buffer is made the first time a line is held. jobs [Deferred2DEnd - Deferred2DPending,
    // Deferred2DEnd) wait to be drawn, and Deferred2DEnd is the slot of the job being built.
    // nothing is held between frames, so none of this is savestated
    std::unique_ptr<GPU2D::LineJob[]> Deferred2DJobs;
    u32 Deferred2DEnd = 0;
    u32 Deferred2DPending = 0;
    // set when this frame holds a line that reads 3D (Snapshot2DScanline), cleared at line 215
    // (see StartHBlank)
    bool CaptureHeldThisFrame = false;
    // the Blit StartScanline(192) left for line 215 (see LateBlit). Sync2D runs it, so whatever
    // syncs before reading or changing the 2D output, the 3D frame or the renderer gets it first
    bool BlitPending = false;
    // HiresCapWrites and HiresCapWritesInCapture as of line 192, for the late Blit (see RunLateBlit)
    u8 LateBlitCapWrites = 0;
    u8 LateBlitCapWritesInCapture = 0;
    // palette or OAM written since queued jobs last got a copy of them (see Start2DJob)
    bool PalOAMWritten = true;

    u32 OAMDirty = 0;
    u32 PaletteDirty = 0;

    u32 Hash2DFrame = 0;

    // threaded 2D: how many jobs the emulator thread has queued and the 2D thread has drawn (both
    // wrap around). equal counts mean the thread is idle, which is all the barrier checks. they're
    // never reset, so they stay equal while threaded 2D is off.
    // each thread writes one counter and reads the other's after every job, so each one has a
    // cache line to itself (the alignas on the following member closes Jobs2DDone's line), with
    // nothing the emulator thread writes per line or per register write on them. none of this is
    // savestated
    alignas(64) std::atomic<u32> Jobs2DQueued {0};
    alignas(64) std::atomic<u32> Jobs2DDone {0};
    // read by both threads (the 2D thread per job) and only written while the thread is idle, so
    // they share a line away from the counters and the per-line state above
    struct Threaded2DState;
    alignas(64) std::unique_ptr<Threaded2DState> Threaded2D;
    std::unique_ptr<GPU2D::Renderer2D> GPU2D_Renderer = nullptr;
};
}

#endif
