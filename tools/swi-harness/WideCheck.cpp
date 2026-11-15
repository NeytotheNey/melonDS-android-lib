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

// Headless WideMelon check (plan-widemelon.md step 2). Run all the games with
//
//   sh tools/swi-harness/wide_check.sh
//
// or one with swi-harness.exe <rom.nds> --wide-check W --frames N --out <dir> [--state <file.mlN>]
// [--tap KEY@FRAME ...] [--box-test wide] [--timing native] [--wide-polys 1]. Writes summary.txt, frames.tsv and
// diffs.txt to <dir>. Exits 0 on PASS, 1 on FAIL, 2 if it couldn't run, 3 if INCONCLUSIVE.
//
// Two emulators run the same frames in lockstep with FreeBIOS: one with WideMelon off, one with
// the view W wide (GPU3D::WideCheck). After every frame it compares what the game can see and
// what the middle 256 shows, and fails on any difference:
// - both screens' 2D planes 0-2 (the accelerated line format, where the 3D layer is a placeholder),
//   plane 1 and 2 only on display mode 1 lines, as GPU::LogHash2D does;
// - the control words with the wings bits masked (18-22, and 8-12, the old wings fade EVY);
// - main RAM, shared and ARM7 WRAM, ITCM, DTCM, all VRAM banks (so the capture banks), palette, OAM;
// - timing, so a slip shows up the frame it happens instead of when it reaches memory: the ARM9,
//   ARM7 and geometry engine timestamps, GXSTAT, the GX FIFO level and DISP3DCNT but bit 13 (the
//   polygon/vertex RAM overflow flag: the wider view keeps more polygons, so it can fill the RAM
//   where the native one doesn't, as the drop counter counts; reported).
// With --box-test wide the game sees other box test results by design and runs differently, so
// memory and timing are then only reported.
// A wide run (W > 256) that never drew 3D, or whose wide view never kept a different number of
// polygons than the native one, didn't exercise the wide path (a hung game, a broken savestate,
// a 2D-only scene): it's INCONCLUSIVE, not a PASS.
// Left out on purpose, since they differ by design:
// - the 3D pixels: software 3D draws the squeezed scene into 256 columns, so both runs hand
//   display capture the same fixed pattern instead (GPU3D::GetLine);
// - geometry timing: the wider view keeps more polygons, which keep the pipeline busy longer. WideMelon
//   times each polygon by the native view's clip result instead, so the game should see stock timing,
//   but by default both runs time every polygon alike (GPU3D::SubmitPolygon), so a slip there can't
//   hide other differences; --timing native checks WideMelon's own timing;
// - box test results: answered as stock in both runs, unless --box-test wide, which squeezes them
//   as WideMelon does and then only reports memory differences;
// - the window edge fix (native columns 0 and 255 take their neighbor's window mask), which changes
//   the middle and display capture on purpose: done in both runs (GPU2D_Soft DrawScanline_BGOBJ);
// - the polygon RAM counts, reported (with how often the game read them).
// With --wide-polys 1 (GPU::WidePolys in both runs) the wide run keeps its scene in WideMelon's wide
// list and the game sees the native view's counts, so DISP3DCNT bit 13 is compared like the rest, the
// wide run's native counts have to be the 256 run's real ones every frame, and the values the game
// read from the count registers (a hash of them, GPU3D::WideCheckCountHash) have to match.
// The wings themselves are drawn by the GL compositor, so there's no wing picture here.
//
// SWI_HARNESS_SHADOW2D=on (the wide run) or both (both runs) also turns the GPU 2D shadow recorder on
// (GPU_Shadow2D.h, mode all, with its self-check), which only records: every result has to stay as it
// is. summary.txt then has a line with its block errors and how many lines it found eligible.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <filesystem>

#include "WideCheck.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#include "GPU_Shadow2D.h"
#include "Savestate.h"

using namespace melonDS;

namespace
{

bool ReadAll(const std::string& path, std::vector<u8>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseeko64(f, 0, SEEK_END);
    s64 len = ftello64(f);
    if (len < 0) { fclose(f); return false; }
    fseeko64(f, 0, SEEK_SET);
    out.resize(len);
    bool ok = fread(out.data(), 1, len, f) == (size_t)len;
    fclose(f);
    return ok;
}

std::unique_ptr<NDS> Boot(const WideCheckOptions& opts, int width, const std::vector<u8>& rom, const std::vector<u8>& state, bool shadow2d)
{
    NDSArgs args {}; // FreeBIOS and the generated firmware, as --bios free
    args.JIT = std::nullopt;
    args.GDB = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);

    nds->GPU.GPU3D.WideCheck = width;
    nds->GPU.GPU3D.WideCheckSqueezeBoxTest = opts.SqueezeBoxTest;
    nds->GPU.GPU3D.WideCheckEqualTiming = opts.EqualTiming;
    nds->GPU.WidePolys = opts.WidePolys; // latched at the reset below
    nds->SetRenderer3D(std::make_unique<SoftRenderer>()); // sizes the framebuffers for the switch
    if (shadow2d)
    {
        nds->GPU.SetShadow2D(2, false, false, 0);
        nds->GPU.Shadow2D->SelfCheck = true;
    }

    auto romdata = std::make_unique<u8[]>(rom.size());
    memcpy(romdata.get(), rom.data(), rom.size());
    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)rom.size(), nullptr, NDSCart::NDSCartArgs {});
    if (!cart) return nullptr;
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->RTC.SetDateTime(opts.RTC[0], opts.RTC[1], opts.RTC[2], opts.RTC[3], opts.RTC[4], opts.RTC[5]);
    nds->SetupDirectBoot(std::filesystem::path(opts.RomPath).filename().string());

    if (!state.empty())
    {
        // a copy of the bytes: Savestate only reads, but takes a non-const buffer
        std::vector<u8> buf = state;
        Savestate st(buf.data(), (u32)buf.size(), false);
        if (st.Error || !nds->DoSavestate(&st) || st.Error) return nullptr;
    }

    nds->Start();
    return nds;
}

struct Region { const char* Name; const u8* Off; const u8* On; u32 Size; };

std::vector<Region> Regions(NDS& off, NDS& on)
{
    std::vector<Region> r = {
        {"mainram", off.MainRAM, on.MainRAM, off.MainRAMMask + 1},
        {"sharedwram", off.SharedWRAM, on.SharedWRAM, off.SharedWRAMSize},
        {"arm7wram", off.ARM7WRAM, on.ARM7WRAM, ARM7WRAMSize},
        {"itcm", off.ARM9.ITCM, on.ARM9.ITCM, ITCMPhysicalSize},
        {"dtcm", off.ARM9.DTCM, on.ARM9.DTCM, DTCMPhysicalSize},
        {"palette", off.GPU.Palette, on.GPU.Palette, sizeof(off.GPU.Palette)},
        {"oam", off.GPU.OAM, on.GPU.OAM, sizeof(off.GPU.OAM)},
    };
    static const char* banks[9] = {"vram_a", "vram_b", "vram_c", "vram_d", "vram_e", "vram_f", "vram_g", "vram_h", "vram_i"};
    for (int i = 0; i < 9; i++)
        r.push_back({banks[i], off.GPU.VRAM[i], on.GPU.VRAM[i], off.GPU.VRAMMask[i] + 1});
    return r;
}

const u32 WingBits = 0x1F00 | 0x7C0000;

// DISP3DCNT bit 13, the polygon/vertex RAM overflow flag, which the wider view can set where the native one doesn't
const u32 Disp3DOverflow = 1 << 13;

// which timing values differ between the runs after a frame (bits as in frames.tsv's timing column)
enum { TimeARM9 = 1, TimeARM7 = 2, Time3D = 4, TimeGXStat = 8, TimeFIFO = 16, TimeDisp3D = 32 };
u32 TimingDiffs(NDS& off, NDS& on, u32 dispMask)
{
    const GPU3D& a = off.GPU.GPU3D;
    const GPU3D& b = on.GPU.GPU3D;
    return (off.ARM9Timestamp != on.ARM9Timestamp ? TimeARM9 : 0)
        | (off.ARM7Timestamp != on.ARM7Timestamp ? TimeARM7 : 0)
        | (a.Timestamp != b.Timestamp ? Time3D : 0)
        | (a.GXStat != b.GXStat ? TimeGXStat : 0)
        | (a.CmdFIFO.Level() != b.CmdFIFO.Level() ? TimeFIFO : 0)
        | (((a.DispCnt ^ b.DispCnt) & dispMask) ? TimeDisp3D : 0);
}

void AppendF(std::string& out, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

// plane 0 of a frame as a BMP, both screens stacked, for looking at a difference
void WritePlane0(NDS& nds, const std::string& path)
{
    std::vector<u32> pixels(256 * 384);
    for (int s = 0; s < 2; s++)
    {
        const u32* fb = nds.GPU.Framebuffer[nds.GPU.FrontBuffer][s].get();
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++)
            {
                u32 c = fb[(256*3 + 1) * y + x];
                u32 r = (c & 0x3F) << 2, g = ((c >> 8) & 0x3F) << 2, b = ((c >> 16) & 0x3F) << 2;
                pixels[(s * 192 + y) * 256 + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    u8 header[54] = {'B', 'M'};
    *(u32*)&header[2] = 54 + (u32)pixels.size() * 4;
    *(u32*)&header[10] = 54;
    *(u32*)&header[14] = 40;
    *(s32*)&header[18] = 256;
    *(s32*)&header[22] = -384;
    *(u16*)&header[26] = 1;
    *(u16*)&header[28] = 32;
    fwrite(header, 1, sizeof(header), f);
    fwrite(pixels.data(), 4, pixels.size(), f);
    fclose(f);
}

}

int RunWideCheck(const WideCheckOptions& opts)
{
    std::filesystem::create_directories(opts.OutDir);
    std::vector<u8> rom, state;
    if (!ReadAll(opts.RomPath, rom)) { fprintf(stderr, "can't read ROM %s\n", opts.RomPath.c_str()); return 2; }
    if (!opts.StatePath.empty() && !ReadAll(opts.StatePath, state)) { fprintf(stderr, "can't read %s\n", opts.StatePath.c_str()); return 2; }

    const char* shadowEnv = getenv("SWI_HARNESS_SHADOW2D");
    bool shadowBoth = shadowEnv && !strcmp(shadowEnv, "both");
    bool shadowOn = shadowEnv && (shadowBoth || !strcmp(shadowEnv, "on"));
    std::unique_ptr<NDS> off = Boot(opts, 256, rom, state, shadowBoth), on = Boot(opts, opts.Width, rom, state, shadowOn);
    if (!off || !on) { fprintf(stderr, "can't boot the ROM%s\n", state.empty() ? "" : " or load the savestate"); return 2; }
    rom.clear();
    rom.shrink_to_fit();
    std::vector<Region> regions = Regions(*off, *on);

    FILE* log = fopen((opts.OutDir + "/frames.tsv").c_str(), "w");
    FILE* diffs = fopen((opts.OutDir + "/diffs.txt").c_str(), "w");
    if (!log || !diffs) { fprintf(stderr, "can't write to %s\n", opts.OutDir.c_str()); return 2; }
    fprintf(log, "frame\tplane_px\tctrl_lines\tmem_regions\ttiming\tpolys_off\tpolys_on\twing3d_lines\twhite_lines\tcapture\n");

    u64 planeDiffs = 0, ctrlDiffs = 0, memDiffs = 0, polyFrames = 0, polysOff = 0, polysOn = 0;
    u32 frames3D = 0, captureFrames = 0;
    u32 failFrames = 0, firstFail = 0, reported = 0;
    u32 dropFrames = 0, maxFrameDrops = 0, lastDrops = 0;
    u32 nativeMisses = 0, nativeMaxPolys = 0, nativeMaxVerts = 0;
    u32 timingFrames = 0, firstTiming = 0, timingKinds = 0, overflowFrames = 0;
    u32 countHashFrames = 0, firstCountHash = 0;
    // with the wide list, bit 13 is the native view's, so it's compared like the rest
    const u32 dispMask = opts.WidePolys ? ~0u : ~Disp3DOverflow;
    for (u32 frame = 1; frame <= opts.Frames; frame++)
    {
        u32 keymask = 0xFFF;
        for (auto& tap : opts.Taps)
            if (frame - 1 >= tap.first && frame - 1 < tap.first + 6)
                keymask &= ~(1 << tap.second);
        off->SetKeyMask(keymask);
        on->SetKeyMask(keymask);
        // enabled by now, display capture runs from line 0 of this frame
        u32 capture = (off->GPU.GPU2D_A.CaptureCnt >> 31) & 1;
        captureFrames += capture;
        off->RunFrame();
        on->RunFrame();

        u32 plane = 0, ctrl = 0, mem = 0, wing3d = 0, white = 0;
        auto note = [&](const char* fmt, auto... args) {
            if (reported++ < 200) { fprintf(diffs, "frame %u: ", frame); fprintf(diffs, fmt, args...); fputc('\n', diffs); }
        };
        for (int s = 0; s < 2; s++)
        {
            const u32* a = off->GPU.Framebuffer[off->GPU.FrontBuffer][s].get();
            const u32* b = on->GPU.Framebuffer[on->GPU.FrontBuffer][s].get();
            for (int y = 0; y < 192; y++, a += 256*3 + 1, b += 256*3 + 1)
            {
                u32 ca = a[256*3], cb = b[256*3];
                wing3d += (cb >> 18) & 1;
                white += (cb >> 21) & 1;
                if ((ca & ~WingBits) != (cb & ~WingBits))
                {
                    ctrl++;
                    note("screen %d line %d control %08X vs %08X", s, y, ca, cb);
                }
                int count = (((ca >> 16) & 0x3) == 1 && ((cb >> 16) & 0x3) == 1) ? 256*3 : 256;
                for (int i = 0; i < count; i++)
                {
                    if (a[i] == b[i]) continue;
                    plane++;
                    note("screen %d line %d plane %d x %d: %08X vs %08X", s, y, i >> 8, i & 0xFF, a[i], b[i]);
                }
            }
        }
        for (const Region& r : regions)
        {
            if (!memcmp(r.Off, r.On, r.Size)) continue;
            mem++;
            u32 i = 0, n = 0;
            while (r.Off[i] == r.On[i]) i++;
            for (u32 j = i; j < r.Size; j++) n += r.Off[j] != r.On[j];
            note("%s: %u bytes differ, first at %05X (%02X vs %02X)", r.Name, n, i, r.Off[i], r.On[i]);
        }

        u32 timing = TimingDiffs(*off, *on, dispMask);
        if (timing)
        {
            if (!timingFrames++) firstTiming = frame;
            timingKinds |= timing;
            if (timingFrames <= 20)
                note("timing differs (%02X): ARM9 %llu vs %llu, ARM7 %llu vs %llu, 3D %llu vs %llu, GXSTAT %08X vs %08X, "
                    "FIFO %u vs %u, DISP3DCNT %04X vs %04X", timing,
                    (unsigned long long)off->ARM9Timestamp, (unsigned long long)on->ARM9Timestamp,
                    (unsigned long long)off->ARM7Timestamp, (unsigned long long)on->ARM7Timestamp,
                    (unsigned long long)off->GPU.GPU3D.Timestamp, (unsigned long long)on->GPU.GPU3D.Timestamp,
                    off->GPU.GPU3D.GXStat, on->GPU.GPU3D.GXStat, off->GPU.GPU3D.CmdFIFO.Level(), on->GPU.GPU3D.CmdFIFO.Level(),
                    off->GPU.GPU3D.DispCnt, on->GPU.GPU3D.DispCnt);
        }
        overflowFrames += ((off->GPU.GPU3D.DispCnt ^ on->GPU.GPU3D.DispCnt) & Disp3DOverflow) != 0;

        u32 pa = off->GPU.GPU3D.RenderNumPolygons, pb = on->GPU.GPU3D.RenderNumPolygons;
        polysOff += pa; polysOn += pb; polyFrames += pa != pb; frames3D += pa != 0;
        u32 drops = on->GPU.GPU3D.DroppedPolygons - lastDrops;
        lastDrops += drops;
        dropFrames += drops > 0;
        maxFrameDrops = std::max(maxFrameDrops, drops);
        // the on run's estimate of the native view's polygon RAM counts against the off run's real ones.
        // with the wide list the game reads them, so they have to match (a fail)
        bool nativeMiss = false, countHashMiss = false;
        if (opts.Width != 256)
        {
            const GPU3D& g0 = off->GPU.GPU3D;
            const GPU3D& g1 = on->GPU.GPU3D;
            u32 dp = (u32)std::abs((s64)g1.GetNativeNumPolygons() - g0.NumPolygons);
            u32 dv = (u32)std::abs((s64)g1.GetNativeNumVertices() - g0.NumVertices);
            nativeMiss = dp || dv;
            nativeMisses += nativeMiss;
            nativeMaxPolys = std::max(nativeMaxPolys, dp);
            nativeMaxVerts = std::max(nativeMaxVerts, dv);
            if (nativeMiss && opts.WidePolys && nativeMisses <= 20)
                note("native counts differ: off %u polygons %u vertices, on (native view) %u / %u", g0.NumPolygons, g0.NumVertices,
                    g1.GetNativeNumPolygons(), g1.GetNativeNumVertices());
            // what the game read from the count registers, so far
            countHashMiss = g0.WideCheckCountHash != g1.WideCheckCountHash;
            if (countHashMiss && !countHashFrames++)
            {
                firstCountHash = frame;
                if (opts.WidePolys)
                    note("polygon RAM count reads differ from here on (%u reads off, %u on)", g0.WideCheckCountReads, g1.WideCheckCountReads);
            }
        }
        planeDiffs += plane; ctrlDiffs += ctrl; memDiffs += mem;
        fprintf(log, "%u\t%u\t%u\t%u\t%02X\t%u\t%u\t%u\t%u\t%u\n", frame, plane, ctrl, mem, timing, pa, pb, wing3d, white, capture);

        if (plane || ctrl || ((mem || timing) && !opts.SqueezeBoxTest) || (opts.WidePolys && (nativeMiss || countHashMiss)))
        {
            if (!failFrames++)
            {
                firstFail = frame;
                WritePlane0(*off, opts.OutDir + "/first_diff_off.bmp");
                WritePlane0(*on, opts.OutDir + "/first_diff_on.bmp");
            }
        }
        if (frame == opts.Frames)
            WritePlane0(*on, opts.OutDir + "/last_frame_plane0.bmp");
    }

    const GPU3D& g0 = off->GPU.GPU3D;
    const GPU3D& g1 = on->GPU.GPU3D;

    // a wide run has to show the wide path ran: 3D drawn, and the wide view keeping another polygon
    // count than the native one at least once. without that a PASS proves nothing about it
    const char* inconclusive = nullptr;
    if (!failFrames && opts.Width > 256)
    {
        if (!frames3D) inconclusive = "no frame drew 3D";
        else if (!polyFrames) inconclusive = "the wide view never kept a different number of polygons";
    }
    const char* result = failFrames ? "FAIL" : inconclusive ? "INCONCLUSIVE" : "PASS";

    std::string summary;
    AppendF(summary, "%s: %u frames, wide %d, %s timing, %s box tests, wide polys %d: %s%s%s\n",
        std::filesystem::path(opts.RomPath).filename().string().c_str(), opts.Frames, opts.Width,
        opts.EqualTiming ? "equalized" : "native", opts.SqueezeBoxTest ? "squeezed" : "stock", opts.WidePolys ? 1 : 0, result,
        inconclusive ? ", " : "", inconclusive ? inconclusive : "");

    // what a PASS here does and doesn't show
    if (opts.Width == 256)
        AppendF(summary, "  a PASS means: both runs had WideMelon off and matched, a sanity run of the check itself\n");
    else if (opts.SqueezeBoxTest)
        AppendF(summary, "  a PASS means: with box tests squeezed as WideMelon does them, the middle 256 (2D planes 0-2,\n"
            "  control words) still matched; the game sees other box test results by design, so memory and\n"
            "  timing were only reported%s\n", opts.EqualTiming ? " (polygon timing equalized)" : "");
    else if (!opts.EqualTiming)
        AppendF(summary, "  a PASS means: WideMelon's own polygon timing (by the native view's clip result) gave the game\n"
            "  stock timing (ARM9/ARM7/geometry timestamps, GXSTAT, FIFO level, DISP3DCNT matched every frame),\n"
            "  and the middle 256 and game memory matched; box tests answered as stock, the window edge fix\n"
            "  and the 3D pixels made alike in both runs\n");
    else
        AppendF(summary, "  a PASS means: the wide 2D path left the middle 256 (2D planes 0-2, control words) and game\n"
            "  memory untouched, and the equalizations held (ARM9/ARM7/geometry timestamps, GXSTAT, FIFO level,\n"
            "  DISP3DCNT matched every frame). equalized in both runs: polygon pipeline timing (every polygon\n"
            "  timed as kept with its sent vertices), box test results (stock), the window edge fix, and the 3D\n"
            "  pixels (a fixed pattern). it doesn't mean WideMelon as a whole is invisible to the game: its box\n"
            "  tests and timing are checked by --box-test wide and --timing native\n");

    AppendF(summary, "  differing frames %u (first %u): plane pixels %llu, control lines %llu, memory regions %llu,\n"
        "  timing in %u frames (first %u%s%s%s%s%s%s)\n",
        failFrames, firstFail, (unsigned long long)planeDiffs, (unsigned long long)ctrlDiffs, (unsigned long long)memDiffs,
        timingFrames, firstTiming,
        (timingKinds & TimeARM9) ? ", ARM9" : "", (timingKinds & TimeARM7) ? ", ARM7" : "", (timingKinds & Time3D) ? ", 3D" : "",
        (timingKinds & TimeGXStat) ? ", GXSTAT" : "", (timingKinds & TimeFIFO) ? ", FIFO" : "", (timingKinds & TimeDisp3D) ? ", DISP3DCNT" : "");
    AppendF(summary, "  frames with 3D %u, with display capture %u\n", frames3D, captureFrames);
    if (opts.WidePolys)
        AppendF(summary, "  wide polys: DISP3DCNT bit 13 compared, the native counts and the count reads have to match: native counts\n"
            "  off in %u frames, count reads differing from frame %u (%u frames)\n", nativeMisses, firstCountHash, countHashFrames);
    AppendF(summary, "  info: polygons drawn off %llu / on %llu (differ in %llu frames), polygon RAM count reads %u off / %u on,\n"
        "  box tests (%s) %u/%u hit off, %u/%u on (%u by the wide retry)\n",
        (unsigned long long)polysOff, (unsigned long long)polysOn, (unsigned long long)polyFrames,
        g0.WideCheckCountReads, g1.WideCheckCountReads, opts.SqueezeBoxTest ? "squeezed, memory not compared" : "stock",
        g0.WideCheckBoxHits, g0.WideCheckBoxTests, g1.WideCheckBoxHits, g1.WideCheckBoxTests, g1.WideCheckBoxRetryHits);
    AppendF(summary, "  polygons dropped as the polygon or vertex RAM was full: off %u / on %u (%u of them would have fit natively,\n"
        "  %u only the wide view kept), on in %u frames, at most %u in one; the on run's native polygon RAM counts were\n"
        "  off in %u frames (by at most %u polygons, %u vertices); the DISP3DCNT overflow flag differed in %u frames\n",
        g0.DroppedPolygons, g1.DroppedPolygons, g1.DroppedNativeFit, g1.DroppedWing, dropFrames, maxFrameDrops,
        nativeMisses, nativeMaxPolys, nativeMaxVerts, overflowFrames);
    if (shadowOn)
    {
        const Shadow2DRecorder& rec = *on->GPU.Shadow2D;
        u32 total = 0;
        for (u32 count : rec.ReasonCounts) total += count;
        AppendF(summary, "  GPU 2D shadow recorder on in %s: the on run's blocks had %u self-check errors, %u of %u lines eligible\n",
            shadowBoth ? "both runs" : "the on run", rec.SelfCheckErrors, rec.ReasonCounts[Shadow2D::R_None], total);
    }
    fputs(summary.c_str(), stdout);
    if (FILE* f = fopen((opts.OutDir + "/summary.txt").c_str(), "w")) { fputs(summary.c_str(), f); fclose(f); }

    fclose(log);
    fclose(diffs);
    off->Stop();
    on->Stop();
    return failFrames ? 1 : inconclusive ? 3 : 0;
}
