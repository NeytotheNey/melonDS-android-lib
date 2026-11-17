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

#include <string.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "NDS.h"
#include "GPU.h"

#include "ARMJIT.h"

#include "GPU2D_Soft.h"
#include "GPU3D.h"
#include "GPU_CaptureReads.h"
#include "GPU_2DStats.h"
#include "GPU_Shadow2D.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

#define LINE_CYCLES  (355*6)
#define HBLANK_CYCLES (48+(256*6))
#define FRAME_CYCLES  (LINE_CYCLES * 263)

enum
{
    LCD_StartHBlank = 0,
    LCD_StartScanline,
    LCD_FinishFrame,
};

// threaded 2D (how it works is explained above GPU::SetThreaded2D)

// a frame queues 193 jobs at most (line 262's sprites, usually drawn inline instead, then lines 0-191) before the frame-end
// barrier, so the ring is only full if a game rewrites VCount to stay in the visible lines. with HeldTo2DThread a frame
// that holds its lines can queue 384 (their engine B parts, then the rest at line 192), and the last of them wait for a slot.
// with bit 16, Jobs2DEnd can be ahead of Jobs2DQueued: the jobs in between are in the ring, but the 2D thread doesn't
// know yet. a wait for any of them shows them first or draws them (Wait2DJobs); a wait for earlier jobs only, such as
// a slot or a palette copy, leaves them hidden, which is fine: nothing waits for them then
static constexpr u32 Ring2DSize = 256;

// flags next to a ring slot's engines (Threaded2DState::JobEngines), for the 2D thread's spin. with
// HeldTo2DThread bit 16: a held line's engine B part, published in batches, so the 2D thread sleeps
// soon once it's drawn them, and reading the clock less often while spinning
static constexpr u32 Job2DBatched = 1 << 8;
static constexpr u32 Job2DSpinLess = 1 << 9;
static constexpr u32 Batch2DLines = 16;

// HeldTo2DThread bit 32: when the emulator thread had to wait for line 192's queued lines before
// the Blit (a game reading its capture in VBlank, say) in HeldEarlyWaits of the last 8 frames that
// queued them, the next HeldBackoffFrames frames that would queue them draw them at line 192 instead
static constexpr u32 HeldEarlyWaits = 2;
static constexpr u32 HeldBackoffFrames = 120;

// copies of palette and OAM that queued jobs read (see Start2DJob). a new one is only made after
// a CPU write, so a game that writes them once a frame uses two at a time
static constexpr u32 PalOAMCopies = 32;

// how long each side spins before it sleeps. while a frame is drawn, lines arrive every few dozen
// microseconds, so the 2D thread spins through the gaps (waking it costs the emulator thread a
// syscall per line). after line 191 it has nothing to do until the next frame starts (line 262's
// sprites are usually drawn inline), so it sleeps soon.
// the barrier waits for lines that are being drawn, usually only one or two
static constexpr u32 Spin2DWorkFrameUs = 100;
static constexpr u32 Spin2DWorkVBlankUs = 10;
// experiment ovh3 (10-29): hardware counters on the Z Fold showed the emulator thread's effective
// clock dropping from 1.26 GHz to 1.0 GHz with threaded 2D on, because it sleeps on the barrier
// about two extra times a frame, and a thread that idles in short bursts gets 20-30% less clock
// than the governor reports. the frame-end wait measured 0.8-1.06 ms at line 192, so spinning
// through it keeps the core awake. with LateBlit it moves to line 215 and mostly overlaps VBlank
// emulation. the right value is still to be tuned
static constexpr u32 Spin2DSyncUs = 2000;

// only exists while threaded 2D is on
struct GPU::Threaded2DState
{
    std::unique_ptr<GPU2D::LineJob[]> Jobs = std::make_unique<GPU2D::LineJob[]>(Ring2DSize);
    // what each slot draws: its own job with both engines, or with HeldTo2DThread a part of a held
    // job in Deferred2DJobs
    const GPU2D::LineJob* JobPtr[Ring2DSize] {};
    u32 JobEngines[Ring2DSize] {};
    // the thread that last put a job in without showing it (HeldTo2DThread bit 16), the only one
    // that may take hidden jobs back (Wait2DJobs)
    std::thread::id HiddenBy;
    Platform::Thread* Thread = nullptr;
    std::atomic<bool> Stopping {false};

    std::mutex Mutex;
    std::condition_variable WorkWake;   // a job was queued, or the thread should stop
    std::condition_variable SyncWake;   // the sleepers' target count of jobs was drawn

    // each thread reads the other's flags after every job, so the 2D thread's flag and the
    // emulator thread's are on cache lines of their own, away from the mutex and condvars that
    // both threads write when they sleep or wake (the struct's alignment pads the last line)
    alignas(64) std::atomic<bool> WorkSleeping {false};
    // a count, in case a barrier on another thread (a savestate) ever waits at the same time
    alignas(64) std::atomic<int> SyncSleepers {0};
    // the job count the sleepers wait for (the lowest one, if there are several). only written
    // under the mutex, by sleepers; see Wait2DJobs
    std::atomic<u32> SyncTarget {0};

    // emulator thread only: the palette and OAM copies, used in turn. PalOAMUntil is the
    // Jobs2DDone count at which the last job reading a copy is drawn
    struct PalOAMCopy
    {
        alignas(u64) u8 Palette[2*1024];
        alignas(u64) u8 OAM[2*1024];
    };
    PalOAMCopy PalOAM[PalOAMCopies];
    u32 PalOAMUntil[PalOAMCopies] {};
    u32 PalOAMCur = 0;
};


/*
    VRAM invalidation tracking

    - we want to know when a VRAM region used for graphics changed
    - for some regions unmapping is mandatory to modify them (Texture, TexPal and ExtPal) and
        we don't want to completely invalidate them every time they're unmapped and remapped

    For this reason we don't track the dirtyness per mapping region, but instead per VRAM bank
    with VRAMDirty.

    This is more or less a description of VRAMTrackingSet::DeriveState
        Each time before the memory is read two things could have happened
        to each 16kb piece (16kb is the smallest unit in which mappings can
        be made thus also the size VRAMMap_* use):
            - this piece was remapped compared to last time we checked,
                which means this location in memory is invalid.
            - this piece wasn't remapped, which means we need to check whether
                it was changed. This can be archived by checking VRAMDirty.
                VRAMDirty need to be reset for the respective VRAM bank.
*/

GPU::GPU(melonDS::NDS& nds, std::unique_ptr<Renderer3D>&& renderer3d, std::unique_ptr<GPU2D::Renderer2D>&& renderer2d) noexcept :
    NDS(nds),
    GPU2D_A(0, *this),
    GPU2D_B(1, *this),
    GPU3D(nds, renderer3d ? std::move(renderer3d) : std::make_unique<SoftRenderer>()),
    GPU2D_Renderer(renderer2d ? std::move(renderer2d) : std::make_unique<GPU2D::SoftRenderer>(*this))
{
    NDS.RegisterEventFuncs(Event_LCD, this,
    {
            MakeEventThunk(GPU, StartHBlank),
            MakeEventThunk(GPU, StartScanline),
            MakeEventThunk(GPU, FinishFrame)
    });
    NDS.RegisterEventFuncs(Event_DisplayFIFO, this, {MakeEventThunk(GPU, DisplayFIFO)});

    InitFramebuffers();
}

GPU::~GPU() noexcept
{
    // All unique_ptr fields are automatically cleaned up, but the 2D thread must stop first
    Stop2DThread();

    NDS.UnregisterEventFuncs(Event_LCD);
    NDS.UnregisterEventFuncs(Event_DisplayFIFO);
}

void GPU::ResetVRAMCache() noexcept
{
    for (int i = 0; i < 9; i++)
        VRAMDirty[i] = NonStupidBitField<128*1024/VRAMDirtyGranularity>();

    VRAMDirty_ABG.Reset();
    VRAMDirty_BBG.Reset();
    VRAMDirty_AOBJ.Reset();
    VRAMDirty_BOBJ.Reset();
    VRAMDirty_ABGExtPal.Reset();
    VRAMDirty_BBGExtPal.Reset();
    VRAMDirty_AOBJExtPal.Reset();
    VRAMDirty_BOBJExtPal.Reset();
    VRAMDirty_Texture.Reset();
    VRAMDirty_TexPal.Reset();

    memset(VRAMFlat_ABG, 0, sizeof(VRAMFlat_ABG));
    memset(VRAMFlat_BBG, 0, sizeof(VRAMFlat_BBG));
    memset(VRAMFlat_AOBJ, 0, sizeof(VRAMFlat_AOBJ));
    memset(VRAMFlat_BOBJ, 0, sizeof(VRAMFlat_BOBJ));
    memset(VRAMFlat_ABGExtPal, 0, sizeof(VRAMFlat_ABGExtPal));
    memset(VRAMFlat_BBGExtPal, 0, sizeof(VRAMFlat_BBGExtPal));
    memset(VRAMFlat_AOBJExtPal, 0, sizeof(VRAMFlat_AOBJExtPal));
    memset(VRAMFlat_BOBJExtPal, 0, sizeof(VRAMFlat_BOBJExtPal));
    memset(VRAMFlat_Texture, 0, sizeof(VRAMFlat_Texture));
    memset(VRAMFlat_TexPal, 0, sizeof(VRAMFlat_TexPal));
}

void GPU::Reset() noexcept
{
    Sync2D();
    CaptureHeldThisFrame = false;

    VCount = 0;
    NextVCount = -1;
    TotalScanlines = 0;

    DispStat[0] = 0;
    DispStat[1] = 0;
    VMatch[0] = 0;
    VMatch[1] = 0;

    memset(Palette, 0, 2*1024);
    memset(OAM, 0, 2*1024);
    PalOAMWritten = true;

    memset(VRAM_A, 0, 128*1024);
    memset(VRAM_B, 0, 128*1024);
    memset(VRAM_C, 0, 128*1024);
    memset(VRAM_D, 0, 128*1024);
    memset(VRAM_E, 0,  64*1024);
    memset(VRAM_F, 0,  16*1024);
    memset(VRAM_G, 0,  16*1024);
    memset(VRAM_H, 0,  32*1024);
    memset(VRAM_I, 0,  16*1024);

    memset(VRAMCNT, 0, 9);
    VRAMSTAT = 0;

    VRAMMap_LCDC = 0;

    memset(VRAMMap_ABG, 0, sizeof(VRAMMap_ABG));
    memset(VRAMMap_AOBJ, 0, sizeof(VRAMMap_AOBJ));
    memset(VRAMMap_BBG, 0, sizeof(VRAMMap_BBG));
    memset(VRAMMap_BOBJ, 0, sizeof(VRAMMap_BOBJ));

    memset(VRAMMap_ABGExtPal, 0, sizeof(VRAMMap_ABGExtPal));
    VRAMMap_AOBJExtPal = 0;
    memset(VRAMMap_BBGExtPal, 0, sizeof(VRAMMap_BBGExtPal));
    VRAMMap_BOBJExtPal = 0;

    memset(VRAMMap_Texture, 0, sizeof(VRAMMap_Texture));
    memset(VRAMMap_TexPal, 0, sizeof(VRAMMap_TexPal));

    VRAMMap_ARM7[0] = 0;
    VRAMMap_ARM7[1] = 0;

    memset(VRAMPtr_ABG, 0, sizeof(VRAMPtr_ABG));
    memset(VRAMPtr_AOBJ, 0, sizeof(VRAMPtr_AOBJ));
    memset(VRAMPtr_BBG, 0, sizeof(VRAMPtr_BBG));
    memset(VRAMPtr_BOBJ, 0, sizeof(VRAMPtr_BOBJ));

    size_t fbsize;
    if (GPU3D.UsesAccelLines())
        fbsize = GPU2D::AccelFramebufferWords;
    else
        fbsize = 256 * 192;

    for (size_t i = 0; i < fbsize; i++)
    {
        Framebuffer[0][0][i] = 0xFFFFFFFF;
        Framebuffer[1][0][i] = 0xFFFFFFFF;
    }
    for (size_t i = 0; i < fbsize; i++)
    {
        Framebuffer[0][1][i] = 0xFFFFFFFF;
        Framebuffer[1][1][i] = 0xFFFFFFFF;
    }

    GPU2D_A.Reset();
    GPU2D_B.Reset();
    GPU3D.Reset();

    int backbuf = FrontBuffer ? 0 : 1;
    GPU2D_Renderer->SetFramebuffer(Framebuffer[backbuf][1].get(), Framebuffer[backbuf][0].get());

    ResetVRAMCache();
    ResetHiresCapture();
    ResetHeldBackoff();
    if (CapReads)
        CapReads->DropAll();
    if (Stats2D)
        Stats2D->Reset();
    if (Shadow2D)
    {
        Shadow2D->Reset();
        // the state blocks start out as no frame's (the fill above made them all ones)
        if (GPU3D.UsesAccelLines())
        {
            for (int buf = 0; buf < 2; buf++)
                for (int screen = 0; screen < 2; screen++)
                    memset(&Framebuffer[buf][screen][GPU2D::AccelShadowStateStart], 0, GPU2D::ShadowStateWords * 192 * 4);
        }
    }

    OAMDirty = 0x3;
    PaletteDirty = 0xF;

    Hash2DFrame = 0;
}

void GPU::Stop() noexcept
{
    Sync2D();

    int fbsize;
    if (GPU3D.UsesAccelLines())
        fbsize = GPU2D::AccelFramebufferWords;
    else
        fbsize = 256 * 192;

    memset(Framebuffer[0][0].get(), 0, fbsize*4);
    memset(Framebuffer[0][1].get(), 0, fbsize*4);
    memset(Framebuffer[1][0].get(), 0, fbsize*4);
    memset(Framebuffer[1][1].get(), 0, fbsize*4);

    GPU3D.Stop(*this);
}

void GPU::DoSavestate(Savestate* file) noexcept
{
    Sync2D(CapFlush_Savestate);

    if (CapReads && file->Saving)
        CapReads->NoteSavestate();

    file->Section("GPUG");

    file->Var16(&VCount);
    file->Var32(&NextVCount);
    file->Var16(&TotalScanlines);

    file->Var16(&DispStat[0]);
    file->Var16(&DispStat[1]);
    file->Var16(&VMatch[0]);
    file->Var16(&VMatch[1]);

    file->VarArray(Palette, 2*1024);
    file->VarArray(OAM, 2*1024);

    file->VarArray(VRAM_A, 128*1024);
    file->VarArray(VRAM_B, 128*1024);
    file->VarArray(VRAM_C, 128*1024);
    file->VarArray(VRAM_D, 128*1024);
    file->VarArray(VRAM_E,  64*1024);
    file->VarArray(VRAM_F,  16*1024);
    file->VarArray(VRAM_G,  16*1024);
    file->VarArray(VRAM_H,  32*1024);
    file->VarArray(VRAM_I,  16*1024);

    file->VarArray(VRAMCNT, 9);
    file->Var8(&VRAMSTAT);

    file->Var32(&VRAMMap_LCDC);

    file->VarArray(VRAMMap_ABG, sizeof(VRAMMap_ABG));
    file->VarArray(VRAMMap_AOBJ, sizeof(VRAMMap_AOBJ));
    file->VarArray(VRAMMap_BBG, sizeof(VRAMMap_BBG));
    file->VarArray(VRAMMap_BOBJ, sizeof(VRAMMap_BOBJ));

    file->VarArray(VRAMMap_ABGExtPal, sizeof(VRAMMap_ABGExtPal));
    file->Var32(&VRAMMap_AOBJExtPal);
    file->VarArray(VRAMMap_BBGExtPal, sizeof(VRAMMap_BBGExtPal));
    file->Var32(&VRAMMap_BOBJExtPal);

    file->VarArray(VRAMMap_Texture, sizeof(VRAMMap_Texture));
    file->VarArray(VRAMMap_TexPal, sizeof(VRAMMap_TexPal));

    file->Var32(&VRAMMap_ARM7[0]);
    file->Var32(&VRAMMap_ARM7[1]);

    if (!file->Saving)
    {
        PalOAMWritten = true;

        for (int i = 0; i < 0x20; i++)
            VRAMPtr_ABG[i] = GetUniqueBankPtr(VRAMMap_ABG[i], i << 14);
        for (int i = 0; i < 0x10; i++)
            VRAMPtr_AOBJ[i] = GetUniqueBankPtr(VRAMMap_AOBJ[i], i << 14);
        for (int i = 0; i < 0x8; i++)
            VRAMPtr_BBG[i] = GetUniqueBankPtr(VRAMMap_BBG[i], i << 14);
        for (int i = 0; i < 0x8; i++)
            VRAMPtr_BOBJ[i] = GetUniqueBankPtr(VRAMMap_BOBJ[i], i << 14);
    }

    GPU2D_A.DoSavestate(file);
    GPU2D_B.DoSavestate(file);
    GPU3D.DoSavestate(file);

    if (!file->Saving)
    {
        ResetVRAMCache();
        // nothing hi-res is saved: the copies are dropped and rebuild from native source B
        ResetHiresCapture();
        ResetHeldBackoff();
        if (CapReads)
            CapReads->DropAll();
        if (Stats2D)
            Stats2D->Reset();
        if (Shadow2D)
            Shadow2D->Reset();
        GPU3D.GetCurrentRenderer().SavestateLoaded();
    }
}

void GPU::SetCaptureReadLogging(bool enable) noexcept
{
    // DoCapture counts in it, and the 2D thread may be running one (HeldTo2DThread)
    Sync2D();
    if (enable && !CapReads)
        CapReads = std::make_unique<CaptureReadCounter>();
    else if (!enable)
        CapReads.reset();
}

void GPU::Set2DStatsLogging(bool enable) noexcept
{
    if (enable && !Stats2D)
        Stats2D = std::make_unique<GPU2DStats>();
    else if (!enable)
        Stats2D.reset();
}

void GPU::SetShadow2D(int mode, bool log, bool check, int wide) noexcept
{
    // nothing it records may be half a frame's when it starts or stops
    Sync2D();
    if (mode <= 0)
    {
        Shadow2D.reset();
        return;
    }
    if (!Shadow2D)
        Shadow2D = std::make_unique<Shadow2DRecorder>();
    Shadow2D->Mode = mode >= 2 ? 2 : 1;
    Shadow2D->Logging = log || check;
    Shadow2D->Check = check;
    Shadow2D->Wide = wide;
    Shadow2D->Reset();
}

// out of line because GPU.h can't see NDS. the CPU doesn't run while an ARM9 DMA holds it stopped
// (NDS::RunFrame), so this tells DMA from the CPU without touching the DMA code. a JIT block that
// starts a DMA finishes first, so reads in the rest of that block count as DMA
void GPU::NoteCaptureRead(u32 bank, u32 addr) noexcept
{
    CapReads->NoteRead(bank, addr, (NDS.CPUStop & CPUStop_DMA9) != 0, VCount);
}

void GPU::NoteCaptureWrite(u32 bank, u32 addr) noexcept
{
    CapReads->NoteWrite(bank, addr);
}

void GPU::NoteCaptureBarrier() noexcept
{
    CapReads->NoteBarrier();
}

void GPU::NoteCaptureDMA(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat) noexcept
{
    CapReads->NoteDMA(channel, ndma, mode, src, dst, words, wordSize, repeat, VCount);
}

void GPU::NoteCaptureDMARun(u32 channel, bool ndma, u32 mode, u32 src, u32 dst, u32 words, u32 wordSize, bool repeat) noexcept
{
    CapReads->NoteDMARun(channel, ndma, mode, src, dst, words, wordSize, repeat);
}

// a CPU or DMA write to a bank a flat copy comes from. R15 is what the JIT last stored, so with the
// JIT it's about where the block started; the interpreter has the instruction + 8 (+ 4 in Thumb)
void GPU::NoteCaptureFlatWrite(u32 tracker, u32 offset, u32 size, bool changed) noexcept
{
    CapReads->NoteFlatWrite(tracker, offset, size, changed, (NDS.CPUStop & CPUStop_DMA9) != 0, NDS.ARM9.R[15]);
}

// each check of the flat copies. the lines held right now are the ones the check's barrier draws
void GPU::NoteCaptureFlatCheck(const CaptureFlatDirty* dirty, u32 count) noexcept
{
    const GPU2D::LineJob* held = Deferred2DPending ? &Deferred2DJobs[Deferred2DEnd - Deferred2DPending] : nullptr;
    CapReads->NoteFlatCheck(*this, dirty, count, held, Deferred2DPending, EngineBNoHold);
}

// before the held lines are drawn: what asked for it, when, and whether the line with the 3D
// readback map (GetLine(0), see GPU3D::GetLine for the abort case) is among them
void GPU::NoteCaptureFlush(u8 flushCause) noexcept
{
    if (flushCause == CapFlush_Read)
        flushCause = (NDS.CPUStop & CPUStop_DMA9) ? CapFlush_ReadDMA : CapFlush_ReadARM9;

    bool maps = false;
    for (u32 i = Deferred2DEnd - Deferred2DPending; i < Deferred2DEnd; i++)
    {
        const GPU2D::LineJob& job = Deferred2DJobs[i];
        maps = maps || (job.Fetch3DLine && job.Line == 0 && !job.Abort3DLine);
    }

    CapReads->NoteFlush(flushCause, VCount, Deferred2DPending, maps);
}

// around the map: the time it took, and with the compute renderer in debug builds the wait and
// spin the frontend's CAPWAIT line sums up, for this map alone. reading them changes nothing
void GPU::NoteCaptureMap(bool end) noexcept
{
    s64 waitNs = 0, spinNs = 0;
#ifdef MELONADE_DEBUG_TOOLS
    const Renderer3D& renderer = GPU3D.GetCurrentRenderer();
    waitNs = renderer.CaptureMapWaitNs;
    spinNs = renderer.CaptureSpinNs;
#endif
    if (end)
        CapReads->NoteMapEnd(waitNs, spinNs);
    else
        CapReads->NoteMapStart(waitNs, spinNs);
}

// the hi-res capture copies no longer match VRAM: all four banks count as written, which drops
// them at the next Blit, and an unconsumed record goes with them
void GPU::ResetHiresCapture() noexcept
{
    HiresCapWrites = 0xF;
    HiresCapWritesInCapture = 0;
    NativeCapturedSincePass = 0;
    if (HiresCapRecord)
    {
        HiresCapRecord->Clear();
        HiresCapRecord->StaleMask = 0;
    }
}

void GPU::SetHiresCapture(bool enable) noexcept
{
    if (enable == HiresCapture)
        return;

    // native capture went on without the record meanwhile (or will), so the copies are stale
    Sync2D();
    HiresCapture = enable;
    ResetHiresCapture();
}

HiresCaptureRecord& GPU::RecordHiresCaptureLine(u32 line, u32 captureCnt) noexcept
{
    if (!HiresCapRecord)
    {
        HiresCapRecord = std::make_unique<HiresCaptureRecord>();
        HiresCapRecord->StaleMask = NativeCapturedSincePass;
    }

    HiresCaptureRecord& record = *HiresCapRecord;
    u32 dstbank = (captureCnt >> 16) & 0x3;
    u32 layout = captureCnt & 0x3F0000;

    bool first = true;
    for (u32 mask : record.LineMask)
        first = first && !mask;
    if (first)
        record.Layout = layout;
    else if (layout != record.Layout)
        record.Skip |= HiresCapSkip_Layout;

    record.LineMask[line >> 5] |= 1u << (line & 31);
    record.DstMask |= 1 << dstbank;
    if (captureCnt & (0x3 << 18))
        record.OffsetMask |= 1 << dstbank;

    // set even when the destination isn't in LCDC mode and nothing gets written: that only makes
    // the bank native for longer, and keeps the rule to one line
    NativeCapturedSincePass |= 1 << dstbank;

    HiresCaptureRecord::Line& entry = record.Lines[line];
    entry = {};
    entry.CaptureCnt = captureCnt;
    return record;
}

// after the compositor read this frame's record and write masks (GLCompositor::RenderFrame): the
// banks it captured into are redone (or dropped) now, and the next frame's writes start afresh
void GPU::HiresCaptureBlitDone() noexcept
{
    if (HiresCapRecord)
    {
        NativeCapturedSincePass &= ~HiresCapRecord->DstMask;
        HiresCapRecord->Clear();
        HiresCapRecord->StaleMask = NativeCapturedSincePass;
    }

    HiresCapWrites = 0;
    HiresCapWritesInCapture = 0;
}

// the Blit StartScanline(192) left for later (see LateBlit and BlitAfterReadback), from Sync2D
// once every line is drawn, or from StartHBlank at line 215
void GPU::RunLateBlit() noexcept
{
    // the Blit reads the 2D back buffers and the hi-res capture record (HeldTo2DThread bit 4 gets
    // here from line 215 without waiting)
    if (Held2DQueued)
        Held2DDone(true);

    BlitPending = false;
    BlitAfter3DPending = false;

    // the compositor gets the bank writes as of line 192, and the ones made since stay for the
    // next Blit, as they would have after a Blit at line 192
    u8 writes = HiresCapWrites;
    u8 writesInCapture = HiresCapWritesInCapture;
    HiresCapWrites = LateBlitCapWrites;
    HiresCapWritesInCapture = LateBlitCapWritesInCapture;

    GPU3D.Blit(*this);
    HiresCaptureBlitDone();

    HiresCapWrites = writes;
    HiresCapWritesInCapture = writesInCapture;
}

void GPU::AssignFramebuffers() noexcept
{
    int backbuf = FrontBuffer ? 0 : 1;
    if (NDS.PowerControl9 & (1<<15))
    {
        GPU2D_Renderer->SetFramebuffer(Framebuffer[backbuf][0].get(), Framebuffer[backbuf][1].get());
    }
    else
    {
        GPU2D_Renderer->SetFramebuffer(Framebuffer[backbuf][1].get(), Framebuffer[backbuf][0].get());
    }
}

void GPU::SetRenderer3D(std::unique_ptr<Renderer3D>&& renderer) noexcept
{
    Sync2D();

    if (renderer == nullptr)
        GPU3D.SetCurrentRenderer(std::make_unique<SoftRenderer>());
    else
        GPU3D.SetCurrentRenderer(std::move(renderer));

    InitFramebuffers();
}

void GPU::InitFramebuffers() noexcept
{
    // queued jobs point into the old framebuffers
    Sync2D();

    int fbsize;
    if (GPU3D.UsesAccelLines())
        fbsize = GPU2D::AccelFramebufferWords;
    else
        fbsize = 256 * 192;

    Framebuffer[0][0] = std::make_unique<u32[]>(fbsize);
    Framebuffer[1][0] = std::make_unique<u32[]>(fbsize);
    Framebuffer[0][1] = std::make_unique<u32[]>(fbsize);
    Framebuffer[1][1] = std::make_unique<u32[]>(fbsize);

    memset(Framebuffer[0][0].get(), 0, fbsize*4);
    memset(Framebuffer[1][0].get(), 0, fbsize*4);
    memset(Framebuffer[0][1].get(), 0, fbsize*4);
    memset(Framebuffer[1][1].get(), 0, fbsize*4);

    AssignFramebuffers();

    // the blocks are gone with the old buffers, and a new renderer has a new compositor
    if (Shadow2D)
        Shadow2D->Reset();
}


// VRAM mapping notes
//
// mirroring:
// unmapped range reads zero
// LCD is mirrored every 0x100000 bytes, the gap between each mirror reads zero
// ABG:
//   bank A,B,C,D,E mirror every 0x80000 bytes
//   bank F,G mirror at base+0x8000, mirror every 0x80000 bytes
// AOBJ:
//   bank A,B,E mirror every 0x40000 bytes
//   bank F,G mirror at base+0x8000, mirror every 0x40000 bytes
// BBG:
//   bank C mirrors every 0x20000 bytes
//   bank H mirrors every 0x10000 bytes
//   bank I mirrors at base+0x4000, mirrors every 0x10000 bytes
// BOBJ:
//   bank D mirrors every 0x20000 bytes
//   bank I mirrors every 0x4000 bytes
//
// untested:
//   ARM7 (TODO)
//   extended palette (mirroring doesn't apply)
//   texture/texpal (does mirroring apply?)
//   -> trying to use extpal/texture/texpal with no VRAM mapped.
//      would likely read all black, but has to be tested.
//
// overlap:
// when reading: values are read from each bank and ORed together
// when writing: value is written to each bank

u8* GPU::GetUniqueBankPtr(u32 mask, u32 offset) noexcept
{
    if (!mask || (mask & (mask - 1)) != 0) return NULL;
    int num = __builtin_ctz(mask);
    return &VRAM[num][offset & VRAMMask[num]];
}

const u8* GPU::GetUniqueBankPtr(u32 mask, u32 offset) const noexcept
{
    if (!mask || (mask & (mask - 1)) != 0) return NULL;
    int num = __builtin_ctz(mask);
    return &VRAM[num][offset & VRAMMask[num]];
}

// a mapping change syncs the 2D thread first while queued lines may still read a bank that was in
// LCDC mode when they were built (Sync2DForLCDC). otherwise they only read the job's snapshot and
// the flat copies, and those sync before they're rebuilt from the new mappings (MakeVRAMFlat_2DBGCoherent)
#define MAP_RANGE(map, base, n)    for (int i = 0; i < n; i++) VRAMMap_##map[(base)+i] |= bankmask;
#define UNMAP_RANGE(map, base, n)  for (int i = 0; i < n; i++) VRAMMap_##map[(base)+i] &= ~bankmask;

#define MAP_RANGE_PTR(map, base, n) \
    for (int i = 0; i < n; i++) { VRAMMap_##map[(base)+i] |= bankmask; VRAMPtr_##map[(base)+i] = GetUniqueBankPtr(VRAMMap_##map[(base)+i], ((base)+i)<<14); }
#define UNMAP_RANGE_PTR(map, base, n) \
    for (int i = 0; i < n; i++) { VRAMMap_##map[(base)+i] &= ~bankmask; VRAMPtr_##map[(base)+i] = GetUniqueBankPtr(VRAMMap_##map[(base)+i], ((base)+i)<<14); }

void GPU::MapVRAM_AB(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x9B;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    // the bank may have been written while it was mapped elsewhere
    NoteHiresCaptureWrite(bank);
    if (CapReads)
        CapReads->NoteRemap(bank, cnt);

    u8 oldofs = (oldcnt >> 3) & 0x3;
    u8 ofs = (cnt >> 3) & 0x3;
    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // ABG
            UNMAP_RANGE_PTR(ABG, oldofs<<3, 8);
            break;

        case 2: // AOBJ
            oldofs &= 0x1;
            UNMAP_RANGE_PTR(AOBJ, oldofs<<3, 8);
            break;

        case 3: // texture
            VRAMMap_Texture[oldofs] &= ~bankmask;
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // ABG
            MAP_RANGE_PTR(ABG, ofs<<3, 8);
            break;

        case 2: // AOBJ
            ofs &= 0x1;
            MAP_RANGE_PTR(AOBJ, ofs<<3, 8);
            break;

        case 3: // texture
            VRAMMap_Texture[ofs] |= bankmask;
            break;
        }
    }
}

void GPU::MapVRAM_CD(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x9F;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    VRAMSTAT &= ~(1 << (bank-2));

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    // the bank may have been written while it was mapped elsewhere
    NoteHiresCaptureWrite(bank);
    if (CapReads)
        CapReads->NoteRemap(bank, cnt);

    u8 oldofs = (oldcnt >> 3) & 0x7;
    u8 ofs = (cnt >> 3) & 0x7;
    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // ABG
            UNMAP_RANGE_PTR(ABG, oldofs<<3, 8);
            break;

        case 2: // ARM7 VRAM
            oldofs &= 0x1;
            VRAMMap_ARM7[oldofs] &= ~bankmask;
            break;

        case 3: // texture
            VRAMMap_Texture[oldofs] &= ~bankmask;
            break;

        case 4: // BBG/BOBJ
            if (bank == 2)
            {
                UNMAP_RANGE_PTR(BBG, 0, 8);
            }
            else
            {
                UNMAP_RANGE_PTR(BOBJ, 0, 8);
            }
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // ABG
            MAP_RANGE_PTR(ABG, ofs<<3, 8);
            break;

        case 2: // ARM7 VRAM
            ofs &= 0x1;
            VRAMMap_ARM7[ofs] |= bankmask;
            memset(VRAMDirty[bank].Data, 0xFF, sizeof(VRAMDirty[bank].Data));
            VRAMSTAT |= (1 << (bank-2));
            NDS.JIT.CheckAndInvalidateWVRAM(ofs);
            break;

        case 3: // texture
            VRAMMap_Texture[ofs] |= bankmask;
            break;

        case 4: // BBG/BOBJ
            if (bank == 2)
            {
                MAP_RANGE_PTR(BBG, 0, 8);
            }
            else
            {
                MAP_RANGE_PTR(BOBJ, 0, 8);
            }
            break;
        }
    }
}

void GPU::MapVRAM_E(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x87;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // ABG
            UNMAP_RANGE_PTR(ABG, 0, 4);
            break;

        case 2: // AOBJ
            UNMAP_RANGE_PTR(AOBJ, 0, 4);
            break;

        case 3: // texture palette
            UNMAP_RANGE(TexPal, 0, 4);
            break;

        case 4: // ABG ext palette
            UNMAP_RANGE(ABGExtPal, 0, 4);
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // ABG
            MAP_RANGE_PTR(ABG, 0, 4);
            break;

        case 2: // AOBJ
            MAP_RANGE_PTR(AOBJ, 0, 4);
            break;

        case 3: // texture palette
            MAP_RANGE(TexPal, 0, 4);
            break;

        case 4: // ABG ext palette
            MAP_RANGE(ABGExtPal, 0, 4);
            break;
        }
    }
}

void GPU::MapVRAM_FG(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x9F;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    u8 oldofs = (oldcnt >> 3) & 0x7;
    u8 ofs = (cnt >> 3) & 0x7;
    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // ABG
            {
                u32 base = (oldofs & 0x1) + ((oldofs & 0x2) << 1);
                VRAMMap_ABG[base] &= ~bankmask;
                VRAMMap_ABG[base + 2] &= ~bankmask;
                VRAMPtr_ABG[base] = GetUniqueBankPtr(VRAMMap_ABG[base], base << 14);
                VRAMPtr_ABG[base + 2] = GetUniqueBankPtr(VRAMMap_ABG[base + 2], (base + 2) << 14);
            }
            break;

        case 2: // AOBJ
            {
                u32 base = (oldofs & 0x1) + ((oldofs & 0x2) << 1);
                VRAMMap_AOBJ[base] &= ~bankmask;
                VRAMMap_AOBJ[base + 2] &= ~bankmask;
                VRAMPtr_AOBJ[base] = GetUniqueBankPtr(VRAMMap_AOBJ[base], base << 14);
                VRAMPtr_AOBJ[base + 2] = GetUniqueBankPtr(VRAMMap_AOBJ[base + 2], (base + 2) << 14);
            }
            break;

        case 3: // texture palette
            VRAMMap_TexPal[(oldofs & 0x1) + ((oldofs & 0x2) << 1)] &= ~bankmask;
            break;

        case 4: // ABG ext palette
            VRAMMap_ABGExtPal[((oldofs & 0x1) << 1)] &= ~bankmask;
            VRAMMap_ABGExtPal[((oldofs & 0x1) << 1) + 1] &= ~bankmask;
            break;

        case 5: // AOBJ ext palette
            VRAMMap_AOBJExtPal &= ~bankmask;
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x7)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // ABG
            {
                u32 base = (ofs & 0x1) + ((ofs & 0x2) << 1);
                VRAMMap_ABG[base] |= bankmask;
                VRAMMap_ABG[base + 2] |= bankmask;
                VRAMPtr_ABG[base] = GetUniqueBankPtr(VRAMMap_ABG[base], base << 14);
                VRAMPtr_ABG[base + 2] = GetUniqueBankPtr(VRAMMap_ABG[base + 2], (base + 2) << 14);
            }
            break;

        case 2: // AOBJ
            {
                u32 base = (ofs & 0x1) + ((ofs & 0x2) << 1);
                VRAMMap_AOBJ[base] |= bankmask;
                VRAMMap_AOBJ[base + 2] |= bankmask;
                VRAMPtr_AOBJ[base] = GetUniqueBankPtr(VRAMMap_AOBJ[base], base << 14);
                VRAMPtr_AOBJ[base + 2] = GetUniqueBankPtr(VRAMMap_AOBJ[base + 2], (base + 2) << 14);
            }
            break;

        case 3: // texture palette
            VRAMMap_TexPal[(ofs & 0x1) + ((ofs & 0x2) << 1)] |= bankmask;
            break;

        case 4: // ABG ext palette
            VRAMMap_ABGExtPal[((ofs & 0x1) << 1)] |= bankmask;
            VRAMMap_ABGExtPal[((ofs & 0x1) << 1) + 1] |= bankmask;
            break;

        case 5: // AOBJ ext palette
            VRAMMap_AOBJExtPal |= bankmask;
            break;
        }
    }
}

void GPU::MapVRAM_H(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x83;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // BBG
            VRAMMap_BBG[0] &= ~bankmask;
            VRAMMap_BBG[1] &= ~bankmask;
            VRAMMap_BBG[4] &= ~bankmask;
            VRAMMap_BBG[5] &= ~bankmask;
            VRAMPtr_BBG[0] = GetUniqueBankPtr(VRAMMap_BBG[0], 0 << 14);
            VRAMPtr_BBG[1] = GetUniqueBankPtr(VRAMMap_BBG[1], 1 << 14);
            VRAMPtr_BBG[4] = GetUniqueBankPtr(VRAMMap_BBG[4], 4 << 14);
            VRAMPtr_BBG[5] = GetUniqueBankPtr(VRAMMap_BBG[5], 5 << 14);
            break;

        case 2: // BBG ext palette
            UNMAP_RANGE(BBGExtPal, 0, 4);
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // BBG
            VRAMMap_BBG[0] |= bankmask;
            VRAMMap_BBG[1] |= bankmask;
            VRAMMap_BBG[4] |= bankmask;
            VRAMMap_BBG[5] |= bankmask;
            VRAMPtr_BBG[0] = GetUniqueBankPtr(VRAMMap_BBG[0], 0 << 14);
            VRAMPtr_BBG[1] = GetUniqueBankPtr(VRAMMap_BBG[1], 1 << 14);
            VRAMPtr_BBG[4] = GetUniqueBankPtr(VRAMMap_BBG[4], 4 << 14);
            VRAMPtr_BBG[5] = GetUniqueBankPtr(VRAMMap_BBG[5], 5 << 14);
            break;

        case 2: // BBG ext palette
            MAP_RANGE(BBGExtPal, 0, 4);
            break;
        }
    }
}

void GPU::MapVRAM_I(u32 bank, u8 cnt) noexcept
{
    cnt &= 0x83;

    u8 oldcnt = VRAMCNT[bank];
    VRAMCNT[bank] = cnt;

    if (oldcnt == cnt) return;
    Sync2DForLCDC(CapFlush_VRAMCNT);

    u32 bankmask = 1 << bank;

    if (oldcnt & (1<<7))
    {
        switch (oldcnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC &= ~bankmask;
            break;

        case 1: // BBG
            VRAMMap_BBG[2] &= ~bankmask;
            VRAMMap_BBG[3] &= ~bankmask;
            VRAMMap_BBG[6] &= ~bankmask;
            VRAMMap_BBG[7] &= ~bankmask;
            VRAMPtr_BBG[2] = GetUniqueBankPtr(VRAMMap_BBG[2], 2 << 14);
            VRAMPtr_BBG[3] = GetUniqueBankPtr(VRAMMap_BBG[3], 3 << 14);
            VRAMPtr_BBG[6] = GetUniqueBankPtr(VRAMMap_BBG[6], 6 << 14);
            VRAMPtr_BBG[7] = GetUniqueBankPtr(VRAMMap_BBG[7], 7 << 14);
            break;

        case 2: // BOBJ
            UNMAP_RANGE_PTR(BOBJ, 0, 8);
            break;

        case 3: // BOBJ ext palette
            VRAMMap_BOBJExtPal &= ~bankmask;
            break;
        }
    }

    if (cnt & (1<<7))
    {
        switch (cnt & 0x3)
        {
        case 0: // LCDC
            VRAMMap_LCDC |= bankmask;
            break;

        case 1: // BBG
            VRAMMap_BBG[2] |= bankmask;
            VRAMMap_BBG[3] |= bankmask;
            VRAMMap_BBG[6] |= bankmask;
            VRAMMap_BBG[7] |= bankmask;
            VRAMPtr_BBG[2] = GetUniqueBankPtr(VRAMMap_BBG[2], 2 << 14);
            VRAMPtr_BBG[3] = GetUniqueBankPtr(VRAMMap_BBG[3], 3 << 14);
            VRAMPtr_BBG[6] = GetUniqueBankPtr(VRAMMap_BBG[6], 6 << 14);
            VRAMPtr_BBG[7] = GetUniqueBankPtr(VRAMMap_BBG[7], 7 << 14);
            break;

        case 2: // BOBJ
            MAP_RANGE_PTR(BOBJ, 0, 8);
            break;

        case 3: // BOBJ ext palette
            VRAMMap_BOBJExtPal |= bankmask;
            break;
        }
    }
}


void GPU::SetPowerCnt(u32 val) noexcept
{
    // POWCNT1 effects:
    // * bit0: asplodes hardware??? not tested.
    // * bit1: disables engine A palette and OAM (zero-filled) (TODO: affects mem timings???)
    // * bit2: disables rendering engine, resets its internal state (polygons and registers)
    // * bit3: disables geometry engine
    // * bit9: disables engine B palette, OAM and rendering (screen turns white)
    // * bit15: screen swap

    // bit0 is the LCD enable: the screens show black while it is clear (see ScreensEnabled)
    if (!(val & (1<<0)))
    {
        if (!LCDPowerLogged) Log(LogLevel::Warn, "!!! CLEARING POWCNT BIT0. DANGER\n");
        LCDPowerLogged = true;
    }
    else
        LCDPowerLogged = false;

    // a Blit left for after the 3D render stays: it reads the back buffers directly, not through
    // the engines' framebuffer pointers AssignFramebuffers changes, the screen swap it reads was
    // latched at StartFrame, and the engine enables and the 3D state only matter to lines and
    // renders after it. dual-screen 3D games flip bit 15 every VBlank
    // HeldTo2DThread bit 8: the lines line 192 queued don't wait. they carry their framebuffers,
    // unit states and palette and OAM copy, and the renderer only reads the job, so neither
    // AssignFramebuffers (the renderer's pointers are only read when a job is built), the enables
    // nor the 3D state change what they draw. nothing else is held or due then (see Queue2DDeferred)
    if (!(Held2DQueued && (HeldTo2DThread & 8)))
        Sync2D(CapFlush_PowCnt, false);

    // ComputePace 2: a render left out for pacing can still be asked for at line 0, and turning the
    // rendering engine off clears the 3D state it would be drawn from. so it's drawn now. a reset or
    // a savestate load has dropped it by the time it gets here
    if (!(val & (1<<2)) && GPU3D.RenderingEnabled && GPU3D.GetCurrentRenderer().NeedsCaptureRedraw())
        GPU3D.GetCurrentRenderer().RedrawSkippedFrame(*this);

    GPU2D_A.SetEnabled(val & (1<<1));
    GPU2D_B.SetEnabled(val & (1<<9));
    GPU3D.SetEnabled(val & (1<<3), val & (1<<2));

    AssignFramebuffers();
}


void GPU::DisplayFIFO(u32 x) noexcept
{
    // sample the FIFO
    // as this starts 16 cycles (~3 pixels) before display start,
    // we aren't aligned to the 8-pixel grid
    if (x > 0)
    {
        if (x == 8)
            GPU2D_A.SampleFIFO(0, 5);
        else
            GPU2D_A.SampleFIFO(x-11, 8);
    }

    if (x < 256)
    {
        // transfer the next 8 pixels
        NDS.CheckDMAs(0, 0x04);
        NDS.ScheduleEvent(Event_DisplayFIFO, true, 6*8, 0, x+8);
    }
    else
        GPU2D_A.SampleFIFO(253, 3); // sample the remaining pixels
}

void GPU::StartFrame() noexcept
{
    ScreensEnabled = !!(NDS.PowerControl9 & (1<<0));
    ScreenSwap = !!(NDS.PowerControl9 & (1<<15));

    // only run the display FIFO if needed:
    // * if it is used for display or capture
    // * if we have display FIFO DMA
    RunFIFO = GPU2D_A.UsesFIFO() || NDS.DMAsInMode(0, 0x04);

    TotalScanlines = 0;
    StartScanline(0);
}

void GPU::StartHBlank(u32 line) noexcept
{
    DispStat[0] |= (1<<1);
    DispStat[1] |= (1<<1);

    // the renderer draws from the job alone. the Units' per-line updates happen around taking
    // their snapshots: the BG pass sees PreDrawScanline, the sprites see PostDrawScanline.
    // PreDrawScanline runs at every HBlank: it latches the DISPCNT enables (so writes during
    // VBlank settle before line 0, and line 262's sprite snapshot sees the latch too) and arms
    // display capture at line 0 (that decides where the job is drawn)
    GPU2D_A.PreDrawScanline(VCount);
    GPU2D_B.PreDrawScanline(VCount);

    if (VCount < 192)
    {
        // draw
        // note: this should start 48 cycles after the scanline start
        if (line < 192)
        {
            GPU2D::LineJob& job = Begin2DJob();
            Snapshot2DScanline(job, line);
            GPU2D_A.PostDrawScanline(VCount);
            GPU2D_B.PostDrawScanline(VCount);

            // sprites are pre-rendered one scanline in advance
            if (line < 191)
                Snapshot2DSprites(job, line+1);

            End2DJob(job);
        }

        NDS.CheckDMAs(0, 0x02);
    }
    else if (VCount == 215)
    {
        // the late barrier and Blit (see StartScanline), before the 3D render replaces the 3D
        // frame the compositor reads
        if (BlitPending)
            Sync2D(CapFlush_LateBlit);

        // lines still held for display capture (a VCount write skipped line 192) read the GL
        // readback buffer, which the 3D render can reallocate, so they're drawn first. only the
        // held lines (Flush2DDeferred waits for the ring, which can hold their engine B parts with
        // HeldTo2DThread), and the ones line 192 queued: software 3D needs no ring sync here.
        // HeldTo2DThread bit 4 waits for those only at the Blit (RunLateBlit), after the render:
        // the render and VCount215 read the 3D state, the polygons and texture VRAM (texture banks'
        // VRAMDirty and flat copies), none of which display capture or a 2D line writes, and the
        // held lines' 3D lines were copied into their jobs at line 192
        if (Held2DQueued && !(HeldTo2DThread & 4))
            Held2DDone(true);
        if (Deferred2DPending)
            Flush2DDeferred(CapFlush_Line215);

        // BlitAfterReadback: the render keeps the 3D frame the Blit reads, unless it's about to
        // resize the targets, and with them the compositor. then the Blit goes first, as before
        Renderer3D& renderer = GPU3D.GetCurrentRenderer();
        bool blitAfter = BlitAfter3DPending && renderer.KeepFrameForBlit(*this);
        if (BlitAfter3DPending && !blitAfter)
            RunLateBlit();

        GPU3D.VCount215(*this);

        // this frame held lines that capture 3D, so the next one most likely reads this 3D
        // frame back too. submit it now: otherwise the GPU only starts on it at the frontend's
        // frame-end flush, and the held lines' map at line 192 waits for that
        if (CaptureHeldThisFrame)
        {
            CaptureHeldThisFrame = false;
            if (BlitAfterReadback && renderer.SupportsBlitAfterRender())
            {
                // and read it back now, which line 0 then skips (SoftRenderer::VBlankEnd), so the
                // GPU has it before the compositor below and before the frontend's frame. the
                // flush is what keeps them apart: the map at line 192 then only waits for this
                renderer.PrepareEarlyCaptureFrame();
                renderer.FlushCommands();
                if (CapReads)
                    CapReads->NoteEarlyReadback();
            }
            else if (EarlyFlushCapture)
                renderer.FlushCommands();
        }

        // the Blit line 192 left, from the 3D frame the render kept, and submitted right away
        if (blitAfter)
        {
            RunLateBlit();
            renderer.FlushCommands();
            if (CapReads)
                CapReads->NoteBlitAfter3D();
        }
    }
    else if (VCount == 262)
    {
        GPU2D_A.ResetOBJMosaicCounters();
        GPU2D_B.ResetOBJMosaicCounters();

        GPU2D::LineJob& job = Begin2DJob();
        Snapshot2DSprites(job, 0);
        End2DJob(job);
    }

    if (DispStat[0] & (1<<4)) NDS.SetIRQ(0, IRQ_HBlank);
    if (DispStat[1] & (1<<4)) NDS.SetIRQ(1, IRQ_HBlank);

    if (VCount < 262)
        NDS.ScheduleEvent(Event_LCD, true, (LINE_CYCLES - HBLANK_CYCLES), LCD_StartScanline, line+1);
    else
        NDS.ScheduleEvent(Event_LCD, true, (LINE_CYCLES - HBLANK_CYCLES), LCD_FinishFrame, line+1);
}

// the GPU state the renderer reads during a line, which is the same for all of a job's draw calls
void GPU::Start2DJob(GPU2D::LineJob& job) noexcept
{
    job.DrawBG = false;
    job.DrawOBJ = false;
    job.Line3D = nullptr;

    job.VCount = VCount;
    job.Accelerated = GPU3D.UsesAccelLines();
    job.RenderXPos = GPU3D.GetRenderXPos();
    job.WideView = job.Accelerated && (GPU3D.GetCurrentRenderer().GetOutputWidth() != 256 || GPU3D.WideCheck > 256);
    job.Framebuffer[0] = GPU2D_Renderer->GetFramebuffer(0);
    job.Framebuffer[1] = GPU2D_Renderer->GetFramebuffer(1);
    job.VRAMMap_LCDC = VRAMMap_LCDC;
    job.ScreensEnabled = ScreensEnabled;
    // only while the compositor's pass would run: at 1x it always skips (outside the check mode),
    // so the record copy and the display flags would be wasted. the copies are dropped on every
    // scale change and a new renderer starts without any, so none can be valid when this turns on
    job.HiresCapture = job.Accelerated && HiresCapture && GPU3D.GetCurrentRenderer().RunsHiresCapture(HiresCaptureDebug);
    job.EngineBNoHold = EngineBNoHold;

    // a queued job reads palette and OAM from a copy made when it's built, so CPU writes don't wait
    // for the 2D thread. the other jobs are drawn before any write (see WritePalette), and so are
    // held ones' engine A parts. with HeldTo2DThread bit 1 a held job's engine B part is queued, and
    // only it reads a copy: engine B writes don't draw the held lines, so the live engine A half
    // stays right for them (see Queue2DDeferred for bit 2)
    bool heldB = Is2DJobDeferred(job) && HeldEngineBQueued();
    if (!Is2DJobQueued(job) || (Is2DJobDeferred(job) && !heldB))
    {
        job.Palette = Palette;
        job.OAM = OAM;
        job.PaletteB = Palette;
        job.OAMB = OAM;
        return;
    }

    Threaded2DState::PalOAMCopy& copy = Threaded2D->PalOAM[Cur2DPalOAM()];
    job.Palette = heldB ? Palette : copy.Palette;
    job.OAM = heldB ? OAM : copy.OAM;
    job.PaletteB = copy.Palette;
    job.OAMB = copy.OAM;
}

// the palette and OAM copy for the job queued next, made now if they were written since the last
// one. it's kept until that job is drawn
u32 GPU::Cur2DPalOAM() noexcept
{
    Threaded2DState& state = *Threaded2D;
    u32 queued = Jobs2DEnd;
    if (PalOAMWritten)
    {
        // the next copy, the oldest, once the jobs that read it are drawn. its last job is
        // still queued if it's between the drawn ones and this one
        u32 next = (state.PalOAMCur + 1) % PalOAMCopies;
        u32 done = Jobs2DDone.load(std::memory_order_acquire);
        if (state.PalOAMUntil[next] - done - 1 < queued - done)
        {
            Wait2DJobs(state.PalOAMUntil[next]);
            // which can draw hidden jobs here and take them back (HeldTo2DThread bit 16)
            queued = Jobs2DEnd;
        }

        memcpy(state.PalOAM[next].Palette, Palette, sizeof(Palette));
        memcpy(state.PalOAM[next].OAM, OAM, sizeof(OAM));
        state.PalOAMCur = next;
        PalOAMWritten = false;
    }

    // a job taken back later (Wait2DJobs) leaves its count here, which a later job gets: that
    // only keeps the copy for longer than needed
    state.PalOAMUntil[state.PalOAMCur] = queued + 1;
    return state.PalOAMCur;
}

// for DrawScanline(line): call after PreDrawScanline, before PostDrawScanline
void GPU::Snapshot2DScanline(GPU2D::LineJob& job, u32 line) noexcept
{
    MakeVRAMFlat_2DBGCoherent(line);

    job.DrawBG = true;
    job.Line = line;
    job.BG[0] = GPU2D_A.State();
    job.BG[1] = GPU2D_B.State();

    // debug only for now, see GPU_Shadow2D.h
    if (Shadow2D)
        Shadow2D->NoteLine(job, PaletteDirty);

    // debug only, see GPU_2DStats.h
    if (Stats2D)
        Stats2D->NoteLine(job);

    // debug only, see GPU_CaptureReads.h. counted here as the line may be drawn on the 2D thread
    if (CapReads && ((job.BG[0].DispCnt >> 16) & 0x3) == 2 && (job.VRAMMap_LCDC & (1 << ((job.BG[0].DispCnt >> 18) & 0x3))))
        CapReads->NoteDisplayLine((job.BG[0].DispCnt >> 18) & 0x3, job.VCount);

    // unit A's 3D line, fetched in the same cases the renderer used to: always with the software
    // renderer, and for display capture with the GL ones (their GetLine maps a GL buffer on line 0).
    // a line held for capture fetches it when it's drawn, with the scroll and abort state of now
    if (!job.Accelerated)
        job.Line3D = GPU3D.GetLine(line);
    else if (GPU2D_A.CaptureLatch && (((GPU2D_A.CaptureCnt >> 29) & 0x3) != 1))
    {
        if (Is2DJobDeferred(job))
        {
            job.Fetch3DLine = true;
            job.Abort3DLine = GPU3D.AbortFrame;
            CaptureHeldThisFrame = true;
        }
        else
        {
            job.Line3D = GPU3D.GetLine(line);
            // hi-res capture only reads it to tell whether this 3D line was blank
            job.Abort3DLine = GPU3D.AbortFrame;
        }
    }

    // the 3D renderer reuses its line buffer, so a queued job keeps a copy
    if (job.Line3D && Is2DJobQueued(job))
    {
        memcpy(job.Line3DBuffer, job.Line3D, sizeof(job.Line3DBuffer));
        job.Line3D = job.Line3DBuffer;
    }

    // VRAM display (display mode 2, unit A only: unit B's mode is masked to 0-1) reads LCDC banks
    // when the 2D thread draws the line, so writes to them must wait for it (see Sync2DForLCDC)
    if (Is2DJobQueued(job) && ((job.BG[0].DispCnt >> 16) & 0x3) == 2)
        Queued2DReadsLCDC = true;

    // display mode 3 and capture source B can read the display FIFO
    if (GPU2D_A.UsesFIFO())
        memcpy(job.DispFIFOBuffer, GPU2D_A.DispFIFOBuffer, sizeof(job.DispFIFOBuffer));
}

// for DrawSprites(line): call after PostDrawScanline (or ResetOBJMosaicCounters for line 0)
void GPU::Snapshot2DSprites(GPU2D::LineJob& job, u32 line) noexcept
{
    MakeVRAMFlat_2DOBJCoherent(line);

    job.DrawOBJ = true;
    job.SpriteLine = line;
    job.OBJ[0] = GPU2D_A.State();
    job.OBJ[1] = GPU2D_B.State();

    // debug only for now, see GPU_Shadow2D.h
    if (Shadow2D)
        Shadow2D->NoteSprites(job, OAMDirty);

    // debug only, see GPU_2DStats.h
    if (Stats2D)
        Stats2D->NoteSprites(job);
}

// the 2D renderer reads the flat VRAM copies. making them coherent consumes the shared VRAMDirty
// bits, so it happens here before the job is built, not in the renderer.
// these used to run between the draw calls of an HBlank and now all run before them. the only
// VRAM the draw calls write is display capture's, to an LCDC bank, which no BG/OBJ mapping
// includes, so the copies come out the same.
// the copies only change here, which is why CPU writes to BG/OBJ VRAM don't need the 2D barrier.
// the dirty sets are all worked out first (that only touches emulator-thread state), and the
// barrier only runs when a copy is about to change under lines the 2D thread hasn't drawn yet
void GPU::MakeVRAMFlat_2DBGCoherent(u32 line) noexcept
{
    auto bgDirtyA = VRAMDirty_ABG.DeriveState(VRAMMap_ABG, *this);
    auto bgExtPalDirtyA = VRAMDirty_ABGExtPal.DeriveState(VRAMMap_ABGExtPal, *this);
    auto objExtPalDirtyA = VRAMDirty_AOBJExtPal.DeriveState(&VRAMMap_AOBJExtPal, *this);
    auto bgDirtyB = VRAMDirty_BBG.DeriveState(VRAMMap_BBG, *this);
    auto bgExtPalDirtyB = VRAMDirty_BBGExtPal.DeriveState(VRAMMap_BBGExtPal, *this);
    auto objExtPalDirtyB = VRAMDirty_BOBJExtPal.DeriveState(&VRAMMap_BOBJExtPal, *this);

    // debug only, see GPU_CaptureReads.h
    if (CapReads)
    {
        const CaptureFlatDirty dirty[] = {
            {CapTrack_ABG, bgDirtyA.Data, bgDirtyA.DataLength},
            {CapTrack_ABGExtPal, bgExtPalDirtyA.Data, bgExtPalDirtyA.DataLength},
            {CapTrack_AOBJExtPal, objExtPalDirtyA.Data, objExtPalDirtyA.DataLength},
            {CapTrack_BBG, bgDirtyB.Data, bgDirtyB.DataLength},
            {CapTrack_BBGExtPal, bgExtPalDirtyB.Data, bgExtPalDirtyB.DataLength},
            {CapTrack_BOBJExtPal, objExtPalDirtyB.Data, objExtPalDirtyB.DataLength},
        };
        NoteCaptureFlatCheck(dirty, 6);
    }

    // debug only, see GPU_2DStats.h
    if (Stats2D)
    {
        Stats2D->NoteBGDirty(0, bgDirtyA.Data, bgDirtyA.DataLength, bgExtPalDirtyA.Data, bgExtPalDirtyA.DataLength, objExtPalDirtyA.Data, objExtPalDirtyA.DataLength);
        Stats2D->NoteBGDirty(1, bgDirtyB.Data, bgDirtyB.DataLength, bgExtPalDirtyB.Data, bgExtPalDirtyB.DataLength, objExtPalDirtyB.Data, objExtPalDirtyB.DataLength);
    }

    // debug only for now, see GPU_Shadow2D.h: which pages the compositor has to upload again, and
    // what they hold before the copies, to tell which ones really change
    if (Shadow2D)
    {
        const u64* const bits[6] = {bgDirtyA.Data, bgExtPalDirtyA.Data, objExtPalDirtyA.Data, bgDirtyB.Data, bgExtPalDirtyB.Data, objExtPalDirtyB.Data};
        const u32 words[6] = {bgDirtyA.DataLength, bgExtPalDirtyA.DataLength, objExtPalDirtyA.DataLength, bgDirtyB.DataLength, bgExtPalDirtyB.DataLength, objExtPalDirtyB.DataLength};
        Shadow2D->NoteBGDirty(*this, line, bits, words);
    }

    // with EngineBNoHold, held lines have no engine B part left to read engine B's copies, and with
    // HeldTo2DThread bit 1 the ring may hold those parts, so then the ring alone is waited for (no
    // flush, as NoteCaptureFlatCheck already counts it with EngineBNoHold)
    bool dirtyA = bgDirtyA || bgExtPalDirtyA || objExtPalDirtyA;
    bool dirtyB = bgDirtyB || bgExtPalDirtyB || objExtPalDirtyB;
    if (EngineBNoHold ? ((dirtyA && Is2DThreadBusy()) || (dirtyB && Is2DRingBusy())) : (Is2DThreadBusy() && (dirtyA || dirtyB)))
    {
        if (EngineBNoHold && !(dirtyA && Is2DThreadBusy()) && Deferred2DPending)
            Wait2DJobs(Jobs2DEnd);
        else
            Sync2D(CapFlush_FlatBG);
    }

    MakeVRAMFlat_ABGCoherent(bgDirtyA);
    MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirtyA);
    MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirtyA);
    MakeVRAMFlat_BBGCoherent(bgDirtyB);
    MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirtyB);
    MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirtyB);

    if (Shadow2D)
        Shadow2D->NoteBGCopied(*this);

    // debug only, see GPU_2DStats.h: which dirty pages really changed
    if (Stats2D)
    {
        Stats2D->NoteCopied(0, GPU2DStats::Flat_BG, VRAMFlat_ABG, bgDirtyA.Data, bgDirtyA.DataLength);
        Stats2D->NoteCopied(0, GPU2DStats::Flat_BGExtPal, VRAMFlat_ABGExtPal, bgExtPalDirtyA.Data, bgExtPalDirtyA.DataLength);
        Stats2D->NoteCopied(0, GPU2DStats::Flat_OBJExtPal, VRAMFlat_AOBJExtPal, objExtPalDirtyA.Data, objExtPalDirtyA.DataLength);
        Stats2D->NoteCopied(1, GPU2DStats::Flat_BG, VRAMFlat_BBG, bgDirtyB.Data, bgDirtyB.DataLength);
        Stats2D->NoteCopied(1, GPU2DStats::Flat_BGExtPal, VRAMFlat_BBGExtPal, bgExtPalDirtyB.Data, bgExtPalDirtyB.DataLength);
        Stats2D->NoteCopied(1, GPU2DStats::Flat_OBJExtPal, VRAMFlat_BOBJExtPal, objExtPalDirtyB.Data, objExtPalDirtyB.DataLength);
    }
}

void GPU::MakeVRAMFlat_2DOBJCoherent(u32 line) noexcept
{
    auto objDirtyA = VRAMDirty_AOBJ.DeriveState(VRAMMap_AOBJ, *this);
    auto objDirtyB = VRAMDirty_BOBJ.DeriveState(VRAMMap_BOBJ, *this);

    // debug only, see GPU_CaptureReads.h
    if (CapReads)
    {
        const CaptureFlatDirty dirty[] = {
            {CapTrack_AOBJ, objDirtyA.Data, objDirtyA.DataLength},
            {CapTrack_BOBJ, objDirtyB.Data, objDirtyB.DataLength},
        };
        NoteCaptureFlatCheck(dirty, 2);
    }

    // debug only, see GPU_2DStats.h
    if (Stats2D)
    {
        Stats2D->NoteOBJDirty(0, objDirtyA.Data, objDirtyA.DataLength);
        Stats2D->NoteOBJDirty(1, objDirtyB.Data, objDirtyB.DataLength);
    }

    // debug only for now, see GPU_Shadow2D.h
    if (Shadow2D)
    {
        const u64* const bits[2] = {objDirtyA.Data, objDirtyB.Data};
        const u32 words[2] = {objDirtyA.DataLength, objDirtyB.DataLength};
        Shadow2D->NoteOBJDirty(*this, line, bits, words);
    }

    if (EngineBNoHold ? ((objDirtyA && Is2DThreadBusy()) || (objDirtyB && Is2DRingBusy())) : (Is2DThreadBusy() && (objDirtyA || objDirtyB)))
    {
        // as for the BG copies
        if (EngineBNoHold && !(objDirtyA && Is2DThreadBusy()) && Deferred2DPending)
            Wait2DJobs(Jobs2DEnd);
        else
            Sync2D(CapFlush_FlatOBJ);
    }

    MakeVRAMFlat_AOBJCoherent(objDirtyA);
    MakeVRAMFlat_BOBJCoherent(objDirtyB);

    if (Shadow2D)
        Shadow2D->NoteOBJCopied(*this);

    if (Stats2D)
    {
        Stats2D->NoteCopied(0, GPU2DStats::Flat_OBJ, VRAMFlat_AOBJ, objDirtyA.Data, objDirtyA.DataLength);
        Stats2D->NoteCopied(1, GPU2DStats::Flat_OBJ, VRAMFlat_BOBJ, objDirtyB.Data, objDirtyB.DataLength);
    }
}

// threaded 2D
//
// when it's on, StartHBlank queues each line's job in a ring and the 2D thread draws them in order.
// the emulator thread still does everything that changes state, so a queued job only needs the
// memory it reads to stay as it was: whatever writes LCDC banks it may read, the flat copies, the
// framebuffers or the renderer runs Sync2D first. palette and OAM are copied into the job instead
// (see Start2DJob). with an accelerated 3D renderer the frame's lines are all drawn by the GL
// compositor's Blit, at line 192, or with LateBlit just before the 3D render at line 215 (with
// BlitAfterReadback, in capture frames, just after it), and the thread is idle between frames
// (FinishFrame and NDS::RunFrame sync), so the frontend can read the framebuffers and save states
// as before.
// while display capture is armed, lines are drawn on the emulator thread instead, after a Sync2D:
// DoCapture writes VRAM and VRAMDirty, and the GL renderers' GetLine needs the GL context. capture
// is armed at line 0 and stays on until VBlank, so a frame that captures goes back to drawing here
// from its first captured line on, and the next frame starts on the thread again.
// the 3D line is copied into queued jobs (see Snapshot2DScanline), so software 3D works threaded too
//
// display capture with an accelerated 3D renderer (and DeferCapture2D): the lines aren't drawn at
// their HBlank but held in Deferred2DJobs, and drawn in order on the emulator thread by
// Flush2DDeferred, at the latest by the Sync2D before the GL compositor at line 192 (capture frames
// keep the Blit there, or with BlitAfterReadback move it to just after the 3D render). the 3D frame
// is submitted at line 215 and read back into a GL buffer at line 0 (with BlitAfterReadback right
// after the render), and the first captured line maps that buffer (GetLine(0)), which waits for
// the GPU. drawing it at line 192 instead gives the GPU the whole frame to finish. this works with
// threaded 2D on or off: the ring is drained when the first line is held, and a line that isn't
// held draws the held ones first, so nothing is queued while lines are held (but see HeldTo2DThread).
// held lines count as not drawn yet for every barrier: Sync2D draws them, Is2DThreadBusy is true
// (the flat copies), LCDC writes sync (Queued2DReadsLCDC is set) and LCDC reads draw them first
// (ReadVRAM_LCDC), since DoCapture writes an LCDC bank and capture source B reads one. a game that
// touches any of that mid-frame gets an early draw, which costs what drawing at HBlank did
//
// HeldTo2DThread moves held lines to the 2D thread, and then held lines read palette and OAM copies
// like queued ones. 2D is still drawn on one thread at a time (the renderer has state of its own),
// so every draw on the emulator thread waits for the ring first. bit 1: engine B's part of each
// held line is queued at its HBlank (a ring slot points at the held job), so the ring holds those
// parts while lines are held, and only those, as it's drained at the first held line. engine B
// reads no LCDC bank and only engine B's flat copies, palette and OAM, so its barriers wait for the
// ring and leave the held lines held, and the held lines' copies of palette and OAM stay until
// their engine A part is drawn (Start2DJob). bit 2: in frames whose Blit waits for the 3D render,
// line 192 fetches the held lines' 3D lines into the jobs and queues them (Queue2DDeferred), so
// DoCapture runs on the 2D thread during lines 192-215. what it writes (the LCDC bank, its
// VRAMDirty, the hi-res capture record, which it makes if there's none yet, and
// NativeCapturedSincePass) is only read or written on the emulator thread after Held2DDone: Sync2D
// (savestates, mapping changes, LCDC writes, POWCNT, a Blit, FinishFrame), LCDC reads, and line 215
// before the 3D render (or, after a VCount write, the next line drawn or the frame end). bit 4
// moves that last wait to the Blit after the render, bit 8 leaves out POWCNT writes and LCDC reads
// of banks the capture doesn't write (Held2DWrites). bit 16: bit 1's engine B parts are put in the
// ring without telling the 2D thread (Jobs2DEnd runs ahead of Jobs2DQueued) and shown in batches;
// every wait for the ring counts them (Is2DRingBusy, Wait2DJobs) and shows them, or draws them
// itself when the 2D thread has nothing else left

static inline void CPURelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#endif
}

// spins until done() is true or the time is up, reading the clock every few rounds (less often
// with HeldTo2DThread bit 16: on ARM a yield is far cheaper than the clock)
template <typename F>
static bool SpinUntil(F done, u32 limitUs, bool readLess = false) noexcept
{
#if defined(__aarch64__) || defined(__arm__)
    const int rounds = readLess ? 512 : 64;
#else
    // pause takes 35-140 cycles, so fewer
    const int rounds = readLess ? 128 : 64;
#endif
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        for (int i = 0; i < rounds; i++)
        {
            if (done()) return true;
            CPURelax();
        }

        if (std::chrono::steady_clock::now() - start >= std::chrono::microseconds(limitUs))
            return false;
    }
}

void GPU::SetThreaded2D(bool threaded) noexcept
{
    if (threaded == IsThreaded2D())
        return;

    if (threaded)
        Start2DThread();
    else
        Stop2DThread();
}

Platform::Thread* GPU::Get2DThread() const noexcept
{
    return Threaded2D ? Threaded2D->Thread : nullptr;
}

void GPU::Start2DThread() noexcept
{
    // the counts are equal here (nothing is queued while threaded 2D is off), so the thread starts idle
    Threaded2D = std::make_unique<Threaded2DState>();
    PalOAMWritten = true;
    Threaded2D->Thread = Platform::Thread_Create([this]() { Run2DThread(); }, "GPU2DThread");
    if (!Threaded2D->Thread)
    {
        // nothing would ever draw the queued lines, so stay with drawing them at HBlank
        Threaded2D.reset();
        Log(LogLevel::Error, "GPU2D: couldn't start the 2D thread, threaded 2D rendering stays off\n");
        return;
    }

    Log(LogLevel::Info, "GPU2D: threaded 2D rendering on\n");
}

void GPU::Stop2DThread() noexcept
{
    if (!Threaded2D)
        return;

    // the thread only ever waits for work, so it can always be woken to stop
    Sync2D();
    {
        std::lock_guard lock(Threaded2D->Mutex);
        Threaded2D->Stopping.store(true, std::memory_order_relaxed);
    }
    Threaded2D->WorkWake.notify_one();

    Platform::Thread_Wait(Threaded2D->Thread);
    Platform::Thread_Free(Threaded2D->Thread);
    Threaded2D.reset();

    Log(LogLevel::Info, "GPU2D: threaded 2D rendering off\n");
}

void GPU::Run2DThread() noexcept
{
    Threaded2DState& state = *Threaded2D;
    u32 done = Jobs2DDone.load(std::memory_order_relaxed);
    bool frameDone = true;
    bool spinLess = false;

    while (Wait2DWork(done, frameDone ? Spin2DWorkVBlankUs : Spin2DWorkFrameUs, spinLess))
    {
        // acquire: the jobs counted so far are fully written
        u32 queued = Jobs2DQueued.load(std::memory_order_acquire);
        while (done != queued)
        {
            const GPU2D::LineJob& job = *state.JobPtr[done % Ring2DSize];
            u32 engines = state.JobEngines[done % Ring2DSize];
            GPU2D_Renderer->RunJob(job, engines & (GPU2D::Renderer2D::EngineA | GPU2D::Renderer2D::EngineB));
            // after line 191 the next job is the next frame's line 0 (line 262's sprites are
            // usually drawn inline), with the frontend's frame gap in between. not after a held
            // line's engine B part (HeldTo2DThread): its engine A part can come at line 192,
            // unless they come in batches, lines apart
            frameDone = (engines & Job2DBatched)
                || ((engines & GPU2D::Renderer2D::EngineA) && (!job.DrawBG || job.Line == 191));
            spinLess = (engines & Job2DSpinLess) != 0;
            done++;

            // a release for the barrier (the line is drawn and its memory read), and seq_cst so
            // that it pairs with SyncSleepers and SyncTarget: a barrier going to sleep either sees
            // this count or gets woken. a sleeper stores its target before it adds itself to the
            // count, and the loads here are in the opposite order, so a sleeper seen here has its
            // target in place, and it's only woken once that count is reached, not for every line
            // before it (Wait2DJobs explains the target with several sleepers)
            Jobs2DDone.store(done, std::memory_order_seq_cst);
            if (state.SyncSleepers.load(std::memory_order_seq_cst) > 0
                && (s32)(done - state.SyncTarget.load(std::memory_order_seq_cst)) >= 0)
            {
                std::lock_guard lock(state.Mutex);
                state.SyncWake.notify_all();
            }
        }
    }
}

// on the 2D thread: waits until more than `done` jobs are queued, spinning for up to `spinUs` before
// it sleeps. false when the thread should stop
bool GPU::Wait2DWork(u32 done, u32 spinUs, bool spinLess) noexcept
{
    Threaded2DState& state = *Threaded2D;
    auto ready = [&]()
    {
        return Jobs2DQueued.load(std::memory_order_acquire) != done || state.Stopping.load(std::memory_order_relaxed);
    };

    if (!SpinUntil(ready, spinUs, spinLess))
    {
        // seq_cst pairs with Publish2DJobs: either it sees WorkSleeping and wakes us, or we see its jobs
        std::unique_lock lock(state.Mutex);
        state.WorkSleeping.store(true, std::memory_order_seq_cst);
        while (Jobs2DQueued.load(std::memory_order_seq_cst) == done && !state.Stopping.load(std::memory_order_relaxed))
            state.WorkWake.wait(lock);
        state.WorkSleeping.store(false, std::memory_order_relaxed);
    }

    return !state.Stopping.load(std::memory_order_relaxed);
}

// waits until the 2D thread has drawn `count` jobs. Sync2D waits for all the queued ones
void GPU::Wait2DJobs(u32 count) noexcept
{
    if (!Threaded2D)
        return;

    Threaded2DState& state = *Threaded2D;
    // jobs put in the ring but not shown to the 2D thread yet (HeldTo2DThread bit 16, held lines'
    // engine B parts) go first. when the thread is done with the others, they're taken back and
    // drawn here instead: it never saw them, and waking it for them would cost more than they take
    // (a game that changes engine B's VRAM every line waits for them at every line)
    u32 published = Jobs2DQueued.load(std::memory_order_relaxed);
    if ((s32)(count - published) > 0)
    {
        // only on the thread that put them there (the emulator thread): another one's barrier
        // (a savestate) shows them instead
        if (Jobs2DDone.load(std::memory_order_acquire) == published && state.HiddenBy == std::this_thread::get_id())
        {
            for (u32 i = published; i != Jobs2DEnd; i++)
                GPU2D_Renderer->RunJob(*state.JobPtr[i % Ring2DSize], state.JobEngines[i % Ring2DSize] & (GPU2D::Renderer2D::EngineA | GPU2D::Renderer2D::EngineB));
            Jobs2DEnd = published;
            return;
        }
        Publish2DJobs();
    }

    // the counts wrap, so this compares the distance
    auto reached = [&]() { return (s32)(Jobs2DDone.load(std::memory_order_acquire) - count) >= 0; };

    if (SpinUntil(reached, Spin2DSyncUs, (HeldTo2DThread & 16) != 0))
        return;

    // seq_cst pairs with Run2DThread, see there. the 2D thread only notifies once SyncTarget is
    // reached, so the target is stored before we show up in SyncSleepers, and it must never sit
    // above the count of a sleeper that still waits. it's the lowest of the pending targets (the
    // counts not reached yet), kept so, each time a sleeper is about to wait:
    // - it takes the target over when it's the only sleeper, when its count is lower, or when the
    //   target was reached (stale: its sleepers have been notified and will leave or re-arm)
    // - a pending target below its count stays: another sleeper is still owed that wake, and it
    //   never raises it over them
    // notify_all wakes every sleeper, and each one re-arms or leaves. until the first of them
    // re-arms a reached target, the 2D thread's check passes on every job it finishes, each a
    // cheap notify_all with nobody (or only sleepers that leave) to wake
    std::unique_lock lock(state.Mutex);
    auto arm = [&]()
    {
        u32 target = state.SyncTarget.load(std::memory_order_relaxed);
        if (state.SyncSleepers.load(std::memory_order_relaxed) == 0
            || (s32)(count - target) < 0
            || (s32)(Jobs2DDone.load(std::memory_order_seq_cst) - target) >= 0)
            state.SyncTarget.store(count, std::memory_order_seq_cst);
    };

    arm();
    state.SyncSleepers.fetch_add(1, std::memory_order_seq_cst);
    for (;;)
    {
        // this load comes after arm()'s store in the seq_cst order, so the 2D thread either sees
        // the new target, or we see the count it stored before it read the old one
        if ((s32)(Jobs2DDone.load(std::memory_order_seq_cst) - count) >= 0)
            break;

        state.SyncWake.wait(lock);
        arm();
    }
    state.SyncSleepers.fetch_sub(1, std::memory_order_relaxed);
}

GPU2D::LineJob& GPU::Begin2DJob() noexcept
{
    // a VCount write can bring back visible lines before line 215, so the late Blit reads this
    // frame's lines first
    if (BlitPending || BlitAfter3DPending)
        Sync2D(CapFlush_LateBlit);

    GPU2D::LineJob* job = &Job2D;

    if (GPU2D_A.CaptureLatch && DeferCapture2D && GPU3D.IsRendererAccelerated())
    {
        // display capture with GL: held until line 192 (see above SetThreaded2D)
        job = BeginDeferred2DJob();
    }
    else
    {
        // held lines are drawn before any line that isn't
        if (Deferred2DPending)
            Flush2DDeferred(CapFlush_NotHeld);

        if (Threaded2D)
        {
            if (VCount == 262 && !Is2DRingBusy())
            {
                // line 262's sprites: drawn here while the thread is idle, as it usually is after
                // the frame-end barrier, which saves FinishFrame's wait and a wake
            }
            else if (!GPU2D_A.CaptureLatch)
            {
                // the next ring slot, once the thread is done with the job that used it last
                u32 queued = Jobs2DEnd;
                if (queued - Jobs2DDone.load(std::memory_order_acquire) >= Ring2DSize)
                {
                    Wait2DJobs(queued - Ring2DSize + 1);
                    queued = Jobs2DEnd;
                }

                job = &Threaded2D->Jobs[queued % Ring2DSize];
            }
            else
            {
                // display capture: drawn here, after the lines queued before it
                Sync2D(CapFlush_NotHeld);
            }
        }
    }

    Start2DJob(*job);
    return *job;
}

// a frame holds 192 lines at most (lines 0-191). if a VCount write skipped VBlank, line 262's
// sprite job would be the 193rd, so the held lines are drawn first and it starts a new buffer
static constexpr u32 Deferred2DSize = 192;

GPU2D::LineJob* GPU::BeginDeferred2DJob() noexcept
{
    // the lines line 192 queued are in the buffer this starts over (Begin2DJob has already synced
    // for the late Blit then, so this is only in case)
    if (Held2DQueued)
        Held2DDone();

    // the lines queued before it are drawn first. only the ring: the held lines stay held. with
    // HeldTo2DThread bit 1, the ring holds only held lines' engine B parts after the first held line
    if (Is2DRingBusy() && (!Deferred2DPending || !HeldEngineBQueued()))
        Wait2DJobs(Jobs2DEnd);

    if (!Deferred2DJobs)
        Deferred2DJobs = std::make_unique<GPU2D::LineJob[]>(Deferred2DSize);

    // a flush waits for the ring, so no slot points at the buffer when it starts over
    if (Deferred2DEnd == Deferred2DSize)
        Flush2DDeferred(CapFlush_Full);
    if (!Deferred2DPending)
        Deferred2DEnd = 0;

    // DoCapture writes an LCDC bank and capture source B can read one, so LCDC writes must sync
    Queued2DReadsLCDC = true;

    GPU2D::LineJob* job = &Deferred2DJobs[Deferred2DEnd];
    job->Fetch3DLine = false;
    job->EngineBDrawn = false;
    return job;
}

// draws the held display capture lines, in order, as StartHBlank would have drawn them. the 2D
// thread is idle here, or with HeldTo2DThread bit 1 waited for (it may still be drawing their
// engine B parts), and the job being built, if any, isn't held yet (it's at Deferred2DEnd)
void GPU::Flush2DDeferred(u8 flushCause) noexcept
{
    if (Deferred2DPending && Is2DRingBusy())
        Wait2DJobs(Jobs2DEnd);

    // debug only, see GPU_CaptureReads.h
    if (CapReads && Deferred2DPending)
        NoteCaptureFlush(flushCause);

    for (u32 i = Deferred2DEnd - Deferred2DPending; i < Deferred2DEnd; i++)
    {
        GPU2D::LineJob& job = Deferred2DJobs[i];

        // the first one with a 3D line maps the GL readback buffer (GetLine(0)), the rest read
        // the copy the renderer made of it
        if (job.Fetch3DLine)
        {
            bool timeMap = CapReads && job.Line == 0 && !job.Abort3DLine;
            if (timeMap)
                NoteCaptureMap(false);
            job.Line3D = GPU3D.GetLine(job.Line, job.RenderXPos, job.Abort3DLine);
            if (timeMap)
                NoteCaptureMap(true);
        }

        GPU2D_Renderer->RunJob(job, job.EngineBDrawn ? (u32)GPU2D::Renderer2D::EngineA : (GPU2D::Renderer2D::EngineA | GPU2D::Renderer2D::EngineB));
    }

    Deferred2DPending = 0;
}

void GPU::End2DJob(GPU2D::LineJob& job) noexcept
{
    if (!Is2DJobQueued(job))
    {
        GPU2D_Renderer->RunJob(job);
        return;
    }

    if (Is2DJobDeferred(job))
    {
        // debug only, see GPU_CaptureReads.h. here and not when the BG pass is snapshotted: the line's
        // sprite snapshot can be a barrier too, and that barrier only draws the lines before this one
        if (CapReads && job.DrawBG)
            CapReads->NoteHeldLine(job.BG[0].CaptureCnt, job.BG[0].CaptureLatch, job.VCount, job.VRAMMap_LCDC);

        // engine B's part is drawn when an unheld line would be, against the same flat copies. a
        // held job then only ever means engine A's part is left, so no barrier needs to know more.
        // with HeldTo2DThread bit 1 it's queued, and the barriers that engine B's part needs wait
        // for the ring
        if (EngineBNoHold)
        {
            job.EngineBDrawn = true;
            if (HeldEngineBQueued())
            {
                // HeldTo2DThread bit 16: shown to the 2D thread every Batch2DLines lines and at
                // the last visible one (and at any wait for the ring, see Wait2DJobs), so it
                // sleeps in between instead of spinning through every line
                bool batch = HeldTo2DThread & 16;
                Queue2DJob(job, GPU2D::Renderer2D::EngineB | (batch ? Job2DBatched : 0),
                           !batch || VCount >= 191 || Jobs2DEnd + 1 - Jobs2DQueued.load(std::memory_order_relaxed) >= Batch2DLines);
            }
            else
                GPU2D_Renderer->RunJob(job, GPU2D::Renderer2D::EngineB);
        }

        Deferred2DEnd++;
        Deferred2DPending++;
        return;
    }

    Queue2DJob(job, GPU2D::Renderer2D::EngineA | GPU2D::Renderer2D::EngineB);
}

// puts a job's draw in the next ring slot: a ring job, or with HeldTo2DThread a part of a held one
void GPU::Queue2DJob(const GPU2D::LineJob& job, u32 engines, bool publish) noexcept
{
    Threaded2DState& state = *Threaded2D;
    u32 queued = Jobs2DEnd;
    // ring jobs waited for their slot in Begin2DJob. the wait can take hidden jobs back
    if (queued - Jobs2DDone.load(std::memory_order_acquire) >= Ring2DSize)
    {
        Wait2DJobs(queued - Ring2DSize + 1);
        queued = Jobs2DEnd;
    }

    state.JobPtr[queued % Ring2DSize] = &job;
    state.JobEngines[queued % Ring2DSize] = engines | ((HeldTo2DThread & 16) ? Job2DSpinLess : 0);
    Jobs2DEnd = queued + 1;
    if (!publish)
        state.HiddenBy = std::this_thread::get_id();

    if (publish)
        Publish2DJobs();
}

// shows the 2D thread the jobs put in the ring so far
void GPU::Publish2DJobs() noexcept
{
    Threaded2DState& state = *Threaded2D;
    // a release (the jobs are written before the count says so), and seq_cst so that it pairs with
    // WorkSleeping: a thread going to sleep either sees these jobs or gets woken
    Jobs2DQueued.store(Jobs2DEnd, std::memory_order_seq_cst);
    if (state.WorkSleeping.load(std::memory_order_seq_cst))
    {
        std::lock_guard lock(state.Mutex);
        state.WorkWake.notify_one();
    }
}

// HeldTo2DThread bit 2, at line 192 in a frame whose Blit waits for the 3D render: the held lines
// are queued instead of drawn here, so the 2D thread draws them (and their display capture) while
// lines 192-215 run. their 3D lines are fetched here, as GetLine needs the GL context, into the jobs'
// own buffers, as the render at line 215 can reallocate the renderer's. false when they're drawn
// here as before
bool GPU::Queue2DDeferred() noexcept
{
    // CAPREADS counts on the emulator thread, also in DoCapture
    if (!(HeldTo2DThread & 2) || !Threaded2D || !Deferred2DPending || CapReads)
        return false;

    // the bank the Blit keeps a copy of (KeepHiresCaptureDst) must be one only the capture lines
    // write until Held2DDone: in LCDC mode, as any write to it or a mapping change syncs then. the
    // jobs' LCDC banks are the current ones for the same reason
    u32 first = Deferred2DEnd - Deferred2DPending;
    if (HiresCapRecord && HiresCapRecord->DstMask && !(VRAMMap_LCDC & (1 << ((HiresCapRecord->Layout >> 16) & 0x3))))
        return false;
    for (u32 i = first; i < Deferred2DEnd; i++)
    {
        const GPU2D::LineJob& job = Deferred2DJobs[i];
        if (job.HiresCapture && job.DrawBG && job.BG[0].CaptureLatch && !(job.VRAMMap_LCDC & (1 << ((job.BG[0].CaptureCnt >> 16) & 0x3))))
            return false;
    }

    // bit 32: backing off, see Held2DDone
    if ((HeldTo2DThread & 32) && HeldBackoff)
    {
        HeldBackoff--;
        HeldBackedOff++;
        if (!HeldBackoff)
            Log(LogLevel::Debug, "GPU2D: held lines go to the 2D thread at line 192 again (%u frames queued, %u drawn here so far)\n",
                HeldQueuedFrames, HeldBackedOff);
        return false;
    }

    // the held parts read the live palette and OAM until now, and what they read of them is as it
    // was when they were built, as writes to it draw the held lines first (see WritePalette). so
    // one copy of them now does for all of them, kept until the last one is drawn. engine B parts
    // bit 1 queued keep the copies they have
    Threaded2DState& state = *Threaded2D;
    u32 copy = Cur2DPalOAM();
    Held2DWrites = 0;
    for (u32 i = first; i < Deferred2DEnd; i++)
    {
        GPU2D::LineJob& job = Deferred2DJobs[i];
        if (job.DrawBG && job.BG[0].CaptureLatch)
            Held2DWrites |= job.VRAMMap_LCDC & (1 << ((job.BG[0].CaptureCnt >> 16) & 0x3));
        // the ring can still be drawing this job's engine B part (bit 1), which never reads Line3D
        // or Line3DBuffer, nor engine A's palette and OAM pointers
        if (job.Fetch3DLine)
        {
            memcpy(job.Line3DBuffer, GPU3D.GetLine(job.Line, job.RenderXPos, job.Abort3DLine), sizeof(job.Line3DBuffer));
            job.Line3D = job.Line3DBuffer;
        }

        job.Palette = state.PalOAM[copy].Palette;
        job.OAM = state.PalOAM[copy].OAM;
        if (!job.EngineBDrawn)
        {
            job.PaletteB = state.PalOAM[copy].Palette;
            job.OAMB = state.PalOAM[copy].OAM;
        }

        Queue2DJob(job, job.EngineBDrawn ? (u32)GPU2D::Renderer2D::EngineA : (GPU2D::Renderer2D::EngineA | GPU2D::Renderer2D::EngineB));
    }
    state.PalOAMUntil[copy] = Jobs2DEnd;

    Deferred2DPending = 0;
    Held2DQueued = true;
    HeldQueuedFrames++;
    return true;
}

// waits for the held lines Queue2DDeferred queued, and keeps the copy line 192 would have kept
// HeldTo2DThread bit 32 starts over
void GPU::ResetHeldBackoff() noexcept
{
    HeldEarlyHistory = 0;
    HeldBackoff = 0;
    HeldQueuedFrames = 0;
    HeldBackedOff = 0;
}

// planned: at line 215 or the Blit (or what stands in for them after a VCount write: the next line
// drawn or the frame end), not at a barrier before them
void GPU::Held2DDone(bool planned) noexcept
{
    // bit 32: a frame whose lines were still being drawn at an early barrier, where the emulator
    // thread now waits for the 2D thread instead of having drawn them itself at line 192
    if (HeldTo2DThread & 32)
    {
        bool early = !planned && Is2DRingBusy();
        HeldEarlyHistory = (HeldEarlyHistory << 1) | (early ? 1 : 0);
    }
    if ((HeldTo2DThread & 32) && (u32)__builtin_popcount(HeldEarlyHistory) >= HeldEarlyWaits)
    {
        HeldEarlyHistory = 0;
        HeldBackoff = HeldBackoffFrames;
        Log(LogLevel::Debug, "GPU2D: held lines waited for early, drawn at line 192 for %u frames (%u frames queued, %u drawn here so far)\n",
            HeldBackoffFrames, HeldQueuedFrames, HeldBackedOff);
    }

    Wait2DJobs(Jobs2DEnd);
    Held2DQueued = false;
    KeepHiresCaptureDst();
}

// BlitAfterReadback: the hi-res capture pass's destination bank as the frame's capture lines left
// it, since the CPU can write it before the Blit
void GPU::KeepHiresCaptureDst() noexcept
{
    if (HiresCapRecord && HiresCapRecord->DstMask)
    {
        HiresCapRecord->DstNativeKept = true;
        memcpy(HiresCapRecord->DstNative, VRAM[(HiresCapRecord->Layout >> 16) & 0x3],
               sizeof(HiresCapRecord->DstNative));
    }
}

void GPU::FinishFrame(u32 lines) noexcept
{
    // line 262 queued the next frame's first sprites. after this the 2D thread is idle until the next frame
    Sync2D(CapFlush_FrameEnd);

    if (CapReads)
        CapReads->EndFrame();
    if (Stats2D)
        Stats2D->EndFrame();

    if (Hash2DLogging)
        LogHash2D();
    else if (Hash3DLogging)
        Hash2DFrame++; // what LogHash2D counts, so HASH3D lines are numbered the same without it

    FrontBuffer = FrontBuffer ? 0 : 1;
    AssignFramebuffers();

    TotalScanlines = lines;

    if (GPU3D.AbortFrame)
    {
        GPU3D.RestartFrame(*this);
        GPU3D.AbortFrame = false;
    }
}

// debug only: logs a checksum (64-bit FNV-1a over 32-bit words) of what the 2D renderer drew
// into the back buffers this frame, so two builds can be checked for byte-identical output
// by diffing their logs.
// with an accelerated 3D renderer, each line is 3x256 words plus a control word. the second and
// third 256 words are only written on lines in display mode 1 (and the compositor only reads them
// there); on other lines they hold whatever an older frame left, so they're left out.
void GPU::LogHash2D() noexcept
{
    int backbuf = FrontBuffer ? 0 : 1;
    bool accel = GPU3D.UsesAccelLines();
    int stride = accel ? (256*3 + 1) : 256;

    u64 hash[2];
    for (int screen = 0; screen < 2; screen++)
    {
        const u32* fb = Framebuffer[backbuf][screen].get();
        if (!fb) return;

        u64 h = 0xCBF29CE484222325ULL;
        for (int line = 0; line < 192; line++)
        {
            const u32* src = &fb[stride * line];
            int count = 256;
            if (accel)
            {
                u32 ctrl = src[256*3];
                h = (h ^ ctrl) * 0x100000001B3ULL;
                if (((ctrl >> 16) & 0x3) == 1)
                    count = 256*3;
            }

            for (int i = 0; i < count; i++)
                h = (h ^ src[i]) * 0x100000001B3ULL;
        }
        hash[screen] = h;
    }

    // two call sites taking turns, like LogHash3D: the Android frontend lets 50 lines a second through
    // from one, and a game at 60 FPS needs a line every frame. the formats differ (the %.0s prints
    // nothing), so the linker can't fold them into one
    u32 frame = Hash2DFrame++;
    if (frame & 1)
        Log(LogLevel::Info, "HASH2D frame=%u top=%016llX bottom=%016llX%.0s\n",
            frame, (unsigned long long)hash[0], (unsigned long long)hash[1], "");
    else
        Log(LogLevel::Info, "HASH2D frame=%u top=%016llX bottom=%016llX\n",
            frame, (unsigned long long)hash[0], (unsigned long long)hash[1]);
}

void GPU::BlankFrame() noexcept
{
    Sync2D();

    int backbuf = FrontBuffer ? 0 : 1;
    int fbsize;
    if (GPU3D.UsesAccelLines())
        fbsize = GPU2D::AccelFramebufferWords;
    else
        fbsize = 256 * 192;

    memset(Framebuffer[backbuf][0].get(), 0, fbsize*4);
    memset(Framebuffer[backbuf][1].get(), 0, fbsize*4);

    FrontBuffer = backbuf;
    AssignFramebuffers();

    TotalScanlines = 263;
}

void GPU::StartScanline(u32 line) noexcept
{
    if (line == 0)
        VCount = 0;
    else if (NextVCount != 0xFFFFFFFF)
        VCount = NextVCount;
    else
        VCount++;

    NextVCount = -1;

    DispStat[0] &= ~(1<<1);
    DispStat[1] &= ~(1<<1);

    if (VCount == VMatch[0])
    {
        DispStat[0] |= (1<<2);

        if (DispStat[0] & (1<<5)) NDS.SetIRQ(0, IRQ_VCount);
    }
    else
        DispStat[0] &= ~(1<<2);

    if (VCount == VMatch[1])
    {
        DispStat[1] |= (1<<2);

        if (DispStat[1] & (1<<5)) NDS.SetIRQ(1, IRQ_VCount);
    }
    else
        DispStat[1] &= ~(1<<2);

    GPU2D_A.CheckWindows(VCount);
    GPU2D_B.CheckWindows(VCount);

    if (VCount >= 2 && VCount < 194)
        NDS.CheckDMAs(0, 0x03);
    else if (VCount == 194)
        NDS.StopDMAs(0, 0x03);

    if (line < 192)
    {
        if (line == 0)
        {
            // hi-res capture: a new frame's record. one that no Blit consumed (a VCount write
            // skipped line 192) is dropped, and its banks stay in NativeCapturedSincePass.
            // jobs still held from that frame (the capture stays armed without its VBlank) are
            // drawn during this one, and their native capture lands after this, so
            // NativeCapturedSincePass misses it: every bank counts as stale for this frame, and as
            // they go into this frame's record with an earlier frame's planes and 3D, its pass is
            // skipped
            if (Deferred2DPending && HiresCapture && !HiresCapRecord)
                HiresCapRecord = std::make_unique<HiresCaptureRecord>();
            if (HiresCapRecord)
            {
                HiresCapRecord->Clear();
                HiresCapRecord->StaleMask = NativeCapturedSincePass;
                if (Deferred2DPending)
                {
                    HiresCapRecord->StaleMask = 0xF;
                    HiresCapRecord->Skip |= HiresCapSkip_VCount;
                }
            }

            // a capture armed since the render at line 215, which may have drawn HalfCamera's in-between
            // scene: VBlankEnd is about to read the 3D back, and that can't go into VRAM. same order as line 215
            if (GPU3D.NeedsRealSceneRedraw())
            {
                if (BlitPending || BlitAfter3DPending)
                    Sync2D(CapFlush_LateBlit);
                if (Deferred2DPending)
                    Flush2DDeferred(CapFlush_HalfCam);
                GPU3D.RedrawRealScene(*this);
            }

            // ComputePace 2: the same for a capture armed after a render that was skipped to let
            // the GPU catch up, which would read an older scene's 3D back. the test is VBlankEnd's
            if ((GPU2D_A.CaptureCnt & (1u << 31)) && ((GPU2D_A.CaptureCnt >> 29) & 0x3) != 1
                && GPU3D.GetCurrentRenderer().NeedsCaptureRedraw())
            {
                if (BlitPending || BlitAfter3DPending)
                    Sync2D(CapFlush_LateBlit);
                if (Deferred2DPending)
                    Flush2DDeferred(CapFlush_HalfCam);
                GPU3D.GetCurrentRenderer().RedrawSkippedFrame(*this);
            }

            GPU2D_Renderer->VBlankEnd(&GPU2D_A, &GPU2D_B);
            GPU2D_A.VBlankEnd();
            GPU2D_B.VBlankEnd();
        }

        if (RunFIFO)
            NDS.ScheduleEvent(Event_DisplayFIFO, false, 32, 0, 0);
    }

    if (VCount == 262)
    {
        // frame end

        DispStat[0] &= ~(1<<0);
        DispStat[1] &= ~(1<<0);
    }
    else
    {
        if (VCount == 192)
        {
            // debug only, see GPU_CaptureReads.h: the Sync2D below ends the frame's held lines
            if (CapReads)
                CapReads->NoteBarrier();

            // debug only for now, see GPU_Shadow2D.h: the frame's sprite runs are complete, whenever
            // the Blit comes. the swi-harness counts the frame's lines here, as it has no Blit
            if (Shadow2D)
            {
                Shadow2D->Finalize();
                if (Shadow2D->SelfCheck && GPU3D.UsesAccelLines())
                    Shadow2D->CountFrame(*this, FrontBuffer ? 0 : 1);
            }

            // the last lines are usually still being drawn here, and nothing below needs them until
            // the GL compositor reads the 2D back buffers (the Blit at the end, or at line 215), so
            // the wait is there. the VBlank DMAs and the CPU only reach what a queued line reads
            // through the write barriers (LCDC banks, the mappings and the flat copies; LCDC reads
            // too, for the lines held for display capture; palette and OAM are copied), the 3D renderers
            // don't touch the 2D output, and with software 3D nothing reads it before FinishFrame,
            // which syncs

            // in reality rendering already finishes at line 144
            // and games might already start to modify texture memory.
            // That doesn't matter for us because we cache the entire
            // texture memory anyway and only update it before the start
            //of the next frame.
            // So we can give the rasteriser a bit more headroom
            GPU3D.VCount144(*this);

            // VBlank
            DispStat[0] |= (1<<0);
            DispStat[1] |= (1<<0);

            NDS.StopDMAs(0, 0x04);

            NDS.CheckDMAs(0, 0x01);
            NDS.CheckDMAs(1, 0x11);

            if (DispStat[0] & (1<<3)) NDS.SetIRQ(0, IRQ_VBlank);
            if (DispStat[1] & (1<<3)) NDS.SetIRQ(1, IRQ_VBlank);

            // before GPU2D_A.VBlank() ends the capture
            if (GPU2D_A.CaptureLatch)
                GPU3D.NoteDisplayCapture(GPU2D_A.CaptureCnt);

            GPU2D_A.VBlank();
            GPU2D_B.VBlank();
            GPU3D.VBlank();

            // Need a better way to identify the openGL renderer in particular
            if (GPU3D.IsRendererAccelerated() && Threaded2D && LateBlit && !BlitPending
                && !Deferred2DPending && !CaptureHeldThisFrame && !(HiresCapRecord && HiresCapRecord->DstMask))
            {
                // the Blit only waits until the 3D render at line 215 replaces the 3D frame it reads,
                // so the 2D thread's last lines overlap VBlank emulation (see LateBlit). of the
                // hi-res capture state it reads, only the bank write masks can change until then
                LateBlitCapWrites = HiresCapWrites;
                LateBlitCapWritesInCapture = HiresCapWritesInCapture;
                HiresCapWrites = 0;
                HiresCapWritesInCapture = 0;
                BlitPending = true;
            }
            else if (GPU3D.IsRendererAccelerated())
            {
                // the compositor uploads this frame's 2D back buffers, so every line must be drawn.
                // this is where the lines held for display capture are usually drawn. with
                // HeldTo2DThread bit 2 and the Blit after the 3D render, the 2D thread draws them by then
                bool blitAfter = BlitAfterReadback && CaptureHeldThisFrame && GPU3D.GetCurrentRenderer().SupportsBlitAfterRender();
                if (!blitAfter || !Queue2DDeferred())
                    Sync2D(CapFlush_Line192);

                if (blitAfter)
                {
                    // the Blit runs right after the 3D render at line 215 (see BlitAfterReadback).
                    // what it reads stays as it is now until then: the 2D back buffers (no line is
                    // drawn before line 215 without a Sync2D), the hi-res capture record (only
                    // capture lines write it) and the 3D frame, which the render keeps. the bank
                    // write masks are kept as of now, like LateBlit's, and the capture pass's
                    // destination bank is copied, as the CPU can write it meanwhile (once the 2D
                    // thread has drawn the held lines, with HeldTo2DThread: see Held2DDone)
                    LateBlitCapWrites = HiresCapWrites;
                    LateBlitCapWritesInCapture = HiresCapWritesInCapture;
                    HiresCapWrites = 0;
                    HiresCapWritesInCapture = 0;
                    if (!Held2DQueued)
                        KeepHiresCaptureDst();
                    BlitAfter3DPending = true;
                }
                else
                {
                    GPU3D.Blit(*this);
                    HiresCaptureBlitDone();

                    // compute renderer: submit the compositor now, while the GPU is idle after the
                    // capture map, instead of ahead of the 3D at the line-215 flush (see BlitFlushCapture)
                    if (BlitFlushCapture && CaptureHeldThisFrame && GPU3D.GetCurrentRenderer().FlushAfterCaptureBlit())
                        GPU3D.GetCurrentRenderer().FlushCommands();
                }
            }
        }
    }

    NDS.ScheduleEvent(Event_LCD, true, HBLANK_CYCLES, LCD_StartHBlank, line);
}


void GPU::SetDispStat(u32 cpu, u16 val, u16 mask) noexcept
{
    const u16 ro_mask = 0x0047;

    val &= (mask & ~ro_mask);
    DispStat[cpu] &= (~mask | ro_mask);
    DispStat[cpu] |= val;

    VMatch[cpu] = (DispStat[cpu] >> 8) | ((DispStat[cpu] & 0x80) << 1);
}

void GPU::SetVCount(u16 val, u16 mask) noexcept
{
    // the VCount register is 9 bits wide. a byte write merges with the value already
    // pending for the next scanline, or with the current VCount when there is none
    // (NextVCount is the no-write sentinel then, not a value to merge into)
    u16 base = (NextVCount != 0xFFFFFFFF) ? (u16)NextVCount : VCount;
    val = ((base & ~mask) | (val & mask)) & 0x1FF;

    // VCount write is delayed until the next scanline

    // TODO: how does the 3D engine react to VCount writes while it's rendering?
    // 3D engine seems to give up on the current frame in that situation, repeating the last two scanlines
    // TODO: also check the various DMA types that can be involved

    GPU3D.AbortFrame |= NextVCount != val;
    NextVCount = val;
}

template <u32 Size, u32 MappingGranularity>
NonStupidBitField<Size/VRAMDirtyGranularity> VRAMTrackingSet<Size, MappingGranularity>::DeriveState(const u32* currentMappings, GPU& gpu)
{
    NonStupidBitField<Size/VRAMDirtyGranularity> result;
    u16 banksToBeZeroed = 0;
    for (u32 i = 0; i < Size / MappingGranularity; i++)
    {
        if (currentMappings[i] != Mapping[i])
        {
            result.SetRange(i*VRAMBitsPerMapping, VRAMBitsPerMapping);
            banksToBeZeroed |= currentMappings[i];
            Mapping[i] = currentMappings[i];
        }
        else
        {
            u32 mapping = Mapping[i];

            banksToBeZeroed |= mapping;

            while (mapping != 0)
            {
                u32 num = __builtin_ctz(mapping);
                mapping &= ~(1 << num);

                // hack for **speed**
                // this could probably be done less ugly but then we would rely
                // on the compiler for vectorisation
                static_assert(VRAMDirtyGranularity == 512, "");
                if (MappingGranularity == 16*1024)
                {
                    u32 dirty = ((u32*)gpu.VRAMDirty[num].Data)[i & (gpu.VRAMMask[num] >> 14)];
                    result.Data[i / 2] |= (u64)dirty << ((i&1)*32);
                }
                else if (MappingGranularity == 8*1024)
                {
                    u16 dirty = ((u16*)gpu.VRAMDirty[num].Data)[i & (gpu.VRAMMask[num] >> 13)];
                    result.Data[i / 4] |= (u64)dirty << ((i&3)*16);
                }
                else if (MappingGranularity == 128*1024)
                {
                    result.Data[i * 4 + 0] |= gpu.VRAMDirty[num].Data[0];
                    result.Data[i * 4 + 1] |= gpu.VRAMDirty[num].Data[1];
                    result.Data[i * 4 + 2] |= gpu.VRAMDirty[num].Data[2];
                    result.Data[i * 4 + 3] |= gpu.VRAMDirty[num].Data[3];
                }
                else
                {
                    // welp
                    abort();
                }
            }
        }
    }

    while (banksToBeZeroed != 0)
    {
        u32 num = __builtin_ctz(banksToBeZeroed);
        banksToBeZeroed &= ~(1 << num);
        gpu.VRAMDirty[num].Clear();
    }

    return result;
}

template NonStupidBitField<32*1024/VRAMDirtyGranularity> VRAMTrackingSet<32*1024, 8*1024>::DeriveState(const u32*, GPU& gpu);
template NonStupidBitField<8*1024/VRAMDirtyGranularity> VRAMTrackingSet<8*1024, 8*1024>::DeriveState(const u32*, GPU& gpu);
template NonStupidBitField<512*1024/VRAMDirtyGranularity> VRAMTrackingSet<512*1024, 128*1024>::DeriveState(const u32*, GPU& gpu);
template NonStupidBitField<128*1024/VRAMDirtyGranularity> VRAMTrackingSet<128*1024, 16*1024>::DeriveState(const u32*, GPU& gpu);
template NonStupidBitField<256*1024/VRAMDirtyGranularity> VRAMTrackingSet<256*1024, 16*1024>::DeriveState(const u32*, GPU& gpu);
template NonStupidBitField<512*1024/VRAMDirtyGranularity> VRAMTrackingSet<512*1024, 16*1024>::DeriveState(const u32*, GPU& gpu);



bool GPU::MakeVRAMFlat_TextureCoherent(NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<128*1024>(VRAMFlat_Texture, VRAMMap_Texture, dirty, &GPU::ReadVRAM_Texture<u64>);
}
bool GPU::MakeVRAMFlat_TexPalCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<16*1024>(VRAMFlat_TexPal, VRAMMap_TexPal, dirty, &GPU::ReadVRAM_TexPal<u64>);
}

bool GPU::MakeVRAMFlat_ABGCoherent(NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<16*1024>(VRAMFlat_ABG, VRAMMap_ABG, dirty, &GPU::ReadVRAM_ABG<u64>);
}
bool GPU::MakeVRAMFlat_BBGCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<16*1024>(VRAMFlat_BBG, VRAMMap_BBG, dirty, &GPU::ReadVRAM_BBG<u64>);
}

bool GPU::MakeVRAMFlat_AOBJCoherent(NonStupidBitField<256*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<16*1024>(VRAMFlat_AOBJ, VRAMMap_AOBJ, dirty, &GPU::ReadVRAM_AOBJ<u64>);
}
bool GPU::MakeVRAMFlat_BOBJCoherent(NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<16*1024>(VRAMFlat_BOBJ, VRAMMap_BOBJ, dirty, &GPU::ReadVRAM_BOBJ<u64>);
}

bool GPU::MakeVRAMFlat_ABGExtPalCoherent(NonStupidBitField<32*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<8*1024>(VRAMFlat_ABGExtPal, VRAMMap_ABGExtPal, dirty, &GPU::ReadVRAM_ABGExtPal<u64>);
}
bool GPU::MakeVRAMFlat_BBGExtPalCoherent(NonStupidBitField<32*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<8*1024>(VRAMFlat_BBGExtPal, VRAMMap_BBGExtPal, dirty, &GPU::ReadVRAM_BBGExtPal<u64>);
}

bool GPU::MakeVRAMFlat_AOBJExtPalCoherent(NonStupidBitField<8*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<8*1024>(VRAMFlat_AOBJExtPal, &VRAMMap_AOBJExtPal, dirty, &GPU::ReadVRAM_AOBJExtPal<u64>);
}
bool GPU::MakeVRAMFlat_BOBJExtPalCoherent(NonStupidBitField<8*1024/VRAMDirtyGranularity>& dirty) noexcept
{
    return CopyLinearVRAM<8*1024>(VRAMFlat_BOBJExtPal, &VRAMMap_BOBJExtPal, dirty, &GPU::ReadVRAM_BOBJExtPal<u64>);
}
}
