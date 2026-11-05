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

#ifndef GPU2D_H
#define GPU2D_H

#include "types.h"
#include "Savestate.h"

namespace melonDS
{
class GPU;

namespace GPU2D
{

// the Unit state the renderer reads. Unit is built around it, so a line's snapshot is a plain copy
// (see LineJob). the registers only the CPU or the per-line updates use stay in Unit
struct UnitState
{
    u32 Num;
    bool Enabled;

    u32 DispCnt;
    u8 LayerEnable;         // DISPCNT bits 8-12 as the BG pass sees them: enabling a layer is delayed by 2 lines
    u8 OBJEnable;           // DISPCNT bit 12 as the sprite pass sees it: enabling is delayed by 1 line
    u8 ForcedBlank;         // DISPCNT bit 7 as the BG pass sees it: leaving forced blank is delayed by 2 lines
    u16 BGCnt[4];

    u16 BGXPos[4];
    u16 BGYPos[4];

    s32 BGXRefInternal[2];  // affine reference points as latched for this line (see Unit::PreDrawScanline)
    s32 BGYRefInternal[2];
    s16 BGRotA[2];
    s16 BGRotB[2];
    s16 BGRotC[2];
    s16 BGRotD[2];

    u8 Win0Coords[4];
    u8 Win1Coords[4];
    u8 WinCnt[4];
    u32 Win0Active;
    u32 Win1Active;

    u8 BGMosaicSize[2];
    u8 OBJMosaicSize[2];
    u32 BGMosaicLine;       // the line the current BG mosaic row started on, for text BGs
    u8 OBJMosaicY;

    u16 BlendCnt;
    u8 EVA, EVB;
    u8 EVY;

    bool CaptureLatch;
    u32 CaptureCnt;

    u16 MasterBrightness;

    void CalculateWindowMask(u32 line, u8* windowMask, const u8* objWindow) const;
};

class Unit : public UnitState
{
public:
    // take a reference to the GPU so we can access its state
    // and ensure that it's not null
    Unit(u32 num, melonDS::GPU& gpu);
    virtual ~Unit() = default;
    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;

    void Reset();

    void DoSavestate(Savestate* file);

    void SetEnabled(bool enable) { Enabled = enable; }

    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    u32 Read32(u32 addr);
    void Write8(u32 addr, u8 val);
    void Write16(u32 addr, u16 val);
    void Write32(u32 addr, u32 val);

    bool UsesFIFO() const
    {
        if (((DispCnt >> 16) & 0x3) == 3)
            return true;
        if ((CaptureCnt & (1<<25)) && ((CaptureCnt >> 29) & 0x3) != 0)
            return true;

        return false;
    }

    void SampleFIFO(u32 offset, u32 num);

    void VBlank();
    virtual void VBlankEnd();

    void CheckWindows(u32 line);

    // per-line state updates that go with drawing a scanline. the renderer only reads snapshots
    // of the Unit, so these run around taking them in GPU::StartHBlank (line = VCount)
    void PreDrawScanline(u32 line);
    void PostDrawScanline(u32 line);
    void ResetOBJMosaicCounters();

    void UpdateMosaicCounters(u32 line);

    const UnitState& State() const { return *this; }

    u16 DispFIFO[16];
    u32 DispFIFOReadPtr;
    u32 DispFIFOWritePtr;

    u16 DispFIFOBuffer[256];

    u32 DispCntLatch[3];    // DispCnt at the last three HBlanks, for the enable latency above

    s32 BGXRef[2];          // the running reference points, advanced every line (PostDrawScanline)
    s32 BGYRef[2];
    s32 BGXRefReload[2];    // as written by the CPU: the running ones restart from them each frame
    s32 BGYRefReload[2];

    u8 BGMosaicY, BGMosaicYMax;
    bool BGMosaicLatch;     // this line starts a BG mosaic row: the renderer's refs and line are latched
    u8 OBJMosaicYCount, OBJMosaicYMax;

    u16 BlendAlpha;
private:
    melonDS::GPU& GPU;
};

// one HBlank's worth of 2D drawing: the BG/OBJ pass of the current line and the sprites of the
// next one, for both Units. GPU::StartHBlank builds it on the emulator thread, and the renderer
// draws only from it plus VRAM (and its flat copies), and palette and OAM through its pointers.
// with threaded 2D, jobs wait in a ring until the 2D thread draws them, and display capture lines
// with an accelerated 3D renderer wait until line 192 (see GPU::Begin2DJob)
struct LineJob
{
    bool DrawBG;            // DrawScanline(Line) for both Units
    bool DrawOBJ;           // then DrawSprites(SpriteLine) for both Units
    u32 Line;               // framebuffer and 3D line
    u32 SpriteLine;
    u16 VCount;             // the line the BG pass draws (differs from Line after a VCount write)

    bool Accelerated;       // OpenGL/Compute 3D: the accelerated line format
    u16 RenderXPos;         // 3D layer X position, for the accelerated format's control word
    bool WideView;          // the 3D output is wider than 256, so the control word carries the wings info
    u32* Framebuffer[2];    // per Unit, as assigned when the job was built (POWCNT can swap them)
    u32* Line3D;            // unit A's 3D line, or null when this line didn't fetch it (the renderer keeps the last one)
    bool Fetch3DLine;       // held for display capture: Line3D is fetched when the job is drawn (GPU::Flush2DDeferred)
    bool Abort3DLine;       // and GPU3D::AbortFrame as it was when the job was built (RenderXPos is above). set on every line that captures 3D
    u32 VRAMMap_LCDC;       // VRAM display and display capture only use LCDC banks
    bool ScreensEnabled;    // POWCNT1 bit 0 as latched at the frame start: clear = both screens black
    bool HiresCapture;      // Accelerated, GPU::HiresCapture and the compositor's pass would run: capture lines fill the record, VRAM display lines get the flag
    const u8* Palette;      // palette and OAM as they were when the job was built: a copy for a queued job (see GPU::Start2DJob)
    const u8* OAM;

    UnitState BG[2];        // after PreDrawScanline, before PostDrawScanline
    UnitState OBJ[2];       // after PostDrawScanline (line 0: after ResetOBJMosaicCounters)

    u16 DispFIFOBuffer[256]; // unit A's, only copied when it uses the display FIFO

    // queued jobs keep their own copy of the 3D line (Line3D points here), since the 3D renderer
    // reuses its line buffer before the 2D thread gets to the job
    u32 Line3DBuffer[256];
};

class Renderer2D
{
public:
    virtual ~Renderer2D() {}

    // the only drawing entry point: everything a line needs comes in the job
    virtual void RunJob(const LineJob& job) = 0;

    virtual void VBlankEnd(Unit* unitA, Unit* unitB) = 0;

    void SetFramebuffer(u32* unitA, u32* unitB)
    {
        Framebuffer[0] = unitA;
        Framebuffer[1] = unitB;
    }
    u32* GetFramebuffer(u32 num) const { return Framebuffer[num]; }
protected:
    u32* Framebuffer[2];

    const LineJob* CurJob;
    const UnitState* CurUnit;
};

}

}
#endif
