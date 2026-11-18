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

#ifndef GPU_SHADOW2D_H
#define GPU_SHADOW2D_H

#include <vector>

#include "types.h"
#include "GPU2D.h"

namespace melonDS
{
class GPU;

// GPU 2D step 3, the shadow renderer (docs/plan-gpu-2d-shadow.md). CPU 2D stays the real result: the
// GL compositor redraws the lines it can prove it matches, at output resolution, for display only.
// this is its emulator-thread half. it runs while each LineJob is built and only records:
// - each line's state block (GPU2D::AccelShadowStateStart, the words in Shadow2D below), written into
//   the line's framebuffer, so it follows POWCNT's screen swap, threaded 2D and the Blit after the
//   readback like the wing planes do;
// - a copy of the palette and OAM as of the frame's last line, since games write them in VBlank before
//   a Blit at line 215 (driven by GPU::PaletteDirty and OAMDirty, which nothing else reads);
// - the frame's sprite instances, one per sprite and run of lines with the same OAM and OBJ state;
// - which pages of the flat VRAM copies changed since the compositor last uploaded them, and the
//   last line of the frame where a page's bytes really changed. lines before that are left to the CPU
//   (until the page log of increment 1b), since the compositor only has the frame's last version.
// nothing here changes emulation, the 2D output or savestates. GPU::Shadow2D is null while it's off,
// so then none of this runs. the GL half is GLCompositor::PrepareShadow2D and RunShadow2D
namespace Shadow2D
{
// the words of a line's state block
enum : u32
{
    W_Flags = 0,    // F_* below
    W_Frame,        // Shadow2DRecorder::Frame when the line was noted
    W_DispCnt,
    W_BGCnt01,      // BGCnt[0] | BGCnt[1] << 16
    W_BGCnt23,
    W_BGPos0,       // BGXPos[n] | BGYPos[n] << 16
    W_BGPos1,
    W_BGPos2,
    W_BGPos3,
    W_RefX2,        // BGXRefInternal[0] (BG2)
    W_RefY2,
    W_RefX3,
    W_RefY3,
    W_RotAB2,       // BGRotA[0] | BGRotB[0] << 16
    W_RotCD2,
    W_RotAB3,
    W_RotCD3,
    W_Win0,         // Win0Coords[0-3], a byte each
    W_Win1,
    W_WinCnt,       // WinCnt[0-3]
    W_WinActive,    // Win0Active | Win1Active << 16
    W_Mosaic,       // BGMosaicSize[0-1], OBJMosaicSize[0-1]
    W_MosaicLines,  // BGMosaicLine | OBJMosaicY << 16
    W_Blend,        // BlendCnt | EVA << 16 | EVB << 24
    W_EVYBackdrop,  // EVY | the backdrop color as of the line (palette entry 0 of the unit) << 16
    W_Lines,        // VCount | Line << 16
    W_Wide,         // 2 bits per layer: BG0-3 in bits 0-7, OBJ in bits 8-9 (Wide_* below)
    W_WideLeft,     // per BG, a byte each: how many columns it may draw past the left edge
    W_WideRight,    // and the right one
    W_OBJDispCnt,   // the sprite pass's DispCnt
    W_OBJState,     // the sprite pass's Enabled, OBJEnable (bits 0-1), OBJMosaicSize (8-15, 16-23)
    W_Version,      // band and page log version indices, for increment 1b. 0 for now
    W_Count
};
static_assert(W_Count == GPU2D::ShadowStateWords, "the state block's words");

enum : u32
{
    F_UnitB       = 1u << 0,
    F_Enabled     = 1u << 1,
    F_ForcedBlank = 1u << 2,
    F_LayerShift  = 3,          // LayerEnable (BG0-3, OBJ), 5 bits
    F_OBJEnable   = 1u << 8,    // the sprite pass's OBJEnable and Enabled
    F_VCountLine  = 1u << 9,    // VCount == Line
    F_PageLog     = 1u << 10,   // reads the page log (increment 1b). never set yet
    F_BG0Is3D     = 1u << 11,   // unit A shows the 3D layer on BG0 (enabled, DISPCNT bit 3)
    F_TextShift   = 12,         // the BGs drawn as text BGs on this line, 4 bits
    F_Wide        = 1u << 16,   // some layer is allowed past the edge (W_Wide)
    F_WinEdge     = 1u << 17,   // the window mask's edge columns take their neighbour's (WideView or
                                // WideCheck, see SoftRenderer::DrawScanline_BGOBJ)
    F_ReasonShift = 20,         // why the line isn't feature-eligible, 5 bits (R_*), 0 if it is
    F_Eligible    = 1u << 30,
    F_Drawn       = 1u << 31,   // only in the compositor's copy: the shadow draws this line
};

enum : u32
{
    Wide_Off = 0,       // transparent past the edge, the default
    Wide_Draw = 1,      // drawn past the edge as the layer goes on (gpu2dwide 1)
    Wide_Repeat = 2,    // edge repeat, reserved for WideMelon step 5a
};

// why a line stays on the CPU. the feature reasons come from the recorder at the line, the others
// from the frame (LineReason) or the compositor
enum : u32
{
    R_None = 0,
    R_ScreensOff,   // POWCNT1 bit 0
    R_Disabled,     // the unit is off (POWCNT1)
    R_DispMode,     // display mode isn't 1
    R_ForcedBlank,
    R_VCount,       // a VCount write: the line isn't the line it's drawn as
    R_Capture,      // unit A, display capture armed
    R_BGType,       // an enabled BG that isn't a text BG
    R_Window,       // windows on with window X/Y state the block's 16-bit fields don't hold (a savestate's)
    R_Blend,        // unit B, no EngineBNoHold: a backdrop alpha blend that would read engine A's leftover plane
    R_Mosaic,       // BG or sprite mosaic
    R_RotSprite,    // an affine sprite on the line, or an affine OBJ window sprite with the OBJ window on
    R_FeatureCount,
    R_MemChanged = R_FeatureCount, // before the frame's last real mid-frame change of its unit's memory
    R_Frame,        // not this frame's block, or the frame's lines were noted out of order
    R_InstanceCap,  // the frame ran out of sprite instances
    R_Hires,        // shows a hi-res capture image
    R_Held,         // a held WideMelon wing line
    R_Policy,       // mode 1: the line gains nothing at this scale and view
    R_Failed,       // the GL side couldn't be set up
    R_Count
};
const char* ReasonName(u32 reason) noexcept;

// a sprite instance as the compositor uploads it (three uvec4 attributes): one sprite over one run of
// lines. Geom is signed: X, ObjTex row of the first line (unit * 192 + line, plus 384 for an OBJ window
// sprite, whose rows are below the sprites'), width, lines
struct Instance
{
    s32 X;
    u32 Row, Width, Lines;
    u32 FirstRow, Attr0, Attr1, Attr2;  // the sprite's row on the first line (before Y flip)
    u32 Index, DispCnt, Flags, Height;  // Flags: unit B (bit 0), OBJ window (bit 1)
};
static_assert(sizeof(Instance) == 48, "three uvec4");

// ObjTex's rows: the OBJ line pixels of both units' 192 lines, then their OBJ window
constexpr u32 ObjWindowRow = 384;
constexpr u32 ObjRows = 768;

// where the flats, extended palettes and palette are in MemTex (R32UI, 256 texels = two 512-byte
// pages a row), in pages
enum : u32
{
    Page_ABG = 0,
    Page_BBG = Page_ABG + 1024,
    Page_AOBJ = Page_BBG + 256,
    Page_BOBJ = Page_AOBJ + 512,
    Page_ABGExtPal = Page_BOBJ + 256,
    Page_BBGExtPal = Page_ABGExtPal + 64,
    Page_AOBJExtPal = Page_BBGExtPal + 64,
    Page_BOBJExtPal = Page_AOBJExtPal + 16,
    Page_Palette = Page_BOBJExtPal + 16,
    Page_Count = Page_Palette + 4,
    MemRows = Page_Count / 2,
};
constexpr u32 MaxInstances = 2048;
}

struct Shadow2DRecorder
{
    // set by GPU::SetShadow2D. Mode 1 draws only the lines that gain something, 2 every eligible line
    int Mode = 1;
    bool Logging = false;       // the GPU2D log line (log, check)
    bool Check = false;         // the 1x readback every 30 frames (check)
    int Wide = 0;               // debug.melonade.gpu2dwide: 1 = every layer draws past the edge
    bool SelfCheck = false;     // the swi-harness: decode every block written and compare it

    // the frame stamp: a new frame starts with the sprite snapshot of line 0 (VCount 262 of the frame
    // before), which comes after that frame's Blit. never 0, which a zeroed block has
    u32 Frame = 1;
    // bumped when the flats or palette can't be trusted to match what the compositor uploaded (reset,
    // savestate load, new framebuffers): the compositor uploads everything again. it comes from a
    // process-wide counter, so no recorder, old or new, ever repeats a value a compositor has seen
    u32 Generation = 0;
    // this frame's lines weren't all noted once and in order (a reset or load mid-frame, a VCount
    // write repeating lines): none is drawn
    bool Broken = true;

    struct Engine
    {
        u64 BGNoted[3], SpriteNoted[3];     // the lines noted this frame, as BG and sprite lines
        u64 SpriteRot[3], SpriteMosaic[3];  // sprite lines with an affine or a mosaic sprite
        u64 SpriteWinRot[3];                // and with an affine OBJ window sprite
        u64 Overflow[3];                    // sprite lines whose instances didn't fit
        // the last line > 0 of this frame where the unit's memory really changed: BG VRAM, the
        // extended palettes and the palette but the backdrop (BG lines), OBJ VRAM (sprite lines)
        u32 LastChangeBG, LastChangeOBJ;

        // the sprite run being built: lines [RunStart, the next change) with this OAM and RunKey
        int RunStart;
        u32 RunKey;                         // the OBJ state that matters, 0 while no sprite is drawn
        u32 RunDispCnt;
        u64 RunRot[3], RunMosaic[3];        // the lines the run's affine and mosaic sprites cover
        u64 RunWinRot[3];                   // and its affine OBJ window sprites
        alignas(u64) u8 OAM[0x400];

        // the last sprite snapshot, for the next BG line's block (sprite line L is snapshotted at
        // the HBlank before BG line L)
        u32 PendingLine;
        u32 PendingOBJDispCnt, PendingOBJState;
    };
    Engine Engines[2] {};
    bool OAMSynced = false;
    alignas(u64) u8 Palette[0x800] {};      // as of the frame's last BG line
    bool PaletteSynced = false;

    // MemTex pages changed since the compositor's last upload, Shadow2D::Page_* order
    u64 Dirty[(Shadow2D::Page_Count + 63) / 64] {};

    // the frame's normal sprites, and its OBJ window sprites, which the OBJ pass draws into ObjTex's
    // rows below the sprites' (Shadow2D::ObjWindowRow). both count towards MaxInstances
    std::vector<Shadow2D::Instance> Sprites;
    std::vector<Shadow2D::Instance> WindowSprites;

    // for the GPU2D log, emulator thread time in the hooks (only measured while Logging)
    s64 RecorderNs = 0;

    // the swi-harness's self-check: block fields that didn't decode back, and lines seen per reason
    u32 SelfCheckErrors = 0;
    u32 ReasonCounts[Shadow2D::R_Count] {};
    u64 CountedInstances = 0;
    // and its replay of the shaders, called from CountFrame
    void (*FrameHook)(const GPU& gpu, int backbuf, void* user) = nullptr;
    void* FrameHookUser = nullptr;

    Shadow2DRecorder() noexcept { Reset(); }

    // GPU::Reset, a savestate load and new framebuffers
    void Reset() noexcept;

    // GPU::MakeVRAMFlat_2DBGCoherent, with the dirty sets DeriveState just worked out, before the flat
    // copies (Dirty), and right after them (which pages really changed). line is the BG line
    void NoteBGDirty(const GPU& gpu, u32 line, const u64* const* bits, const u32* words) noexcept;
    void NoteBGCopied(const GPU& gpu) noexcept;
    // GPU::MakeVRAMFlat_2DOBJCoherent, the same for the sprite line
    void NoteOBJDirty(const GPU& gpu, u32 line, const u64* const* bits, const u32* words) noexcept;
    void NoteOBJCopied(const GPU& gpu) noexcept;

    // GPU::Snapshot2DScanline: the palette, then each unit's block. consumes PaletteDirty. winEdge:
    // SoftRenderer gives the window mask's edge columns their neighbour's (job.WideView or WideCheck)
    void NoteLine(GPU2D::LineJob& job, u32& paletteDirty, bool winEdge) noexcept;
    // GPU::Snapshot2DSprites: OAM and the OBJ state, then the sprite runs. consumes OAMDirty
    void NoteSprites(const GPU2D::LineJob& job, u32& oamDirty) noexcept;
    // closes the frame's sprite runs (GPU::StartScanline at line 192 and the compositor, either first)
    void Finalize() noexcept;

    // whether the line whose block this is can be drawn as far as the recorder knows: R_None, the
    // line's feature reason, or one of the frame's
    u32 LineReason(const u32* block) const noexcept;

    // the source of a MemTex page (the flats, or the palette copy)
    const u8* PageSource(const GPU& gpu, u32 page) const noexcept;

    // the MemTex uploads that bring a copy up to date: every region (full), or the Dirty pages, as
    // runs of whole rows (both pages dirty) within a region and half rows for a lone page.
    // upload(x, row, width, rows, source) is in texels (words) and rows. the compositor and the
    // swi-harness's replay both upload through this, then clear Dirty
    template <typename F>
    void ForEachUpload(const GPU& gpu, bool full, F upload) const noexcept
    {
        using namespace Shadow2D;
        static constexpr u32 regionStarts[] = {Page_ABG, Page_BBG, Page_AOBJ, Page_BOBJ, Page_ABGExtPal, Page_BBGExtPal,
                                               Page_AOBJExtPal, Page_BOBJExtPal, Page_Palette, Page_Count};
        if (full)
        {
            // each region is one array, so one upload each
            for (u32 r = 0; r + 1 < sizeof(regionStarts) / sizeof(regionStarts[0]); r++)
                upload(0u, regionStarts[r] / 2, 256u, (regionStarts[r + 1] - regionStarts[r]) / 2, PageSource(gpu, regionStarts[r]));
            return;
        }

        auto dirty = [&](u32 page) { return (Dirty[page >> 6] >> (page & 63)) & 1; };
        u32 region = 0;
        for (u32 row = 0; row < MemRows; )
        {
            while (regionStarts[region + 1] <= row * 2)
                region++;
            bool lo = dirty(row * 2), hi = dirty(row * 2 + 1);
            if (lo && hi)
            {
                u32 end = row + 1;
                while (end * 2 < regionStarts[region + 1] && dirty(end * 2) && dirty(end * 2 + 1))
                    end++;
                upload(0u, row, 256u, end - row, PageSource(gpu, row * 2));
                row = end;
                continue;
            }
            if (lo || hi)
                upload(lo ? 0u : 128u, row, 128u, 1u, PageSource(gpu, row * 2 + (lo ? 0 : 1)));
            row++;
        }
    }

    // the swi-harness: counts every line of the frame just finished by LineReason, from the buffer
    // the frame was drawn into (call it at line 192, before the Blit)
    void CountFrame(const GPU& gpu, int backbuf) noexcept;

private:
    struct OldPage
    {
        u32 Page;
        u32 Unit;
        alignas(u64) u8 Bytes[512];
    };
    std::vector<OldPage> OldPages;
    u32 OldLine = 0;
    bool OldOBJ = false;

    void NoteDirty(const GPU& gpu, u32 line, bool obj, const u64* const* bits, const u32* words) noexcept;
    void NoteCopied(const GPU& gpu) noexcept;
    void WriteBlock(const GPU2D::LineJob& job, u32 num, bool winEdge) noexcept;
    void ScanRun(Engine& e) noexcept;
    void FlushRun(u32 num, u32 end) noexcept;
    void CheckBlock(const u32* block, const GPU2D::UnitState& u, const GPU2D::LineJob& job) noexcept;
};

}

#endif
