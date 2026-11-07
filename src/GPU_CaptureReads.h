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

#ifndef GPU_CAPTUREREADS_H
#define GPU_CAPTUREREADS_H

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "GPU.h"
#include "Platform.h"

namespace melonDS
{

// debug only (debug.melonade.capreads, GPU::CapReads): how often games really read what display
// capture wrote, per capture, to size up "lazy capture" (keep the capture on the GPU and only copy
// it to the CPU when something needs it). it only counts: nothing here changes emulation.
//
// display capture only writes a bank mapped to LCDC (SoftRenderer::DoCapture), and a bank has one
// mapping at a time (GPU::MapVRAM_AB/CD), so what a capture wrote can be read in these ways only:
// - must-copy: an ARM9 or DMA read through GPU::ReadVRAM_LCDC, or a VRAMCNT change that hands the
//   bank to BG, OBJ, texture or ARM7. both count however much of the bank they touch.
// - the GPU could do it: display mode 2 (GPU::Snapshot2DScanline, when the line's job is built) and
//   capture source B (DoCapture). these go by the row they read, and the compositor says per frame
//   and per bank whether its hi-res copy was what got used (NoteHiresDisplay, NoteHiresSrcB).
// - a savestate or rewind snapshot being saved.
//
// a capture holds the bank rows (512 bytes each) it wrote, until later captures have replaced all of
// them and it has ended. so captures into different parts of a bank are alive side by side, and a
// read later on counts for each of them. a CPU write doesn't end a capture: it's only flagged, as the
// rest of the capture can still be read. a capture that ended unread is "ow" if the CPU wrote to the
// bank meanwhile, and "owrow" if that was in the rows it held.
//
// all of this runs on the emulator thread. capture lines are never drawn on the 2D thread
// (GPU::Begin2DJob), and display mode 2 is counted when the job is built, not when it's drawn.
//
// the same switch also traces why held capture lines get drawn early (CAPFLUSH once a second, and
// CAPTRACE per frame): GPU::Flush2DDeferred says what asked for it and whether it maps the 3D
// readback, DMA and NDMA starts reading LCDC VRAM are noted, and each frame's reads of a captured
// bank are judged against the lines a barrier that only cares about rows would still be holding:
//
// - ModelRows is what those lines would have written. a held line adds its destination row when its
//   job is built, whether or not the real barrier has drawn it since: the real flush for a read
//   empties the real held lines, which says nothing about what the row-precise barrier would hold.
// - any other barrier empties ModelRows (NoteBarrier and the non-read causes in NoteFlush), as it
//   would flush the same lines in that world too. that includes those that found nothing to do for
//   real because a read had just flushed. a read doesn't empty it, unless it hits a row in it.
// - a read of a row in ModelRows is an overlap: it needs what held lines wrote, so even that barrier
//   has to flush, and from there it holds nothing until the next line is held. any other read while
//   lines are modeled as held is disjoint, including a row a later line will overwrite, which
//   correctly gets the old data. rows are 512 bytes, and a 128-wide capture counts for its whole row.
//
// a frame's verdict: "ovl" with at least one overlap, "dis" with reads while lines were held and no
// overlap, "noheld" when all reads came with no lines held (so they never were an early flush, as
// with a DMA after line 192), "noread" for a capture frame with no reads of a captured bank. only
// "dis" and "ovl" say anything about a narrower barrier. reads of LCDC addresses outside banks A-D
// or of unmapped ones flush too and hit no row: they count as "stray" and make the frame "dis".
//
// the flat VRAM barrier gets a closer look (CAPDIRTY per frame, and the "dirty" part of CAPFLUSH): each
// check of the flat copies that finds something dirty while lines are held says which tracker fired,
// which held lines it flushes, who wrote since the last check, and whether those lines could care.
// a line job holds both engines (LineJob::BG[2], OBJ[2], drawn together by SoftRenderer::RunJob), so
// a write on either engine's copy flushes both engines' lines. display capture only reads engine A's
// output, so a write to engine B's copy can only change engine B's held lines, never the capture.
// with GPU::EngineBNoHold, engine B's part is drawn right away and only engine A's stays held: a dirty
// engine B copy is no barrier, used and effect only ask about engine A, and engine B's writes are only
// added up as bskip (checks with lines held), bchg and bsame.
//
// "used" is one-sided: used=no means no enabled layer on any held line of that engine can read a dirty
// page, so the barrier wasn't needed for it. used=yes only means it can't be ruled out. it works at
// 512-byte page granularity (the trackers' own), and takes a whole layer's range from its BGCNT and
// DISPCNT: all of its map and all 1024 (or 256) of its tiles or its whole bitmap, whatever the scroll,
// the line, the windows or the map really select, so most of its error is a yes that could be a no.
// OBJ is coarser still: any held line with sprites on counts as using all of that engine's OBJ VRAM,
// and any BG or OBJ extended palette in use counts for the slot or the whole palette. "effect" asks the
// same about only the pages where a write changed the value (with no write seen, a mapping change or
// so, all dirty pages count as changed). that's by page number modulo 16 KB, to catch a small bank's
// mirrors, so it can say yes for a page that didn't change. a write changing a word and one changing it
// back both count as changed. a mapping change in the same line as a write isn't seen, and with several
// banks overlapping a location the value compared is them ORed together: those two can make a wrong no.
struct CaptureFlatDirty
{
    u32 Tracker;
    const u64* Bits;    // the tracker's dirty pages, 512 bytes each
    u32 Words;
};

struct CaptureReadCounter
{
    enum : u32
    {
        Flag_ARM9       = 1 << 0,   // a read by the ARM9 (or anything else on its bus that isn't DMA)
        Flag_DMA        = 1 << 1,   // a read by ARM9 DMA or NDMA
        Flag_RowHit     = 1 << 2,   // one of those reads was in a row this capture holds
        Flag_ABG        = 1 << 3,   // VRAMCNT handed the bank to... engine A BG
        Flag_AOBJ       = 1 << 4,   // engine A OBJ
        Flag_BBG        = 1 << 5,   // engine B BG
        Flag_BOBJ       = 1 << 6,   // engine B OBJ
        Flag_Texture    = 1 << 7,   // 3D textures
        Flag_ARM7       = 1 << 8,   // the ARM7
        Flag_Off        = 1 << 9,   // nobody: disabled, or an MST value with no mapping. not a read
        Flag_Written    = 1 << 10,  // a CPU or DMA write to the bank
        Flag_WrittenRow = 1 << 11,  // one of those was in a row this capture holds
        Flag_Mode2      = 1 << 12,  // display mode 2 showed a row of it
        Flag_Mode2Hi    = 1 << 13,  // in a frame where the bank's mode 2 lines all showed the hi-res copy
        Flag_Mode2Nat   = 1 << 14,  // in a frame where some showed native VRAM
        Flag_Mode2Frame = 1 << 15,  // this frame, so the compositor's answer still has to be applied
        Flag_SrcB       = 1 << 16,  // capture source B read a row of it
        Flag_SrcBHi     = 1 << 17,  // as above: the capture pass took the bank's source B from the hi-res copy
        Flag_SrcBNat    = 1 << 18,  // or from native VRAM, or no pass ran
        Flag_SrcBFrame  = 1 << 19,
        Flag_State      = 1 << 20,  // a savestate or rewind snapshot was saved while it was alive
        Flag_SrcBOnly   = 1 << 21,  // a source B only capture, which never maps the 3D readback

        Flags_MustCopy  = Flag_ARM9 | Flag_DMA | Flag_ABG | Flag_AOBJ | Flag_BBG | Flag_BOBJ | Flag_Texture | Flag_ARM7,
    };

    struct Slot
    {
        u64 Rows[4];    // the bank rows it still holds
        u32 Flags;
        bool Used;
        bool Ended;     // no rows left: it's counted at the end of the frame, once the compositor has answered
    };

    struct Stats
    {
        u32 Captures, Ended;
        u32 NoRead, Overwritten, OverwrittenRow, SkipOK, Skip3D;    // ended captures only, see EndSlot
        u32 ARM9, DMA, RowHit, Remap, ABG, AOBJ, BBG, BOBJ, Texture, ARM7, Off;
        u32 Mode2, Mode2Hi, Mode2Nat, SrcB, SrcBHi, SrcBNat, State, SrcBOnly;
    };

    // eight is more than four captures in 64-row pieces can ever need. if a bank runs out, its oldest
    // capture is counted as ended early and Overflow says so: its later reads would go uncounted
    static constexpr u32 SlotsPerBank = 8;

    Slot Slots[4][SlotsPerBank] {};
    Slot* Cur = nullptr;    // the capture this frame's lines go into
    u32 CurLayout = 0;
    u32 CurLine = 0;

    u8 Mode2HiMask = 0, Mode2NatMask = 0;   // this frame, from the compositor
    u8 SrcBHiMask = 0, SrcBNatMask = 0;

    Stats Second {};
    Stats Total {};
    u32 Dropped = 0;        // alive when the game was reset or a savestate loaded, so never finished
    u32 Overflow = 0;       // see SlotsPerBank
    u32 NoLCDCLines = 0;    // capture lines whose destination wasn't in LCDC mode, so they wrote nothing
    s64 LogStartNs = 0;

    struct FlushCauseStats
    {
        u32 Count, Maps;    // flushes for this cause, and the ones of them that mapped the readback
        u32 VMin, VMax;     // the VCount range they came at
        u64 MapNs;          // the time those maps took
    };

    struct FlushStats
    {
        FlushCauseStats Causes[CapFlush_Count];
        u32 Disjoint, Overlap, NoHeld, NoRead, Stray;   // frames, and stray reads that flushed
        u32 DMA[9];         // starts: DMA modes 0-7, then NDMA
    };

    struct FrameFlush
    {
        u8 Cause;
        bool Maps;
        u16 VCount;
        u16 Held;
    };

    struct FrameDMA
    {
        bool NDMA, Repeat;
        u8 Channel, Mode, WordSize;
        u32 Src, Dst, Words;    // of its first start this frame
        u32 Starts, TotalWords;
        u16 VFirst, VLast;
    };

    struct DMASetup
    {
        bool NDMA, Repeat;
        u8 Channel, Mode, WordSize;
        u32 Src, Dst, Words;
    };

    // writes to a bank behind a flat copy, since the last check of it
    struct FlatWrites
    {
        u32 ARM9, DMA, Changed, Same;
        u32 Min, Max;       // the byte range, as a flat copy offset
        u32 FirstPC;
        bool FirstByDMA;
        DMASetup FirstDMA;
        // which pages got a new value, by page number modulo 16 KB. a bank smaller than its mapping shows
        // up at several offsets of a copy, 16 KB apart at the least, and they all change together
        u32 ChangedPages;
    };

    // the first flat flush of a frame
    struct DirtyEvent
    {
        u16 VCount, Held, HeldCapture, ActiveA, ActiveB;    // lines held: all, capturing, and with a BG on for each engine
        u8 Tracker;         // the first tracker with something dirty
        u16 FirstBit, LastBit;
        u32 Banks;          // what backs its first dirty page
        u32 ARM9, DMA, Changed, Same;   // this check's writes, all trackers
        u32 Min, Max;       // and the first tracker with writes' byte range
        u32 PC;
        bool ByDMA;
        DMASetup Setup;
        bool Used, Effect;
        s8 UsedLayer;       // the first thing found to use it: BG 0-3, or 4 for OBJ
        char UsedKind;      // m map, t tiles, b bitmap, p extended palette, o OBJ
    };

    struct FrameDirty
    {
        u32 Events;
        u32 Tracker[CapTrack_Count];
        u32 BGA, BGB, OBJA, OBJB;
        u32 ByARM9, ByDMA, ByBoth, ByNone;
        u32 Writes, Changed, Same;
        u32 UsedYes, UsedNo, EffectYes, EffectNo;
        u32 BSkip, BChanged, BSame;     // with EngineBNoHold: engine B checks that found something dirty, and their writes
        u32 ObjIdleA, ObjIdleB;         // OBJ checks that found something dirty with nothing held
        DirtyEvent First;
    };

    struct DirtyStats
    {
        u32 BGA, BGB, BGBoth, OBJA, OBJB;
        u32 ByARM9, ByDMA, ByBoth, ByNone;
        u64 Changed, Same;
        u32 UsedYes, UsedNo, EffectYes, EffectNo;
        u32 BSkip;
        u64 BChanged, BSame;
        u32 ObjIdleA, ObjIdleB;
    };

    // reads of a captured bank (or of any bank while lines are modeled as held)
    struct FrameTrace
    {
        bool Capture;           // this frame held or drew a capture line
        u32 NumFlushes;
        FrameFlush Flushes[4];
        u8 MapCause;
        bool Mapped;
        s64 MapNs, WaitNs, SpinNs;

        u32 Reads;              // words read
        u16 FirstV, FirstRow, MinRow, MaxRow;   // MinRow and MaxRow are in the first read's bank
        u8 FirstBank, BankMask;
        s16 HeldFirst, HeldLast;    // at the first read: the rows held lines would have written in its bank, or -1
        u16 HeldLines;          // and how many lines were modeled as held
        bool ReadHeld, ReadNoHeld, Overlap;
        u16 OverlapV;           // the first overlap's VCount
        u32 ModelFlushes;       // how many flushes the row-precise barrier would have done for reads
        u32 Stray;

        u32 NumDMAs, DMAStarts;
        FrameDMA DMAs[3];

        FrameDirty Dirty;
    };

    // how many capture frames get CAPTRACE and CAPDIRTY lines: the first ones of a session, then every
    // so often. a frame counts if it held or drew a capture line, read a captured bank or started a
    // DMA from LCDC VRAM. the frontend's log drops what's over 50 lines a second from one call site
    // (DebugLog.cpp), so there's a limit under that: past it a frame goes unlogged, and the first
    // ones just take longer to fill up
    static constexpr u32 TraceFirstLines = 120;
    static constexpr u32 TraceEvery = 60;
    static constexpr u32 TraceLinesPerSecond = 40;

    struct PCCount { u32 PC, Count; };
    struct DMACount { DMASetup Setup; u32 Count; };

    FlatWrites Writes[CapTrack_Count] {};
    DMASetup CurDMA {};
    DirtyStats DirtyTotal {};
    PCCount TopPCs[16] {};      // by flushes whose first writer was at that PC, first come first kept
    DMACount TopDMAs[8] {};
    u32 OtherPCs = 0, OtherDMAs = 0;
    u32 TraceLines = 0;
    u32 TraceSecondLines = 0;
    s64 TraceSecondNs = 0;

    FlushStats FlushSecond {};
    FlushStats FlushTotal {};
    FrameTrace Frame {};
    FrameTrace LastFrame {};    // the frame before this one
    const char* LastVerdict = "";
    u32 TraceFrames = 0;
    u64 ModelRows[4][4] {};     // see the comment at the top
    u32 ModelLines = 0;
    bool ReadFlushOpen = false; // a read asked for a flush, and hasn't shown up in NoteRead yet
    s64 MapStartNs = 0, MapStartWaitNs = 0, MapStartSpinNs = 0;

    // GPU::Snapshot2DScanline, for a line about to be held. the destination row as DoCapture will
    // work it out, if it will write one
    void NoteHeldLine(u32 captureCnt, bool latch, u32 vcount, u32 lcdcMap) noexcept
    {
        Frame.Capture = true;
        ModelLines++;

        static constexpr u32 widths[4] = {128, 256, 256, 256};
        static constexpr u32 heights[4] = {128, 64, 128, 192};
        u32 size = (captureCnt >> 20) & 0x3;
        u32 bank = (captureCnt >> 16) & 0x3;
        if (!latch || vcount >= heights[size] || !(lcdcMap & (1 << bank)))
            return;

        u32 row = ((((captureCnt >> 18) & 0x3) << 14) + vcount * widths[size]) & 0xFFFF;
        row >>= 8;
        ModelRows[bank][row >> 6] |= 1ull << (row & 63);
    }

    // DMA::Run9 and DSi_NDMA::Run9: the writes coming up are this DMA's, as long as CPUStop says so
    void NoteDMARun(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat) noexcept
    {
        CurDMA = {ndma, repeat, (u8)channel, (u8)mode, (u8)wordSize, src, dst, words};
    }

    void NoteFlatWrite(u32 tracker, u32 offset, u32 size, bool changed, bool dma, u32 pc) noexcept
    {
        FlatWrites& w = Writes[tracker];
        if (!w.ARM9 && !w.DMA)
        {
            w.Min = offset;
            w.Max = offset + size - 1;
            w.FirstPC = pc;
            w.FirstByDMA = dma;
            w.FirstDMA = CurDMA;
        }
        if (dma) w.DMA++;
        else w.ARM9++;
        if (offset < w.Min) w.Min = offset;
        if (offset + size - 1 > w.Max) w.Max = offset + size - 1;
        if (changed)
        {
            w.Changed++;
            w.ChangedPages |= 1u << ((offset >> 9) & 31);
        }
        else
            w.Same++;
    }

    // GPU::MakeVRAMFlat_2DBG/OBJCoherent, each time, before the barrier: held is the lines it would draw
    void NoteFlatCheck(const GPU& gpu, const CaptureFlatDirty* dirty, u32 count, const GPU2D::LineJob* held, u32 numHeld, bool engineBNoHold) noexcept
    {
        s32 first = -1;
        bool bDirty = false;
        for (u32 i = 0; i < count; i++)
        {
            const CaptureFlatDirty& d = dirty[i];
            if (FirstBit(d) < 0)
                continue;

            // what no flush can be for without lines held. this check runs after the BG pass's, so
            // when that flushed there's nothing held here, and a dirty OBJ copy says nothing
            if (!numHeld && d.Tracker == CapTrack_AOBJ) { Frame.Dirty.ObjIdleA++; DirtyTotal.ObjIdleA++; }
            if (!numHeld && d.Tracker == CapTrack_BOBJ) { Frame.Dirty.ObjIdleB++; DirtyTotal.ObjIdleB++; }

            if (engineBNoHold && IsEngineB(d.Tracker))
            {
                bDirty = true;
                const FlatWrites& w = Writes[d.Tracker];
                Frame.Dirty.BChanged += w.Changed;
                Frame.Dirty.BSame += w.Same;
                DirtyTotal.BChanged += w.Changed;
                DirtyTotal.BSame += w.Same;
            }
            else if (first < 0)
                first = i;
        }
        if (bDirty && numHeld)
        {
            Frame.Dirty.BSkip++;
            DirtyTotal.BSkip++;
        }

        if (first >= 0)
        {
            NoteBarrier();
            if (numHeld)
                NoteFlatFlush(gpu, dirty, count, dirty[first], held, numHeld, engineBNoHold);
        }

        // the check consumes the dirty bits with or without a flush, so the writes start over
        for (u32 i = 0; i < count; i++)
            Writes[dirty[i].Tracker] = {};
    }

    // a barrier other than a read: in the row-precise world it would have flushed what's held
    void NoteBarrier() noexcept
    {
        if (ModelLines)
            ClearModel();
    }

    // GPU::Flush2DDeferred, before it draws anything
    void NoteFlush(u8 cause, u32 vcount, u32 held, bool maps) noexcept
    {
        CloseReadFlush();

        if (cause == CapFlush_ReadARM9 || cause == CapFlush_ReadDMA)
            ReadFlushOpen = true;
        else
            NoteBarrier();

        if (Frame.NumFlushes < 4)
            Frame.Flushes[Frame.NumFlushes] = {cause, maps, (u16)vcount, (u16)held};
        Frame.NumFlushes++;
        if (maps)
        {
            Frame.MapCause = cause;
            Frame.Mapped = true;
        }

        for (FlushStats* stats : {&FlushSecond, &FlushTotal})
        {
            FlushCauseStats& c = stats->Causes[cause];
            c.VMin = c.Count ? (c.VMin < vcount ? c.VMin : vcount) : vcount;
            c.VMax = c.Count ? (c.VMax > vcount ? c.VMax : vcount) : vcount;
            c.Count++;
            if (maps)
                c.Maps++;
        }
    }

    void NoteMapStart(s64 waitNs, s64 spinNs) noexcept
    {
        MapStartNs = NowNs();
        MapStartWaitNs = waitNs;
        MapStartSpinNs = spinNs;
    }

    void NoteMapEnd(s64 waitNs, s64 spinNs) noexcept
    {
        s64 ns = NowNs() - MapStartNs;
        Frame.MapNs += ns;
        Frame.WaitNs += waitNs - MapStartWaitNs;
        Frame.SpinNs += spinNs - MapStartSpinNs;
        FlushSecond.Causes[Frame.MapCause].MapNs += ns;
        FlushTotal.Causes[Frame.MapCause].MapNs += ns;
    }

    // DMA::Start and DSi_NDMA::Start, with the source in LCDC VRAM. a start mode that repeats
    // starts again each time it fires, so an HBlank one comes here once per line: those are added up
    void NoteDMA(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat, u32 vcount) noexcept
    {
        u32 bucket = ndma ? 8 : (mode & 0x7);
        FlushSecond.DMA[bucket]++;
        FlushTotal.DMA[bucket]++;

        Frame.DMAStarts++;
        for (u32 i = 0; i < Frame.NumDMAs; i++)
        {
            FrameDMA& dma = Frame.DMAs[i];
            if (dma.Channel == channel && dma.NDMA == ndma && dma.Mode == mode)
            {
                dma.Starts++;
                dma.TotalWords += words;
                dma.VLast = vcount;
                return;
            }
        }
        if (Frame.NumDMAs < 3)
            Frame.DMAs[Frame.NumDMAs++] = {ndma, repeat, (u8)channel, (u8)mode, (u8)wordSize, src, dst, words, 1, words, (u16)vcount, (u16)vcount};
    }

    static bool HasRow(const Slot& slot, u32 row) noexcept
    {
        return (slot.Rows[row >> 6] >> (row & 63)) & 1;
    }

    // ReadVRAM_LCDC. addr is relative to the bank
    void NoteRead(u32 bank, u32 addr, bool dma, u32 vcount) noexcept
    {
        u32 row = addr >> 9;
        bool captured = false;
        for (Slot& slot : Slots[bank])
        {
            if (!slot.Used || slot.Ended)
                continue;
            captured = true;
            slot.Flags |= dma ? Flag_DMA : Flag_ARM9;
            if (HasRow(slot, row))
                slot.Flags |= Flag_RowHit;
        }

        ReadFlushOpen = false;
        if (captured || ModelLines)
            TraceRead(bank, row, vcount);
    }

    // WriteVRAM_LCDC. addr is relative to the bank
    void NoteWrite(u32 bank, u32 addr) noexcept
    {
        u32 row = addr >> 9;
        for (Slot& slot : Slots[bank])
        {
            if (!slot.Used || slot.Ended)
                continue;
            slot.Flags |= Flag_Written;
            if (HasRow(slot, row))
                slot.Flags |= Flag_WrittenRow;
        }
    }

    // MapVRAM_AB/CD, with the new VRAMCNT value of banks A-D. a bank moving to LCDC needs nothing.
    // A and B have MST 0-3, C and D 0-4 and C can't reach OBJ or D BG on engine B; the other values
    // map the bank nowhere, and the palette mappings are only for banks E-I
    void NoteRemap(u32 bank, u8 cnt) noexcept
    {
        u32 flag = Flag_Off;
        if (cnt & 0x80)
        {
            if (bank < 2)
            {
                static constexpr u32 targets[4] = {0, Flag_ABG, Flag_AOBJ, Flag_Texture};
                flag = targets[cnt & 0x3];
            }
            else
            {
                u32 engineB = bank == 2 ? Flag_BBG : Flag_BOBJ;
                const u32 targets[8] = {0, Flag_ABG, Flag_ARM7, Flag_Texture, engineB, Flag_Off, Flag_Off, Flag_Off};
                flag = targets[cnt & 0x7];
            }
        }

        for (Slot& slot : Slots[bank])
        {
            if (slot.Used && !slot.Ended)
                slot.Flags |= flag;
        }
    }

    // a line in display mode 2 showing this bank's row
    void NoteDisplayLine(u32 bank, u32 row) noexcept
    {
        NoteRowUse(bank, row, Flag_Mode2 | Flag_Mode2Frame);
    }

    // a line DoCapture is about to write into `row` of the destination bank. srcBank is the bank
    // source B reads `srcRow` of, or -1. the source goes first: a line reads before it writes, so
    // a feedback capture reads the last capture's row
    void NoteCaptureLine(u32 line, u32 captureCnt, u32 row, s32 srcBank, u32 srcRow) noexcept
    {
        if (srcBank >= 0)
            NoteRowUse(srcBank, srcRow, Flag_SrcB | Flag_SrcBFrame);

        Frame.Capture = true;

        u32 bank = (captureCnt >> 16) & 0x3;
        u32 layout = captureCnt & 0x3F0000;
        if (!Cur || layout != CurLayout || line <= CurLine)
            BeginCapture(bank, captureCnt);
        CurLayout = layout;
        CurLine = line;

        // this row now belongs to this capture, and a capture left with no rows has ended
        for (Slot& slot : Slots[bank])
        {
            if (!slot.Used || slot.Ended || &slot == Cur || !HasRow(slot, row))
                continue;
            slot.Rows[row >> 6] &= ~(1ull << (row & 63));
            slot.Ended = !(slot.Rows[0] | slot.Rows[1] | slot.Rows[2] | slot.Rows[3]);
        }
        Cur->Rows[row >> 6] |= 1ull << (row & 63);
    }

    void NoteNoLCDCLine() noexcept
    {
        NoLCDCLines++;
    }

    // the compositor, once a frame it draws with hi-res capture on: the banks whose mode 2 lines
    // showed the hi-res copy, and the ones with lines left native. a frame with no answer counts
    // as native, as nothing else can have shown them
    void NoteHiresDisplay(u8 hires, u8 native) noexcept
    {
        Mode2HiMask |= hires;
        Mode2NatMask |= native;
    }

    // the compositor's capture pass, when it runs: the banks source B came from, the same way
    void NoteHiresSrcB(u8 hires, u8 native) noexcept
    {
        SrcBHiMask |= hires;
        SrcBNatMask |= native;
    }

    void NoteSavestate() noexcept
    {
        for (auto& bank : Slots)
        {
            for (Slot& slot : bank)
            {
                if (slot.Used && !slot.Ended)
                    slot.Flags |= Flag_State;
            }
        }
    }

    // GPU::Reset and a savestate load: VRAM isn't what the captures wrote anymore
    void DropAll() noexcept
    {
        for (auto& bank : Slots)
        {
            for (Slot& slot : bank)
            {
                if (slot.Used)
                    Dropped++;
                slot = {};
            }
        }
        Cur = nullptr;
        Mode2HiMask = Mode2NatMask = SrcBHiMask = SrcBNatMask = 0;
        Frame = {};
        ClearModel();
        ReadFlushOpen = false;
    }

    // GPU::FinishFrame, after the last line of the frame is drawn and the compositor has run
    void EndFrame() noexcept
    {
        Cur = nullptr;

        for (u32 bank = 0; bank < 4; bank++)
        {
            for (Slot& slot : Slots[bank])
            {
                if (!slot.Used)
                    continue;
                ApplyFrame(slot, bank);
                if (slot.Ended)
                    EndSlot(slot);
            }
        }
        Mode2HiMask = Mode2NatMask = SrcBHiMask = SrcBNatMask = 0;

        EndFrameTrace();

        s64 now = NowNs();
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;
        LogStartNs = now;
        LogLine();
    }

    // one line per second. the first part is that second, with the flags of the captures that ended
    // in it. the second part is the session so far: there the flags include the captures still
    // alive, while noread, ow, owrow, skipok and skip3d are ended captures only, and openunread is how many
    // alive ones have had no must-copy read yet. so the last line in a log is the final answer
    void LogLine() noexcept
    {
        Stats total = Total;
        u32 open = 0, openUnread = 0;
        for (auto& bank : Slots)
        {
            for (const Slot& slot : bank)
            {
                if (!slot.Used)
                    continue;
                open++;
                if (!(slot.Flags & Flags_MustCopy))
                    openUnread++;
                AddFlags(total, slot.Flags);
            }
        }

        char secondText[440], totalText[440];
        FormatStats(secondText, sizeof(secondText), Second);
        FormatStats(totalText, sizeof(totalText), total);
        Platform::Log(Platform::LogLevel::Info, "CAPREADS sec %s | total %s open=%u openunread=%u dropped=%u overflow=%u nolcdc=%u\n",
                      secondText, totalText, open, openUnread, Dropped, Overflow, NoLCDCLines);
        Second = {};

        // the total and dirty parts are the ones that matter on the last line, so they get more room
        char flushSecondText[220], flushTotalText[380], dirtyText[350];
        FormatFlushStats(flushSecondText, sizeof(flushSecondText), FlushSecond);
        FormatFlushStats(flushTotalText, sizeof(flushTotalText), FlushTotal);
        FormatDirtyStats(dirtyText, sizeof(dirtyText));
        Platform::Log(Platform::LogLevel::Info, "CAPFLUSH sec %s | total %s | %s\n", flushSecondText, flushTotalText, dirtyText);
        FlushSecond = {};
    }

    static const char* TrackerName(u32 tracker) noexcept
    {
        static const char* const names[CapTrack_Count] = {"abg", "abgx", "aobjx", "bbg", "bbgx", "bobjx", "aobj", "bobj"};
        return names[tracker];
    }

    static const char* CauseName(u32 cause) noexcept
    {
        static const char* const names[CapFlush_Count] = {
            "other", "read", "read9", "readdma", "write", "vramcnt", "pal", "oam", "flatbg", "flatobj",
            "line192", "line215", "lateblit", "state", "notheld", "full", "frameend", "powcnt", "halfcam",
        };
        return names[cause];
    }

private:
    static bool IsEngineB(u32 tracker) noexcept
    {
        return tracker >= CapTrack_BBG && tracker != CapTrack_AOBJ;
    }

    static s32 FirstBit(const CaptureFlatDirty& d) noexcept
    {
        for (u32 i = 0; i < d.Words; i++)
        {
            if (d.Bits[i])
                return i * 64 + __builtin_ctzll(d.Bits[i]);
        }
        return -1;
    }

    static s32 LastBit(const CaptureFlatDirty& d) noexcept
    {
        for (s32 i = d.Words - 1; i >= 0; i--)
        {
            if (d.Bits[i])
                return i * 64 + 63 - __builtin_clzll(d.Bits[i]);
        }
        return -1;
    }

    // whether any page of a range of bytes is set, wrapping at the copy's mask as the renderer does
    static bool RangeHits(const u64* bits, u32 base, u32 size, u32 mask) noexcept
    {
        u32 pages = (mask + 1) >> 9;
        u32 first = (base & mask) >> 9;
        u32 count = ((base & 0x1FF) + size + 0x1FF) >> 9;
        if (count > pages) count = pages;
        for (u32 i = 0; i < count; i++)
        {
            u32 page = (first + i) & (pages - 1);
            if ((bits[page >> 6] >> (page & 63)) & 1)
                return true;
        }
        return false;
    }

public:
    enum { Layer_None, Layer_Text, Layer_Affine, Layer_Extended, Layer_Large };

    // as SoftRenderer::DrawScanlineBGMode, Mode6 and Mode7 pick them. a 3D BG0 reads no VRAM
    static int LayerType(const GPU2D::UnitState& u, u32 layer, bool engineB) noexcept
    {
        if (!(u.LayerEnable & (1 << layer)))
            return Layer_None;

        u32 mode = u.DispCnt & 0x7;
        bool bg0Is3D = !engineB && (u.DispCnt & 0x8);
        if (mode == 6)
            return layer == 2 ? Layer_Large : Layer_None;
        if (mode == 7)
            return (layer == 1 || (layer == 0 && !bg0Is3D)) ? Layer_Text : Layer_None;

        switch (layer)
        {
        case 3: return mode >= 3 ? Layer_Extended : (mode >= 1 ? Layer_Affine : Layer_Text);
        case 2: return mode == 5 ? Layer_Extended : ((mode == 4 || mode == 2) ? Layer_Affine : Layer_Text);
        case 1: return Layer_Text;
        default: return bg0Is3D ? Layer_None : Layer_Text;
        }
    }

private:
    // a held line's BG pass reading a dirty page of the engine's BG copy. the whole of each layer, see the top
    static bool BGReads(const GPU2D::UnitState& u, bool engineB, const u64* bits, s8& layerOut, char& kindOut) noexcept
    {
        if (!u.Enabled || u.ForcedBlank)
            return false;

        const u32 mask = engineB ? 0x1FFFF : 0x7FFFF;
        for (u32 layer = 0; layer < 4; layer++)
        {
            int type = LayerType(u, layer, engineB);
            if (type == Layer_None)
                continue;

            u32 bgcnt = u.BGCnt[layer];
            u32 tiles = (bgcnt & 0x003C) << 12;
            u32 map = (bgcnt & 0x1F00) << 3;
            if (!engineB)
            {
                tiles += (u.DispCnt & 0x07000000) >> 8;
                map += (u.DispCnt & 0x38000000) >> 11;
            }
            u32 sizeBits = (bgcnt >> 14) & 0x3;

            u32 tilesSize = 0, mapSize = 0, bitmapBase = 0, bitmapSize = 0;
            switch (type)
            {
            case Layer_Text:
                tilesSize = (bgcnt & 0x0080) ? 0x10000 : 0x8000;
                mapSize = 0x800 << ((sizeBits & 1) + (sizeBits >> 1));
                break;
            case Layer_Affine:
                tilesSize = 0x4000;
                mapSize = 0x100 << (sizeBits * 2);
                break;
            case Layer_Extended:
                if (bgcnt & 0x0080)
                {
                    static constexpr u32 pixels[4] = {0x4000, 0x10000, 0x20000, 0x40000};
                    bitmapBase = (bgcnt & 0x1F00) << 6;
                    bitmapSize = pixels[sizeBits] * ((bgcnt & 0x0004) ? 2 : 1);
                }
                else
                {
                    tilesSize = 0x10000;
                    mapSize = 0x200 << (sizeBits * 2);
                }
                break;
            case Layer_Large:
                bitmapSize = mask + 1;
                break;
            }

            char kind = 0;
            if (mapSize && RangeHits(bits, map, mapSize, mask)) kind = 'm';
            else if (tilesSize && RangeHits(bits, tiles, tilesSize, mask)) kind = 't';
            else if (bitmapSize && RangeHits(bits, bitmapBase, bitmapSize, mask)) kind = 'b';
            if (kind)
            {
                layerOut = layer;
                kindOut = kind;
                return true;
            }
        }
        return false;
    }

    // a held line reading a dirty page of this tracker's copy
    static bool JobReads(const GPU2D::LineJob& job, u32 tracker, const u64* bits, s8& layerOut, char& kindOut) noexcept
    {
        bool engineB = IsEngineB(tracker);
        const GPU2D::UnitState& bg = job.BG[engineB ? 1 : 0];
        const GPU2D::UnitState& obj = job.OBJ[engineB ? 1 : 0];

        switch (tracker)
        {
        case CapTrack_ABG:
        case CapTrack_BBG:
            return job.DrawBG && BGReads(bg, engineB, bits, layerOut, kindOut);

        case CapTrack_ABGExtPal:
        case CapTrack_BBGExtPal:
            if (!job.DrawBG || !bg.Enabled || bg.ForcedBlank || !(bg.DispCnt & 0x40000000))
                return false;
            for (u32 layer = 0; layer < 4; layer++)
            {
                int type = LayerType(bg, layer, engineB);
                if (type != Layer_Text && type != Layer_Extended)
                    continue;
                u32 slot = (type == Layer_Text && layer < 2 && (bg.BGCnt[layer] & 0x2000)) ? (2 + layer) : layer;
                if (RangeHits(bits, slot * 0x2000, 0x2000, 0x7FFF))
                {
                    layerOut = layer;
                    kindOut = 'p';
                    return true;
                }
            }
            return false;

        case CapTrack_AOBJExtPal:
        case CapTrack_BOBJExtPal:
            if (job.DrawBG && bg.Enabled && !bg.ForcedBlank && (bg.LayerEnable & 0x10) && (bg.DispCnt & 0x80000000))
            {
                layerOut = 4;
                kindOut = 'p';
                return true;
            }
            return false;

        default:    // OBJ VRAM: tile by tile would need all of OAM, so any sprites on count
            if (job.DrawOBJ && obj.Enabled && obj.OBJEnable)
            {
                layerOut = 4;
                kindOut = 'o';
                return true;
            }
            return false;
        }
    }

    static u32 FirstDirtyBanks(const GPU& gpu, u32 tracker, u32 bit) noexcept
    {
        u32 offset = bit * 512;
        switch (tracker)
        {
        case CapTrack_ABG:        return gpu.VRAMMap_ABG[(offset >> 14) & 0x1F];
        case CapTrack_BBG:        return gpu.VRAMMap_BBG[(offset >> 14) & 0x7];
        case CapTrack_AOBJ:       return gpu.VRAMMap_AOBJ[(offset >> 14) & 0xF];
        case CapTrack_BOBJ:       return gpu.VRAMMap_BOBJ[(offset >> 14) & 0x7];
        case CapTrack_ABGExtPal:  return gpu.VRAMMap_ABGExtPal[(offset >> 13) & 0x3];
        case CapTrack_BBGExtPal:  return gpu.VRAMMap_BBGExtPal[(offset >> 13) & 0x3];
        case CapTrack_AOBJExtPal: return gpu.VRAMMap_AOBJExtPal;
        default:                  return gpu.VRAMMap_BOBJExtPal;
        }
    }

    void NoteFlatFlush(const GPU& gpu, const CaptureFlatDirty* dirty, u32 count, const CaptureFlatDirty& first, const GPU2D::LineJob* held, u32 numHeld, bool engineBNoHold) noexcept
    {
        DirtyEvent ev {};
        ev.VCount = gpu.VCount;
        ev.Held = numHeld;
        ev.Tracker = first.Tracker;
        ev.FirstBit = FirstBit(first);
        ev.LastBit = LastBit(first);
        ev.Banks = FirstDirtyBanks(gpu, first.Tracker, ev.FirstBit);
        ev.UsedLayer = -1;

        static constexpr u32 heights[4] = {128, 64, 128, 192};
        for (u32 i = 0; i < numHeld; i++)
        {
            const GPU2D::LineJob& job = held[i];
            if (!job.DrawBG)
                continue;
            const GPU2D::UnitState& a = job.BG[0];
            if (a.CaptureLatch && job.VCount < heights[(a.CaptureCnt >> 20) & 0x3])
                ev.HeldCapture++;
            if (a.Enabled && !a.ForcedBlank && (a.LayerEnable & 0xF))
                ev.ActiveA++;
            const GPU2D::UnitState& b = job.BG[1];
            if (b.Enabled && !b.ForcedBlank && (b.LayerEnable & 0xF))
                ev.ActiveB++;
        }

        bool engineA = false, engineB = false, writes = false;
        for (u32 i = 0; i < count; i++)
        {
            const CaptureFlatDirty& d = dirty[i];
            if (FirstBit(d) < 0 || (engineBNoHold && IsEngineB(d.Tracker)))
                continue;

            Frame.Dirty.Tracker[d.Tracker]++;
            if (IsEngineB(d.Tracker)) engineB = true;
            else engineA = true;

            const FlatWrites& w = Writes[d.Tracker];
            ev.ARM9 += w.ARM9;
            ev.DMA += w.DMA;
            ev.Changed += w.Changed;
            ev.Same += w.Same;
            if ((w.ARM9 || w.DMA) && !writes)
            {
                writes = true;
                ev.Min = w.Min;
                ev.Max = w.Max;
                ev.PC = w.FirstPC;
                ev.ByDMA = w.FirstByDMA;
                ev.Setup = w.FirstDMA;
            }

            // the dirty pages that may hold a new value. with no write seen the copy changed some
            // other way, so every dirty page counts as changed
            u64 changedBits[16];
            u64 pattern = (w.ARM9 || w.DMA) ? ((u64)w.ChangedPages | ((u64)w.ChangedPages << 32)) : ~0ull;
            for (u32 j = 0; j < d.Words; j++)
                changedBits[j] = d.Bits[j] & pattern;

            for (u32 j = 0; j < numHeld; j++)
            {
                s8 layer = -1;
                char kind = 0;
                if (!ev.Used && JobReads(held[j], d.Tracker, d.Bits, layer, kind))
                {
                    ev.Used = true;
                    ev.UsedLayer = layer;
                    ev.UsedKind = kind;
                }
                if (!ev.Effect && JobReads(held[j], d.Tracker, changedBits, layer, kind))
                    ev.Effect = true;
                if (ev.Used && ev.Effect)
                    break;
            }
        }

        // OBJ checks are the ones on the sprite trackers
        bool objCheck = first.Tracker == CapTrack_AOBJ || first.Tracker == CapTrack_BOBJ;
        FrameDirty& f = Frame.Dirty;
        if (!f.Events)
            f.First = ev;
        f.Events++;
        if (objCheck)
        {
            if (engineA) { f.OBJA++; DirtyTotal.OBJA++; }
            if (engineB) { f.OBJB++; DirtyTotal.OBJB++; }
        }
        else
        {
            if (engineA) f.BGA++;
            if (engineB) f.BGB++;
            if (engineA && engineB) DirtyTotal.BGBoth++;
            else if (engineA) DirtyTotal.BGA++;
            else DirtyTotal.BGB++;
        }

        if (ev.ARM9 && ev.DMA) { f.ByBoth++; DirtyTotal.ByBoth++; }
        else if (ev.ARM9) { f.ByARM9++; DirtyTotal.ByARM9++; }
        else if (ev.DMA) { f.ByDMA++; DirtyTotal.ByDMA++; }
        else { f.ByNone++; DirtyTotal.ByNone++; }

        f.Writes += ev.ARM9 + ev.DMA;
        f.Changed += ev.Changed;
        f.Same += ev.Same;
        DirtyTotal.Changed += ev.Changed;
        DirtyTotal.Same += ev.Same;
        if (ev.Used) { f.UsedYes++; DirtyTotal.UsedYes++; }
        else { f.UsedNo++; DirtyTotal.UsedNo++; }
        if (ev.Effect) { f.EffectYes++; DirtyTotal.EffectYes++; }
        else { f.EffectNo++; DirtyTotal.EffectNo++; }

        if (writes && ev.ByDMA)
            CountDMA(ev.Setup);
        else if (writes)
            CountPC(ev.PC);
    }

    void CountPC(u32 pc) noexcept
    {
        for (PCCount& entry : TopPCs)
        {
            if (entry.Count && entry.PC != pc)
                continue;
            entry.PC = pc;
            entry.Count++;
            return;
        }
        OtherPCs++;
    }

    void CountDMA(const DMASetup& setup) noexcept
    {
        for (DMACount& entry : TopDMAs)
        {
            if (entry.Count && (entry.Setup.Channel != setup.Channel || entry.Setup.Mode != setup.Mode || entry.Setup.NDMA != setup.NDMA))
                continue;
            if (!entry.Count)
                entry.Setup = setup;
            entry.Count++;
            return;
        }
        OtherDMAs++;
    }

    static s64 NowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void ClearModel() noexcept
    {
        for (auto& rows : ModelRows)
            rows[0] = rows[1] = rows[2] = rows[3] = 0;
        ModelLines = 0;
    }

    // a read that asked for a flush and never got to NoteRead: outside banks A-D, or a bank not in LCDC mode
    void CloseReadFlush() noexcept
    {
        if (!ReadFlushOpen)
            return;
        ReadFlushOpen = false;
        Frame.Stray++;
        FlushSecond.Stray++;
        FlushTotal.Stray++;
    }

    // one word read, judged as the comment at the top says
    void TraceRead(u32 bank, u32 row, u32 vcount) noexcept
    {
        const u64* rows = ModelRows[bank];
        if (!Frame.Reads)
        {
            Frame.FirstV = vcount;
            Frame.FirstBank = bank;
            Frame.FirstRow = Frame.MinRow = Frame.MaxRow = row;
            Frame.HeldFirst = Frame.HeldLast = -1;
            Frame.HeldLines = ModelLines;
            for (s32 r = 0; r < 256; r++)
            {
                if (!((rows[r >> 6] >> (r & 63)) & 1))
                    continue;
                if (Frame.HeldFirst < 0)
                    Frame.HeldFirst = r;
                Frame.HeldLast = r;
            }
        }
        Frame.Reads++;
        Frame.BankMask |= 1 << bank;
        if (bank == Frame.FirstBank)
        {
            if (row < Frame.MinRow) Frame.MinRow = row;
            if (row > Frame.MaxRow) Frame.MaxRow = row;
        }

        if (!ModelLines)
        {
            Frame.ReadNoHeld = true;
            return;
        }

        Frame.ReadHeld = true;
        if ((rows[row >> 6] >> (row & 63)) & 1)
        {
            if (!Frame.Overlap)
                Frame.OverlapV = vcount;
            Frame.Overlap = true;
            Frame.ModelFlushes++;
            ClearModel();
        }
    }

    void EndFrameTrace() noexcept
    {
        CloseReadFlush();

        const char* verdict = "none";
        u32 FlushStats::*counter = nullptr;
        if (Frame.Overlap) { verdict = "ovl"; counter = &FlushStats::Overlap; }
        else if (Frame.ReadHeld || Frame.Stray) { verdict = "dis"; counter = &FlushStats::Disjoint; }
        else if (Frame.Reads) { verdict = "noheld"; counter = &FlushStats::NoHeld; }
        else if (Frame.Capture) { verdict = "noread"; counter = &FlushStats::NoRead; }
        if (counter)
        {
            FlushSecond.*counter += 1;
            FlushTotal.*counter += 1;
        }

        if (Frame.Capture || Frame.Reads || Frame.DMAStarts)
        {
            TraceFrames++;
            if (TraceLines < TraceFirstLines || (TraceFrames % TraceEvery) == 0)
            {
                s64 now = NowNs();
                if (now - TraceSecondNs >= 1000000000)
                {
                    TraceSecondNs = now;
                    TraceSecondLines = 0;
                }
                if (TraceSecondLines < TraceLinesPerSecond)
                {
                    TraceSecondLines++;
                    TraceLines++;
                    LogFrame(verdict);
                    if (Frame.Dirty.Events || Frame.Dirty.BSkip || Frame.Dirty.ObjIdleA || Frame.Dirty.ObjIdleB)
                        LogDirty();
                }
            }
        }

        LastFrame = Frame;
        LastVerdict = verdict;
        Frame = {};
        ClearModel();
    }

    template <typename... Args>
    static void Append(char*& text, const char* end, const char* format, Args... args) noexcept
    {
        if (text >= end)
            return;
        int length = snprintf(text, end - text, format, args...);
        if (length > 0)
            text = (length < end - text) ? text + length : (char*)end;
    }

    // a flush is cause@VCount/held lines, with "m" when it maps the readback. map_us is the time
    // spent in that map; wait_us and spin_us are CAPWAIT's numbers for it (0 outside the compute
    // renderer in debug builds). "reads" is words of a captured bank, first at VCount/bank/row, then
    // the row range in that bank, then what the row-precise barrier would hold at the first read
    void LogFrame(const char* verdict) const noexcept
    {
        char text[860];
        char* p = text;
        const char* end = text + sizeof(text);

        Append(p, end, "f=%u cap=%d flushes=%u", TraceFrames, Frame.Capture ? 1 : 0, Frame.NumFlushes);
        u32 listed = Frame.NumFlushes < 4 ? Frame.NumFlushes : 4;
        for (u32 i = 0; i < listed; i++)
        {
            const FrameFlush& flush = Frame.Flushes[i];
            Append(p, end, " %s@%u/%u%s", CauseName(flush.Cause), flush.VCount, flush.Held, flush.Maps ? "m" : "");
        }

        if (Frame.Mapped)
            Append(p, end, " map=%s map_us=%lld wait_us=%lld spin_us=%lld", CauseName(Frame.MapCause),
                   (long long)(Frame.MapNs / 1000), (long long)(Frame.WaitNs / 1000), (long long)(Frame.SpinNs / 1000));
        else
            Append(p, end, "%s", " map=none");

        if (Frame.Reads)
        {
            Append(p, end, " reads=%u first=v%u/%c/r%u rows=%u-%u banks=%X", Frame.Reads, Frame.FirstV, 'A' + Frame.FirstBank,
                   Frame.FirstRow, Frame.MinRow, Frame.MaxRow, Frame.BankMask);
            if (Frame.HeldFirst >= 0)
                Append(p, end, " held=%d-%d/%u", Frame.HeldFirst, Frame.HeldLast, Frame.HeldLines);
            else
                Append(p, end, " held=none/%u", Frame.HeldLines);
        }
        else
            Append(p, end, "%s", " reads=0");
        if (Frame.Stray)
            Append(p, end, " stray=%u", Frame.Stray);

        Append(p, end, " verdict=%s", verdict);
        if (Frame.Overlap)
            Append(p, end, " ovlv=%u mflush=%u", Frame.OverlapV, Frame.ModelFlushes);

        if (Frame.DMAStarts)
            Append(p, end, " dmastarts=%u", Frame.DMAStarts);
        for (u32 i = 0; i < Frame.NumDMAs; i++)
        {
            const FrameDMA& dma = Frame.DMAs[i];
            if (dma.NDMA)
                Append(p, end, " ndma%u mode=%02X", dma.Channel, dma.Mode);
            else
                Append(p, end, " dma%u mode=%s", dma.Channel, DMAModeName(dma.Mode));
            Append(p, end, " src=%08X dst=%08X n=%u w=%u rep=%d v=%u-%u starts=%u total=%u", dma.Src, dma.Dst, dma.Words,
                   dma.WordSize, dma.Repeat ? 1 : 0, dma.VFirst, dma.VLast, dma.Starts, dma.TotalWords);
        }

        Platform::Log(Platform::LogLevel::Info, "CAPTRACE %s\n", text);
    }

    // first=... is the frame's first flat flush: the tracker, its dirty pages and what they are in VRAM,
    // the bank mask behind them, the lines held at it (all, capturing, with a BG on engine A and B), who
    // wrote since the last check, and the used and effect estimates. frame=... adds up all of them
    void LogDirty() const noexcept
    {
        char text[700];
        char* p = text;
        const char* end = text + sizeof(text);
        const FrameDirty& f = Frame.Dirty;
        const DirtyEvent& ev = f.First;

        if (!f.Events)
        {
            Platform::Log(Platform::LogLevel::Info, "CAPDIRTY f=%u flushes=0 bskip=%u bchg=%u bsame=%u objidle=a%u,b%u\n",
                          TraceFrames, f.BSkip, f.BChanged, f.BSame, f.ObjIdleA, f.ObjIdleB);
            return;
        }

        static constexpr u32 bases[CapTrack_Count] = {0x06000000, 0, 0, 0x06200000, 0, 0, 0x06400000, 0x06600000};
        u32 base = bases[ev.Tracker];
        Append(p, end, "f=%u flushes=%u bgA=%u bgB=%u objA=%u objB=%u first v=%u trk=%s bits=%u-%u addr=%08X-%08X banks=%X held=%u cap=%u actA=%u actB=%u",
               TraceFrames, f.Events, f.BGA, f.BGB, f.OBJA, f.OBJB, ev.VCount, TrackerName(ev.Tracker), ev.FirstBit, ev.LastBit,
               base + ev.FirstBit * 512, base + ev.LastBit * 512 + 511, ev.Banks, ev.Held, ev.HeldCapture, ev.ActiveA, ev.ActiveB);

        if (!ev.ARM9 && !ev.DMA)
            Append(p, end, "%s", " who=none");
        else if (ev.ByDMA)
            Append(p, end, " who=%s%u/%s src=%08X dst=%08X n=%u w=%u rep=%d", ev.Setup.NDMA ? "ndma" : "dma", ev.Setup.Channel, ev.Setup.NDMA ? "n" : DMAModeName(ev.Setup.Mode),
                   ev.Setup.Src, ev.Setup.Dst, ev.Setup.Words, ev.Setup.WordSize, ev.Setup.Repeat ? 1 : 0);
        else
            Append(p, end, " who=arm9 pc=%08X", ev.PC);
        if (ev.ARM9 || ev.DMA)
            Append(p, end, " writes=%u+%u chg=%u same=%u bytes=%05X-%05X", ev.ARM9, ev.DMA, ev.Changed, ev.Same, ev.Min, ev.Max);

        if (ev.Used)
            Append(p, end, " used=yes by=%d%c", ev.UsedLayer, ev.UsedKind);
        else
            Append(p, end, "%s", " used=no");
        Append(p, end, " effect=%s frame arm9=%u dma=%u both=%u none=%u writes=%u chg=%u same=%u used=%u/%u effect=%u/%u bskip=%u bchg=%u bsame=%u objidle=a%u,b%u",
               ev.Effect ? "yes" : "no", f.ByARM9, f.ByDMA, f.ByBoth, f.ByNone, f.Writes, f.Changed, f.Same, f.UsedYes, f.UsedNo, f.EffectYes, f.EffectNo,
               f.BSkip, f.BChanged, f.BSame, f.ObjIdleA, f.ObjIdleB);

        Platform::Log(Platform::LogLevel::Info, "CAPDIRTY %s\n", text);
    }

    // the running totals of the flat flushes: per engine, by who wrote since the check, the writes'
    // values, the used and effect estimates, and the PCs and DMAs behind the most flushes
    void FormatDirtyStats(char* text, size_t size) const noexcept
    {
        char* p = text;
        const char* end = text + size;
        const DirtyStats& s = DirtyTotal;

        Append(p, end, "dirty bg=a%u,b%u,ab%u obj=a%u,b%u objidle=a%u,b%u who=arm9:%u,dma:%u,both:%u,none:%u chg=%llu same=%llu used=y%u,n%u effect=y%u,n%u bskip=%u bchg=%llu bsame=%llu",
               s.BGA, s.BGB, s.BGBoth, s.OBJA, s.OBJB, s.ObjIdleA, s.ObjIdleB, s.ByARM9, s.ByDMA, s.ByBoth, s.ByNone,
               (unsigned long long)s.Changed, (unsigned long long)s.Same, s.UsedYes, s.UsedNo, s.EffectYes, s.EffectNo,
               s.BSkip, (unsigned long long)s.BChanged, (unsigned long long)s.BSame);

        PCCount pcs[16];
        memcpy(pcs, TopPCs, sizeof(pcs));
        std::sort(pcs, pcs + 16, [](const PCCount& x, const PCCount& y) { return x.Count > y.Count; });
        Append(p, end, "%s", " pcs");
        for (u32 i = 0; i < 3 && pcs[i].Count; i++)
            Append(p, end, " %08X:%u", pcs[i].PC, pcs[i].Count);
        if (OtherPCs)
            Append(p, end, " more:%u", OtherPCs);

        DMACount dmas[8];
        memcpy(dmas, TopDMAs, sizeof(dmas));
        std::sort(dmas, dmas + 8, [](const DMACount& x, const DMACount& y) { return x.Count > y.Count; });
        Append(p, end, "%s", " dmas");
        for (u32 i = 0; i < 2 && dmas[i].Count; i++)
        {
            const DMASetup& d = dmas[i].Setup;
            Append(p, end, " %s%u/%s/%08X>%08X/%u:%u", d.NDMA ? "n" : "d", d.Channel, d.NDMA ? "n" : DMAModeName(d.Mode), d.Src, d.Dst, d.Words, dmas[i].Count);
        }
        if (OtherDMAs)
            Append(p, end, " more:%u", OtherDMAs);
    }

    static const char* DMAModeName(u32 mode) noexcept
    {
        static const char* const names[8] = {"imm", "vbl", "hbl", "disp", "fifo", "cart", "gba", "gx"};
        return names[mode & 0x7];
    }

    // only the causes that happened, each as count/maps@VCount range/time in those maps. the rest of
    // the causes are cut off with "..." rather than running past the log's line limit
    static void FormatFlushStats(char* text, size_t size, const FlushStats& s) noexcept
    {
        char* p = text;
        const char* end = text + size;
        const char* stop = end - 96;    // room for the frames and DMA parts

        Append(p, end, "%s", "flush");
        bool any = false;
        for (u32 cause = 0; cause < CapFlush_Count; cause++)
        {
            const FlushCauseStats& c = s.Causes[cause];
            if (!c.Count)
                continue;
            any = true;
            if (p >= stop - 44)
            {
                Append(p, end, "%s", " ...");
                break;
            }
            Append(p, end, " %s=%u/%u@%u-%u/%.1fms", CauseName(cause), c.Count, c.Maps, c.VMin, c.VMax, c.MapNs / 1e6);
        }
        if (!any)
            Append(p, end, "%s", " none");

        Append(p, end, " frames dis=%u ovl=%u noheld=%u noread=%u stray=%u dma", s.Disjoint, s.Overlap, s.NoHeld, s.NoRead, s.Stray);
        for (u32 mode = 0; mode < 8; mode++)
        {
            if (s.DMA[mode])
                Append(p, end, " %s=%u", DMAModeName(mode), s.DMA[mode]);
        }
        if (s.DMA[8])
            Append(p, end, " ndma=%u", s.DMA[8]);
    }

    void NoteRowUse(u32 bank, u32 row, u32 flags) noexcept
    {
        for (Slot& slot : Slots[bank])
        {
            if (slot.Used && !slot.Ended && HasRow(slot, row))
                slot.Flags |= flags;
        }
    }

    void BeginCapture(u32 bank, u32 captureCnt) noexcept
    {
        Slot* free = nullptr;
        for (Slot& slot : Slots[bank])
        {
            if (!slot.Used)
            {
                free = &slot;
                break;
            }
        }
        if (!free)
        {
            free = &Slots[bank][0];
            ApplyFrame(*free, bank);
            EndSlot(*free);
            Overflow++;
        }

        *free = {};
        free->Used = true;
        if (((captureCnt >> 29) & 0x3) == 1)
            free->Flags |= Flag_SrcBOnly;
        Cur = free;

        Second.Captures++;
        Total.Captures++;
    }

    void ApplyFrame(Slot& slot, u32 bank) const noexcept
    {
        u32 bit = 1 << bank;
        if (slot.Flags & Flag_Mode2Frame)
            slot.Flags |= ((Mode2NatMask & bit) || !(Mode2HiMask & bit)) ? Flag_Mode2Nat : Flag_Mode2Hi;
        if (slot.Flags & Flag_SrcBFrame)
            slot.Flags |= ((SrcBNatMask & bit) || !(SrcBHiMask & bit)) ? Flag_SrcBNat : Flag_SrcBHi;
        slot.Flags &= ~(Flag_Mode2Frame | Flag_SrcBFrame);
    }

    static void AddFlags(Stats& stats, u32 flags) noexcept
    {
        auto add = [&](u32& counter, u32 flag) { if (flags & flag) counter++; };
        add(stats.ARM9, Flag_ARM9);
        add(stats.DMA, Flag_DMA);
        add(stats.RowHit, Flag_RowHit);
        add(stats.Remap, Flag_ABG | Flag_AOBJ | Flag_BBG | Flag_BOBJ | Flag_Texture | Flag_ARM7);
        add(stats.ABG, Flag_ABG);
        add(stats.AOBJ, Flag_AOBJ);
        add(stats.BBG, Flag_BBG);
        add(stats.BOBJ, Flag_BOBJ);
        add(stats.Texture, Flag_Texture);
        add(stats.ARM7, Flag_ARM7);
        add(stats.Off, Flag_Off);
        add(stats.Mode2, Flag_Mode2);
        add(stats.Mode2Nat, Flag_Mode2Nat);
        add(stats.SrcB, Flag_SrcB);
        add(stats.SrcBNat, Flag_SrcBNat);
        add(stats.State, Flag_State);
        add(stats.SrcBOnly, Flag_SrcBOnly);
        // always hi-res, so the GPU could have done all of this capture's uses of that kind
        if ((flags & Flag_Mode2Hi) && !(flags & Flag_Mode2Nat))
            stats.Mode2Hi++;
        if ((flags & Flag_SrcBHi) && !(flags & Flag_SrcBNat))
            stats.SrcBHi++;
    }

    // noread: no must-copy read. skipok: lazy capture would never have had to copy it, with no
    // must-copy read, no savestate, no native mode 2 or source B and no CPU write into its rows.
    // skip3d, the headline number: the skipok ones that would map the 3D readback, which is the
    // blocking wait lazy capture is meant to remove. a source B only capture never maps it
    void EndSlot(Slot& slot) noexcept
    {
        for (Stats* stats : {&Second, &Total})
        {
            stats->Ended++;
            AddFlags(*stats, slot.Flags);
            if (!(slot.Flags & Flags_MustCopy))
            {
                stats->NoRead++;
                if (slot.Flags & Flag_Written)
                    stats->Overwritten++;
                if (slot.Flags & Flag_WrittenRow)
                    stats->OverwrittenRow++;
                if (!(slot.Flags & (Flag_State | Flag_Mode2Nat | Flag_SrcBNat | Flag_WrittenRow)))
                {
                    stats->SkipOK++;
                    if (!(slot.Flags & Flag_SrcBOnly))
                        stats->Skip3D++;
                }
            }
        }
        slot = {};
    }

    static void FormatStats(char* text, size_t size, const Stats& s) noexcept
    {
        snprintf(text, size,
                 "caps=%u ended=%u noread=%u ow=%u owrow=%u arm9=%u dma=%u rowhit=%u "
                 "remap=%u abg=%u aobj=%u bbg=%u bobj=%u tex=%u arm7=%u off=%u "
                 "mode2=%u m2hi=%u m2nat=%u srcb=%u sbhi=%u sbnat=%u state=%u bonly=%u skipok=%u skip3d=%u",
                 s.Captures, s.Ended, s.NoRead, s.Overwritten, s.OverwrittenRow, s.ARM9, s.DMA, s.RowHit,
                 s.Remap, s.ABG, s.AOBJ, s.BBG, s.BOBJ, s.Texture, s.ARM7, s.Off,
                 s.Mode2, s.Mode2Hi, s.Mode2Nat, s.SrcB, s.SrcBHi, s.SrcBNat, s.State, s.SrcBOnly, s.SkipOK, s.Skip3D);
    }
};

}

#endif
