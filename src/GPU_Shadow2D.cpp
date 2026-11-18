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

#include "GPU_Shadow2D.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string.h>

#include "GPU.h"

namespace melonDS
{
using namespace Shadow2D;

namespace
{

constexpr s32 SpriteWidths[16] = {8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8};
constexpr s32 SpriteHeights[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};

bool LineBit(const u64* lines, u32 line) noexcept
{
    return (lines[line >> 6] >> (line & 63)) & 1;
}

void SetLineBit(u64* lines, u32 line) noexcept
{
    lines[line >> 6] |= 1ull << (line & 63);
}

// the lines 0-191 a sprite starting at y covers with height rows, as SoftRenderer::DrawSprites
// tests them: ((line - y) & 0xFF) < height
void MarkLines(u64* lines, u32 y, u32 height) noexcept
{
    for (u32 line = 0; line < 192; line++)
    {
        if (((line - y) & 0xFF) < height)
            SetLineBit(lines, line);
    }
}

// the time spent in a hook, for the GPU2D log only
struct HookTimer
{
    s64* Total;
    std::chrono::steady_clock::time_point Start;

    explicit HookTimer(s64* total) noexcept : Total(total)
    {
        if (Total)
            Start = std::chrono::steady_clock::now();
    }
    ~HookTimer()
    {
        if (Total)
            *Total += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count();
    }
};

// Shadow2DRecorder::Generation's source, for every recorder in the process. 0 is never handed out,
// as a compositor that has uploaded nothing holds it
std::atomic<u32> NextGeneration {1};

// the flats in the order GPU::MakeVRAMFlat_2DBGCoherent and 2DOBJCoherent pass their dirty sets
struct Region { u32 Page; u32 Unit; };
constexpr Region BGRegions[6] = {
    {Page_ABG, 0}, {Page_ABGExtPal, 0}, {Page_AOBJExtPal, 0},
    {Page_BBG, 1}, {Page_BBGExtPal, 1}, {Page_BOBJExtPal, 1},
};
constexpr Region OBJRegions[2] = {{Page_AOBJ, 0}, {Page_BOBJ, 1}};

}

const char* Shadow2D::ReasonName(u32 reason) noexcept
{
    static const char* const names[R_Count] = {
        "none", "screensoff", "disabled", "dispmode", "forcedblank", "vcount", "capture", "bgtype",
        "window", "bleftover", "mosaic", "rotsprite", "memchanged", "frame", "instcap", "hires", "held",
        "policy", "failed",
    };
    return reason < R_Count ? names[reason] : "?";
}

void Shadow2DRecorder::Reset() noexcept
{
    Frame++;
    if (Frame == 0)
        Frame = 1;
    Generation = NextGeneration.fetch_add(1, std::memory_order_relaxed);
    if (Generation == 0)
        Generation = NextGeneration.fetch_add(1, std::memory_order_relaxed);
    Broken = true;
    for (Engine& e : Engines)
    {
        e = {};
        e.RunStart = -1;
        e.PendingLine = ~0u;
    }
    OAMSynced = false;
    PaletteSynced = false;
    Sprites.clear();
    WindowSprites.clear();
    OldPages.clear();
}

const u8* Shadow2DRecorder::PageSource(const GPU& gpu, u32 page) const noexcept
{
    if (page < Page_BBG) return gpu.VRAMFlat_ABG + (page - Page_ABG) * 512;
    if (page < Page_AOBJ) return gpu.VRAMFlat_BBG + (page - Page_BBG) * 512;
    if (page < Page_BOBJ) return gpu.VRAMFlat_AOBJ + (page - Page_AOBJ) * 512;
    if (page < Page_ABGExtPal) return gpu.VRAMFlat_BOBJ + (page - Page_BOBJ) * 512;
    if (page < Page_BBGExtPal) return gpu.VRAMFlat_ABGExtPal + (page - Page_ABGExtPal) * 512;
    if (page < Page_AOBJExtPal) return gpu.VRAMFlat_BBGExtPal + (page - Page_BBGExtPal) * 512;
    if (page < Page_BOBJExtPal) return gpu.VRAMFlat_AOBJExtPal + (page - Page_AOBJExtPal) * 512;
    if (page < Page_Palette) return gpu.VRAMFlat_BOBJExtPal + (page - Page_BOBJExtPal) * 512;
    return Palette + (page - Page_Palette) * 512;
}

void Shadow2DRecorder::NoteDirty(const GPU& gpu, u32 line, bool obj, const u64* const* bits, const u32* words) noexcept
{
    HookTimer timer(Logging ? &RecorderNs : nullptr);

    OldPages.clear();
    OldLine = line;
    OldOBJ = obj;

    const Region* regions = obj ? OBJRegions : BGRegions;
    u32 count = obj ? 2 : 6;
    for (u32 r = 0; r < count; r++)
    {
        for (u32 w = 0; w < words[r]; w++)
        {
            for (u64 b = bits[r][w]; b; b &= b - 1)
            {
                u32 page = regions[r].Page + w * 64 + __builtin_ctzll(b);
                Dirty[page >> 6] |= 1ull << (page & 63);

                // mid-frame: keep what the page holds now, to see whether the copy changes it
                if (line > 0)
                {
                    OldPages.emplace_back();
                    OldPage& old = OldPages.back();
                    old.Page = page;
                    old.Unit = regions[r].Unit;
                    memcpy(old.Bytes, PageSource(gpu, page), 512);
                }
            }
        }
    }
}

void Shadow2DRecorder::NoteCopied(const GPU& gpu) noexcept
{
    if (OldPages.empty())
        return;
    HookTimer timer(Logging ? &RecorderNs : nullptr);

    for (const OldPage& old : OldPages)
    {
        if (memcmp(old.Bytes, PageSource(gpu, old.Page), 512) == 0)
            continue;
        Engine& e = Engines[old.Unit];
        u32& last = OldOBJ ? e.LastChangeOBJ : e.LastChangeBG;
        last = std::max(last, OldLine);
    }
    OldPages.clear();
}

void Shadow2DRecorder::NoteBGDirty(const GPU& gpu, u32 line, const u64* const* bits, const u32* words) noexcept
{
    NoteDirty(gpu, line, false, bits, words);
}

void Shadow2DRecorder::NoteBGCopied(const GPU& gpu) noexcept
{
    NoteCopied(gpu);
}

void Shadow2DRecorder::NoteOBJDirty(const GPU& gpu, u32 line, const u64* const* bits, const u32* words) noexcept
{
    NoteDirty(gpu, line, true, bits, words);
}

void Shadow2DRecorder::NoteOBJCopied(const GPU& gpu) noexcept
{
    NoteCopied(gpu);
}

void Shadow2DRecorder::NoteLine(GPU2D::LineJob& job, u32& paletteDirty, bool winEdge) noexcept
{
    HookTimer timer(Logging ? &RecorderNs : nullptr);
    const u32 line = job.Line;

    // the palette: a page whose colors really changed after line 0 sends the unit's earlier lines
    // to the CPU. the backdrop color doesn't count, as each line's block has its own
    for (u32 page = 0; page < 4; page++)
    {
        if (PaletteSynced && !(paletteDirty & (1u << page)))
            continue;

        const u8* src = job.Palette + page * 512;
        u8* copy = Palette + page * 512;
        u32 dirtyPage = Page_Palette + page;
        if (!PaletteSynced)
        {
            memcpy(copy, src, 512);
            Dirty[dirtyPage >> 6] |= 1ull << (dirtyPage & 63);
            continue;
        }

        u32 skip = (page & 1) ? 0 : 2;
        bool changed = memcmp(copy + skip, src + skip, 512 - skip) != 0;
        if (changed || memcmp(copy, src, skip) != 0)
        {
            memcpy(copy, src, 512);
            Dirty[dirtyPage >> 6] |= 1ull << (dirtyPage & 63);
        }
        if (changed && line > 0)
        {
            u32& last = Engines[page >> 1].LastChangeBG;
            last = std::max(last, line);
        }
    }
    PaletteSynced = true;
    paletteDirty = 0;

    if (line >= 192)
        return;

    for (u32 num = 0; num < 2; num++)
    {
        Engine& e = Engines[num];
        if (LineBit(e.BGNoted, line))
            Broken = true;
        SetLineBit(e.BGNoted, line);

        // only the accelerated line format has room for the block
        if (job.Accelerated)
            WriteBlock(job, num, winEdge);
    }
}

void Shadow2DRecorder::WriteBlock(const GPU2D::LineJob& job, u32 num, bool winEdge) noexcept
{
    const GPU2D::UnitState& u = job.BG[num];
    const Engine& e = Engines[num];
    const u32 line = job.Line;
    u32* block = &job.Framebuffer[num][GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords];

    // the BGs drawn as text BGs, affine BGs (DrawBG_Affine) and tiled extended BGs (DrawBG_Extended
    // with a 16-bit tilemap), and BG0 as the 3D layer, as DrawScanlineBGMode<N>, 6 and 7 pick them.
    // bitmap extended BGs and mode 6's large BG are other (increment 7)
    u32 mode = u.DispCnt & 0x7;
    bool bg0Is3D = !num && (u.DispCnt & 0x8);
    u32 text = 0, affine = 0, ext = 0, other = 0;
    bool has3D = false;
    for (u32 layer = 0; layer < 4; layer++)
    {
        if (!(u.LayerEnable & (1 << layer)))
            continue;

        if (layer == 0 && bg0Is3D && mode != 6)
            has3D = true;
        else if (mode == 6)
        {
            if (layer == 2) other |= 1 << layer;
            else if (layer == 0 && bg0Is3D) has3D = true;
        }
        else if (mode == 7)
        {
            if (layer <= 1) text |= 1 << layer;
        }
        else if (layer == 3)
        {
            if (mode >= 3) ((u.BGCnt[layer] & 0x0080) ? other : ext) |= 1 << layer;
            else if (mode >= 1) affine |= 1 << layer;
            else text |= 1 << layer;
        }
        else if (layer == 2)
        {
            if (mode == 5) ((u.BGCnt[layer] & 0x0080) ? other : ext) |= 1 << layer;
            else if (mode == 4 || mode == 2) affine |= 1 << layer;
            else text |= 1 << layer;
        }
        else
            text |= 1 << layer;
    }

    bool mosaicBG = false;
    for (u32 layer = 0; layer < 4; layer++)
    {
        if (((text | affine | ext) & (1 << layer)) && (u.BGCnt[layer] & 0x0040))
            mosaicBG = true;
    }

    // feature-eligible: the base rules, then the increments' (text BGs and normal sprites, increment
    // 1; affine and tiled extended BGs, increment 2, from the line's latched reference points in the
    // block, so a game rewriting them mid-frame is drawn as the CPU draws it). BLDCNT blending
    // (increment 5) is all done by the composite, but for one case: without
    // EngineBNoHold, unit B's pass starts with the second plane unit A's pass of the line left
    // (SoftRenderer only clears it with EngineBNoHold), so where B draws nothing over the backdrop,
    // an alpha blend with the backdrop as 1st target blends with that leftover, which the shadow
    // doesn't have. those lines stay on the CPU
    bool blendLeftover = num && !job.EngineBNoHold && ((u.BlendCnt >> 6) & 0x3) == 1
        && (u.BlendCnt & 0x0020) && (u.BlendCnt & 0x3F00);
    // windows (increment 4) are worked out by the planes pass from the block, but for two cases.
    // CalculateWindowMask tests the whole Win0Active/Win1Active word against 3, and the block keeps
    // 16 bits of each, which only bits 0-1 should ever use: a savestate can load anything there.
    // and an affine OBJ window sprite needs increment 3, so with the OBJ window on its lines stay
    // on the CPU as an affine sprite's do
    bool windows = (u.DispCnt & 0xE000) != 0;
    bool winState = ((u.DispCnt & 0x2000) && (u.Win0Active & ~0x3u)) || ((u.DispCnt & 0x4000) && (u.Win1Active & ~0x3u));
    bool winRot = (u.DispCnt & 0x8000) && LineBit(e.SpriteWinRot, line);
    u32 dispMode = (u.DispCnt >> 16) & (num ? 0x1 : 0x3);
    u32 reason = R_None;
    if (!job.ScreensEnabled) reason = R_ScreensOff;
    else if (!u.Enabled) reason = R_Disabled;
    else if (dispMode != 1) reason = R_DispMode;
    else if (u.ForcedBlank) reason = R_ForcedBlank;
    else if (job.VCount != job.Line) reason = R_VCount;
    else if (!num && u.CaptureLatch) reason = R_Capture;
    else if (other) reason = R_BGType;
    else if (winState) reason = R_Window;
    else if (blendLeftover) reason = R_Blend;
    else if (mosaicBG || LineBit(e.SpriteMosaic, line)) reason = R_Mosaic;
    else if (LineBit(e.SpriteRot, line) || winRot) reason = R_RotSprite;

    // the sprite pass of this line, snapshotted one HBlank earlier
    bool spritesOn = (e.PendingLine == line) && (e.PendingOBJState & 0x3) == 0x3;

    u32 wideModes = 0, wideLeft = 0, wideRight = 0;
    if (Wide == 1)
    {
        wideModes = 0x155;      // Wide_Draw for BG0-3 and OBJ
        wideLeft = 0xFFFFFFFF;
        wideRight = 0xFFFFFFFF;
    }
    bool wide = wideModes && ((text | affine | ext) || (u.LayerEnable & 0x10));

    u16 backdrop = *(const u16*)&job.Palette[num ? 0x400 : 0];

    block[W_Flags] = (num ? F_UnitB : 0) | (u.Enabled ? F_Enabled : 0) | (u.ForcedBlank ? F_ForcedBlank : 0)
        | ((u32)(u.LayerEnable & 0x1F) << F_LayerShift) | (spritesOn ? F_OBJEnable : 0)
        | (job.VCount == job.Line ? F_VCountLine : 0) | (has3D ? F_BG0Is3D : 0) | (text << F_TextShift)
        | (wide ? F_Wide : 0) | (windows && winEdge ? F_WinEdge : 0) | (reason << F_ReasonShift)
        | ((affine >> 2) << F_AffineShift) | ((ext >> 2) << F_ExtShift) | (reason == R_None ? F_Eligible : 0);
    block[W_Frame] = Frame;
    block[W_DispCnt] = u.DispCnt;
    block[W_BGCnt01] = u.BGCnt[0] | ((u32)u.BGCnt[1] << 16);
    block[W_BGCnt23] = u.BGCnt[2] | ((u32)u.BGCnt[3] << 16);
    for (u32 layer = 0; layer < 4; layer++)
        block[W_BGPos0 + layer] = u.BGXPos[layer] | ((u32)u.BGYPos[layer] << 16);
    block[W_RefX2] = (u32)u.BGXRefInternal[0];
    block[W_RefY2] = (u32)u.BGYRefInternal[0];
    block[W_RefX3] = (u32)u.BGXRefInternal[1];
    block[W_RefY3] = (u32)u.BGYRefInternal[1];
    block[W_RotAB2] = (u16)u.BGRotA[0] | ((u32)(u16)u.BGRotB[0] << 16);
    block[W_RotCD2] = (u16)u.BGRotC[0] | ((u32)(u16)u.BGRotD[0] << 16);
    block[W_RotAB3] = (u16)u.BGRotA[1] | ((u32)(u16)u.BGRotB[1] << 16);
    block[W_RotCD3] = (u16)u.BGRotC[1] | ((u32)(u16)u.BGRotD[1] << 16);
    memcpy(&block[W_Win0], u.Win0Coords, 4);
    memcpy(&block[W_Win1], u.Win1Coords, 4);
    memcpy(&block[W_WinCnt], u.WinCnt, 4);
    block[W_WinActive] = (u.Win0Active & 0xFFFF) | ((u.Win1Active & 0xFFFF) << 16);
    block[W_Mosaic] = u.BGMosaicSize[0] | ((u32)u.BGMosaicSize[1] << 8) | ((u32)u.OBJMosaicSize[0] << 16) | ((u32)u.OBJMosaicSize[1] << 24);
    block[W_MosaicLines] = (u.BGMosaicLine & 0xFFFF) | ((u32)u.OBJMosaicY << 16);
    block[W_Blend] = u.BlendCnt | ((u32)u.EVA << 16) | ((u32)u.EVB << 24);
    block[W_EVYBackdrop] = u.EVY | ((u32)backdrop << 16);
    block[W_Lines] = (job.VCount & 0xFFFF) | (line << 16);
    block[W_Wide] = wideModes;
    block[W_WideLeft] = wideLeft;
    block[W_WideRight] = wideRight;
    block[W_OBJDispCnt] = e.PendingLine == line ? e.PendingOBJDispCnt : 0;
    block[W_OBJState] = e.PendingLine == line ? e.PendingOBJState : 0;
    block[W_Version] = 0;

    if (SelfCheck)
        CheckBlock(block, u, job);
}

// the swi-harness: every field read back the way the shader and the compositor read it
void Shadow2DRecorder::CheckBlock(const u32* block, const GPU2D::UnitState& u, const GPU2D::LineJob& job) noexcept
{
    u32 errors = 0;
    u32 flags = block[W_Flags];
    errors += ((flags & F_UnitB) != 0) != (u.Num != 0);
    errors += ((flags & F_Enabled) != 0) != u.Enabled;
    errors += ((flags & F_ForcedBlank) != 0) != (u.ForcedBlank != 0);
    errors += ((flags >> F_LayerShift) & 0x1F) != (u32)(u.LayerEnable & 0x1F);
    errors += ((flags & F_Eligible) != 0) != (((flags >> F_ReasonShift) & 0x1F) == R_None);
    // a BG is drawn as one kind at most, and only if it's enabled
    u32 text = (flags >> F_TextShift) & 0xF;
    u32 affine = (flags >> F_AffineShift) & 0x3, ext = (flags >> F_ExtShift) & 0x3;
    errors += (affine & ext) != 0 || (text & ((affine | ext) << 2)) != 0;
    errors += ((text | ((affine | ext) << 2)) & ~(u32)u.LayerEnable) != 0;
    errors += block[W_Frame] != Frame;
    errors += block[W_DispCnt] != u.DispCnt;
    for (u32 layer = 0; layer < 4; layer++)
    {
        u32 cnt = (block[W_BGCnt01 + (layer >> 1)] >> ((layer & 1) * 16)) & 0xFFFF;
        u32 pos = block[W_BGPos0 + layer];
        errors += cnt != u.BGCnt[layer];
        errors += (pos & 0xFFFF) != u.BGXPos[layer];
        errors += ((pos >> 16) & 0xFFFF) != u.BGYPos[layer];
    }
    for (u32 i = 0; i < 2; i++)
    {
        errors += (s32)block[W_RefX2 + i * 2] != u.BGXRefInternal[i];
        errors += (s32)block[W_RefY2 + i * 2] != u.BGYRefInternal[i];
        // sign-extended from 16 bits, as the shader will (& 0xFFFF, then the sign bit)
        auto low = [](u32 w) { return (s32)(s16)(w & 0xFFFF); };
        auto high = [](u32 w) { return (s32)(s16)((w >> 16) & 0xFFFF); };
        errors += low(block[W_RotAB2 + i * 2]) != u.BGRotA[i];
        errors += high(block[W_RotAB2 + i * 2]) != u.BGRotB[i];
        errors += low(block[W_RotCD2 + i * 2]) != u.BGRotC[i];
        errors += high(block[W_RotCD2 + i * 2]) != u.BGRotD[i];
    }
    for (u32 i = 0; i < 4; i++)
    {
        errors += ((block[W_Win0] >> (i * 8)) & 0xFF) != u.Win0Coords[i];
        errors += ((block[W_Win1] >> (i * 8)) & 0xFF) != u.Win1Coords[i];
        errors += ((block[W_WinCnt] >> (i * 8)) & 0xFF) != u.WinCnt[i];
    }
    errors += (block[W_WinActive] & 0xFFFF) != (u.Win0Active & 0xFFFF);
    errors += ((block[W_WinActive] >> 16) & 0xFFFF) != (u.Win1Active & 0xFFFF);
    errors += (block[W_Mosaic] & 0xFF) != u.BGMosaicSize[0];
    errors += ((block[W_Mosaic] >> 8) & 0xFF) != u.BGMosaicSize[1];
    errors += ((block[W_Mosaic] >> 16) & 0xFF) != u.OBJMosaicSize[0];
    errors += ((block[W_Mosaic] >> 24) & 0xFF) != u.OBJMosaicSize[1];
    errors += (block[W_MosaicLines] & 0xFFFF) != (u.BGMosaicLine & 0xFFFF);
    errors += ((block[W_MosaicLines] >> 16) & 0xFF) != u.OBJMosaicY;
    errors += (block[W_Blend] & 0xFFFF) != u.BlendCnt;
    errors += ((block[W_Blend] >> 16) & 0xFF) != u.EVA;
    errors += ((block[W_Blend] >> 24) & 0xFF) != u.EVB;
    errors += (block[W_EVYBackdrop] & 0xFF) != u.EVY;
    errors += ((block[W_EVYBackdrop] >> 16) & 0xFFFF) != *(const u16*)&job.Palette[u.Num ? 0x400 : 0];
    errors += (block[W_Lines] & 0xFFFF) != job.VCount;
    errors += ((block[W_Lines] >> 16) & 0xFFFF) != job.Line;
    SelfCheckErrors += errors;
}

void Shadow2DRecorder::NoteSprites(const GPU2D::LineJob& job, u32& oamDirty) noexcept
{
    HookTimer timer(Logging ? &RecorderNs : nullptr);
    const u32 line = job.SpriteLine;

    if (line == 0)
    {
        // a new frame. the last one's Blit is done by now (it runs at line 192 or 215)
        Frame++;
        if (Frame == 0)
            Frame = 1;
        Broken = false;
        Sprites.clear();
        WindowSprites.clear();
        for (Engine& e : Engines)
        {
            memset(e.BGNoted, 0, sizeof(e.BGNoted));
            memset(e.SpriteNoted, 0, sizeof(e.SpriteNoted));
            memset(e.SpriteRot, 0, sizeof(e.SpriteRot));
            memset(e.SpriteMosaic, 0, sizeof(e.SpriteMosaic));
            memset(e.SpriteWinRot, 0, sizeof(e.SpriteWinRot));
            memset(e.Overflow, 0, sizeof(e.Overflow));
            e.LastChangeBG = 0;
            e.LastChangeOBJ = 0;
            e.RunStart = -1;
        }
    }
    if (line >= 192)
        return;

    for (u32 num = 0; num < 2; num++)
    {
        Engine& e = Engines[num];
        const GPU2D::UnitState& u = job.OBJ[num];
        const u8* oam = job.OAM + num * 0x400;

        bool changed = !OAMSynced;
        if (OAMSynced && (oamDirty & (1u << num)))
            changed = memcmp(e.OAM, oam, 0x400) != 0;

        // what SoftRenderer::DrawSprites reads besides OAM: the unit and sprites on, the tile and
        // bitmap mappings and boundaries, and the extended palette bit
        u32 key = (u.Enabled && u.OBJEnable) ? ((u.DispCnt & 0x80700070) | 1) : 0;
        if (e.RunStart >= 0 && (changed || key != e.RunKey))
            FlushRun(num, line);
        if (changed)
            memcpy(e.OAM, oam, 0x400);
        if (e.RunStart < 0)
        {
            e.RunStart = line;
            e.RunKey = key;
            e.RunDispCnt = u.DispCnt;
            ScanRun(e);
        }

        if (LineBit(e.SpriteNoted, line))
            Broken = true;
        SetLineBit(e.SpriteNoted, line);
        if (LineBit(e.RunRot, line))
            SetLineBit(e.SpriteRot, line);
        if (LineBit(e.RunMosaic, line))
            SetLineBit(e.SpriteMosaic, line);
        if (LineBit(e.RunWinRot, line))
            SetLineBit(e.SpriteWinRot, line);

        e.PendingLine = line;
        e.PendingOBJDispCnt = u.DispCnt;
        e.PendingOBJState = (u.Enabled ? 1 : 0) | (u.OBJEnable ? 2 : 0)
            | ((u32)u.OBJMosaicSize[0] << 8) | ((u32)u.OBJMosaicSize[1] << 16);
    }
    OAMSynced = true;
    oamDirty &= ~0x3u;
}

// the lines the run's affine sprites and mosaic sprites cover, which stay on the CPU in increment 1,
// and those its affine OBJ window sprites cover, which do where the OBJ window is on. OBJ window
// sprites ignore the mosaic bit (SoftRenderer::DrawSprites)
void Shadow2DRecorder::ScanRun(Engine& e) noexcept
{
    memset(e.RunRot, 0, sizeof(e.RunRot));
    memset(e.RunMosaic, 0, sizeof(e.RunMosaic));
    memset(e.RunWinRot, 0, sizeof(e.RunWinRot));
    if (!e.RunKey)
        return;

    const u16* oam = (const u16*)e.OAM;
    for (u32 i = 0; i < 128; i++)
    {
        u16 attr0 = oam[i * 4];
        u16 attr1 = oam[i * 4 + 1];
        bool window = ((attr0 >> 10) & 0x3) == 2;
        if (window && !(attr0 & 0x0100))
            continue;

        u32 size = (attr0 >> 14) | ((attr1 & 0xC000) >> 12);
        s32 width = SpriteWidths[size];
        s32 height = SpriteHeights[size];
        s32 xpos = (s32)((u32)attr1 << 23) >> 23;
        u32 y = attr0 & 0xFF;

        if (attr0 & 0x0100)
        {
            if (attr0 & 0x0200)
            {
                width <<= 1;
                height <<= 1;
            }
            if (xpos <= -width)
                continue;
            MarkLines(window ? e.RunWinRot : e.RunRot, y, height);
        }
        else if (!(attr0 & 0x0200) && (attr0 & 0x1000))
        {
            if (xpos <= -width)
                continue;
            MarkLines(e.RunMosaic, y, height);
        }
    }
}

// the run's normal sprites, and its OBJ window sprites, become instances, one per sprite and stretch
// of lines it covers
void Shadow2DRecorder::FlushRun(u32 num, u32 end) noexcept
{
    Engine& e = Engines[num];
    u32 start = (u32)e.RunStart;
    e.RunStart = -1;
    if (!e.RunKey || end <= start)
        return;

    const u16* oam = (const u16*)e.OAM;
    for (u32 i = 0; i < 128; i++)
    {
        u16 attr0 = oam[i * 4];
        u16 attr1 = oam[i * 4 + 1];
        u16 attr2 = oam[i * 4 + 2];

        // affine sprites keep their lines on the CPU (ScanRun), and 0x200 alone is a hidden sprite
        if (attr0 & 0x0300)
            continue;
        u32 mode = (attr0 >> 10) & 0x3;
        // an OBJ window sprite is drawn as a tiled one whatever its mode, and without mosaic, into
        // the OBJ window (SoftRenderer::DrawSprites and DrawSprite_Normal<true>). it only matters
        // where the BG pass turns the OBJ window on, which isn't known yet, so it's always drawn
        bool window = mode == 2;
        if ((attr0 & 0x1000) && !window)
            continue;
        // bitmap sprites with alpha 0, or the reserved bitmap mapping, draw nothing
        if (mode == 3 && (!(attr2 >> 12) || (e.RunDispCnt & 0x60) == 0x60))
            continue;

        u32 size = (attr0 >> 14) | ((attr1 & 0xC000) >> 12);
        s32 width = SpriteWidths[size];
        s32 height = SpriteHeights[size];
        s32 xpos = (s32)((u32)attr1 << 23) >> 23;
        if (xpos <= -width)
            continue;

        // the lines with ((line - y) & 0xFF) < height: [y, y + height) and what wraps past 255
        u32 y = attr0 & 0xFF;
        u32 pieces[2][2] = {{y, std::min<u32>(y + height, 256)}, {0, y + height > 256 ? y + height - 256 : 0}};
        for (auto& piece : pieces)
        {
            u32 first = std::max(piece[0], start);
            u32 last = std::min(piece[1], end);
            if (first >= last)
                continue;

            if (Sprites.size() + WindowSprites.size() >= MaxInstances)
            {
                for (u32 l = start; l < end; l++)
                    SetLineBit(e.Overflow, l);
                return;
            }

            Instance inst;
            inst.X = xpos;
            inst.Row = (window ? ObjWindowRow : 0) + num * 192 + first;
            inst.Width = width;
            inst.Lines = last - first;
            inst.FirstRow = (first - y) & 0xFF;
            inst.Attr0 = attr0;
            inst.Attr1 = attr1;
            inst.Attr2 = attr2;
            inst.Index = i;
            inst.DispCnt = e.RunDispCnt;
            inst.Flags = num | (window ? 2 : 0);
            inst.Height = height;
            (window ? WindowSprites : Sprites).push_back(inst);
        }
    }
}

void Shadow2DRecorder::Finalize() noexcept
{
    for (u32 num = 0; num < 2; num++)
    {
        if (Engines[num].RunStart >= 0)
            FlushRun(num, 192);
    }
}

u32 Shadow2DRecorder::LineReason(const u32* block) const noexcept
{
    if (block[W_Frame] != Frame || Broken)
        return R_Frame;

    u32 flags = block[W_Flags];
    u32 reason = (flags >> F_ReasonShift) & 0x1F;
    if (reason != R_None)
        return reason;

    u32 line = (block[W_Lines] >> 16) & 0xFFFF;
    if (line >= 192)
        return R_Frame;
    const Engine& e = Engines[flags & F_UnitB];
    if (!LineBit(e.BGNoted, line) || !LineBit(e.SpriteNoted, line))
        return R_Frame;
    if (line < e.LastChangeBG || line < e.LastChangeOBJ)
        return R_MemChanged;
    if (LineBit(e.Overflow, line))
        return R_InstanceCap;
    return R_None;
}

void Shadow2DRecorder::CountFrame(const GPU& gpu, int backbuf) noexcept
{
    CountedInstances += Sprites.size() + WindowSprites.size();
    if (FrameHook)
        FrameHook(gpu, backbuf, FrameHookUser);
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        if (!fb)
            return;
        for (int line = 0; line < 192; line++)
            ReasonCounts[LineReason(&fb[GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords])]++;
    }
}

}
