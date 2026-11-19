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
struct GPU2DStats;
struct Shadow2DRecorder;

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
// line-192 Blit (or the one after the line-215 render, see GPU::BlitAfterReadback), and GPU clears
// it right after (see GPU::HiresCaptureBlitDone)
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
    // the destination bank (Layout's) as native capture left it at line 192, when the Blit waits
    // for line 215 (GPU::BlitAfterReadback): the pass keeps its native rows, and the CPU can write
    // the bank before then. without it the pass reads VRAM, at line 192
    bool DstNativeKept;
    u16 DstNative[256][256];

    void Clear() noexcept
    {
        Skip = 0;
        for (u32& mask : LineMask) mask = 0;
        Layout = 0;
        DstMask = 0;
        OffsetMask = 0;
        DstNativeKept = false;
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
    /// LateBlit and BlitAfterReadback). Call it before changing anything a queued line reads: LCDC
    /// VRAM banks, VRAM mappings, the flat VRAM copies, the framebuffers or the 2D renderer. It's
    /// only a check while no line is waiting
    void Sync2D(u8 flushCause = CapFlush_Other, bool blitAfter3D = true) noexcept
    {
        // the lines line 192 queued first: Held2DDone waits for the whole ring itself, and bit 32
        // needs to see whether the 2D thread was still drawing them. after a VCount write that
        // skipped line 215, the frame end or the next line drawn is where they're waited for as planned
        if (Held2DQueued)
            Held2DDone(flushCause == CapFlush_FrameEnd || flushCause == CapFlush_LateBlit);
        if (Is2DRingBusy())
            Wait2DJobs(Jobs2DEnd);
        if (Deferred2DPending)
            Flush2DDeferred(flushCause);
        if (BlitPending || (BlitAfter3DPending && blitAfter3D))
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

            // a Blit left for after the 3D render stays: it reads no LCDC bank (see BlitAfter3DPending)
            Sync2D(flushCause, false);
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
        // display capture writes an LCDC bank, so the lines held back for it are drawn first, and
        // the ones line 192 queued for the 2D thread (HeldTo2DThread) waited for: with bit 8 only
        // for a bank they write
        if (Deferred2DPending)
            Flush2DDeferred(CapFlush_Read);
        if (Held2DQueued && !(HeldTo2DThread & 8))
            Held2DDone();

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

        if (Held2DQueued && (Held2DWrites & (1 << bank)))
            Held2DDone();

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
    // [buffer][screen]: 256 x 192 BGRA, or in the accelerated line format GPU2D::AccelFramebufferWords
    // (the lines, then the WideMelon wing planes)
    std::unique_ptr<u32[]> Framebuffer[2][2] {};

    // debug only: log a checksum of both 2D framebuffers at the end of every frame (see LogHash2D)
    bool Hash2DLogging = false;
    // hold display capture lines back until line 192 with an accelerated 3D renderer (see
    // Begin2DJob). only turned off by a debug switch, to compare against drawing them at HBlank
    bool DeferCapture2D = true;
    // while display capture lines are held, draw engine B's part of each line right away and hold
    // only engine A's, which is all that display capture reads and all that needs the 3D readback.
    // then a game writing engine B's VRAM, palette or OAM doesn't draw the held lines early. on by
    // default since it passed gameplay, save state and OnePlus tests; debug builds can turn it off
    // (DebugProperties) to compare. set between games only
    bool EngineBNoHold = true;
    // threaded 2D, experiment: held capture lines are drawn on the 2D thread too, a sum of bits.
    // 1: with EngineBNoHold, engine B's part of each held line is queued at its HBlank instead of
    // drawn on the emulator thread. 2: in frames whose Blit waits for the 3D render
    // (BlitAfterReadback), the held lines are queued at line 192 instead of drawn there, with their
    // 3D lines fetched first (GetLine needs the GL context), so they're drawn while lines 192-215
    // run (the parts queued read palette and OAM copies, see Start2DJob). with 2: 4 waits for them
    // at the Blit after the 3D render instead of before the render at line 215, 8 doesn't wait for
    // them at a POWCNT write (SetPowerCnt), nor at an LCDC read of a bank they don't write. 16: bit
    // 1's engine B parts are shown to the 2D thread in batches (Publish2DJobs), so it sleeps instead
    // of spinning between them, a wait for the ring draws the ones not shown yet itself when the
    // thread is idle (Wait2DJobs), and both threads read the clock less often while they spin.
    // 32: with 2, when the emulator thread keeps waiting for the queued lines before the Blit (in 2
    // of the last 8 frames that queued them), they're drawn at line 192 for the next 120 such
    // frames. 0 is the old path; 63, the default, is all of them. only changed by a debug switch,
    // set between games
    int HeldTo2DThread = 63;
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
    // OpenGL renderer, experiment T1 (docs/plan-gpu-discovery.md): leave out translucent-pass draws
    // that can't change a pixel, a bit each. 3 is the default in every build (pixel-exact on the
    // Fold and OnePlus, 4-15% fewer GPU cycles); 0 draws as before, for A/B runs against hash3d.
    // bit 0: with the clear-alpha-0 pass, pass 3 doesn't draw a need-opaque polygon's opaque part
    // again (not depth-equal ones), since pass 2 drew it and depth only went down since. bit 1: a
    // need-opaque polygon's opaque draw is left out when its texture has no texel that ends up at
    // alpha 31, its translucent draw (not a shadow's) when none ends up at 1-30
    // (GLRenderer::ClassifyTransAlpha). bit 2 (T1c): with GL_EXT_shader_framebuffer_fetch, the
    // clear-alpha-0 pass is left out and pass 3's translucent draws skip blending where the
    // destination alpha is 0 (GLRenderer::FetchClear0); off by default, it didn't pay off reliably.
    // only changed by a debug switch, set before the game runs
    int GLTransSkip = 3;
    // OpenGL renderer, experiment E2 (docs/plan-gpu-discovery.md): 1 leaves out the edge-marking
    // draw of the final pass, and the whole pass (the depth detach, binds and reattach) when fog is
    // off too, in frames where nothing set the edge flag (attr G): GLEdgeMark 0, or no polygon drawn
    // opaque. exact: the edge shader only marks pixels whose flag is set, and blends (0, 0, 0, 0)
    // over the rest, which keeps them as they are. 1 is the default in every build since 11-12;
    // 0 draws as before, for A/Bs. only changed by a debug switch
    int GLEdgeSkip = 1;
    // OpenGL renderer, experiment E1 (docs/plan-gpu-discovery.md), edge marking (DISP3DCNT bit 5),
    // which the GL renderer never drew before E1. 0: no edge flags, and the compute renderer leaves
    // its edge marking out too (the app's "Edge marking" setting off). 1: opaque fragments next to their
    // polygon's outline (a neighbor one DS pixel away outside it) set the edge flag, the
    // software renderer's rule at any scale, through program builds used only in edge-marking
    // frames (GLRenderer::PrepareEdgeShaders, BuildEdges). 2: every opaque fragment sets the flag
    // (no outline work, marks more where a polygon is just in front of another inside its outline).
    // both use a one-DS-pixel neighbor offset, the clear plane past the target's borders, and the
    // depth-equal polygons' own depth (their margin bias undone). 1 is the default since 11-15; the
    // frontend sets 0 or 1 from its setting, and a debug switch can set any of them
    int GLEdgeMark = 1;
    // OpenGL renderer, E1 speedups (docs/plan-gpu-discovery.md), a bit each, exact on the desktop,
    // the OnePlus 7 Pro and the Pixel 8. bit 0 (off): the edge pass discards pixels without the edge
    // flag before fetching their depth, and is scissored to the box of the polygons drawn opaque (a
    // stencil test can't be used there, the depth/stencil texture is detached for it); no gain
    // measured on Adreno or Mali (11-15). bit 1 (on): with GLEdgeMark 1, polygons with up to 4 edges
    // hand them to the fragment shader as flat varyings (3 vec4) instead of texel fetches (OnePlus
    // opaque pass -11%, Pixel 8 neutral at 8x and -9% at 16x); if those programs don't build, the
    // texture ones are used. bit 2 (debug builds): those programs fail on purpose. 2 is the default
    // since 11-15; only changed by a debug switch
    int GLEdgeFast = 2;
    // OpenGL renderer, experiment D1 (docs/plan-gpu-discovery.md), a bit each. bit 0: the plain
    // clear (not the clear bitmap, DISP3DCNT bit 14) is a glClearBuffer per attachment instead of a
    // full-screen quad, with the same values the quad's shader writes. bit 1: after the frame, the
    // attr and depth/stencil targets are invalidated (glInvalidateFramebuffer), since nothing reads
    // them before the next frame's clear. 3 is the default in every build since 11-12; 0 draws as
    // before, for A/Bs. only changed by a debug switch
    int GLClearFast = 3;
    // OpenGL renderer, experiment R1 step 1 (docs/plan-gpu-discovery.md): 1 draws the opaque pass's
    // polygons that can't discard a fragment (polygon alpha 31, not a shadow, and untextured, decal,
    // or a palette texture without a transparent color 0) with a program built without `discard`,
    // so the GPU can test depth early. Z-buffer frames only (W-buffer ones write gl_FragDepth
    // anyway). exact: the discard it leaves out can't fire for them. 1 is the default in every
    // build since 11-13 (hash3d-exact on the Fold, Pixel and OnePlus; Pixel up to −27% GPU
    // cycles); 0 draws as before, for A/Bs. bit 1 (value 2, R1 step 2, on by default since 11-13, so
    // the default is 3; exact on the desktop and the Fold and Pixel, Contra 4 −1 to −2%) adds the
    // textures that need a scan: direct color, 4x4 and palettes with color 0 transparent, when no
    // texel the shader can fetch comes out under alpha 31 (GLRenderer::ScanOpaqueTexture, cached
    // until texture VRAM under it changes). bit 2 (value 4) is a check: the cache is dropped every
    // frame, so each frame scans afresh. any nonzero value turns step 1 on (2 acts as 3). only
    // changed by a debug switch
    int GLNoDiscard = 3;
    // OpenGL renderer, floor seams (docs/plan-gl-floor-seams.md), a sum of bits. GL only fills pixels
    // whose center is inside a polygon, where the DS fills each span from the left edge's pixel to the
    // right edge's inclusive, so where a T-junction's vertex is rounded off its neighbor's edge
    // (Pokemon Gen 4 floors) GL leaves a crack under a pixel wide that the DS covers. bit 0 (1): the
    // vertices keep the hi-res position's fraction at scales above 1x instead of being truncated to
    // whole target pixels. bit 1 (2): opaque polygons (not lines, translucent, wireframe, depth-equal,
    // shadow or shadow mask polygons) get a skirt a target pixel wide (half a DS pixel at 1x) along
    // their edge normals, its outer corners' depth, color and texcoords taken from the polygon's own
    // planes, drawn after the opaque pass and only over the clear plane (GLRenderer::BuildSkirt). 0,
    // the default, draws as before. only changed by a debug switch
    int GLSeams = 0;
    // OpenGL renderer, a measurement probe for experiment T2 (docs/plan-gpu-discovery.md), and it
    // renders WRONG on purpose: it bounds what a decoded texture cache could save. 1 makes the render
    // programs' texture lookup one texelFetch from TexPalMem at a coordinate taken from the texcoord,
    // with alpha 1, instead of decoding texture VRAM (no wrap, no format decode, no palette fetch);
    // 2 makes it the constant 1.0, no fetch at all, so 1 vs 2 is the fetch and 0 vs 1 the decode.
    // 3 is 1 after the old path's TexcoordWrap on both axes (1 vs 3 is the wrap), 4 the wrap and two
    // dependent fetches like T2's (a TexMem byte at I8's address, then the palette entry it indexes).
    // everything else (passes, draws, program keys) stays as it is. the programs are rebuilt the
    // first frame it differs from what they were built with (GLRenderer::SetRenderVariant). 0, the
    // default, draws as before. only changed by a debug switch
    int GLTexProbe = 0;
    // OpenGL renderer, experiment T2 step 1 (docs/plan-t2-texcache.md): textures are decoded once on
    // the CPU into texel records kept in a heap texture until texture VRAM under them changes, and
    // the render programs look those up instead of decoding VRAM per fragment, with the same float
    // math, so the image stays the same. a polygon whose texture isn't in the heap (out of range,
    // budget, heap full, changing too often) takes the old path. a sum of bits: 1 on (any nonzero
    // value is), 2 a check that decodes every used texture again every frame, 4 a tiny budget (4K
    // texels a frame) and heap (128K texels) so both paths mix, 8 no budget and no volatile rule.
    // step 2 is the default: the programs most polygons draw with are built without the old decode
    // path, and the polygons the heap doesn't serve carry a render key bit and draw with twins that
    // have it (made the first frame one needs them); 16 asks for step 1 instead, one program with both
    // paths, for A/Bs. 32 (debug builds) makes the twins fail to build, which turns the cache off
    // until the value changes, to test that fallback. 64 is step 3: the heap lookup's per-polygon
    // wrap constants are made in the vertex shader (one more flat varying) and the fragment shader
    // wraps without branches, the same texels as TexcoordWrap (65 = step 2 with it, 81 = step 1).
    // 128 is steps 4a and 4b: I2/I4/I8/A3I5/A5I3 textures also get a color plane (an
    // RGB5_A1 texture the heap's size, each texel's palette word resolved on the CPU) that the shader
    // reads without the record's dependent fetch. 4b tracks the palette words each plane was resolved
    // from (rehash after a palette write under them, re-resolve the plane alone when they changed),
    // and textures whose palette keeps changing read their records for a while (193 = steps 2, 3 and
    // 4, 129 without step 3). 256 (with 128 and step 2) builds the cache-only programs without
    // the record path at all; served textures without a current color plane (4x4, direct,
    // palette-volatile) draw with the twins.
    // GLTexProbe wins while it's nonzero. 449 (steps 2, 3, 4 and 256) is the default in every build
    // since 11-14 (hash3d-exact on the desktop, Fold, Pixel and OnePlus; vs 1, Fold −8% / −5% GL time
    // and Pixel −8% / −5% core-active cycles in W2 title / town); 1 was the default from 11-13 (Fold
    // −19% / −10% GPU cycles vs 0); 0 makes no GL objects and draws as before, for A/Bs. only changed
    // by a debug switch
    int GLTexCache = 449;
    // OpenGL and compute renderers, experiment K2a (docs/plan-gpu-discovery.md): the compositor's
    // main draw starts without the output's old contents, so a tiled GPU needn't load them. 1
    // clears the whole output to 0 first, 2 invalidates it and clears only the padding rows
    // between the screens. both keep the output the same, as that draw covers every other pixel
    // (GLCompositor::ClearOutput). 0, the default, draws as before. only changed by a debug
    // switch, set before the game runs
    int CompClear = 0;
    // OpenGL and compute renderers, experiment K2c (docs/plan-gpu-discovery.md): a screen the scale
    // adds nothing to (no 3D, no wings that aren't plain, no held wings, no hi-res capture lines) is
    // drawn at 1x into the first ViewWidth x 192 texels of its own half of the output, and the frame's
    // OutputLayout (GPU3D.h) tells the frontend to show it nearest-upscaled from there (see
    // GLCompositor::RenderFrame). 1 only in a wide view, whose program is highp, so the frontend shows
    // exactly what the full draw would; 2 in the 256 view too, whose mediump program can round the full
    // draw differently on an fp16 GPU. 3 is a check: the full draw as usual, and every 30 frames the 1x
    // one into a target of its own, read back and compared with it (the COMP1X line); the frontend
    // never sees it. 0 draws as before. 1, the default in every build since 11-11 (exact on the Fold, -6 to
    // -8% GPU cycles at the White 2 title and overworld); a debug switch changes it, set before the
    // game runs. with filtered 2D (experiment K3) such a screen is drawn with the filter's program at
    // the filter map's scale instead of at 1x (OutputLayout::HalfScale), in either view (that program
    // doesn't read its varying there, except in Filter2D 3's 256 view, which is drawn in full), when
    // the output's scale is a multiple of the map's above it (4x at 8x, 12x and 16x; 2x at 4x and 6x
    // with Filter2D 2)
    int Comp1x = 1;
    // set by the frontend before each frame, for Comp1x 1 and 2: whether everything that shows the
    // output can show a half drawn at 1x. off, every half is drawn in full
    bool Comp1xFrontend = false;
    // OpenGL and compute renderers, experiment K4 (docs/plan-gpu-discovery.md): the compositor's
    // output is capped at the scale the frontend shows it at, while the 3D keeps rendering at the full
    // internal scale. the output goes down to scale / d, the largest d up to 4 that divides the scale
    // and leaves the output at least the need (GLCompositor::CappedScale), and the compositor's main
    // draw makes each output pixel from the d x d 3D pixels under it (the DOWN3D programs, in the
    // 256 view and in a wide one, see OutputCap3D). hi-res capture's copies are at the output's
    // scale too, with the capture pass reading the 3D at its own (GLCompositor::RunHiresCapture). 1
    // caps by the frontend's need (Renderer3D::SetOutputNeed, from how big the screens are drawn), the
    // default, and 0 keeps the output at the internal scale as before. a need of 0 (unknown, as the
    // desktop build has) is no cap. filtered 2D is capped too (its map stays at the scale the 3D's
    // scale gives it, see GLCompositor::PrepareFilter2D), but not its identity mode (Filter2D 3), nor
    // the GPU 2D shadow (GLCompositor::OutputNeed). docs/plan-deband-outcap.md part B: the output can
    // also go to a scale that doesn't divide the 3D's, above half of it (8x gives 5x-7x, so 6x for a 6x
    // screen instead of staying at 8x; 12x 7x-11x, 16x 9x-15x), the smallest one that's at least the
    // need, whole ratio or not. the output's scale stays whole, so its pixels keep native pixel edges,
    // and the DOWN3D programs weigh each 3D pixel by how much of it the output pixel covers (an area
    // resample, exact in OutputCap3D 0). it only pays while the frontend debands the frame, whose cost
    // goes with the frame's size, so by default (1) it follows OutputCapDeband. bit 2 (3) takes it
    // always, bit 4 (5) never, which keeps every output as before; bit 2 wins over bit 4
    // (GLCompositor::OutputAnyRatio). only changed by a debug switch
    int OutputCap = 1;
    // set by the frontend before each frame: whether anything that shows the output debands it (the
    // screen filter's deband), which lets OutputCap take a ratio that isn't whole (see above)
    bool OutputCapDeband = false;
    // with OutputCap 1: above 0, the need to cap by instead of the frontend's, for A/Bs and the
    // desktop build, which has none. 0, the default, takes the frontend's
    int OutputCapNeed = 0;
    // with OutputCap capping the output: how the capped draw (and the capped hi-res capture pass)
    // turns the ratio x ratio 3D pixels under an output pixel into it. 0 is the exact supersample (a
    // composite per 3D pixel, averaged), exactly a box filter of the full-scale output. the cheaper,
    // inexact ones composite once per output pixel: 1 on the plain average of the 3D pixels' color and
    // alpha, read with bilinear taps (off by a few levels on 3D pixels only, more on 3D edges over 2D,
    // where transparent pixels' color mixes in), 2 on their alpha-weighted color and averaged alpha,
    // read with a texelFetch each. both are also off where a 3D-on-bottom blend saturates (EVA + EVB
    // above 16), as clamping the average's composite isn't averaging the clamped composites. 3, the
    // default, reads the alphas first with textureGather and is 1 where they're the same, and 0 where
    // they differ or the blend can saturate, so it's only off by a color average's rounding
    // (kCompositorFS_Down3D_Entry). only changed by a debug switch
    int OutputCap3D = 3;
    // debug only: the OpenGL renderer logs the masks and shadows of this many frames (that have any)
    int ShadowLogFrames = 0;
    // compute renderer: size the SortWork and Rasterise dispatches on the CPU instead of having the
    // GPU write indirect arguments, which Adreno's driver waits on (see ComputeRenderer::RenderFrame).
    // only turned off by a debug switch, to compare
    bool ComputeDirectDispatch = true;
    // compute renderer: the tile memory and X span buffers start small and grow before a frame whose
    // polygons could need more (ComputeRenderer::GrowWorkBuffers), instead of always taking the most
    // a frame could use (about 190 MB at 4x). only turned off by a debug switch, to compare
    bool ComputeGrow = true;
    // compute renderer, with ComputeGrow: start the work buffers at the room the game grew to last
    // time at the same scale, width and layout (GPU3D_ComputeRooms.h), so a grow that's sure to come
    // happens at the first frame and not mid-game. read when the renderer is set or reset; only
    // turned off by a debug switch, which also leaves the rooms file alone
    bool ComputeRooms = true;
    // compute renderer: at the first frame after the targets are set up, write every byte of the
    // work buffers once on the GPU, and the grown ones at a grow, so the driver commits their
    // memory then and not in the middle of a game's first heavy render (White 2's title at 8x: 95
    // ms down to about 50). read when the renderer is set or reset; only turned off by a debug
    // switch, to compare
    bool ComputeWarm = true;
    // compute renderer: load programs compiled before from their binaries (OpenGL::CompileComputeProgram)
    // instead of compiling them again at game start and at each scale, width or layout change. it
    // needs a file the frontend sets (OpenGL::SetShaderCachePath). read when the renderer is set or
    // reset; only turned off by a debug switch, which also leaves the file alone
    bool ShaderCache = true;
    // debug only, compute renderer: logs a COMPTIME line once a second with the GPU time of each
    // pass and of the compositor, from GL timer queries (GPU3D_ComputeStats.h)
    bool ComputeTimers = false;
    // debug only, compute renderer: logs a COMPPEAK line once a second with the most a frame used
    // of the work buffers. it reads binning's work count back every frame, which waits for the GPU,
    // so it isn't for use with ComputeTimers or a speed test
    bool ComputePeaks = false;
    // debug only, compute renderer: logs a COMPSYNC line once a second with each pass timed on the
    // CPU between two glFinish calls, for drivers whose timer queries read 0 around compute
    // dispatches. much slower than normal, and it turns ComputeTimers and ComputePeaks off
    bool ComputeSync = false;
    // compute renderer: keeps the emulator thread from queueing renders faster than the GPU
    // finishes them, going by a fence after each one (ComputeRenderer::UpdatePace). without it a
    // scene with no capture readback to hold the thread back got the driver's queue over half a
    // second deep on an Adreno 640. 0 off. 1 waits before a render until at most one earlier render
    // is unfinished. 2 leaves the render out instead while two or more are, and keeps showing the
    // last 3D image, when that's safe (ComputeRenderer::CanSkipForPace), and waits like 1 when it
    // isn't. only changed by a debug switch, which also turns the COMPPACE log line on
    int ComputePace = 2;
    bool ComputePaceLog = false;
    // OpenGL renderer: the same for its frames, going by a fence after each Blit's compositor
    // draws, which covers the 3D render they show too (GLRenderer::UpdatePace). without it a frame
    // whose GPU work overran (the GPU 2D shadow in mode all on the OnePlus 7 Pro, before its fix)
    // queued up behind the one before until presented fps fell to about 2. 0 off. 1 waits before
    // a Blit until at most one earlier frame is unfinished, which never happens while the GPU keeps
    // up. there is no skipping like ComputePace 2. only changed by a debug switch, which also turns
    // the GLPACE log line on
    int GLPace = 1;
    bool GLPaceLog = false;
    // debug only, compute renderer: 8, 16 or 32 forces that tile size at any scale where it can
    // work, for A/B runs. 0 is the size the scale picks (TileSizeForScale)
    int ComputeTileSize = 0;
    // compute renderer: shader changes that cut GPU work without changing a pixel, a bit each
    // (the Opt defines in GPU3D_Compute_shaders.h). all but bits 5 and 8 on in every build, only
    // changed by a debug switch, to compare or to find the one behind a wrong pixel. 0 is the
    // shaders as they were.
    // bit 0: Rasterise skips the perspective factor's divide where it isn't used (a linear span
    // with z-buffering). bit 1: Rasterise reads only the polygon and span words its coverage test
    // needs, and the rest for covered pixels. bit 2: DepthBlend only stores the second layer
    // values the final pass reads (see there). bit 3: Rasterise drops a branch no covered pixel
    // takes and gets the texture size's 1/16 from the CPU. bit 4 isn't a shader's: a frame with
    // the same polygons isn't drawn again for a write to texture or palette VRAM that changed no
    // cached texture (nor the clear bitmap while that's on), see ComputeRenderer::RenderFrame.
    // bit 5: nor for a SWAP_BUFFERS that brought exactly the last swap's scene again with the same
    // registers (GPU3D::MatchRenderScene). bit 6: with Smooth 30 FPS on, pacing can leave a render
    // out as it does without it, except around in-between frames and while the swaps come every
    // other frame (HalfCamera::CanSkipRender). bit 7: Rasterise stores a pixel's edge flags and
    // coverage in the 9 bits its 6-6-6-5 color leaves free, so the tile memory has two layers of
    // 4 bytes a pixel and not three. bit 8, only with bit 7: those two are one buffer with a
    // pixel's color and depth side by side, where that fits GL_MAX_SHADER_STORAGE_BLOCK_SIZE
    // (ComputeRenderer::TileLayersFor), and it does nothing without bit 7
    // bit 5 is off: it halves the renders but costs frame-time spikes on the OnePlus 7 Pro at 4x
    // (see docs/plan-compute-efficiency.md). 0xFF turns it on
    // bit 8 is off: one buffer measured no faster than two on the OnePlus 7 Pro at 4x. 0x1DF
    // turns it on. 0x5F is the tile memory with three layers, as it was
    int ComputeOpt = 0xDF;
    // debug only, compute renderer: logs a COMPWHY line once a second on how many frames were
    // drawn, how many were left out as identical, and what kept the drawn ones from that
    // (ComputeRenderer::LogWhy). it also has GPU3D hash every scene, to tell a swap that brought
    // the same polygons again
    bool ComputeWhy = false;
    // diagnostics, compute renderer on Mali (docs/compute-on-mali.md), all off by default.
    // ComputeCheck N: every Nth render with polygons waits for the GPU, reads binning's and the
    // x-spans' buffers back and logs a COMPCHECK line on what's inconsistent
    // (ComputeRenderer::RunCheck). ComputeBin 1-3: BinCombined experiments (BinFirstPass32,
    // BinNoShared, BinPlainPack in the shader). ComputeSSBO: InterpXSpans reads the setup indices
    // from a storage buffer instead of the texture buffer. ComputeCalc 1: CalcOffsets hands out the
    // sorted offsets with the old atomic on VariantWorkCount[1].w, which Mali's driver adds to [0].w
    // (CalcAtomic in the shader). ComputeFlat: VariantWorkCount is a flat uint array in the
    // shaders, as Mali's driver applies an atomic on a uvec4 element's component to element 0;
    // only the switch sets it to 0, the uvec4s as before (VariantCountVec4). the last four
    // recompile the programs
    int ComputeCheck = 0;
    int ComputeBin = 0;
    bool ComputeSSBO = false;
    int ComputeCalc = 0;
    int ComputeFlat = 1;
    // debug only, GPU discovery (docs/plan-gpu-discovery.md, all set before the game runs):
    // OpenGL renderer: 1 logs a GLTIME line once a second with each pass's GPU time from GL timer
    // queries, 2 the same passes timed on the CPU between glFinish calls (much slower, shares only)
    int GLTimers = 0;
    // 1 logs a SCENESTATS line once a second on the scenes line 215 renders, from the CPU's polygon
    // data (GPU3D_SceneStats.h), with any renderer. 2 also has the OpenGL renderer count the fragments
    // its render shaders run and the pixels polygons cover (SCENEFRAGS), which slows it down
    int SceneStats = 0;
    // compute renderer: logs a COMPDEPTH line once a second on what DepthBlend does per pixel
    bool ComputeDepthStats = false;
    // WideMelon (docs/plan-widemelon-polylist.md): with the view wider than 256, the polygon and vertex
    // RAM counts the game sees, and DISP3DCNT bit 13, are the native view's, and the scene is kept in a
    // list twice the DS's size, half of it for the polygons only the wings show, so the wide view no
    // longer drops polygons the DS would draw (GPU3D::WideListOn). latched with the view width, at a
    // polygon RAM swap. on by default since 11-14 (phones checked); off is as before, for A/Bs
    bool WidePolys = true;
    // Precise depth (the app's setting, off by default): Z-buffered polygons' vertex depth is
    // computed at full precision, (Z * 0x800000) / W + 0x7FFE00, instead of the DS's 0x200 steps,
    // ((Z * 0x4000) / W + 0x3FFF) * 0x200 (GBATEK). W-buffered ones are untouched. it fixes shadows
    // and decals the game puts closer to the surface under them than one of those steps (the bench
    // shadows on White 2's Castelia pier); real hardware may not keep more precision, so it's an
    // enhancement. every renderer gets it through Polygon::FinalZ (OpenGL adds back the bits its
    // 16-bit vertex depth drops, GLRenderer's PackDepth). latched with the view width, at a polygon
    // RAM swap (GPU3D::PreciseDepthOn), so a scene never mixes the two
    bool PreciseDepth = false;
    // WideMelon camera stage 2 (docs/plan-widemelon.md): false treats the renderers' vertical margin
    // (SetViewMargin) as 0, for A/Bs. read where the margin is latched, with the view width
    bool WideY = true;
    // logs a HASH3D line per frame with a checksum of the 3D image at native resolution, numbered like
    // HASH2D (Renderer3D::QueueHash3D)
    bool Hash3DLogging = false;
    // the frame HASH2D numbers (counted while Hash2DLogging or Hash3DLogging is on): the one running
    // now, from its line 0 to the end of line 262
    [[nodiscard]] u32 GetHashFrame() const noexcept { return Hash2DFrame; }
    // compute renderer (Renderer3D::FlushAfterCaptureBlit): in frames that hold capture lines,
    // flush right after the line-192 Blit, so the GPU runs the compositor while it would otherwise
    // idle (after the capture map) until the line-215 flush, instead of ahead of the 3D there.
    // only for a Blit that stays at line 192: with BlitAfterReadback it moves after the 3D and is
    // flushed there. only turned off by a debug switch, to compare
    bool BlitFlushCapture = true;
    // threaded 2D with an accelerated 3D renderer: the frame-end 2D barrier and the Blit move from
    // line 192 to just before the 3D render at line 215, which is the first thing after them that
    // changes what the compositor reads, so the 2D thread's last lines overlap VBlank emulation.
    // not in frames that hold capture lines or record any for hi-res capture. only turned off by a
    // debug switch, to compare
    bool LateBlit = true;
    // OpenGL and compute renderers (Renderer3D::SupportsBlitAfterRender), in frames that hold
    // capture lines: the Blit waits from line 192 until right after the 3D render at line 215,
    // which draws into the other of two 3D color buffers while the compositor reads the one it
    // replaced, and the next frame's capture readback is issued between the two instead of at
    // line 0, into a pixel buffer of its own that line 0 swaps in. then the GPU runs
    // the 3D render and the readback that line 192's map waits for before the compositor and the
    // filtered 2D passes, not after them. what the game reads back is the same frame either way.
    // on by default since it passed the OnePlus, Fold and three-game tests; debug builds can turn
    // it off (DebugProperties) to compare
    bool BlitAfterReadback = true;
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
    // filtered 2D, OpenGL and compute renderers, at 3D scale 2 and up: the GL compositor smooths 2D
    // edges with MMPX, for the screen only (GLCompositor::RunFilter2D). read at each Blit.
    // 0 off, 1 on. debug.melonade.filter2d adds 2: stop at the 2x map at every scale, and
    // 3: run every pass but write identity coordinates, which has to look exactly like off
    int Filter2D = 0;
    // debug only: logs a FILTER2D line once a second
    bool Filter2DLogging = false;
    // filtered 2D, experiment K3 (docs/plan-gpu-discovery.md): in a frame whose keys changed on
    // only some lines, the map passes redraw only the rows those lines can reach, each pass
    // scissored to them (GLCompositor::RunFilter2D), instead of the whole map. 0 redraws all of it
    // whenever a key changed, as before. 2 is a check: the partial redraw, and every 30th one the
    // whole map drawn again over it, both read back and compared (the FILTER2D line's rowchecks).
    // 1, the default in every build since 11-11; a debug switch changes it, set before the game runs
    int Filter2DRows = 1;
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

    // debug only: counts what a GPU 2D renderer would have to deal with and logs a GPU2DSTATS line per
    // engine once a second (GPU_2DStats.h). null while it's off, the default, so that's all it costs.
    // emulator thread only, and nothing in it goes into emulation or savestates
    std::unique_ptr<GPU2DStats> Stats2D;
    void Set2DStatsLogging(bool enable) noexcept;

    // the GPU 2D shadow renderer (GPU_Shadow2D.h, docs/plan-gpu-2d-shadow.md): the app's "Sharper
    // rotated 2D" setting turns on mode 1, and debug.melonade.gpu2d sets any mode over it. the
    // recorder notes each line's state while the jobs are built, and the GL compositor redraws
    // the lines it can at output resolution, for display only. null while it's off, the default, so
    // that's all it costs. mode 1 draws the lines that gain something, 2 every eligible line, 3 only
    // the lines with a layer past the edge (the app's Gen 4 battles without the setting); log
    // adds the GPU2D log line, check (with mode 2) the 1x readback; wide 1 lets every layer draw past
    // the edge (debug.melonade.gpu2dwide). set between frames (it waits for the 2D thread and starts
    // the recorder over), or between games before the reset
    std::unique_ptr<Shadow2DRecorder> Shadow2D;
    void SetShadow2D(int mode, bool log, bool check, int wide) noexcept;
    // unit A's per-layer wide modes (Shadow2D::Wide_*, packed as Shadow2D::WideModes makes them) and
    // how far each BG may draw past the left and right edges (a byte each), for the frames to come.
    // the frontend sets them between frames from what the game is doing (the app's Gen 4 battles);
    // nothing while the shadow is off. 0 is every layer stopping at the edge, the default
    void SetShadow2DWide(u32 modes, u32 left, u32 right) noexcept;

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
    void MakeVRAMFlat_2DBGCoherent(u32 line) noexcept;
    void MakeVRAMFlat_2DOBJCoherent(u32 line) noexcept;

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
    // HeldTo2DThread: bit 1 is on, so the ring can hold held lines' engine B parts while they're held
    bool HeldEngineBQueued() const noexcept { return (HeldTo2DThread & 1) && EngineBNoHold && Threaded2D; }
    bool Queue2DDeferred() noexcept;
    u32 Cur2DPalOAM() noexcept;
    void Held2DDone(bool planned = false) noexcept;
    void ResetHeldBackoff() noexcept;
    void KeepHiresCaptureDst() noexcept;
    void Queue2DJob(const GPU2D::LineJob& job, u32 engines, bool publish = true) noexcept;
    void Publish2DJobs() noexcept;
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
        return Jobs2DEnd != Jobs2DDone.load(std::memory_order_acquire);
    }
    void Wait2DJobs(u32 count) noexcept;
    void Start2DThread() noexcept;
    void Stop2DThread() noexcept;
    void Run2DThread() noexcept;
    bool Wait2DWork(u32 done, u32 spinUs, bool spinLess) noexcept;

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
    // threaded 2D, emulator thread only: the jobs put in the ring, ahead of Jobs2DQueued by the
    // ones not shown to the 2D thread yet (HeldTo2DThread bit 16, see Publish2DJobs). the barriers
    // count from it. here, off the line the 2D thread polls Jobs2DQueued on
    u32 Jobs2DEnd = 0;
    // set when this frame holds a line that reads 3D (Snapshot2DScanline), cleared at line 215
    // (see StartHBlank)
    bool CaptureHeldThisFrame = false;
    // HeldTo2DThread bit 2: line 192 queued the held lines (Queue2DDeferred), and Held2DDone hasn't
    // waited for them yet. it does at line 215 before the 3D render, or with bit 4 at the Blit
    // after it (after a VCount write that skips line 215, at the next line drawn or the frame end),
    // and before an LCDC write or mapping change, an LCDC read (with bit 8 only of a bank they
    // write) or a POWCNT write (not with bit 8)
    bool Held2DQueued = false;
    // and the banks their display capture writes (in LCDC mode, the only VRAM a 2D line writes)
    u8 Held2DWrites = 0;
    // HeldTo2DThread bit 32 (see Held2DDone): the last 8 frames that queued them, a bit set for each
    // one waited for early, and how many frames that would queue them still draw them at line 192.
    // the counts are for the log line
    u8 HeldEarlyHistory = 0;
    u32 HeldBackoff = 0;
    u32 HeldQueuedFrames = 0;
    u32 HeldBackedOff = 0;
    // the Blit StartScanline(192) left for line 215 (see LateBlit). Sync2D runs it, so whatever
    // syncs before reading or changing the 2D output, the 3D frame or the renderer gets it first
    bool BlitPending = false;
    // the Blit StartScanline(192) left for right after the 3D render at line 215 (see
    // BlitAfterReadback). never set together with BlitPending. Sync2D runs it too, except for LCDC
    // writes (Sync2DForLCDC: the only LCDC bank it reads, the hi-res capture destination, was
    // copied at line 192) and POWCNT writes (SetPowerCnt)
    bool BlitAfter3DPending = false;
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
