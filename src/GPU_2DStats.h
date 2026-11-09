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

#ifndef GPU_2DSTATS_H
#define GPU_2DSTATS_H

#include <chrono>
#include <memory>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "GPU.h"
#include "GPU_CaptureReads.h"
#include "Platform.h"

namespace melonDS
{

// debug only (debug.melonade.gpu2dstats, GPU::Stats2D): what a GPU 2D renderer would have to deal with
// in a game (docs/plan-gpu-2d.md, step 1). how many lines would look different at hi-res, and how often
// memory changes mid-frame, which forces it to draw in bands or fall back. it only counts: nothing here
// changes emulation, runs a barrier or goes into savestates, and it's all on the emulator thread, when
// each LineJob is built, so threaded 2D, deferred capture and the renderer make no difference.
//
// a BG pass reads memory as of its own line's HBlank, and a sprite pass as of the HBlank before its line
// (sprite line 0 at VCount 262), so a change is counted at the line it first shows on. all of OBJ VRAM and
// OAM go by sprite line. line 0 is the frame's starting state, so it's the between-frame uploads and not a
// mid-frame change. a frame's sprite line 0 is counted in the frame before it, as it's built before
// FinishFrame. the first frame after a reset or a load is dropped, because the flat copies' trackers and
// the private copies below start over and everything would show as changed in it.
//
// approximate:
// - VRAM goes by the flat copies' dirty sets, so it's 512-byte pages written or remapped, whatever the value
//   and whether or not anything reads them. that's the same thing a renderer uploading from those sets would see.
// - palette, OAM and registers are compared with the last line's, so a change undone within a line isn't seen.
// - the affine reference points are only checked for a write without vertical mosaic on that BG.
// - sprite features only count sprites that reach the screen, by their box. spr counts the off-screen ones too.
//
// one line per second per engine, as space separated key=value with commas inside a value. text through 3d
// and the sprite fields only count while the engine is on and not forced blank. the rest count either way.
//
//   eng     A or B
//   frames  frames in this line. the first average below is over all of them, not just those with any
//   lines   lines seen, active: engine on and not forced blank
//   disp    lines per display mode: 0,1,2,3
//   text affrot affid ext bmp8 bmp16 large: lines per enabled layer, so a line with two layers counts twice.
//           affrot and affid are affine layers with and without a non-identity matrix
//   extrot bmprot largerot: how many of the ext, bmp8+bmp16 and large ones have a non-identity matrix
//   rotbg rotobj hires: lines in display mode 1 with a non-identity BG, a non-identity sprite, or either.
//           hires is the number of visible lines that would look different at a higher resolution
//   bgmos objmos win blend 3d: lines with BG mosaic (on an enabled layer, size not 0), a sprite with mosaic on
//           that line (size not 0), a window on (DISPCNT), BLDCNT effect not 0, 3D on BG0
//   bright  lines where master brightness does something: mode 1 or 2, factor not 0
//   cap     lines DoCapture runs for, even if the bank isn't in LCDC mode. always 0 on engine B
//   spr vis sprrot sprid sprbmp sprmos sprwin sprsemi: per frame from OAM at sprite line 0, as avg,max:
//           enabled sprites, those on screen, and of those: affine with a non-identity matrix, affine with
//           identity, bitmap, mosaic (size not 0), OBJ window, semi-transparent. 0 if OBJ is off for that line
//   bgv objv extp pal bd oam regs: lines 1-191 at which a class changed, as frames with any,avg,max per frame:
//           BG VRAM, OBJ VRAM, extended palettes (BG and OBJ), standard palette without the backdrop, the
//           backdrop color only, OAM, and DISPCNT/BGCNT (and the layer enable and forced blank latches)
//   bands   the same for the distinct lines where any of those changed. the key figure
//   bandsnb the same again without bd, for a renderer that takes the backdrop color per line
//   scroll  the same for scroll changes on an enabled text BG or 3D BG0, per line state with no band
//   aff     the same for matrix or reference point writes on an enabled affine, extended or large BG, no band
//   upbg upobj: BG and OBJ VRAM dirty between frames, as avg,max KB per frame, to size uploads
//   bandsvis as bands, but only for lines that show: on, not forced blank, display mode 1. a change on a line
//           that doesn't show counts at the next line that does, as that one depends on it, and only if an
//           earlier line of the frame showed, since otherwise nothing has been drawn that it could cut short.
//           a change with no line showing after it doesn't count. capture reads lines that don't show,
//           so with display mode 2 and capture this is too low for what the capture needs
//   uptotal all dirty pages of the frame's checks as avg,max KB per frame: BG, OBJ and extended palettes, between
//           frames and mid-frame. a page dirty at two checks counts twice, as a renderer uploading from
//           each check would send it twice. it's every check, including those on lines that don't show
//   dirtymid real: BG, OBJ and extended palette pages dirty at lines 1-191, and of those the ones whose bytes
//           changed (against a private copy of the flats), as frames with any,avg,max pages per frame. a page
//           dirty at two lines counts twice. realpal: the same for the two 512-byte standard palette pages,
//           without the backdrop color
//   log logev: the page log's size per frame as frames with any,avg,max: distinct pages that really changed
//           mid-frame (VRAM and palette), and all of their changes (real + realpal). logkb is log in KB.
//           logboth logbothkb logbothev, on A only: the same for both engines together
struct GPU2DStats
{
    enum : u32
    {
        Change_BGVRAM = 0,
        Change_OBJVRAM,
        Change_ExtPal,
        Change_Palette,
        Change_Backdrop,
        Change_OAM,
        Change_Regs,
        Change_LastBand = Change_Regs,  // the ones above force a band
        Change_Scroll,
        Change_Affine,
        Change_Count,
    };

    enum : u32
    {
        Sprite_Enabled = 0,
        Sprite_Visible,
        Sprite_AffineRot,
        Sprite_AffineID,
        Sprite_Bitmap,
        Sprite_Mosaic,
        Sprite_Window,
        Sprite_Semi,
        Sprite_Count,
    };

    // one number per frame, over a log line
    struct Series
    {
        u32 FramesWithAny;
        u64 Sum;
        u32 Max;

        void Add(u32 value) noexcept
        {
            if (value)
                FramesWithAny++;
            Sum += value;
            if (value > Max)
                Max = value;
        }
    };

    struct Period
    {
        u32 Frames, Lines, Active, Disp[4];
        u32 Text, AffRot, AffID, Ext, Bmp8, Bmp16, Large, ExtRot, BmpRot, LargeRot;
        u32 RotBG, RotOBJ, Hires;
        u32 BGMosaic, OBJMosaic, Window, Blend, BG3D, Bright, Capture;
        Series Sprites[Sprite_Count];
        Series Changes[Change_Count];
        Series Bands, BandsNoBackdrop;
        Series UploadBG, UploadOBJ;     // in 512-byte pages
        Series BandsVisible;
        Series UploadTotal;             // in 512-byte pages
        Series Dirty, Real, RealPal, Log, LogEvents, Both, BothEvents;  // pages
    };

    // the flats per engine for real, as 512-byte pages: BG, BG and OBJ extended palettes, OBJ, then the
    // standard palette's BG and OBJ pages
    enum : u32 { Flat_BG = 0, Flat_BGExtPal, Flat_OBJExtPal, Flat_OBJ, Flat_Count };
    static constexpr u32 FlatPages[2][Flat_Count] = {{1024, 64, 16, 512}, {256, 64, 16, 256}};
    static constexpr u32 PalettePage = 1616, PageWords = (1618 + 63) / 64, ShadowPages = 1616 + 592;

    // sets of the visible lines, one bit each
    struct Frame
    {
        u32 Changes[Change_Count];
        u64 Bands[3], BandsNoBackdrop[3];
        u64 RotBG[3], RotOBJ[3], Mode1[3];
        u64 Visible[3];     // on, not forced blank and display mode 1
        u32 UploadPages;
        u32 DirtyPages, RealPages, RealPalPages, LogPages;
        u64 Logged[PageWords];
    };

    struct Engine
    {
        Period Second;
        Frame Cur;
        GPU2D::UnitState Prev;  // the last line's BG snapshot
        u32 BGPages, OBJPages, ExtPages;    // what this line's checks of the flat copies found
        u8 Palette[0x400];      // as of the last line, since PaletteDirty and OAMDirty are never read
        u8 OAM[0x400];
        u16 PendingBG[1104], PendingOBJ[512];   // pages that really changed at this line's checks
        u32 NumPendingBG, NumPendingOBJ;
    };

    Engine Engines[2] {};
    bool SkipFrame = true;
    s64 LogStartNs = 0;
    u32 Synced = 0;                 // flats whose private copy is whole, one bit per engine and Flat_
    std::unique_ptr<u8[]> Shadow;   // the private copy, ShadowPages pages, kept over Reset
    u32 LogFrames = 0;              // the swi-harness: a line every this many frames, not once a second. kept over Reset

    // GPU::Reset and a savestate load
    void Reset() noexcept
    {
        std::unique_ptr<u8[]> shadow = std::move(Shadow);
        u32 logFrames = LogFrames;
        *this = GPU2DStats();
        Shadow = std::move(shadow);
        LogFrames = logFrames;
    }

    // after the flat copy: which of the dirty pages really changed. the private copy is taken whole the
    // first time, then only the dirty pages, and the flats only change at these copies
    void NoteCopied(u32 num, u32 flat, const u8* data, const u64* bits, u32 words) noexcept
    {
        if (!Shadow)
            Shadow = std::make_unique<u8[]>(ShadowPages * 512);
        u32 base = 0;
        for (u32 i = 0; i < flat; i++)
            base += FlatPages[num][i];
        u8* shadow = Shadow.get() + ((num ? 1616 : 0) + base) * 512;

        u32 bit = 1u << (num * Flat_Count + flat);
        if (!(Synced & bit))
        {
            memcpy(shadow, data, FlatPages[num][flat] * 512);
            Synced |= bit;
            return;
        }

        Engine& e = Engines[num];
        for (u32 w = 0; w < words; w++)
        {
            for (u64 b = bits[w]; b; b &= b - 1)
            {
                u32 offset = (w * 64 + __builtin_ctzll(b)) * 512;
                if (memcmp(shadow + offset, data + offset, 512) == 0)
                    continue;
                memcpy(shadow + offset, data + offset, 512);
                u16 page = (u16)(base + offset / 512);
                if (flat == Flat_OBJ)
                    e.PendingOBJ[e.NumPendingOBJ++] = page;
                else
                    e.PendingBG[e.NumPendingBG++] = page;
            }
        }
    }

    // GPU::MakeVRAMFlat_2DBGCoherent, with the dirty sets it just worked out, which nothing else can get
    // once DeriveState has cleared the banks' bits. they're per engine and BG or OBJ already
    void NoteBGDirty(u32 num, const u64* bits, u32 words, const u64* bgExtBits, u32 bgExtWords, const u64* objExtBits, u32 objExtWords) noexcept
    {
        Engines[num].BGPages = CountPages(bits, words);
        Engines[num].ExtPages = CountPages(bgExtBits, bgExtWords) + CountPages(objExtBits, objExtWords);
    }

    // GPU::MakeVRAMFlat_2DOBJCoherent
    void NoteOBJDirty(u32 num, const u64* bits, u32 words) noexcept
    {
        Engines[num].OBJPages = CountPages(bits, words);
    }

    // GPU::Snapshot2DScanline
    void NoteLine(const GPU2D::LineJob& job) noexcept
    {
        for (u32 num = 0; num < 2; num++)
            NoteEngineLine(Engines[num], job, num);
    }

    // GPU::Snapshot2DSprites
    void NoteSprites(const GPU2D::LineJob& job) noexcept
    {
        for (u32 num = 0; num < 2; num++)
            NoteEngineSprites(Engines[num], job, num);
    }

    // GPU::FinishFrame
    void EndFrame() noexcept
    {
        Period& a = Engines[0].Second;
        a.Both.Add(Engines[0].Cur.LogPages + Engines[1].Cur.LogPages);
        a.BothEvents.Add(Engines[0].Cur.RealPages + Engines[0].Cur.RealPalPages
                         + Engines[1].Cur.RealPages + Engines[1].Cur.RealPalPages);
        for (Engine& e : Engines)
            FinishEngineFrame(e);

        if (SkipFrame)
        {
            SkipFrame = false;
            for (Engine& e : Engines)
                e.Second = {};
            return;
        }

        if (LogFrames)
        {
            if (Engines[0].Second.Frames < LogFrames)
                return;
        }
        else
        {
            s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (LogStartNs == 0)
                LogStartNs = now;
            if (now - LogStartNs < 1000000000)
                return;
            LogStartNs = now;
        }

        for (u32 num = 0; num < 2; num++)
        {
            LogPeriod(Engines[num].Second, num);
            Engines[num].Second = {};
        }
    }

private:
    static u32 CountPages(const u64* bits, u32 words) noexcept
    {
        u32 pages = 0;
        for (u32 i = 0; i < words; i++)
            pages += __builtin_popcountll(bits[i]);
        return pages;
    }

    // a page that really changed mid-frame, which the page log would keep
    static void LogPage(Engine& e, u32 page) noexcept
    {
        u64 bit = 1ull << (page & 63);
        if (!(e.Cur.Logged[page >> 6] & bit))
            e.Cur.LogPages++;
        e.Cur.Logged[page >> 6] |= bit;
    }

    static void SetLine(u64* lines, u32 line) noexcept
    {
        lines[line >> 6] |= 1ull << (line & 63);
    }

    static void MarkChange(Engine& e, u32 change, u32 line) noexcept
    {
        e.Cur.Changes[change]++;
        if (change <= Change_LastBand)
        {
            SetLine(e.Cur.Bands, line);
            if (change != Change_Backdrop)
                SetLine(e.Cur.BandsNoBackdrop, line);
        }
    }

    static bool IsIdentity(s16 a, s16 b, s16 c, s16 d) noexcept
    {
        return a == 0x100 && b == 0 && c == 0 && d == 0x100;
    }

    static void NoteEngineLine(Engine& e, const GPU2D::LineJob& job, u32 num) noexcept
    {
        const GPU2D::UnitState& u = job.BG[num];
        const u32 line = job.Line;
        Period& s = e.Second;

        s.Lines++;
        u32 dispMode = (u.DispCnt >> 16) & (num ? 0x1 : 0x3);
        s.Disp[dispMode]++;
        if (dispMode == 1)
            SetLine(e.Cur.Mode1, line);

        u32 brightMode = u.MasterBrightness >> 14;
        if ((brightMode == 1 || brightMode == 2) && (u.MasterBrightness & 0x1F))
            s.Bright++;

        // the check in front of DoCapture in SoftRenderer::DrawScanline
        static constexpr u32 captureHeights[4] = {128, 64, 128, 192};
        if (!num && u.CaptureLatch && job.VCount < captureHeights[(u.CaptureCnt >> 20) & 0x3])
            s.Capture++;

        bool active = u.Enabled && !u.ForcedBlank;
        if (active)
            CountLayers(e, u, line, num, dispMode);
        if (active && dispMode == 1)
            SetLine(e.Cur.Visible, line);

        e.Cur.UploadPages += e.BGPages + e.ExtPages;

        const u8* palette = job.Palette + num * 0x400;
        bool backdrop = memcmp(e.Palette, palette, 2) != 0;
        bool palBG = memcmp(e.Palette + 2, palette + 2, 0x200 - 2) != 0;
        bool palOBJ = memcmp(e.Palette + 0x200, palette + 0x200, 0x200) != 0;
        bool standard = palBG || palOBJ;
        if (backdrop || standard)
            memcpy(e.Palette, palette, 0x400);

        if (line != 0)
        {
            e.Cur.DirtyPages += e.BGPages + e.ExtPages;
            e.Cur.RealPages += e.NumPendingBG;
            e.Cur.RealPalPages += palBG + palOBJ;
            for (u32 i = 0; i < e.NumPendingBG; i++)
                LogPage(e, e.PendingBG[i]);
            if (palBG) LogPage(e, PalettePage);
            if (palOBJ) LogPage(e, PalettePage + 1);
        }
        e.NumPendingBG = 0;

        if (line == 0)
        {
            s.UploadBG.Add(e.BGPages);
        }
        else
        {
            if (e.BGPages) MarkChange(e, Change_BGVRAM, line);
            if (e.ExtPages) MarkChange(e, Change_ExtPal, line);
            if (standard) MarkChange(e, Change_Palette, line);
            if (backdrop) MarkChange(e, Change_Backdrop, line);

            const GPU2D::UnitState& p = e.Prev;
            if (u.DispCnt != p.DispCnt || u.LayerEnable != p.LayerEnable || u.ForcedBlank != p.ForcedBlank
                || memcmp(u.BGCnt, p.BGCnt, sizeof(u.BGCnt)) != 0)
                MarkChange(e, Change_Regs, line);

            if (active && p.Enabled)
                CompareLayerRegs(e, u, line, num);
        }

        e.Prev = u;
    }

    static void CountLayers(Engine& e, const GPU2D::UnitState& u, u32 line, u32 num, u32 dispMode) noexcept
    {
        Period& s = e.Second;
        s.Active++;
        if (u.DispCnt & 0xE000)
            s.Window++;
        if ((u.BlendCnt >> 6) & 0x3)
            s.Blend++;
        if (!num && (u.LayerEnable & 0x1) && (u.DispCnt & 0x8))
            s.BG3D++;

        bool mosaic = false, rot = false;
        for (u32 layer = 0; layer < 4; layer++)
        {
            int type = CaptureReadCounter::LayerType(u, layer, num != 0);
            if (type == CaptureReadCounter::Layer_None)
                continue;

            if ((u.BGCnt[layer] & 0x0040) && (u.BGMosaicSize[0] || u.BGMosaicSize[1]))
                mosaic = true;

            if (type == CaptureReadCounter::Layer_Text)
            {
                s.Text++;
                continue;
            }

            u32 i = layer - 2;
            bool identity = IsIdentity(u.BGRotA[i], u.BGRotB[i], u.BGRotC[i], u.BGRotD[i]);
            rot = rot || !identity;
            if (type == CaptureReadCounter::Layer_Affine)
            {
                if (identity) s.AffID++;
                else s.AffRot++;
            }
            else if (type == CaptureReadCounter::Layer_Large)
            {
                s.Large++;
                if (!identity) s.LargeRot++;
            }
            else if (u.BGCnt[layer] & 0x0080)
            {
                if (u.BGCnt[layer] & 0x0004) s.Bmp16++;
                else s.Bmp8++;
                if (!identity) s.BmpRot++;
            }
            else
            {
                s.Ext++;
                if (!identity) s.ExtRot++;
            }
        }

        if (mosaic)
            s.BGMosaic++;
        if (rot && dispMode == 1)
            SetLine(e.Cur.RotBG, line);
    }

    static void CompareLayerRegs(Engine& e, const GPU2D::UnitState& u, u32 line, u32 num) noexcept
    {
        const GPU2D::UnitState& p = e.Prev;
        bool scroll = false, affine = false;
        for (u32 layer = 0; layer < 4; layer++)
        {
            int type = CaptureReadCounter::LayerType(u, layer, num != 0);
            bool bg3D = !num && layer == 0 && (u.LayerEnable & 0x1) && (u.DispCnt & 0x8);
            if (type == CaptureReadCounter::Layer_Text || bg3D)
            {
                scroll = scroll || u.BGXPos[layer] != p.BGXPos[layer] || u.BGYPos[layer] != p.BGYPos[layer];
                continue;
            }
            if (type == CaptureReadCounter::Layer_None)
                continue;

            u32 i = layer - 2;
            affine = affine || u.BGRotA[i] != p.BGRotA[i] || u.BGRotB[i] != p.BGRotB[i]
                || u.BGRotC[i] != p.BGRotC[i] || u.BGRotD[i] != p.BGRotD[i];

            // Unit::PostDrawScanline advances them by B and D on every line the layer is enabled, so
            // anything else was a write. with vertical mosaic the latched ones don't follow along
            if ((u.BGCnt[layer] | p.BGCnt[layer]) & 0x0040)
                continue;
            bool advanced = p.LayerEnable & (4 << i);
            s32 expectX = (s32)((u32)p.BGXRefInternal[i] + (advanced ? (u32)(s32)p.BGRotB[i] : 0));
            s32 expectY = (s32)((u32)p.BGYRefInternal[i] + (advanced ? (u32)(s32)p.BGRotD[i] : 0));
            affine = affine || u.BGXRefInternal[i] != expectX || u.BGYRefInternal[i] != expectY;
        }

        if (scroll) MarkChange(e, Change_Scroll, line);
        if (affine) MarkChange(e, Change_Affine, line);
    }

    static void NoteEngineSprites(Engine& e, const GPU2D::LineJob& job, u32 num) noexcept
    {
        const GPU2D::UnitState& u = job.OBJ[num];
        const u32 line = job.SpriteLine;
        const u8* oam = job.OAM + num * 0x400;
        Period& s = e.Second;

        bool changed = memcmp(e.OAM, oam, 0x400) != 0;
        if (changed)
            memcpy(e.OAM, oam, 0x400);

        e.Cur.UploadPages += e.OBJPages;
        if (line != 0)
        {
            e.Cur.DirtyPages += e.OBJPages;
            e.Cur.RealPages += e.NumPendingOBJ;
            for (u32 i = 0; i < e.NumPendingOBJ; i++)
                LogPage(e, e.PendingOBJ[i]);
        }
        e.NumPendingOBJ = 0;

        if (line == 0)
        {
            s.UploadOBJ.Add(e.OBJPages);
        }
        else
        {
            if (e.OBJPages) MarkChange(e, Change_OBJVRAM, line);
            if (changed) MarkChange(e, Change_OAM, line);
        }

        u32 counts[Sprite_Count] {};
        bool mosaicLine = false, rotLine = false;
        if (u.Enabled && u.OBJEnable && !u.ForcedBlank)
        {
            static constexpr u8 widths[16] = {8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8};
            static constexpr u8 heights[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};
            bool mosaicOn = u.OBJMosaicSize[0] || u.OBJMosaicSize[1];
            const u16* attribs = (const u16*)oam;

            for (u32 i = 0; i < 128; i++)
            {
                u16 attr0 = attribs[i * 4];
                u16 attr1 = attribs[i * 4 + 1];

                // as SoftRenderer::DrawSprites decides what's drawn
                bool isAffine = attr0 & 0x0100;
                bool doubleSize = isAffine && (attr0 & 0x0200);
                if (!isAffine && (attr0 & 0x0200))
                    continue;
                counts[Sprite_Enabled]++;

                u32 size = (attr0 >> 14) | ((attr1 & 0xC000) >> 12);
                s32 width = widths[size] << (doubleSize ? 1 : 0);
                s32 height = heights[size] << (doubleSize ? 1 : 0);
                u32 y = attr0 & 0xFF;
                s32 x = (s32)((u32)attr1 << 23) >> 23;
                if (x <= -width || (y >= 192 && y + height <= 256))
                    continue;

                bool rot = false, affineID = false;
                if (isAffine)
                {
                    const u16* params = &attribs[(((attr1 >> 9) & 0x1F) * 16) + 3];
                    affineID = IsIdentity((s16)params[0], (s16)params[4], (s16)params[8], (s16)params[12]);
                    rot = !affineID;
                }
                u32 mode = (attr0 >> 10) & 0x3;
                bool mosaic = mosaicOn && (attr0 & 0x1000) && mode != 2;

                if (((line - y) & 0xFF) < (u32)height)
                {
                    mosaicLine = mosaicLine || mosaic;
                    rotLine = rotLine || rot;
                }

                counts[Sprite_Visible]++;
                if (rot) counts[Sprite_AffineRot]++;
                if (affineID) counts[Sprite_AffineID]++;
                if (mode == 3) counts[Sprite_Bitmap]++;
                if (mosaic) counts[Sprite_Mosaic]++;
                if (mode == 2) counts[Sprite_Window]++;
                if (mode == 1) counts[Sprite_Semi]++;
            }
        }

        if (mosaicLine)
            s.OBJMosaic++;
        if (rotLine)
            SetLine(e.Cur.RotOBJ, line);

        if (line == 0)
        {
            for (u32 i = 0; i < Sprite_Count; i++)
                s.Sprites[i].Add(counts[i]);
        }
    }

    static void FinishEngineFrame(Engine& e) noexcept
    {
        Period& s = e.Second;
        Frame& f = e.Cur;

        s.Frames++;
        for (u32 i = 0; i < Change_Count; i++)
            s.Changes[i].Add(f.Changes[i]);

        u32 bands = 0, bandsNoBackdrop = 0;
        for (u32 i = 0; i < 3; i++)
        {
            bands += __builtin_popcountll(f.Bands[i]);
            bandsNoBackdrop += __builtin_popcountll(f.BandsNoBackdrop[i]);
            s.RotBG += __builtin_popcountll(f.RotBG[i] & f.Mode1[i]);
            s.RotOBJ += __builtin_popcountll(f.RotOBJ[i] & f.Mode1[i]);
            s.Hires += __builtin_popcountll((f.RotBG[i] | f.RotOBJ[i]) & f.Mode1[i]);
        }
        s.Bands.Add(bands);
        s.BandsNoBackdrop.Add(bandsNoBackdrop);

        u32 bandsVisible = 0;
        bool pending = false, drawn = false;
        for (u32 line = 0; line < 192; line++)
        {
            pending = pending || ((f.Bands[line >> 6] >> (line & 63)) & 1);
            if (!((f.Visible[line >> 6] >> (line & 63)) & 1))
                continue;
            if (pending && drawn)
                bandsVisible++;
            pending = false;
            drawn = true;
        }
        s.BandsVisible.Add(bandsVisible);
        s.UploadTotal.Add(f.UploadPages);
        s.Dirty.Add(f.DirtyPages);
        s.Real.Add(f.RealPages);
        s.RealPal.Add(f.RealPalPages);
        s.Log.Add(f.LogPages);
        s.LogEvents.Add(f.RealPages + f.RealPalPages);

        f = {};
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

    static void LogPeriod(const Period& s, u32 num) noexcept
    {
        char text[1400];
        char* p = text;
        const char* end = text + sizeof(text);
        double frames = s.Frames;

        Append(p, end, "eng=%c frames=%u lines=%u active=%u disp=%u,%u,%u,%u", num ? 'B' : 'A', s.Frames, s.Lines, s.Active,
               s.Disp[0], s.Disp[1], s.Disp[2], s.Disp[3]);
        Append(p, end, " text=%u affrot=%u affid=%u ext=%u bmp8=%u bmp16=%u large=%u extrot=%u bmprot=%u largerot=%u",
               s.Text, s.AffRot, s.AffID, s.Ext, s.Bmp8, s.Bmp16, s.Large, s.ExtRot, s.BmpRot, s.LargeRot);
        Append(p, end, " rotbg=%u rotobj=%u hires=%u bgmos=%u objmos=%u win=%u blend=%u 3d=%u bright=%u cap=%u",
               s.RotBG, s.RotOBJ, s.Hires, s.BGMosaic, s.OBJMosaic, s.Window, s.Blend, s.BG3D, s.Bright, s.Capture);

        static const char* const spriteNames[Sprite_Count] = {"spr", "vis", "sprrot", "sprid", "sprbmp", "sprmos", "sprwin", "sprsemi"};
        for (u32 i = 0; i < Sprite_Count; i++)
            Append(p, end, " %s=%.1f,%u", spriteNames[i], s.Sprites[i].Sum / frames, s.Sprites[i].Max);

        static const char* const changeNames[Change_Count] = {"bgv", "objv", "extp", "pal", "bd", "oam", "regs", "scroll", "aff"};
        auto appendChange = [&](const char* name, const Series& series)
        {
            Append(p, end, " %s=%u,%.1f,%u", name, series.FramesWithAny, series.Sum / frames, series.Max);
        };
        for (u32 i = 0; i <= Change_LastBand; i++)
            appendChange(changeNames[i], s.Changes[i]);
        appendChange("bands", s.Bands);
        appendChange("bandsnb", s.BandsNoBackdrop);
        for (u32 i = Change_LastBand + 1; i < Change_Count; i++)
            appendChange(changeNames[i], s.Changes[i]);

        Append(p, end, " upbg=%.1f,%.1f upobj=%.1f,%.1f", s.UploadBG.Sum * 0.5 / frames, s.UploadBG.Max * 0.5,
               s.UploadOBJ.Sum * 0.5 / frames, s.UploadOBJ.Max * 0.5);
        appendChange("bandsvis", s.BandsVisible);
        Append(p, end, " uptotal=%.1f,%.1f", s.UploadTotal.Sum * 0.5 / frames, s.UploadTotal.Max * 0.5);
        appendChange("dirtymid", s.Dirty);
        appendChange("real", s.Real);
        appendChange("realpal", s.RealPal);
        appendChange("log", s.Log);
        Append(p, end, " logkb=%.1f,%.1f", s.Log.Sum * 0.5 / frames, s.Log.Max * 0.5);
        appendChange("logev", s.LogEvents);
        if (!num)
        {
            appendChange("logboth", s.Both);
            Append(p, end, " logbothkb=%.1f,%.1f", s.Both.Sum * 0.5 / frames, s.Both.Max * 0.5);
            appendChange("logbothev", s.BothEvents);
        }

        Platform::Log(Platform::LogLevel::Info, "GPU2DSTATS %s\n", text);
    }
};

}

#endif
