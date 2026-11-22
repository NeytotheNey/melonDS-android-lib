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

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <type_traits>
#include "NDS.h"
#include "GPU.h"
#include "WideMelon.h"
#include "FIFO.h"
#include "GPU3D_Soft.h"
#include "Platform.h"
#include "GPU3D.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash/xxhash.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

// 3D engine notes
//
// vertex/polygon RAM is filled when a complete polygon is defined, after it's been culled and clipped
// 04000604 reads from bank used by renderer
// bank used by renderer is emptied at scanline ~192
// banks are swapped at scanline ~194
// TODO: needs more investigation. it's weird.
//
// clipping rules:
// * if a shared vertex in a strip is clipped, affected polygons are converted into single polygons
//   strip is resumed at the first eligible polygon
//
// clipping exhibits oddities on the real thing. bad precision? fancy algorithm? TODO: investigate.
//
// vertex color precision:
// * vertex colors are kept at 5-bit during clipping. makes for shitty results.
// * vertex colors are converted to 9-bit before drawing, as such:
//   if (x > 0) x = (x << 4) + 0xF
//   the added bias affects interpolation.
//
// depth buffer:
// Z-buffering mode: val = ((Z * 0x800 * 0x1000) / W) + 0x7FFEFF (nope, wrong. TODO update)
// W-buffering mode: val = W
//
// formula for clear depth: (GBAtek is wrong there)
// clearZ = (val * 0x200) + 0x1FF;
//
// alpha is 5-bit
//
// matrix push/pop on the position matrix are always applied to the vector matrix too, even in position-only mode
// store/restore too, probably (TODO: confirm)
// (the idea is that each position matrix has an associated vector matrix)
//
// TODO: check if translate works on the vector matrix? seems pointless
//
// viewport Y coordinates are upside-down
//
// several registers are latched upon VBlank, the renderer uses the latched registers
// latched registers include:
// DISP3DCNT
// alpha test ref value
// fog color, offset, density table
// toon table
// edge table
// clear attributes
//
// TODO: check how DISP_1DOT_DEPTH works and whether it's latched
//
// TODO: emulate GPU hanging
// * when calling BEGIN with an incomplete polygon defined
// * probably same with BOXTEST
// * when sending vertices immediately after a BOXTEST
//
// TODO: test results should probably not be presented immediately, even if we set the busy flag


// command execution notes
//
// timings given by GBAtek are for individual commands
// actual display lists have different timing characteristics
// * vertex pipeline: individual vertex commands are able to execute in parallel
//   with certain other commands
// * similarly, the normal command can execute in parallel with a subsequent vertex
// * polygon pipeline: each vertex which completes a polygon takes longer to run
//   and imposes rules on when further vertex commands can run
//   (one every 9-cycle time slot during polygon setup)
//   polygon setup time is 27 cycles for a triangle and 36 for a quad
//   except: only one time slot is taken if the polygon is rejected by culling/clipping
// * additionally, some commands (BEGIN, LIGHT_VECTOR, BOXTEST) stall the polygon pipeline


const u8 CmdNumParams[256] =
{
    // 0x00
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x10
    1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3,
    0, 0, 0,
    // 0x20
    1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0,
    // 0x30
    1, 1, 1, 1, 32,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x40
    1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x50
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x60
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x70
    3, 2, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x80+
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void MatrixLoadIdentity(s32* m);

GPU3D::GPU3D(melonDS::NDS& nds, std::unique_ptr<Renderer3D>&& renderer) noexcept :
    NDS(nds),
    CurrentRenderer(renderer ? std::move(renderer) : std::make_unique<SoftRenderer>())
{
}

GPU3D::GPU3D(melonDS::NDS& nds, ShadowTag) noexcept :
    NDS(nds)
{
}

void Vertex::DoSavestate(Savestate* file) noexcept
{
    file->VarArray(Position, sizeof(Position));
    file->VarArray(Color, sizeof(Color));
    file->VarArray(TexCoords, sizeof(TexCoords));

    file->Bool32(&Clipped);

    file->VarArray(FinalPosition, sizeof(FinalPosition));
    file->VarArray(FinalColor, sizeof(FinalColor));
    file->VarArray(HiresPosition, sizeof(HiresPosition));
}

// savestates have upstream's layout, NativePolygonRAMSize polygons and NativeVertexRAMSize vertices a
// bank, where the banks here are PolygonRAMSize and VertexRAMSize apart. what's past those in a bank has
// no index of its own there, it's written as the bank's first one (GP3W has it, see DoWideSavestate)
static u32 LogicalIndex(u32 physical, u32 bankSize, u32 nativeSize) noexcept
{
    u32 i = physical % bankSize;
    return (physical / bankSize) * nativeSize + (i < nativeSize ? i : 0);
}

static u32 PhysicalIndex(u32 logical, u32 bankSize, u32 nativeSize) noexcept
{
    return (logical / nativeSize) * bankSize + logical % nativeSize;
}

// a polygon's fields in a savestate, but for its vertex pointers
static void PolygonFieldsSavestate(Polygon* poly, Savestate* file) noexcept
{
    file->Var32(&poly->NumVertices);

    file->VarArray(poly->FinalZ, sizeof(s32)*10);
    file->VarArray(poly->FinalW, sizeof(s32)*10);
    file->Bool32(&poly->WBuffer);

    file->Var32(&poly->Attr);
    file->Var32(&poly->TexParam);
    file->Var32(&poly->TexPalette);

    file->Bool32(&poly->FacingView);
    file->Bool32(&poly->Translucent);

    file->Bool32(&poly->IsShadowMask);
    file->Bool32(&poly->IsShadow);

    if (file->IsAtLeastVersion(4, 1))
        file->Var32((u32*)&poly->Type);
    else
        poly->Type = 0;

    file->Var32(&poly->VTop);
    file->Var32(&poly->VBottom);
    file->Var32((u32*)&poly->YTop);
    file->Var32((u32*)&poly->YBottom);
    file->Var32((u32*)&poly->XTop);
    file->Var32((u32*)&poly->XBottom);

    file->Var32(&poly->SortKey);
}

// after a load, once its vertex pointers are in
static void LoadedPolygonDegenerate(Polygon* poly) noexcept
{
    poly->Degenerate = false;
    poly->PreciseDepth = false;
    poly->Camera = GPU3D::NoSceneCamera;

    for (u32 j = 0; j < poly->NumVertices; j++)
    {
        if (poly->Vertices[j]->Position[3] == 0)
            poly->Degenerate = true;
    }

    if (poly->YBottom > 192) poly->Degenerate = true;
}

void GPU3D::SetCurrentRenderer(std::unique_ptr<Renderer3D>&& renderer) noexcept
{
    CurrentRenderer = std::move(renderer);
    CurrentRenderer->Reset(NDS.GPU);

    // the compute renderer's, which sets them again at its first RenderFrame. left on, the other
    // renderers would hash and copy every scene for nothing
    SceneHashLogging = false;
    SceneRepeatCheck = false;
    SceneCopyValid = false;

    // software only does 256, so don't keep squeezing geometry for it. WideMelon's wide list stays as
    // the scene started (or as a state load brought it) until the swap: it works at 256 too, where the
    // native view is the view, so the native counts a bank past the DS's limits relies on stay right
    ViewWidth = RendererViewWidth();
    ViewMargin = RendererViewMargin();
}

void GPU3D::LatchViewWidth() noexcept
{
    ViewWidth = RendererViewWidth();
    ViewMargin = RendererViewMargin();
    WideListOn = ViewWidth != WideMelon::NativeWidth && NDS.GPU.WidePolys;
    PreciseDepthOn = NDS.GPU.PreciseDepth;
}

void GPU3D::SetVertexIdLogging(int mode) noexcept
{
    VertexIdLog = mode > 0 ? std::make_unique<VertexIdLogger>(mode) : nullptr;
}

// CurSceneCamera for the projection and viewport now: the one it was if they're the same, else the table's entry for them, made
// if there's room
void GPU3D::NoteSceneCamera() noexcept
{
    SceneCameraDirty = false;
    auto isCurrent = [this](const CameraCount& camera)
    {
        return !memcmp(camera.Proj, ProjMatrix, sizeof(camera.Proj)) && !memcmp(camera.Viewport, Viewport, sizeof(camera.Viewport));
    };
    if (CurSceneCamera != NoSceneCamera && isCurrent(SceneCameras[CurSceneCamera]))
        return;
    for (u32 i = 0; i < NumSceneCameras; i++)
    {
        if (isCurrent(SceneCameras[i]))
        {
            CurSceneCamera = (u8)i;
            return;
        }
    }
    if (NumSceneCameras == MaxSceneCameras)
    {
        CurSceneCamera = NoSceneCamera;
        return;
    }
    CameraCount& camera = SceneCameras[NumSceneCameras];
    memcpy(camera.Proj, ProjMatrix, sizeof(camera.Proj));
    memcpy(camera.Viewport, Viewport, sizeof(camera.Viewport));
    camera.Polygons = 0;
    CurSceneCamera = (u8)NumSceneCameras++;
}

void GPU3D::ResetSceneCameras() noexcept
{
    NumSceneCameras = 0;
    OtherCameraPolygons = 0;
    CurSceneCamera = NoSceneCamera;
    SceneCameraDirty = true;
}

// the scene being built's camera, at its swap (or HalfCamera's replay's end): the most polygons' (the first of a tie)
GPU3D::SceneCamera GPU3D::PickSceneCamera() const noexcept
{
    SceneCamera pick;
    u32 most = 0, counted = OtherCameraPolygons;
    for (u32 i = 0; i < NumSceneCameras; i++)
    {
        counted += SceneCameras[i].Polygons;
        if (SceneCameras[i].Polygons > most)
        {
            most = SceneCameras[i].Polygons;
            pick.Index = (u8)i;
        }
    }
    if (pick.Index == NoSceneCamera)
        return pick;

    const CameraCount& camera = SceneCameras[pick.Index];
    memcpy(pick.Proj, camera.Proj, sizeof(pick.Proj));
    memcpy(pick.Viewport, camera.Viewport, sizeof(pick.Viewport));
    // clip w is (v x, y, z, 1) dotted with the matrix's fourth column, which a perspective projection makes depend on the
    // position; an orthographic one leaves it at Proj[15]. every polygon of a scene shares its W-buffer setting
    const bool perspective = camera.Proj[3] != 0 || camera.Proj[7] != 0 || camera.Proj[11] != 0;
    const bool wbuffer = NumPolygons != 0 && CurPolygonRAM[0].WBuffer;
    pick.Valid = perspective && !wbuffer && (u64)most * 2 >= NumPolygons && counted == NumPolygons;
    return pick;
}

void GPU3D::SetHalfCamera(int mode) noexcept
{
    // set again by a new game's start only, with no 3D frame in progress
    if (HalfCam) HalfCam->Invalidate();
    HalfCam = mode > 0 ? std::make_unique<HalfCamera>(*this, mode, false) : nullptr;
}

void GPU3D::SetHalfCameraSmoothing(bool isEnabled) noexcept
{
    if (!HalfCam && isEnabled)
        HalfCam = std::make_unique<HalfCamera>(*this, 4, true);
    if (HalfCam)
        HalfCam->SetPaused(!isEnabled);
}

void GPU3D::ResetRenderingState() noexcept
{
    RenderNumPolygons = 0;
    RenderCamera = {};

    RenderDispCnt = 0;
    RenderAlphaRef = 0;

    memset(RenderEdgeTable, 0, 8*2);
    memset(RenderToonTable, 0, 32*2);

    RenderFogColor = 0;
    RenderFogOffset = 0;
    RenderFogShift = 0;
    memset(RenderFogDensityTable, 0, 34);

    RenderClearAttr1 = 0x3F000000;
    RenderClearAttr2 = 0x00007FFF;
}

void GPU3D::MarkRenderStateReplaced() noexcept
{
    MarkRenderFrameChanged();
    RenderStateReplaced = true;
    SceneCopyValid = false;
}

void GPU3D::Reset() noexcept
{
    if (VertexIdLog) VertexIdLog->Invalidate();
    if (HalfCam) HalfCam->Invalidate();
    ResetSceneCameras();

    CmdFIFO.Clear();
    CmdPIPE.Clear();

    CmdStallQueue.Clear();

    ZeroDotWLimit = 0xFFFFFF;

    GXStat = 0;

    memset(ExecParams, 0, 32*4);
    ExecParamCount = 0;

    CycleCount = 0;
    VertexPipeline = 0;
    NormalPipeline = 0;
    PolygonPipeline = 0;
    VertexSlotCounter = 0;
    VertexSlotsFree = 1;

    NumPushPopCommands = 0;
    NumTestCommands = 0;

    MatrixMode = 0;

    MatrixLoadIdentity(ProjMatrix);
    MatrixLoadIdentity(PosMatrix);
    MatrixLoadIdentity(VecMatrix);
    MatrixLoadIdentity(TexMatrix);

    ClipMatrixDirty = true;
    UpdateClipMatrix();

    memset(Viewport, 0, sizeof(Viewport));

    memset(ProjMatrixStack, 0, 16*4);
    memset(PosMatrixStack, 0, 31 * 16*4);
    memset(VecMatrixStack, 0, 31 * 16*4);
    memset(TexMatrixStack, 0, 16*4);

    ProjMatrixStackPointer = 0;
    PosMatrixStackPointer = 0;
    TexMatrixStackPointer = 0;

    NumCommands = 0;
    CurCommand = 0;
    ParamCount = 0;
    TotalParams = 0;

    GeometryEnabled = false;
    RenderingEnabled = false;

    DispCnt = 0;
    AlphaRefVal = 0;
    AlphaRef = 0;

    memset(ToonTable, 0, sizeof(ToonTable));
    memset(EdgeTable, 0, sizeof(EdgeTable));

    // TODO: confirm initial polyid/color/fog values
    FogOffset = 0;
    FogColor = 0;
    memset(FogDensityTable, 0, sizeof(FogDensityTable));

    ClearAttr1 = 0x3F000000;
    ClearAttr2 = 0x00007FFF;

    ResetRenderingState();
    MarkRenderStateReplaced();

    AbortFrame = false;

    Timestamp = 0;

    PolygonMode = 0;
    memset(CurVertex, 0, sizeof(CurVertex));
    memset(VertexColor, 0, sizeof(VertexColor));
    memset(TexCoords, 0, sizeof(TexCoords));
    memset(RawTexCoords, 0, sizeof(RawTexCoords));
    memset(Normal, 0, sizeof(Normal));

    memset(LightDirection, 0, sizeof(LightDirection));
    memset(LightColor, 0, sizeof(LightColor));
    memset(MatDiffuse, 0, sizeof(MatDiffuse));
    memset(MatAmbient, 0, sizeof(MatAmbient));
    memset(MatSpecular, 0, sizeof(MatSpecular));
    memset(MatEmission, 0, sizeof(MatSpecular));

    UseShininessTable = false;
    // Shininess table seems to be uninitialized garbage, at least on n3dsxl hw?
    // Also doesn't seem to be cleared properly unless the system is fully powered off?
    memset(ShininessTable, 0, sizeof(ShininessTable));

    PolygonAttr = 0;
    CurPolygonAttr = 0;

    TexParam = 0;
    TexPalette = 0;

    memset(PosTestResult, 0, 4*4);
    memset(VecTestResult, 0, 2*3);

    memset(TempVertexBuffer, 0, sizeof(TempVertexBuffer));
    VertexNum = 0;
    VertexNumInPoly = 0;
    NumConsecutivePolygons = 0;
    LastStripPolygon = nullptr;
    NumOpaquePolygons = 0;

    CurVertexRAM = &VertexRAM[0];
    CurPolygonRAM = &PolygonRAM[0];
    NumVertices = 0;
    NumPolygons = 0;
    CurRAMBank = 0;

    memset(TempVertexNativeX, 0, sizeof(TempVertexNativeX));
    memset(TempVertexNativeY, 0, sizeof(TempVertexNativeY));
    NativeNumVertices = 0;
    NativeNumPolygons = 0;
    NativeStripVerts = 0;
    WingNumPolygons = 0;
    WingNumVertices = 0;
    WideListOverflow = false;
    DroppedPolygons = 0;
    DroppedNativeFit = 0;
    DroppedWing = 0;
    WideDrops.SceneStartDrops = WideDrops.SecondStartDrops = WideDrops.SecondStartNativeFit = WideDrops.SecondStartWing = 0;

    FlushRequest = 0;
    FlushAttributes = 0;

    RenderXPos = 0;

    if (CurrentRenderer)
        LatchViewWidth();
    else
    {
        ViewWidth = WideMelon::NativeWidth;
        ViewMargin = 0;
        WideListOn = false;
        PreciseDepthOn = NDS.GPU.PreciseDepth;
    }
    RenderViewWidth = ViewWidth;
    RenderViewMargin = ViewMargin;
    RenderWideList = WideListOn;

    if (CurrentRenderer)
        CurrentRenderer->Reset(NDS.GPU);
}

void GPU3D::DoSavestate(Savestate* file) noexcept
{
    file->Section("GP3D");

    SoftRenderer* softRenderer = dynamic_cast<SoftRenderer*>(CurrentRenderer.get());
    if (softRenderer && softRenderer->IsThreaded())
    {
        softRenderer->SetupRenderThread(NDS.GPU);
    }

    CmdFIFO.DoSavestate(file);
    CmdPIPE.DoSavestate(file);

    file->Var32(&NumCommands);
    file->Var32(&CurCommand);
    file->Var32(&ParamCount);
    file->Var32(&TotalParams);

    file->Var32(&NumPushPopCommands);
    file->Var32(&NumTestCommands);

    file->Var32(&DispCnt);
    file->Var8(&AlphaRefVal);
    file->Var8(&AlphaRef);

    file->VarArray(ToonTable, 32*2);
    file->VarArray(EdgeTable, 8*2);

    file->Var32(&FogColor);
    file->Var32(&FogOffset);
    file->VarArray(FogDensityTable, 32);

    file->Var32(&ClearAttr1);
    file->Var32(&ClearAttr2);

    file->Var32(&RenderDispCnt);
    file->Var8(&RenderAlphaRef);

    file->VarArray(RenderToonTable, 32*2);
    file->VarArray(RenderEdgeTable, 8*2);

    file->Var32(&RenderFogColor);
    file->Var32(&RenderFogOffset);
    file->Var32(&RenderFogShift);
    file->VarArray(RenderFogDensityTable, 34);

    file->Var32(&RenderClearAttr1);
    file->Var32(&RenderClearAttr2);

    file->Var16(&RenderXPos);

    file->Var32(&ZeroDotWLimit);

    file->Var32(&GXStat);

    file->VarArray(ExecParams, 32*4);
    file->Var32(&ExecParamCount);
    file->Var32((u32*)&CycleCount);
    file->Var64(&Timestamp);

    file->Var32(&MatrixMode);

    file->VarArray(ProjMatrix, 16*4);
    file->VarArray(PosMatrix, 16*4);
    file->VarArray(VecMatrix, 16*4);
    file->VarArray(TexMatrix, 16*4);

    file->VarArray(ProjMatrixStack, 16*4);
    file->VarArray(PosMatrixStack, 32*16*4);
    file->VarArray(VecMatrixStack, 32*16*4);
    file->VarArray(TexMatrixStack, 16*4);

    file->Var32((u32*)&ProjMatrixStackPointer);
    file->Var32((u32*)&PosMatrixStackPointer);
    file->Var32((u32*)&TexMatrixStackPointer);

    file->VarArray(Viewport, sizeof(Viewport));

    file->VarArray(PosTestResult, 4*4);
    file->VarArray(VecTestResult, 2*3);

    file->Var32(&VertexNum);
    file->Var32(&VertexNumInPoly);
    file->Var32(&NumConsecutivePolygons);

    for (Vertex& vtx : TempVertexBuffer)
    {
        vtx.DoSavestate(file);
    }

    if (file->Saving)
    {
        // one past what GP3D's layout holds is none here (GP3W has it)
        u32 index = UINT32_MAX;
        if (LastStripPolygon)
        {
            u32 physical = (u32)(LastStripPolygon - &PolygonRAM[0]);
            if (physical % PolygonRAMSize < NativePolygonRAMSize)
                index = LogicalIndex(physical, PolygonRAMSize, NativePolygonRAMSize);
        }
        file->Var32(&index);
    }
    else
    {
        u32 index = UINT32_MAX;
        file->Var32(&index);
        LastStripPolygon = (index == UINT32_MAX) ? nullptr : &PolygonRAM[PhysicalIndex(index, PolygonRAMSize, NativePolygonRAMSize)];
    }

    file->Var32(&CurRAMBank);
    // any other value is bank 1, as everything here reads it; GP3W's checks go by 0 or 1
    if (!file->Saving)
        CurRAMBank = CurRAMBank ? 1 : 0;
    if (file->Saving)
    {
        // WideMelon's wide list can go past what GP3D holds: the counts are capped (GP3W has the real
        // ones), and the opaque polygons counted over the ones it has
        u32 numVertices = std::min(NumVertices, NativeVertexRAMSize);
        u32 numPolygons = std::min(NumPolygons, NativePolygonRAMSize);
        u32 numOpaquePolygons = NumOpaquePolygons;
        if (numPolygons != NumPolygons)
        {
            numOpaquePolygons = 0;
            for (u32 i = 0; i < numPolygons; i++)
                numOpaquePolygons += !CurPolygonRAM[i].Translucent;
        }
        file->Var32(&numVertices);
        file->Var32(&numPolygons);
        file->Var32(&numOpaquePolygons);
    }
    else
    {
        file->Var32(&NumVertices);
        file->Var32(&NumPolygons);
        file->Var32(&NumOpaquePolygons);
    }

    if (!file->Saving)
    {
        // WideMelon's native view counts, X and Y aren't stored: as if the native view had the same so far
        for (int i = 0; i < 4; i++)
        {
            TempVertexNativeX[i] = TempVertexBuffer[i].Position[0];
            TempVertexNativeY[i] = TempVertexBuffer[i].Position[1];
        }
        NativeNumVertices = NumVertices;
        NativeNumPolygons = NumPolygons;
        // and nothing in the wide list's wing budget, unless GP3W says otherwise
        WingNumVertices = 0;
        WingNumPolygons = 0;
        WideListOverflow = false;
    }

    file->Var32(&FlushRequest);
    file->Var32(&FlushAttributes);

    // in upstream's layout, NativeVertexRAMSize vertices and NativePolygonRAMSize polygons a bank
    for (u32 bank = 0; bank < 2; bank++)
    {
        for (u32 i = 0; i < NativeVertexRAMSize; i++)
            VertexRAM[bank * VertexRAMSize + i].DoSavestate(file);
    }

    for (u32 i = 0; i < NativePolygonRAMSize*2; i++)
    {
        Polygon* poly = &PolygonRAM[PhysicalIndex(i, PolygonRAMSize, NativePolygonRAMSize)];

        // this is a bit ugly, but eh
        // we can't save the pointers as-is, that's a bad idea
        if (file->Saving)
        {
            for (int j = 0; j < 10; j++)
            {
                Vertex* ptr = poly->Vertices[j];
                u32 index = ptr ? LogicalIndex((u32)(ptr - &VertexRAM[0]), VertexRAMSize, NativeVertexRAMSize) : UINT32_MAX;
                file->Var32(&index);
            }
        }
        else
        {
            for (int j = 0; j < 10; j++)
            {
                u32 index = UINT32_MAX;
                file->Var32(&index);
                poly->Vertices[j] = index == UINT32_MAX ? nullptr : &VertexRAM[PhysicalIndex(index, VertexRAMSize, NativeVertexRAMSize)];
            }
        }

        PolygonFieldsSavestate(poly, file);

        if (!file->Saving)
            LoadedPolygonDegenerate(poly);
    }

    if (!file->Saving)
    {
        // and the native view's last strip polygon from the real one, now that the polygon and vertex RAM are in
        NativeStripVerts = LastStripPolygon ? LastStripPolygon->NumVertices : 0;
        NativeStripClipped = 0;
        for (u32 i = 0; i < NativeStripVerts && i < 10; i++)
            NativeStripClipped |= (LastStripPolygon->Vertices[i] && LastStripPolygon->Vertices[i]->Clipped) << i;
    }

    CmdStallQueue.DoSavestate(file);

    file->Var32((u32*)&VertexPipeline);
    file->Var32((u32*)&NormalPipeline);
    file->Var32((u32*)&PolygonPipeline);
    file->Var32((u32*)&VertexSlotCounter);
    file->Var32(&VertexSlotsFree);

    if (!file->Saving)
    {
        ClipMatrixDirty = true;
        UpdateClipMatrix();

        if (VertexIdLog) VertexIdLog->Invalidate();
        // the scene's polygons so far aren't counted, which leaves its pick invalid
        ResetSceneCameras();

        CurVertexRAM = &VertexRAM[CurRAMBank ? VertexRAMSize : 0];
        CurPolygonRAM = &PolygonRAM[CurRAMBank ? PolygonRAMSize : 0];
    }

    // never the in-between scene, the state has the scene itself. first on a load, where the state
    // replaces the list it would put back
    if (HalfCam && !file->Saving)
        HalfCam->Invalidate();
    const RenderList* renderList = &RenderPolygonRAM;
    u32 renderNumPolygons = RenderNumPolygons;
    if (HalfCam && file->Saving)
        HalfCam->GetRealRenderList(renderList, renderNumPolygons);

    // as GP3D's layout holds it, capped like the counts above
    u32 savedNumPolygons = std::min(renderNumPolygons, NativePolygonRAMSize);
    file->Var32(&savedNumPolygons);
    if (!file->Saving)
    {
        RenderNumPolygons = savedNumPolygons;
        // not in the state. after HalfCam's Invalidate above, which puts the real scene's back
        RenderCamera = {};
    }
    if (file->Saving)
    {
        for (u32 i = 0; i < NativePolygonRAMSize; i++)
        {
            const Polygon* p = (*renderList)[i];
            u32 index = p ? LogicalIndex((u32)(p - &PolygonRAM[0]), PolygonRAMSize, NativePolygonRAMSize) : UINT32_MAX;

            file->Var32(&index);
        }
    }
    else
    {
        for (u32 i = 0; i < NativePolygonRAMSize; ++i)
        {
            u32 index = UINT32_MAX;
            file->Var32(&index);

            RenderPolygonRAM[i] = index == UINT32_MAX ? nullptr : &PolygonRAM[PhysicalIndex(index, PolygonRAMSize, NativePolygonRAMSize)];
        }
        std::fill(RenderPolygonRAM.begin() + NativePolygonRAMSize, RenderPolygonRAM.end(), nullptr);
    }

    file->VarArray(CurVertex, sizeof(s16)*3);
    file->VarArray(VertexColor, sizeof(u8)*3);
    file->VarArray(TexCoords, sizeof(s16)*2);
    file->VarArray(RawTexCoords, sizeof(s16)*2);
    file->VarArray(Normal, sizeof(s16)*3);

    file->VarArray(LightDirection, sizeof(s16)*4*3);
    file->VarArray(LightColor, sizeof(u8)*4*3);
    file->VarArray(MatDiffuse, sizeof(u8)*3);
    file->VarArray(MatAmbient, sizeof(u8)*3);
    file->VarArray(MatSpecular, sizeof(u8)*3);
    file->VarArray(MatEmission, sizeof(u8)*3);

    file->Bool32(&UseShininessTable);
    file->VarArray(ShininessTable, 128*sizeof(u8));

    file->Bool32(&AbortFrame);
    file->Bool32(&GeometryEnabled);
    file->Bool32(&RenderingEnabled);
    file->Var32(&PolygonMode);
    file->Var32(&PolygonAttr);
    file->Var32(&CurPolygonAttr);
    file->Var32(&TexParam);
    file->Var32(&TexPalette);

    // WideMelon's wide list, in a section of its own: only with it on, or a bank holding more than GP3D
    // can, so a state saved without it is as before
    bool wideSection = file->Saving ? (WideListOn || WideSavestateExtra(renderList, renderNumPolygons)) : file->HasSection("GP3W");
    if (wideSection)
        DoWideSavestate(file, renderList, renderNumPolygons);
    // the list a load brought is the wide list's if it's longer than the DS's, uses vertices past
    // theirs (only a GP3W can bring either), or if the list is on now
    if (!file->Saving)
    {
        RenderWideList = WideListOn || RenderNumPolygons > NativePolygonRAMSize;
        for (u32 i = 0; wideSection && !RenderWideList && i < RenderNumPolygons; i++)
        {
            const Polygon* poly = RenderPolygonRAM[i];
            for (u32 j = 0; poly && j < poly->NumVertices && j < 10; j++)
            {
                if (poly->Vertices[j] && (u32)(poly->Vertices[j] - &VertexRAM[0]) % VertexRAMSize >= NativeVertexRAMSize)
                    RenderWideList = true;
            }
        }
    }

    RenderFrameIdentical = false;
    // the state's scene isn't the one the renderers drew. the first VBlank would otherwise call a
    // frame without a swap identical, going by the state's registers alone
    if (!file->Saving)
        MarkRenderStateReplaced();
    if (softRenderer && softRenderer->IsThreaded())
    {
        softRenderer->EnableRenderThread();
    }
}

// how far into each bank what a savestate needs goes: the scene being built, its last strip polygon
// and the render list's polygons, with their vertices
void GPU3D::WideSavestateHighWater(const RenderList* renderList, u32 renderNumPolygons, u32 polygons[2], u32 vertices[2]) const noexcept
{
    polygons[0] = polygons[1] = vertices[0] = vertices[1] = 0;
    polygons[CurRAMBank ? 1 : 0] = NumPolygons;
    vertices[CurRAMBank ? 1 : 0] = NumVertices;
    auto note = [&](const Polygon* poly)
    {
        u32 index = (u32)(poly - &PolygonRAM[0]);
        if (index >= PolygonRAMSize * 2)
            return;
        u32 bank = index / PolygonRAMSize;
        polygons[bank] = std::max(polygons[bank], index % PolygonRAMSize + 1);
        for (u32 j = 0; j < poly->NumVertices && j < 10; j++)
        {
            u32 vertex = poly->Vertices[j] ? (u32)(poly->Vertices[j] - &VertexRAM[0]) : UINT32_MAX;
            if (vertex < VertexRAMSize * 2)
                vertices[vertex / VertexRAMSize] = std::max(vertices[vertex / VertexRAMSize], vertex % VertexRAMSize + 1);
        }
    };
    for (u32 i = 0; i < NumPolygons && i < PolygonRAMSize; i++)
        note(&CurPolygonRAM[i]);
    if (LastStripPolygon)
        note(LastStripPolygon);
    for (u32 i = 0; i < renderNumPolygons && i < PolygonRAMSize; i++)
    {
        if ((*renderList)[i])
            note((*renderList)[i]);
    }
}

// whether a bank holds more than GP3D's layout can
bool GPU3D::WideSavestateExtra(const RenderList* renderList, u32 renderNumPolygons) const noexcept
{
    u32 polygons[2], vertices[2];
    WideSavestateHighWater(renderList, renderNumPolygons, polygons, vertices);
    return polygons[0] > NativePolygonRAMSize || polygons[1] > NativePolygonRAMSize
        || vertices[0] > NativeVertexRAMSize || vertices[1] > NativeVertexRAMSize;
}

// the GP3W section, after GP3D (see DoSavestate): what WideMelon's wide list needs that GP3D's upstream
// layout doesn't have. whether the scene was being built into the wide list, the native view's counts,
// strip state and X, and the wing budget's counts, then only when a bank holds more than GP3D can
// (extra): the real counts, each bank's high-water marks, the polygons and vertices past GP3D's in each
// bank, the vertex indices of every polygon up to there as they are here, the whole render list and the
// last strip polygon, which GP3D has capped or as each bank's first. read only when it's there, version
// 3, which has the native view's Y and the margin the scene was built with after its X (camera stage 2),
// or 2, without them, whose Y is then taken as GP3D's (version 1 was a layout without the wide list flag,
// only in builds before this one). the margin is only checked, as the width isn't stored either. a load
// reads it all and checks it before anything GP3D loaded changes, but for the polygons and vertices past
// GP3D's, which nothing points at until then; any other version leaves GP3D's state
void GPU3D::DoWideSavestate(Savestate* file, const RenderList* renderList, u32 renderNumPolygons) noexcept
{
    file->Section("GP3W");

    u32 version = 3;
    file->Var32(&version);
    if (version != 2 && version != 3)
        return;

    // the scalars, as members when saving and checked before they're kept when loading
    struct
    {
        u32 WideList, NativePolygons, NativeVertices, NativeStripVerts, NativeStripClipped, WingPolygons, WingVertices;
        s32 NativeX[4];
        s32 NativeY[4];
        u32 Margin;
        u32 Extra;
        u32 Vertices, Polygons, OpaquePolygons, RenderPolygons;
        u32 BankPolygons[2], BankVertices[2];
        u32 LastStrip;
    } w {};
    if (file->Saving)
    {
        w.WideList = WideListOn ? 1 : 0;
        w.NativePolygons = NativeNumPolygons;
        w.NativeVertices = NativeNumVertices;
        w.NativeStripVerts = NativeStripVerts;
        w.NativeStripClipped = NativeStripClipped;
        w.WingPolygons = WingNumPolygons;
        w.WingVertices = WingNumVertices;
        memcpy(w.NativeX, TempVertexNativeX, sizeof(w.NativeX));
        memcpy(w.NativeY, TempVertexNativeY, sizeof(w.NativeY));
        w.Margin = (u32)ViewMargin;
        WideSavestateHighWater(renderList, renderNumPolygons, w.BankPolygons, w.BankVertices);
        w.Extra = w.BankPolygons[0] > NativePolygonRAMSize || w.BankPolygons[1] > NativePolygonRAMSize
            || w.BankVertices[0] > NativeVertexRAMSize || w.BankVertices[1] > NativeVertexRAMSize;
        w.Vertices = NumVertices;
        w.Polygons = NumPolygons;
        w.OpaquePolygons = NumOpaquePolygons;
        w.RenderPolygons = renderNumPolygons;
        w.LastStrip = LastStripPolygon ? (u32)(LastStripPolygon - &PolygonRAM[0]) : UINT32_MAX;
    }

    file->Var32(&w.WideList);
    file->Var32(&w.NativePolygons);
    file->Var32(&w.NativeVertices);
    file->Var32(&w.NativeStripVerts);
    file->Var32(&w.NativeStripClipped);
    file->Var32(&w.WingPolygons);
    file->Var32(&w.WingVertices);
    file->VarArray(w.NativeX, sizeof(w.NativeX));
    if (version >= 3)
    {
        file->VarArray(w.NativeY, sizeof(w.NativeY));
        file->Var32(&w.Margin);
    }
    else
    {
        for (int i = 0; i < 4; i++)
            w.NativeY[i] = TempVertexBuffer[i].Position[1];
        w.Margin = 0;
    }
    file->Var32(&w.Extra);

    auto fail = [file]() { file->Error = true; };
    if (!file->Saving && (file->Error || w.WideList > 1 || w.Extra > 1 || w.Margin > (u32)WideMelon::MaxMargin
        || w.NativePolygons > NativePolygonRAMSize || w.NativeVertices > NativeVertexRAMSize || w.NativeStripVerts > 10
        || w.WingPolygons > NativePolygonRAMSize || w.WingVertices > NativeVertexRAMSize))
        return fail();

    // the vertex indices of every polygon up to the high-water marks, bank 0's then bank 1's, as loaded
    std::vector<u32> vertexIndices;
    if (w.Extra)
    {
        file->Var32(&w.Vertices);
        file->Var32(&w.Polygons);
        file->Var32(&w.OpaquePolygons);
        file->Var32(&w.RenderPolygons);
        for (u32 bank = 0; bank < 2; bank++)
        {
            file->Var32(&w.BankPolygons[bank]);
            file->Var32(&w.BankVertices[bank]);
        }
        const u32 cur = CurRAMBank ? 1 : 0;
        if (!file->Saving && (file->Error || CurRAMBank > 1
            || w.BankPolygons[0] > PolygonRAMSize || w.BankPolygons[1] > PolygonRAMSize
            || w.BankVertices[0] > VertexRAMSize || w.BankVertices[1] > VertexRAMSize
            || w.Polygons > w.BankPolygons[cur] || w.Vertices > w.BankVertices[cur]
            || w.OpaquePolygons > w.Polygons || w.RenderPolygons > PolygonRAMSize))
            return fail();

        if (!file->Saving)
            vertexIndices.resize((size_t)(w.BankPolygons[0] + w.BankPolygons[1]) * 10);
        size_t next = 0;
        for (u32 bank = 0; bank < 2; bank++)
        {
            // past GP3D's, which nothing points at yet: loaded in place
            for (u32 i = NativeVertexRAMSize; i < w.BankVertices[bank]; i++)
                VertexRAM[bank * VertexRAMSize + i].DoSavestate(file);
            for (u32 i = NativePolygonRAMSize; i < w.BankPolygons[bank]; i++)
                PolygonFieldsSavestate(&PolygonRAM[bank * PolygonRAMSize + i], file);

            for (u32 i = 0; i < w.BankPolygons[bank]; i++)
            {
                const Polygon* poly = &PolygonRAM[bank * PolygonRAMSize + i];
                for (int j = 0; j < 10; j++)
                {
                    u32 index = (file->Saving && poly->Vertices[j]) ? (u32)(poly->Vertices[j] - &VertexRAM[0]) : UINT32_MAX;
                    file->Var32(&index);
                    if (!file->Saving)
                        vertexIndices[next++] = index;
                }
            }
        }

        if (!file->Saving && file->Error)
            return fail();
        // every polygon drawn with vertices that are there
        next = 0;
        for (u32 bank = 0; !file->Saving && bank < 2; bank++)
        {
            for (u32 i = 0; i < w.BankPolygons[bank]; i++, next += 10)
            {
                const Polygon& poly = PolygonRAM[bank * PolygonRAMSize + i];
                if (poly.NumVertices > 10)
                    return fail();
                for (u32 j = 0; j < 10; j++)
                {
                    u32 index = vertexIndices[next + j];
                    if ((index == UINT32_MAX && j < poly.NumVertices) || (index != UINT32_MAX && index >= VertexRAMSize * 2))
                        return fail();
                }
            }
        }
    }

    // a polygon index inside what was saved of its bank
    auto savedPolygon = [&](u32 index) { return index < PolygonRAMSize * 2 && index % PolygonRAMSize < w.BankPolygons[index / PolygonRAMSize]; };
    std::vector<u32> renderIndices;
    if (w.Extra)
    {
        if (!file->Saving)
            renderIndices.resize(w.RenderPolygons);
        for (u32 i = 0; i < w.RenderPolygons; i++)
        {
            const Polygon* p = file->Saving ? (*renderList)[i] : nullptr;
            u32 index = p ? (u32)(p - &PolygonRAM[0]) : UINT32_MAX;
            file->Var32(&index);
            if (!file->Saving)
                renderIndices[i] = index;
        }
        file->Var32(&w.LastStrip);

        if (!file->Saving)
        {
            if (file->Error || (w.LastStrip != UINT32_MAX && !savedPolygon(w.LastStrip)))
                return fail();
            // a render list has no holes before its count
            for (u32 index : renderIndices)
            {
                if (!savedPolygon(index))
                    return fail();
            }
        }
    }

    if (file->Saving)
        return;

    // all checked: the state's from here on
    WideListOn = w.WideList != 0;
    NativeNumPolygons = w.NativePolygons;
    NativeNumVertices = w.NativeVertices;
    NativeStripVerts = w.NativeStripVerts;
    NativeStripClipped = w.NativeStripClipped;
    WingNumPolygons = w.WingPolygons;
    WingNumVertices = w.WingVertices;
    memcpy(TempVertexNativeX, w.NativeX, sizeof(TempVertexNativeX));
    memcpy(TempVertexNativeY, w.NativeY, sizeof(TempVertexNativeY));
    if (!w.Extra)
        return;

    NumVertices = w.Vertices;
    NumPolygons = w.Polygons;
    // counted again, as VBlank puts the translucent ones from there on (the saved one is the same count)
    NumOpaquePolygons = 0;
    for (u32 i = 0; i < NumPolygons; i++)
        NumOpaquePolygons += !CurPolygonRAM[i].Translucent;
    size_t next = 0;
    for (u32 bank = 0; bank < 2; bank++)
    {
        for (u32 i = 0; i < w.BankPolygons[bank]; i++, next += 10)
        {
            Polygon* poly = &PolygonRAM[bank * PolygonRAMSize + i];
            for (u32 j = 0; j < 10; j++)
            {
                u32 index = vertexIndices[next + j];
                poly->Vertices[j] = index == UINT32_MAX ? nullptr : &VertexRAM[index];
            }
            LoadedPolygonDegenerate(poly);
        }
    }
    for (u32 i = 0; i < w.RenderPolygons; i++)
        RenderPolygonRAM[i] = renderIndices[i] == UINT32_MAX ? nullptr : &PolygonRAM[renderIndices[i]];
    RenderNumPolygons = w.RenderPolygons;
    LastStripPolygon = w.LastStrip == UINT32_MAX ? nullptr : &PolygonRAM[w.LastStrip];
}



void GPU3D::SetEnabled(bool geometry, bool rendering) noexcept
{
    // the rendering engine going off empties the scene the last image was drawn from, so that
    // image can't stand as identical: the empty scene is drawn once (see VBlank). only then, as
    // every POWCNT1 write gets here and with the engine already off nothing changes
    if (!rendering && RenderingEnabled)
        MarkRenderStateReplaced();

    GeometryEnabled = geometry;
    RenderingEnabled = rendering;

    if (!rendering) ResetRenderingState();
}



void MatrixLoadIdentity(s32* m)
{
    m[0] = 0x1000; m[1] = 0;      m[2] = 0;       m[3] = 0;
    m[4] = 0;      m[5] = 0x1000; m[6] = 0;       m[7] = 0;
    m[8] = 0;      m[9] = 0;      m[10] = 0x1000; m[11] = 0;
    m[12] = 0;     m[13] = 0;     m[14] = 0;      m[15] = 0x1000;
}

void MatrixLoad4x4(s32* m, s32* s)
{
    memcpy(m, s, 16*4);
}

void MatrixLoad4x3(s32* m, s32* s)
{
    m[0] = s[0];  m[1] = s[1];  m[2] = s[2];    m[3] = 0;
    m[4] = s[3];  m[5] = s[4];  m[6] = s[5];    m[7] = 0;
    m[8] = s[6];  m[9] = s[7];  m[10] = s[8];   m[11] = 0;
    m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}

void MatrixMult4x4(s32* m, s32* s)
{
    s32 tmp[16];
    memcpy(tmp, m, 16*4);

    // m = s*m
    m[0] = ((s64)s[0]*tmp[0] + (s64)s[1]*tmp[4] + (s64)s[2]*tmp[8] + (s64)s[3]*tmp[12]) >> 12;
    m[1] = ((s64)s[0]*tmp[1] + (s64)s[1]*tmp[5] + (s64)s[2]*tmp[9] + (s64)s[3]*tmp[13]) >> 12;
    m[2] = ((s64)s[0]*tmp[2] + (s64)s[1]*tmp[6] + (s64)s[2]*tmp[10] + (s64)s[3]*tmp[14]) >> 12;
    m[3] = ((s64)s[0]*tmp[3] + (s64)s[1]*tmp[7] + (s64)s[2]*tmp[11] + (s64)s[3]*tmp[15]) >> 12;

    m[4] = ((s64)s[4]*tmp[0] + (s64)s[5]*tmp[4] + (s64)s[6]*tmp[8] + (s64)s[7]*tmp[12]) >> 12;
    m[5] = ((s64)s[4]*tmp[1] + (s64)s[5]*tmp[5] + (s64)s[6]*tmp[9] + (s64)s[7]*tmp[13]) >> 12;
    m[6] = ((s64)s[4]*tmp[2] + (s64)s[5]*tmp[6] + (s64)s[6]*tmp[10] + (s64)s[7]*tmp[14]) >> 12;
    m[7] = ((s64)s[4]*tmp[3] + (s64)s[5]*tmp[7] + (s64)s[6]*tmp[11] + (s64)s[7]*tmp[15]) >> 12;

    m[8] = ((s64)s[8]*tmp[0] + (s64)s[9]*tmp[4] + (s64)s[10]*tmp[8] + (s64)s[11]*tmp[12]) >> 12;
    m[9] = ((s64)s[8]*tmp[1] + (s64)s[9]*tmp[5] + (s64)s[10]*tmp[9] + (s64)s[11]*tmp[13]) >> 12;
    m[10] = ((s64)s[8]*tmp[2] + (s64)s[9]*tmp[6] + (s64)s[10]*tmp[10] + (s64)s[11]*tmp[14]) >> 12;
    m[11] = ((s64)s[8]*tmp[3] + (s64)s[9]*tmp[7] + (s64)s[10]*tmp[11] + (s64)s[11]*tmp[15]) >> 12;

    m[12] = ((s64)s[12]*tmp[0] + (s64)s[13]*tmp[4] + (s64)s[14]*tmp[8] + (s64)s[15]*tmp[12]) >> 12;
    m[13] = ((s64)s[12]*tmp[1] + (s64)s[13]*tmp[5] + (s64)s[14]*tmp[9] + (s64)s[15]*tmp[13]) >> 12;
    m[14] = ((s64)s[12]*tmp[2] + (s64)s[13]*tmp[6] + (s64)s[14]*tmp[10] + (s64)s[15]*tmp[14]) >> 12;
    m[15] = ((s64)s[12]*tmp[3] + (s64)s[13]*tmp[7] + (s64)s[14]*tmp[11] + (s64)s[15]*tmp[15]) >> 12;
}

void MatrixMult4x3(s32* m, s32* s)
{
    s32 tmp[16];
    memcpy(tmp, m, 16*4);

    // m = s*m
    m[0] = ((s64)s[0]*tmp[0] + (s64)s[1]*tmp[4] + (s64)s[2]*tmp[8]) >> 12;
    m[1] = ((s64)s[0]*tmp[1] + (s64)s[1]*tmp[5] + (s64)s[2]*tmp[9]) >> 12;
    m[2] = ((s64)s[0]*tmp[2] + (s64)s[1]*tmp[6] + (s64)s[2]*tmp[10]) >> 12;
    m[3] = ((s64)s[0]*tmp[3] + (s64)s[1]*tmp[7] + (s64)s[2]*tmp[11]) >> 12;

    m[4] = ((s64)s[3]*tmp[0] + (s64)s[4]*tmp[4] + (s64)s[5]*tmp[8]) >> 12;
    m[5] = ((s64)s[3]*tmp[1] + (s64)s[4]*tmp[5] + (s64)s[5]*tmp[9]) >> 12;
    m[6] = ((s64)s[3]*tmp[2] + (s64)s[4]*tmp[6] + (s64)s[5]*tmp[10]) >> 12;
    m[7] = ((s64)s[3]*tmp[3] + (s64)s[4]*tmp[7] + (s64)s[5]*tmp[11]) >> 12;

    m[8] = ((s64)s[6]*tmp[0] + (s64)s[7]*tmp[4] + (s64)s[8]*tmp[8]) >> 12;
    m[9] = ((s64)s[6]*tmp[1] + (s64)s[7]*tmp[5] + (s64)s[8]*tmp[9]) >> 12;
    m[10] = ((s64)s[6]*tmp[2] + (s64)s[7]*tmp[6] + (s64)s[8]*tmp[10]) >> 12;
    m[11] = ((s64)s[6]*tmp[3] + (s64)s[7]*tmp[7] + (s64)s[8]*tmp[11]) >> 12;

    m[12] = ((s64)s[9]*tmp[0] + (s64)s[10]*tmp[4] + (s64)s[11]*tmp[8] + (s64)0x1000*tmp[12]) >> 12;
    m[13] = ((s64)s[9]*tmp[1] + (s64)s[10]*tmp[5] + (s64)s[11]*tmp[9] + (s64)0x1000*tmp[13]) >> 12;
    m[14] = ((s64)s[9]*tmp[2] + (s64)s[10]*tmp[6] + (s64)s[11]*tmp[10] + (s64)0x1000*tmp[14]) >> 12;
    m[15] = ((s64)s[9]*tmp[3] + (s64)s[10]*tmp[7] + (s64)s[11]*tmp[11] + (s64)0x1000*tmp[15]) >> 12;
}

void MatrixMult3x3(s32* m, s32* s)
{
    s32 tmp[12];
    memcpy(tmp, m, 12*4);

    // m = s*m
    m[0] = ((s64)s[0]*tmp[0] + (s64)s[1]*tmp[4] + (s64)s[2]*tmp[8]) >> 12;
    m[1] = ((s64)s[0]*tmp[1] + (s64)s[1]*tmp[5] + (s64)s[2]*tmp[9]) >> 12;
    m[2] = ((s64)s[0]*tmp[2] + (s64)s[1]*tmp[6] + (s64)s[2]*tmp[10]) >> 12;
    m[3] = ((s64)s[0]*tmp[3] + (s64)s[1]*tmp[7] + (s64)s[2]*tmp[11]) >> 12;

    m[4] = ((s64)s[3]*tmp[0] + (s64)s[4]*tmp[4] + (s64)s[5]*tmp[8]) >> 12;
    m[5] = ((s64)s[3]*tmp[1] + (s64)s[4]*tmp[5] + (s64)s[5]*tmp[9]) >> 12;
    m[6] = ((s64)s[3]*tmp[2] + (s64)s[4]*tmp[6] + (s64)s[5]*tmp[10]) >> 12;
    m[7] = ((s64)s[3]*tmp[3] + (s64)s[4]*tmp[7] + (s64)s[5]*tmp[11]) >> 12;

    m[8] = ((s64)s[6]*tmp[0] + (s64)s[7]*tmp[4] + (s64)s[8]*tmp[8]) >> 12;
    m[9] = ((s64)s[6]*tmp[1] + (s64)s[7]*tmp[5] + (s64)s[8]*tmp[9]) >> 12;
    m[10] = ((s64)s[6]*tmp[2] + (s64)s[7]*tmp[6] + (s64)s[8]*tmp[10]) >> 12;
    m[11] = ((s64)s[6]*tmp[3] + (s64)s[7]*tmp[7] + (s64)s[8]*tmp[11]) >> 12;
}

void MatrixScale(s32* m, s32* s)
{
    m[0] = ((s64)s[0]*m[0]) >> 12;
    m[1] = ((s64)s[0]*m[1]) >> 12;
    m[2] = ((s64)s[0]*m[2]) >> 12;
    m[3] = ((s64)s[0]*m[3]) >> 12;

    m[4] = ((s64)s[1]*m[4]) >> 12;
    m[5] = ((s64)s[1]*m[5]) >> 12;
    m[6] = ((s64)s[1]*m[6]) >> 12;
    m[7] = ((s64)s[1]*m[7]) >> 12;

    m[8] = ((s64)s[2]*m[8]) >> 12;
    m[9] = ((s64)s[2]*m[9]) >> 12;
    m[10] = ((s64)s[2]*m[10]) >> 12;
    m[11] = ((s64)s[2]*m[11]) >> 12;
}

void MatrixTranslate(s32* m, s32* s)
{
    m[12] += ((s64)s[0]*m[0] + (s64)s[1]*m[4] + (s64)s[2]*m[8]) >> 12;
    m[13] += ((s64)s[0]*m[1] + (s64)s[1]*m[5] + (s64)s[2]*m[9]) >> 12;
    m[14] += ((s64)s[0]*m[2] + (s64)s[1]*m[6] + (s64)s[2]*m[10]) >> 12;
    m[15] += ((s64)s[0]*m[3] + (s64)s[1]*m[7] + (s64)s[2]*m[11]) >> 12;
}

void GPU3D::UpdateClipMatrix() noexcept
{
    if (!ClipMatrixDirty) return;
    ClipMatrixDirty = false;
    // the projection may be another one now, which the next polygon looks up
    SceneCameraDirty = true;

    memcpy(ClipMatrix, ProjMatrix, 16*4);
    MatrixMult4x4(ClipMatrix, PosMatrix);
}



void GPU3D::AddCycles(s32 num) noexcept
{
    CycleCount += num;

    if (VertexPipeline > 0)
    {
        if (VertexPipeline > num) VertexPipeline -= num;
        else                      VertexPipeline = 0;
    }

    if (PolygonPipeline > 0)
    {
        if (PolygonPipeline > num)
        {
            PolygonPipeline -= num;
            VertexSlotCounter += num;
            while (VertexSlotCounter > 9)
            {
                VertexSlotCounter -= 9;
                VertexSlotsFree >>= 1;
            }
        }
        else
        {
            PolygonPipeline = 0;
            VertexSlotCounter = 0;
            VertexSlotsFree = 0x1;
        }
    }
}

void GPU3D::NextVertexSlot() noexcept
{
    s32 num = (9 - VertexSlotCounter) + 1;

    for (;;)
    {
        CycleCount += num;

        if (VertexPipeline > 0)
        {
            if (VertexPipeline > num) VertexPipeline -= num;
            else                      VertexPipeline = 0;
        }

        if (PolygonPipeline > 0)
        {
            if (PolygonPipeline > num)
            {
                PolygonPipeline -= num;
                VertexSlotCounter = 1;
                VertexSlotsFree >>= 1;
                if (VertexSlotsFree & 0x1)
                {
                    VertexSlotsFree &= ~0x1;
                    break;
                }
                else
                {
                    num = 9;
                    continue;
                }
            }
            else
            {
                PolygonPipeline = 0;
                VertexSlotCounter = 0;
                VertexSlotsFree = 1;
                break;
            }
        }
    }
}

void GPU3D::StallPolygonPipeline(s32 delay, s32 nonstalldelay) noexcept
{
    if (PolygonPipeline > 0)
    {
        CycleCount += PolygonPipeline + delay;

        // can be safely assumed those two will go to zero
        VertexPipeline = 0;
        NormalPipeline = 0;

        PolygonPipeline = 0;
        VertexSlotCounter = 0;
        VertexSlotsFree = 1;
    }
    else
    {
        if (VertexPipeline > nonstalldelay)
            AddCycles((VertexPipeline - nonstalldelay) + 1);
        else
            AddCycles(NormalPipeline + 1);
    }
}



template<int comp, s32 plane, bool attribs, typename V>
void ClipSegment(V* outbuf, V* vin, V* vout)
{
    s64 factor_num = vin->Position[3] - (plane*vin->Position[comp]);
    s32 factor_den = factor_num - (vout->Position[3] - (plane*vout->Position[comp]));

#define INTERPOLATE(var)  { outbuf->var = (vin->var + ((vout->var - vin->var) * factor_num) / factor_den); }

    if (comp != 0) INTERPOLATE(Position[0]);
    if (comp != 1) INTERPOLATE(Position[1]);
    if (comp != 2) INTERPOLATE(Position[2]);
    INTERPOLATE(Position[3]);
    outbuf->Position[comp] = plane*outbuf->Position[3];

    if constexpr (attribs)
    {
        INTERPOLATE(Color[0]);
        INTERPOLATE(Color[1]);
        INTERPOLATE(Color[2]);

        INTERPOLATE(TexCoords[0]);
        INTERPOLATE(TexCoords[1]);
    }

    outbuf->Clipped = true;

#undef INTERPOLATE
}

template<int comp, bool attribs, typename V>
int ClipAgainstPlane(const GPU3D& gpu, V* vertices, int nverts, int clipstart)
{
    V temp[10];
    int prev, next;
    int c = clipstart;

    if (clipstart == 2)
    {
        temp[0] = vertices[0];
        temp[1] = vertices[1];
    }

    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        V vtx = vertices[i];
        if (vtx.Position[comp] > vtx.Position[3])
        {
            if ((comp == 2) && (!(gpu.CurPolygonAttr & (1<<12)))) return 0;

            V* vprev = &vertices[prev];
            if (vprev->Position[comp] <= vprev->Position[3])
            {
                ClipSegment<comp, 1, attribs>(&temp[c], &vtx, vprev);
                c++;
            }

            V* vnext = &vertices[next];
            if (vnext->Position[comp] <= vnext->Position[3])
            {
                ClipSegment<comp, 1, attribs>(&temp[c], &vtx, vnext);
                c++;
            }
        }
        else
            temp[c++] = vtx;
    }

    nverts = c; c = clipstart;
    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        V vtx = temp[i];
        if (vtx.Position[comp] < -vtx.Position[3])
        {
            V* vprev = &temp[prev];
            if (vprev->Position[comp] >= -vprev->Position[3])
            {
                ClipSegment<comp, -1, attribs>(&vertices[c], &vtx, vprev);
                c++;
            }

            V* vnext = &temp[next];
            if (vnext->Position[comp] >= -vnext->Position[3])
            {
                ClipSegment<comp, -1, attribs>(&vertices[c], &vtx, vnext);
                c++;
            }
        }
        else
            vertices[c++] = vtx;
    }

    // checkme
    if constexpr (std::is_same_v<V, Vertex>)
    {
        for (int i = 0; i < c; i++)
        {
            Vertex* vtx = &vertices[i];

            vtx->Color[0] &= ~0xFFF; vtx->Color[0] += 0xFFF;
            vtx->Color[1] &= ~0xFFF; vtx->Color[1] += 0xFFF;
            vtx->Color[2] &= ~0xFFF; vtx->Color[2] += 0xFFF;
        }
    }

    return c;
}

template<bool attribs, typename V>
int ClipPolygon(GPU3D& gpu, V* vertices, int nverts, int clipstart)
{
    // clip.
    // for each vertex:
    // if it's outside, check if the previous and next vertices are inside
    // if so, place a new vertex at the edge of the view volume

    // TODO: check for 1-dot polygons
    // TODO: the hardware seems to use a different algorithm. it reacts differently to vertices with W=0
    // some vertices that should get Y=-0x1000 get Y=0x1000 for some reason on hardware. it doesn't make sense.
    // clipping seems to process the Y plane before the X plane.

    // Z clipping
    nverts = ClipAgainstPlane<2, attribs>(gpu, vertices, nverts, clipstart);

    // Y clipping
    nverts = ClipAgainstPlane<1, attribs>(gpu, vertices, nverts, clipstart);

    // X clipping
    nverts = ClipAgainstPlane<0, attribs>(gpu, vertices, nverts, clipstart);

    return nverts;
}

bool ClipCoordsEqual(Vertex* a, Vertex* b)
{
    return a->Position[0] == b->Position[0] &&
           a->Position[1] == b->Position[1] &&
           a->Position[2] == b->Position[2] &&
           a->Position[3] == b->Position[3];
}

// the dot product SubmitPolygon culls by, from the first three vertices, with their X and Y given
// separately (from before WideMelon's squeeze)
static s64 CullingDot(const Vertex* v, const s32* x, const s32* y)
{
    // TODO: work out how it works on the real thing
    // the normalization part is a wild guess

    s64 normalX, normalY, normalZ;

    normalX = ((s64)(y[0]-y[1]) * (v[2].Position[3]-v[1].Position[3]))
        - ((s64)(v[0].Position[3]-v[1].Position[3]) * (y[2]-y[1]));
    normalY = ((s64)(v[0].Position[3]-v[1].Position[3]) * (x[2]-x[1]))
        - ((s64)(x[0]-x[1]) * (v[2].Position[3]-v[1].Position[3]));
    normalZ = ((s64)(x[0]-x[1]) * (y[2]-y[1]))
        - ((s64)(y[0]-y[1]) * (x[2]-x[1]));

    while ((((normalX>>31) ^ (normalX>>63)) != 0) ||
           (((normalY>>31) ^ (normalY>>63)) != 0) ||
           (((normalZ>>31) ^ (normalZ>>63)) != 0))
    {
        normalX >>= 4;
        normalY >>= 4;
        normalZ >>= 4;
    }

    return ((s64)x[1] * normalX) + ((s64)y[1] * normalY) + ((s64)v[1].Position[3] * normalZ);
}

void GPU3D::SubmitPolygon() noexcept
{
    Vertex clippedvertices[10];
    Vertex* reusedvertices[2];
    int clipstart = 0;
    int lastpolyverts = 0;

    int nverts = PolygonMode & 0x1 ? 4:3;
    int prev, next;

    // submitting a polygon starts the polygon pipeline
    // noting that for now we are only reserving one vertex slot
    // further slots only get reserved if the polygon makes it through culling/clipping
    PolygonPipeline = 8;
    VertexSlotCounter = 1;
    VertexSlotsFree = 0b11110;

    // the wide check: the wider view lets more polygons through clipping, and a kept polygon keeps
    // the pipeline busy longer (below, by its clipped vertex count), which the game sees as timing.
    // WideMelon times polygons by the native view for that (below), but by default the check times
    // every polygon as kept with the vertices it was sent with, culled or not, in both runs, so their
    // geometry engines stay in step whatever that gets wrong
    const bool equaltiming = WideCheck && WideCheckEqualTiming;
    if (equaltiming)
    {
        PolygonPipeline = (nverts == 4) ? 35 : 26;
        VertexSlotsFree = (nverts == 4) ? ((PolygonMode & 0x2) ? 0b11100 : 0b11110) : ((PolygonMode & 0x2) ? 0b1000 : 0b1110);
    }

    // WideMelon: the native view, what this polygon would do without the squeeze. it gets its polygon and
    // vertex RAM counts (NativeNumPolygons), for the drop counter, and it picks the polygon's timing, so
    // the game sees stock geometry timing. the polygon is the same but for the X before the squeeze
    // and Y (TempVertexNativeX and Y), and both cull alike (below)
    const bool wide = ViewWidth != WideMelon::NativeWidth;
    const int sentverts = nverts;
    // a kept polygon keeps the pipeline busy by its clipped vertex count
    auto startpipeline = [this](int n)
    {
        if (n == 4)
        {
            PolygonPipeline = 35;
            VertexSlotCounter = 1;
            if (PolygonMode & 0x2) VertexSlotsFree = 0b11100;
            else                   VertexSlotsFree = 0b11110;
        }
        else
        {
            PolygonPipeline = 26;
            VertexSlotCounter = 1;
            if (PolygonMode & 0x2) VertexSlotsFree = 0b1000;
            else                   VertexSlotsFree = 0b1110;
        }
    };
    // [n] if the native view has room for a polygon of [n] vertices, else 0
    auto fitsnative = [this](int n) { return (NativeNumPolygons < 2048 && NativeNumVertices + n <= 6144) ? n : 0; };
    // the wide list's wing budget has room for a polygon of [n] vertices
    auto fitswing = [this](int n) { return WingNumPolygons < NativePolygonRAMSize && WingNumVertices + n <= NativeVertexRAMSize; };
    // the native view's last strip polygon: this one's from here on, which stays none unless that view keeps it
    const u32 nativeprevverts = NativeStripVerts, nativeprevclipped = NativeStripClipped;
    NativeStripVerts = 0;
    u32 nativeclipped = 0;
    auto keepnative = [&](int n, bool reused)
    {
        NativeNumPolygons++;
        NativeNumVertices += reused ? n - 2 : n;
        if (PolygonMode >= 2)
        {
            NativeStripVerts = n;
            NativeStripClipped = nativeclipped;
        }
    };

    // culling
    // from the X and Y before WideMelon's squeeze, the same as the vertices' with it off. the squeeze
    // keeps which way a polygon faces but rounds, which flips the sign for some seen almost edge on, so
    // this way the wide view culls what the native one does

    s64 dot = CullingDot(TempVertexBuffer, TempVertexNativeX, TempVertexNativeY);

    bool facingview = (dot <= 0);

    if (dot < 0)
    {
        if (!(CurPolygonAttr & (1<<7)))
        {
            LastStripPolygon = NULL;
            return;
        }
    }
    else if (dot > 0)
    {
        if (!(CurPolygonAttr & (1<<6)))
        {
            LastStripPolygon = NULL;
            return;
        }
    }

    // for strips, check whether we can attach to the previous polygon
    // this requires two original vertices shared with the previous polygon, and that
    // the two polygons be of the same type

    // (the native view decides that from its own last strip polygon)
    bool nativestrip = false;
    if (PolygonMode >= 2)
    {
        int id0, id1;
        if (PolygonMode == 2)
        {
            if (NumConsecutivePolygons & 1)
            {
                id0 = 2;
                id1 = 1;
            }
            else
            {
                id0 = 0;
                id1 = 2;
            }

            lastpolyverts = 3;
        }
        else
        {
            id0 = 3;
            id1 = 2;

            lastpolyverts = 4;
        }

        nativestrip = nativeprevverts == (u32)lastpolyverts && !(nativeprevclipped & ((1 << id0) | (1 << id1)));

        if (LastStripPolygon && LastStripPolygon->NumVertices == lastpolyverts &&
            !LastStripPolygon->Vertices[id0]->Clipped &&
            !LastStripPolygon->Vertices[id1]->Clipped)
        {
            reusedvertices[0] = LastStripPolygon->Vertices[id0];
            reusedvertices[1] = LastStripPolygon->Vertices[id1];

            clippedvertices[0] = *reusedvertices[0];
            clippedvertices[1] = *reusedvertices[1];

            clipstart = 2;
        }
    }

    for (int i = clipstart; i < nverts; i++)
        clippedvertices[i] = TempVertexBuffer[i];

    // detect lines, for the OpenGL renderer

    int polytype = 0;
    if (nverts == 3)
    {
        if (ClipCoordsEqual(&clippedvertices[0], &clippedvertices[1]) ||
            ClipCoordsEqual(&clippedvertices[0], &clippedvertices[2]) ||
            ClipCoordsEqual(&clippedvertices[1], &clippedvertices[2]))
        {
            polytype = 1;
        }
    }
    else if (nverts == 4)
    {
        // TODO
    }

    // clipping

    nverts = ClipPolygon<true>(*this, clippedvertices, nverts, clipstart);
    if (nverts == 0)
    {
        LastStripPolygon = NULL;
        return;
    }

    // the native view: how many vertices it would store this polygon with (nativeclip, 0 if it clips it
    // away), and 0 if it wouldn't, as its counts are full too (nativeverts). one the wide view clips away is
    // gone there too. zero-dot polygons are checked again below
    // (with the wide list kept at 256 after a renderer switch or a load, that's the view itself)
    int nativeclip = (wide || WideListOn) ? NativeClipVertices(sentverts, nverts, clippedvertices, nativeclipped) : 0;
    int nativeverts = fitsnative(nativeclip);
    bool nativereused = nativestrip && nativeverts == lastpolyverts;

    // reject the polygon if it's not going to fit in polygon/vertex RAM

    if (WideListOn)
    {
        // WideMelon's wide list: a polygon the native view keeps is dropped where the DS drops it, when the
        // native counts are full, and then from the wide view too, so the middle 256 never shows one the DS
        // didn't draw. only that sets bit 13. one only the wide view keeps takes the wing budget, and only
        // while the native counts could still take a polygon its size, so the wings stop at the same point
        // of the scene as the middle. a native one the whole list has no room for (only once the native
        // polygons' wide vertex counts pass VertexRAMSize) is the wide view's cost, nativefit
        bool listfull = NumPolygons >= PolygonRAMSize || NumVertices+nverts > VertexRAMSize;
        const bool nativedrop = nativeclip && !nativeverts;
        bool drop = true;
        if (nativedrop)
            DispCnt |= (1<<13);
        else if (!nativeclip)
        {
            drop = listfull || !fitswing(nverts) || !fitsnative(nverts);
            DroppedWing += drop;
        }
        else if (listfull)
        {
            // the native view keeps it unless it's zero-dot there (as below)
            if (!(CurPolygonAttr & (1<<13)) && ZeroDotWLimit < 0xFFFFFF && NativeZeroDot(sentverts))
                nativeverts = 0;
            if (nativeverts)
            {
                DroppedNativeFit++;
                keepnative(nativeverts, nativereused);
                if (!equaltiming) startpipeline(nativeverts);
            }
            else
                DroppedWing++;
        }
        else
            drop = false;

        if (drop)
        {
            DroppedPolygons++;
            WideListOverflow = WideListOverflow || !nativedrop;
            if (VertexIdLog) VertexIdLog->OnPolygonRejected();
            LastStripPolygon = NULL;
            return;
        }
    }
    else if (NumPolygons >= 2048 || NumVertices+nverts > 6144)
    {
        DroppedPolygons++;
        // the native view has room: its zero-dot check is next (as below)
        if (nativeverts && !(CurPolygonAttr & (1<<13)) && ZeroDotWLimit < 0xFFFFFF && NativeZeroDot(sentverts))
            nativeverts = 0;
        if (nativeverts)
        {
            DroppedNativeFit++;
            keepnative(nativeverts, nativereused);
            if (!equaltiming) startpipeline(nativeverts);
        }
        // for the WIDEDROPS line: one only the wide view keeps
        else if (wide && !nativeclip)
            DroppedWing++;
        if (VertexIdLog) VertexIdLog->OnPolygonRejected();
        LastStripPolygon = NULL;
        DispCnt |= (1<<13);
        return;
    }

    // compute screen coordinates

    for (int i = clipstart; i < nverts; i++)
    {
        Vertex* vtx = &clippedvertices[i];

        // W is truncated to 24 bits at this point
        // if this W is zero, the polygon isn't rendered
        vtx->Position[3] &= 0x00FFFFFF;

        // viewport transform
        // note: the DS performs these divisions using a 32-bit divider
        // thus, if W is greater than 0xFFFF, some precision is sacrificed
        // to make the numbers fit into the divider
        u32 posX, posY;
        u32 w = vtx->Position[3];
        if (w == 0)
        {
            posX = 0;
            posY = 0;
        }
        else
        {
            posX = vtx->Position[0] + w;
            posY = -vtx->Position[1] + w;
            u32 den = w;

            if (w > 0xFFFF)
            {
                posX >>= 1;
                posY >>= 1;
                den  >>= 1;
            }

            den <<= 1;
            posX = ((posX * Viewport[4]) / den) + Viewport[0];
            posY = ((posY * Viewport[5]) / den) + Viewport[3];
        }

        vtx->FinalPosition[0] = posX & 0x1FF;
        vtx->FinalPosition[1] = posY & 0xFF;

        // hi-res positions
        // to consider: only do this when using the GL renderer? apply the aforementioned quirk to this?
        if (w != 0)
        {
            posX = ((((s64)(vtx->Position[0] + w) * Viewport[4]) << 4) / (((s64)w) << 1)) + (Viewport[0] << 4);
            posY = ((((s64)(-vtx->Position[1] + w) * Viewport[5]) << 4) / (((s64)w) << 1)) + (Viewport[3] << 4);

            vtx->HiresPosition[0] = posX & 0x1FFF;
            vtx->HiresPosition[1] = posY & 0xFFF;

            if (ViewWidth != WideMelon::NativeWidth)
            {
                // X was squeezed by 256/W around clip X = 0, which is the viewport's centre, so it's
                // stretched back by W/256 around that same point, on a target (W-256)/2 wider each side
                s64 centre = ((s64)Viewport[0] << 4) + ((s64)Viewport[4] << 3) + ((ViewWidth - WideMelon::NativeWidth) << 3);
                s64 num = (s64)vtx->Position[0] * Viewport[4] * ViewWidth;
                s64 den = (s64)w << 5;
                s64 wideX = centre + (num >= 0 ? num / den : -((den - 1 - num) / den));

                // the native formula can't go negative, but a wrapped viewport (x1 < x0) can here
                vtx->HiresPosition[0] = std::clamp<s64>(wideX, 0, 0x3FFF);
            }
            if (ViewMargin != 0)
            {
                // the same for Y (camera stage 2): squeezed by 192/H around clip Y = 0, the viewport's
                // centre, so stretched back by H/192 around it, on a target ViewMargin rows taller
                // each side. clip Y points up and the rows down, as in the native formula
                s64 centre = ((s64)Viewport[3] << 4) + ((s64)Viewport[5] << 3) + ((s64)ViewMargin << 4);
                s64 num = -(s64)vtx->Position[1] * Viewport[5] * ViewHeight();
                s64 den = (s64)w * 24;
                s64 wideY = centre + (num >= 0 ? num / den : -((den - 1 - num) / den));

                vtx->HiresPosition[1] = std::clamp<s64>(wideY, 0, 0x3FFF);
            }
        }
    }

    // zero-dot W check:
    // * if the polygon's vertices all have the same screen coordinates, it is considered to be zero-dot
    // * if all the vertices have a W greater than the threshold defined in register 0x04000610,
    //   the polygon is rejected, unless bit13 in the polygon attributes is set

    if (!(CurPolygonAttr & (1<<13)))
    {
        bool zerodot = true;
        bool allbehind = true;

        for (int i = 0; i < nverts; i++)
        {
            Vertex* vtx = &clippedvertices[i];

            if (vtx->FinalPosition[0] != clippedvertices[0].FinalPosition[0] ||
                vtx->FinalPosition[1] != clippedvertices[0].FinalPosition[1])
            {
                zerodot = false;
                break;
            }

            if (vtx->Position[3] <= ZeroDotWLimit)
            {
                allbehind = false;
                break;
            }
        }

        // the native view. nothing is zero-dot while ZeroDotWLimit is at its reset value, as no W gets past
        // it. with the same clip in both, as the squeeze narrows by 256/W, only a polygon at most a pixel
        // wide on one row here, with every W past the limit, can be zero-dot in one view and not the other,
        // so only that one is worked out again. one the native X planes cut always is (it's clipped already)
        if (nativeverts && ZeroDotWLimit < 0xFFFFFF)
        {
            bool check = true;
            if (NativeClipSame)
            {
                s32 x0 = clippedvertices[0].FinalPosition[0], x1 = x0, y = clippedvertices[0].FinalPosition[1];
                for (int i = 1; i < nverts && check; i++)
                {
                    s32 x = clippedvertices[i].FinalPosition[0];
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    check = clippedvertices[i].FinalPosition[1] == y && x1 - x0 <= 1;
                }
                for (int i = 0; i < nverts && check; i++)
                    check = clippedvertices[i].Position[3] > ZeroDotWLimit;
            }
            if (check && NativeZeroDot(sentverts))
                nativeverts = 0;
        }

        if (zerodot && allbehind)
        {
            // one the native view keeps
            if (nativeverts)
            {
                keepnative(nativeverts, nativereused);
                if (!equaltiming) startpipeline(nativeverts);
            }
            LastStripPolygon = NULL;
            return;
        }
    }

    // the wide list: one the native view keeps but drops as zero-dot (above) is the wings' as well
    const bool wingpoly = WideListOn && !nativeverts;
    if (wingpoly && nativeclip && !fitswing(nverts))
    {
        DroppedPolygons++;
        DroppedWing++;
        WideListOverflow = true;
        if (VertexIdLog) VertexIdLog->OnPolygonRejected();
        LastStripPolygon = NULL;
        return;
    }

    // build the actual polygon

    // the timing: with WideMelon on, from the native view, which leaves a polygon it wouldn't keep at
    // the 8 cycles of a rejected one (from the top). the wide check's equal timing is the top's again
    if (equaltiming) startpipeline(sentverts);
    else if (wide) { if (nativeverts) startpipeline(nativeverts); }
    else startpipeline(nverts);

    if (nativeverts) keepnative(nativeverts, nativereused);

    const u32 vertsbefore = NumVertices;
    Polygon* poly = &CurPolygonRAM[NumPolygons++];
    if (VertexIdLog) VertexIdLog->OnPolygon(NumPolygons - 1, *this);
    if (SceneCameraDirty) NoteSceneCamera();
    poly->Camera = CurSceneCamera;
    if (CurSceneCamera != NoSceneCamera) SceneCameras[CurSceneCamera].Polygons++;
    else                                 OtherCameraPolygons++;
    poly->NumVertices = 0;

    poly->Attr = CurPolygonAttr;
    poly->TexParam = TexParam;
    poly->TexPalette = TexPalette;

    poly->Degenerate = false;
    poly->Type = 0;

    poly->FacingView = facingview;

    u32 texfmt = (TexParam >> 26) & 0x7;
    u32 polyalpha = (CurPolygonAttr >> 16) & 0x1F;
    poly->Translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);

    poly->IsShadowMask = ((CurPolygonAttr & 0x3F000030) == 0x00000030);
    poly->IsShadow = ((CurPolygonAttr & 0x30) == 0x30) && !poly->IsShadowMask;

    if (!poly->Translucent) NumOpaquePolygons++;

    poly->Type = polytype;

    if (LastStripPolygon && clipstart > 0)
    {
        if (nverts == lastpolyverts)
        {
            poly->Vertices[0] = reusedvertices[0];
            poly->Vertices[1] = reusedvertices[1];
        }
        else
        {
            Vertex v0 = *reusedvertices[0];
            Vertex v1 = *reusedvertices[1];

            CurVertexRAM[NumVertices] = v0;
            poly->Vertices[0] = &CurVertexRAM[NumVertices];
            CurVertexRAM[NumVertices+1] = v1;
            poly->Vertices[1] = &CurVertexRAM[NumVertices+1];
            NumVertices += 2;
        }

        poly->NumVertices += 2;
    }

    for (int i = clipstart; i < nverts; i++)
    {
        Vertex* vtx = &CurVertexRAM[NumVertices];
        *vtx = clippedvertices[i];
        poly->Vertices[i] = vtx;

        NumVertices++;
        poly->NumVertices++;

        vtx->FinalColor[0] = vtx->Color[0] >> 12;
        if (vtx->FinalColor[0]) vtx->FinalColor[0] = ((vtx->FinalColor[0] << 4) + 0xF);
        vtx->FinalColor[1] = vtx->Color[1] >> 12;
        if (vtx->FinalColor[1]) vtx->FinalColor[1] = ((vtx->FinalColor[1] << 4) + 0xF);
        vtx->FinalColor[2] = vtx->Color[2] >> 12;
        if (vtx->FinalColor[2]) vtx->FinalColor[2] = ((vtx->FinalColor[2] << 4) + 0xF);
    }

    if (wingpoly)
    {
        WingNumPolygons++;
        WingNumVertices += NumVertices - vertsbefore;
    }

    // determine bounds of the polygon
    // also determine the W shift and normalize W
    // normalization works both ways
    // (ie two W's that span 12 bits or less will be brought to 16 bits)

    u32 vtop = 0, vbot = 0;
    s32 ytop = 192, ybot = 0;
    s32 xtop = 256, xbot = 0;
    u32 wsize = 0;

    for (int i = 0; i < nverts; i++)
    {
        Vertex* vtx = poly->Vertices[i];

        if (vtx->FinalPosition[1] < ytop)
        {
            xtop = vtx->FinalPosition[0];
            ytop = vtx->FinalPosition[1];
            vtop = i;
        }
        if (vtx->FinalPosition[1] > ybot || (vtx->FinalPosition[1] == ybot && vtx->FinalPosition[0] > xbot))
        {
            xbot = vtx->FinalPosition[0];
            ybot = vtx->FinalPosition[1];
            vbot = i;
        }

        u32 w = (u32)vtx->Position[3];
        if (w == 0) poly->Degenerate = true;

        while ((w >> wsize) && (wsize < 32))
            wsize += 4;
    }

    poly->VTop = vtop; poly->VBottom = vbot;
    poly->YTop = ytop; poly->YBottom = ybot;
    poly->XTop = xtop; poly->XBottom = xbot;

    if (ybot > 192) poly->Degenerate = true;

    poly->SortKey = (ybot << 8) | ytop;
    if (poly->Translucent) poly->SortKey |= 0x10000;

    poly->WBuffer = (FlushAttributes & 0x2);
    poly->PreciseDepth = PreciseDepthOn && !poly->WBuffer;

    for (int i = 0; i < nverts; i++)
    {
        Vertex* vtx = poly->Vertices[i];
        s32 w, wshifted;

        // W is normalized, such that all the polygon's W values fit within 16 bits
        // the viewport transform for X/Y/Z uses the original W values, but
        // when W-buffering is used, the normalized W is used
        // W normalization is applied to separate polygons, even within strips

        if (wsize < 16)
        {
            w = vtx->Position[3] << (16 - wsize);
            wshifted = w >> (16 - wsize);
        }
        else
        {
            w = vtx->Position[3] >> (wsize - 16);
            wshifted = w << (wsize - 16);
        }

        s32 z;
        if (FlushAttributes & 0x2)
            z = wshifted;
        else if (vtx->Position[3] && poly->PreciseDepth)
        {
            // GPU::PreciseDepth: the same depth without truncating it to 0x200 steps first
            s64 pz = (((s64)vtx->Position[2] * 0x800000) / vtx->Position[3]) + 0x7FFE00;
            z = (s32)std::clamp<s64>(pz, 0, 0xFFFFFF);
        }
        else if (vtx->Position[3])
            z = ((((s64)vtx->Position[2] * 0x4000) / vtx->Position[3]) + 0x3FFF) * 0x200;
        else
            z = 0x7FFE00;

        // checkme (Z<0 shouldn't be possible, but Z>0xFFFFFF is possible)
        if (z < 0) z = 0;
        else if (z > 0xFFFFFF) z = 0xFFFFFF;

        poly->FinalZ[i] = z;
        poly->FinalW[i] = w;
    }

    if (PolygonMode >= 2)
        LastStripPolygon = poly;
    else
        LastStripPolygon = NULL;
}

// WideMelon: the vertex count the polygon in TempVertexBuffer would have after clipping with the native
// view, 0 if it would be clipped away, given that the squeezed view keeps it with [wideverts]. the
// squeeze only scales X and Y (Y with a margin) by positive factors, so the Z planes come out the same,
// and only the X and Y planes can differ: a polygon inside them natively clips the same way in both
// views, one wholly past one of them is gone, and only one across them is clipped again, positions only
// [clippedmask] gets which of its vertices clipping made, a bit each. [wideclipped] is the squeezed
// view's clipped polygon
int GPU3D::NativeClipVertices(int sentverts, int wideverts, const Vertex* wideclipped, u32& clippedmask) noexcept
{
    clippedmask = 0;
    NativeClipSame = false;
    int inside = 0, left = 0, right = 0, below = 0, above = 0;
    for (int i = 0; i < sentverts; i++)
    {
        s32 x = TempVertexNativeX[i], y = TempVertexNativeY[i], w = TempVertexBuffer[i].Position[3];
        bool in = true;
        if (x > w) { right++; in = false; }
        else if (x < -w) { left++; in = false; }
        // without a margin Y isn't squeezed, so its planes clip alike in both views (as before the margin)
        if (ViewMargin != 0)
        {
            if (y > w) { above++; in = false; }
            else if (y < -w) { below++; in = false; }
        }
        inside += in;
    }
    if (inside == sentverts)
    {
        NativeClipSame = true;
        for (int i = 0; i < wideverts; i++)
            clippedmask |= wideclipped[i].Clipped << i;
        return wideverts;
    }
    if (left == sentverts || right == sentverts || below == sentverts || above == sentverts) return 0;

    ClipPosition* native = NativeClipped;
    for (int i = 0; i < sentverts; i++)
    {
        native[i] = {{TempVertexNativeX[i], TempVertexNativeY[i], TempVertexBuffer[i].Position[2], TempVertexBuffer[i].Position[3]}, false};
    }
    int n = NativeClippedCount = ClipPolygon<false>(*this, native, sentverts, 0);
    for (int i = 0; i < n; i++)
        clippedmask |= native[i].Clipped << i;
    return n;
}

// WideMelon: whether SubmitPolygon's zero-dot check would reject the polygon in TempVertexBuffer in
// the native view: clipped there, it has all its vertices on the same pixel and behind ZeroDotWLimit.
// after NativeClipVertices, whose clip it reuses when that had one of its own
bool GPU3D::NativeZeroDot(int sentverts) noexcept
{
    ClipPosition* native = NativeClipped;
    int n = NativeClippedCount;
    if (NativeClipSame)
    {
        for (int i = 0; i < sentverts; i++)
        {
            native[i] = {{TempVertexNativeX[i], TempVertexNativeY[i], TempVertexBuffer[i].Position[2], TempVertexBuffer[i].Position[3]}, false};
        }
        n = ClipPolygon<false>(*this, native, sentverts, 0);
    }

    // the viewport transform as SubmitPolygon does it
    u32 firstX = 0, firstY = 0;
    for (int i = 0; i < n; i++)
    {
        u32 w = native[i].Position[3] & 0x00FFFFFF;
        if (w <= ZeroDotWLimit)
            return false;

        u32 posX = native[i].Position[0] + w;
        u32 posY = -native[i].Position[1] + w;
        u32 den = w;
        if (w > 0xFFFF)
        {
            posX >>= 1;
            posY >>= 1;
            den  >>= 1;
        }
        den <<= 1;
        posX = (((posX * Viewport[4]) / den) + Viewport[0]) & 0x1FF;
        posY = (((posY * Viewport[5]) / den) + Viewport[3]) & 0xFF;

        if (i == 0) { firstX = posX; firstY = posY; }
        else if (posX != firstX || posY != firstY) return false;
    }
    return n > 0;
}

void GPU3D::SubmitVertex() noexcept
{
    if (VertexIdLog) VertexIdLog->OnVertex(*this);

    s64 vertex[4] = {(s64)CurVertex[0], (s64)CurVertex[1], (s64)CurVertex[2], 0x1000};
    Vertex* vertextrans = &TempVertexBuffer[VertexNumInPoly];

    UpdateClipMatrix();
    vertextrans->Position[0] = (vertex[0]*ClipMatrix[0] + vertex[1]*ClipMatrix[4] + vertex[2]*ClipMatrix[8] + vertex[3]*ClipMatrix[12]) >> 12;
    vertextrans->Position[1] = (vertex[0]*ClipMatrix[1] + vertex[1]*ClipMatrix[5] + vertex[2]*ClipMatrix[9] + vertex[3]*ClipMatrix[13]) >> 12;
    vertextrans->Position[2] = (vertex[0]*ClipMatrix[2] + vertex[1]*ClipMatrix[6] + vertex[2]*ClipMatrix[10] + vertex[3]*ClipMatrix[14]) >> 12;
    vertextrans->Position[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;

    TempVertexNativeX[VertexNumInPoly] = vertextrans->Position[0];
    TempVertexNativeY[VertexNumInPoly] = vertextrans->Position[1];
    if (ViewWidth != WideMelon::NativeWidth)
        vertextrans->Position[0] = WideMelon::ProjectX(vertextrans->Position[0], ViewWidth);
    if (ViewMargin != 0)
        vertextrans->Position[1] = WideMelon::ProjectY(vertextrans->Position[1], ViewHeight());

    // this probably shouldn't be.
    // the way color is handled during clipping needs investigation. TODO
    vertextrans->Color[0] = (VertexColor[0] << 12) + 0xFFF;
    vertextrans->Color[1] = (VertexColor[1] << 12) + 0xFFF;
    vertextrans->Color[2] = (VertexColor[2] << 12) + 0xFFF;

    if ((TexParam >> 30) == 3)
    {
        vertextrans->TexCoords[0] = ((vertex[0]*TexMatrix[0] + vertex[1]*TexMatrix[4] + vertex[2]*TexMatrix[8]) >> 24) + RawTexCoords[0];
        vertextrans->TexCoords[1] = ((vertex[0]*TexMatrix[1] + vertex[1]*TexMatrix[5] + vertex[2]*TexMatrix[9]) >> 24) + RawTexCoords[1];
    }
    else
    {
        vertextrans->TexCoords[0] = TexCoords[0];
        vertextrans->TexCoords[1] = TexCoords[1];
    }

    vertextrans->Clipped = false;

    VertexNum++;
    VertexNumInPoly++;

    switch (PolygonMode)
    {
    case 0: // triangle
        if (VertexNumInPoly == 3)
        {
            VertexNumInPoly = 0;
            SubmitPolygon();
            NumConsecutivePolygons++;
        }
        break;

    case 1: // quad
        if (VertexNumInPoly == 4)
        {
            VertexNumInPoly = 0;
            SubmitPolygon();
            NumConsecutivePolygons++;
        }
        break;

    case 2: // triangle strip
        if (NumConsecutivePolygons & 1)
        {
            Vertex tmp = TempVertexBuffer[1];
            TempVertexBuffer[1] = TempVertexBuffer[0];
            TempVertexBuffer[0] = tmp;
            std::swap(TempVertexNativeX[0], TempVertexNativeX[1]);
            std::swap(TempVertexNativeY[0], TempVertexNativeY[1]);

            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[1] = TempVertexBuffer[2];
            TempVertexNativeX[1] = TempVertexNativeX[2];
            TempVertexNativeY[1] = TempVertexNativeY[2];
        }
        else if (VertexNumInPoly == 3)
        {
            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[0] = TempVertexBuffer[1];
            TempVertexBuffer[1] = TempVertexBuffer[2];
            TempVertexNativeX[0] = TempVertexNativeX[1];
            TempVertexNativeX[1] = TempVertexNativeX[2];
            TempVertexNativeY[0] = TempVertexNativeY[1];
            TempVertexNativeY[1] = TempVertexNativeY[2];
        }
        break;

    case 3: // quad strip
        if (VertexNumInPoly == 4)
        {
            Vertex tmp = TempVertexBuffer[3];
            TempVertexBuffer[3] = TempVertexBuffer[2];
            TempVertexBuffer[2] = tmp;
            std::swap(TempVertexNativeX[2], TempVertexNativeX[3]);
            std::swap(TempVertexNativeY[2], TempVertexNativeY[3]);

            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[0] = TempVertexBuffer[3];
            TempVertexBuffer[1] = TempVertexBuffer[2];
            TempVertexNativeX[0] = TempVertexNativeX[3];
            TempVertexNativeX[1] = TempVertexNativeX[2];
            TempVertexNativeY[0] = TempVertexNativeY[3];
            TempVertexNativeY[1] = TempVertexNativeY[2];
        }
        break;
    }

    VertexPipeline = 7;
    AddCycles(3);
}

void GPU3D::CalculateLighting() noexcept
{
    if ((TexParam >> 30) == 2)
    {
        TexCoords[0] = RawTexCoords[0] + (((s64)Normal[0]*TexMatrix[0] + (s64)Normal[1]*TexMatrix[4] + (s64)Normal[2]*TexMatrix[8]) >> 21);
        TexCoords[1] = RawTexCoords[1] + (((s64)Normal[0]*TexMatrix[1] + (s64)Normal[1]*TexMatrix[5] + (s64)Normal[2]*TexMatrix[9]) >> 21);
    }

    s32 normaltrans[3]; // should be 1 bit sign 10 bits frac
    normaltrans[0] = ((Normal[0]*VecMatrix[0] + Normal[1]*VecMatrix[4] + Normal[2]*VecMatrix[8]) << 9) >> 21;
    normaltrans[1] = ((Normal[0]*VecMatrix[1] + Normal[1]*VecMatrix[5] + Normal[2]*VecMatrix[9]) << 9) >> 21;
    normaltrans[2] = ((Normal[0]*VecMatrix[2] + Normal[1]*VecMatrix[6] + Normal[2]*VecMatrix[10]) << 9) >> 21;

    s32 c = 0;
    u32 vtxbuff[3] =
    {
        (u32)MatEmission[0] << 14,
        (u32)MatEmission[1] << 14,
        (u32)MatEmission[2] << 14
    };
    for (int i = 0; i < 4; i++)
    {
        if (!(CurPolygonAttr & (1<<i)))
            continue;

        // (credit to azusa for working out most of the details of the diff. algorithm, and essentially the entire spec. algorithm)
        
        // calculate dot product
        // bottom 9 bits are discarded after multiplying and before adding
        s32 dot = ((LightDirection[i][0]*normaltrans[0]) >> 9) +
                  ((LightDirection[i][1]*normaltrans[1]) >> 9) +
                  ((LightDirection[i][2]*normaltrans[2]) >> 9);

        s32 shinelevel;
        if (dot > 0) 
        {
            // -- diffuse lighting --
            
            // convert dot to signed 11 bit int
            // then we truncate the result of the multiplications to an unsigned 20 bits before adding to the vtx color
            s32 diffdot = (dot << 21) >> 21;
            vtxbuff[0] += (MatDiffuse[0] * LightColor[i][0] * diffdot) & 0xFFFFF;
            vtxbuff[1] += (MatDiffuse[1] * LightColor[i][1] * diffdot) & 0xFFFFF;
            vtxbuff[2] += (MatDiffuse[2] * LightColor[i][2] * diffdot) & 0xFFFFF;

            // -- specular lighting --
        
            // reuse the dot product from diffuse lighting
            dot += normaltrans[2];

            // convert to s11, then square it, and truncate to 10 bits
            dot = (dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;

            // multiply dot and reciprocal, the subtract '1'
            shinelevel = ((dot * SpecRecip[i]) >> 8) - (1<<9);

            if (shinelevel < 0) shinelevel = 0;
            else
            {
                // sign extend to convert to signed 14 bit integer
                shinelevel = (shinelevel << 18) >> 18;
                if (shinelevel < 0) shinelevel = 0; // for some reason there seems to be a redundant check for <0?
                else if (shinelevel > 0x1FF) shinelevel = 0x1FF;
            }
        }
        else shinelevel = 0;

        // convert shinelevel to use for lookup in the shininess table if enabled.
        if (UseShininessTable)
        {
            shinelevel >>= 2;
            shinelevel = ShininessTable[shinelevel];
            shinelevel <<= 1;
        }

        // Note: ambient seems to be a plain bitshift
        vtxbuff[0] += ((MatSpecular[0] * shinelevel) + (MatAmbient[0] << 9)) * LightColor[i][0];
        vtxbuff[1] += ((MatSpecular[1] * shinelevel) + (MatAmbient[1] << 9)) * LightColor[i][1];
        vtxbuff[2] += ((MatSpecular[2] * shinelevel) + (MatAmbient[2] << 9)) * LightColor[i][2];

        c++;
    }

    VertexColor[0] = (vtxbuff[0] >> 14 > 31) ? 31 : (vtxbuff[0] >> 14);
    VertexColor[1] = (vtxbuff[1] >> 14 > 31) ? 31 : (vtxbuff[1] >> 14);
    VertexColor[2] = (vtxbuff[2] >> 14 > 31) ? 31 : (vtxbuff[2] >> 14);

    if (c < 1) c = 1;
    NormalPipeline = 7;
    AddCycles(c);
}


// whether any face of the box (clip space corners, see BoxTest) is left after clipping
static bool BoxTestHits(GPU3D& gpu, const Vertex* cube)
{
    // front (-Z), back (+Z), left (-X), right (+X), bottom (-Y), top (+Y)
    static const u8 faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 3, 4, 5}, {1, 2, 7, 6}, {0, 1, 6, 5}, {2, 3, 4, 7}};
    Vertex face[10];
    for (const u8* f : faces)
    {
        for (int i = 0; i < 4; i++)
            face[i] = cube[f[i]];
        if (ClipPolygon<false>(gpu, face, 4, 0) > 0)
            return true;
    }
    return false;
}

void GPU3D::BoxTest(const u32* params) noexcept
{
    Vertex cube[8];

    AddCycles(254);
    GXStat &= ~(1<<1);

    s16 x0 = (s16)(params[0] & 0xFFFF);
    s16 y0 = ((s32)params[0]) >> 16;
    s16 z0 = (s16)(params[1] & 0xFFFF);
    s16 x1 = ((s32)params[1]) >> 16;
    s16 y1 = (s16)(params[2] & 0xFFFF);
    s16 z1 = ((s32)params[2]) >> 16;

    x1 += x0;
    y1 += y0;
    z1 += z0;

    cube[0].Position[0] = x0; cube[0].Position[1] = y0; cube[0].Position[2] = z0;
    cube[1].Position[0] = x1; cube[1].Position[1] = y0; cube[1].Position[2] = z0;
    cube[2].Position[0] = x1; cube[2].Position[1] = y1; cube[2].Position[2] = z0;
    cube[3].Position[0] = x0; cube[3].Position[1] = y1; cube[3].Position[2] = z0;
    cube[4].Position[0] = x0; cube[4].Position[1] = y1; cube[4].Position[2] = z1;
    cube[5].Position[0] = x0; cube[5].Position[1] = y0; cube[5].Position[2] = z1;
    cube[6].Position[0] = x1; cube[6].Position[1] = y0; cube[6].Position[2] = z1;
    cube[7].Position[0] = x1; cube[7].Position[1] = y1; cube[7].Position[2] = z1;

    UpdateClipMatrix();
    for (int i = 0; i < 8; i++)
    {
        s32 x = cube[i].Position[0];
        s32 y = cube[i].Position[1];
        s32 z = cube[i].Position[2];

        cube[i].Position[0] = ((s64)x*ClipMatrix[0] + (s64)y*ClipMatrix[4] + (s64)z*ClipMatrix[8] + (s64)0x1000*ClipMatrix[12]) >> 12;
        cube[i].Position[1] = ((s64)x*ClipMatrix[1] + (s64)y*ClipMatrix[5] + (s64)z*ClipMatrix[9] + (s64)0x1000*ClipMatrix[13]) >> 12;
        cube[i].Position[2] = ((s64)x*ClipMatrix[2] + (s64)y*ClipMatrix[6] + (s64)z*ClipMatrix[10] + (s64)0x1000*ClipMatrix[14]) >> 12;
        cube[i].Position[3] = ((s64)x*ClipMatrix[3] + (s64)y*ClipMatrix[7] + (s64)z*ClipMatrix[11] + (s64)0x1000*ClipMatrix[15]) >> 12;
    }

    bool hit = BoxTestHits(*this, cube);

    // WideMelon: a box that misses the native view gets one more try against the wide view plus about
    // 4 px a side (the squeeze for a view 8 px wider, in pixels of a full-screen viewport), so games that
    // cull with box tests don't drop objects in the wings. with a margin, Y is squeezed the same way, 4 rows
    // past it top and bottom. a native hit stays a hit, so nothing that shows natively changes. the wide
    // check answers them as stock unless asked (WideCheckSqueezeBoxTest), so the game's memory can be compared
    if (!hit && ViewWidth != WideMelon::NativeWidth && (!WideCheck || WideCheckSqueezeBoxTest))
    {
        for (int i = 0; i < 8; i++)
        {
            cube[i].Position[0] = WideMelon::ProjectX(cube[i].Position[0], ViewWidth + 8);
            if (ViewMargin != 0)
                cube[i].Position[1] = WideMelon::ProjectY(cube[i].Position[1], ViewHeight() + 8);
        }
        hit = BoxTestHits(*this, cube);
        if (WideCheck) WideCheckBoxRetryHits += hit;
    }

    if (hit)
        GXStat |= (1<<1);
}

void GPU3D::PosTest() noexcept
{
    s64 vertex[4] = {(s64)CurVertex[0], (s64)CurVertex[1], (s64)CurVertex[2], 0x1000};

    UpdateClipMatrix();
    PosTestResult[0] = (vertex[0]*ClipMatrix[0] + vertex[1]*ClipMatrix[4] + vertex[2]*ClipMatrix[8] + vertex[3]*ClipMatrix[12]) >> 12;
    PosTestResult[1] = (vertex[0]*ClipMatrix[1] + vertex[1]*ClipMatrix[5] + vertex[2]*ClipMatrix[9] + vertex[3]*ClipMatrix[13]) >> 12;
    PosTestResult[2] = (vertex[0]*ClipMatrix[2] + vertex[1]*ClipMatrix[6] + vertex[2]*ClipMatrix[10] + vertex[3]*ClipMatrix[14]) >> 12;
    PosTestResult[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;

    AddCycles(5);
}

void GPU3D::VecTest(u32 param) noexcept
{
    // TODO: maybe it overwrites the normal registers, too

    s16 normal[3];

    normal[0] = (s16)((param & 0x000003FF) << 6) >> 6;
    normal[1] = (s16)((param & 0x000FFC00) >> 4) >> 6;
    normal[2] = (s16)((param & 0x3FF00000) >> 14) >> 6;

    VecTestResult[0] = (normal[0]*VecMatrix[0] + normal[1]*VecMatrix[4] + normal[2]*VecMatrix[8]) >> 9;
    VecTestResult[1] = (normal[0]*VecMatrix[1] + normal[1]*VecMatrix[5] + normal[2]*VecMatrix[9]) >> 9;
    VecTestResult[2] = (normal[0]*VecMatrix[2] + normal[1]*VecMatrix[6] + normal[2]*VecMatrix[10]) >> 9;

    if (VecTestResult[0] & 0x1000) VecTestResult[0] |= 0xF000;
    if (VecTestResult[1] & 0x1000) VecTestResult[1] |= 0xF000;
    if (VecTestResult[2] & 0x1000) VecTestResult[2] |= 0xF000;

    AddCycles(4);
}



void GPU3D::CmdFIFOWrite(const CmdFIFOEntry& entry) noexcept
{
    if (CmdFIFO.IsEmpty() && !CmdPIPE.IsFull())
    {
        CmdPIPE.Write(entry);
    }
    else
    {
        if (CmdFIFO.IsFull())
        {
            // store it to the stall queue. stall the system.
            // worst case is if a STMxx opcode causes this, which is why our stall queue
            // has 64 entries. this is less complicated than trying to make STMxx stall-able.

            CmdStallQueue.Write(entry);
            NDS.GXFIFOStall();
            return;
        }

        CmdFIFO.Write(entry);
    }

    GXStat |= (1<<27);

    if (entry.Command == 0x11 || entry.Command == 0x12)
    {
        GXStat |= (1<<14); // push/pop matrix
        NumPushPopCommands++;
    }
    else if (entry.Command == 0x70 || entry.Command == 0x71 || entry.Command == 0x72)
    {
        GXStat |= (1<<0); // box/pos/vec test
        NumTestCommands++;
    }
}

GPU3D::CmdFIFOEntry GPU3D::CmdFIFORead() noexcept
{
    CmdFIFOEntry ret = CmdPIPE.Read();

    if (CmdPIPE.Level() <= 2)
    {
        if (!CmdFIFO.IsEmpty())
            CmdPIPE.Write(CmdFIFO.Read());
        if (!CmdFIFO.IsEmpty())
            CmdPIPE.Write(CmdFIFO.Read());

        // empty stall queue if needed
        // CmdFIFO should not be full at this point.
        if (!CmdStallQueue.IsEmpty())
        {
            while (!CmdStallQueue.IsEmpty())
            {
                if (CmdFIFO.IsFull()) break;
                CmdFIFOEntry entry = CmdStallQueue.Read();
                CmdFIFOWrite(entry);
            }

            if (CmdStallQueue.IsEmpty())
                NDS.GXFIFOUnstall();
        }

        CheckFIFODMA();
        CheckFIFOIRQ();
    }

    return ret;
}

// inlined into both of its callers, so that the emulator's only cost from replay is ExecuteCommand's check
[[gnu::always_inline]] inline void GPU3D::RunCommand(CmdFIFOEntry entry) noexcept
{
    //printf("FIFO: processing %02X %08X. Levels: FIFO=%d, PIPE=%d\n", entry.Command, entry.Param, CmdFIFO->Level(), CmdPIPE->Level());

    // each FIFO entry takes 1 cycle to be processed
    // commands (presumably) run when all the needed parameters have been read
    // which is where we add the remaining cycles if any

    u32 paramsRequiredCount = CmdNumParams[entry.Command];
    if (paramsRequiredCount <= 1)
    {
        // fast path for command which only have a single parameter

        /*printf("[GXS:%08X] 0x%02X,  0x%08X", GXStat, entry.Command, entry.Param);*/

        switch (entry.Command)
        {
        case 0x10: // matrix mode
            VertexPipelineCmdDelayed4();
            MatrixMode = entry.Param & 0x3;
            break;

        case 0x11: // push matrix
            if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, &entry.Param, *this);
            VertexPipelineCmdDelayed4();
            NumPushPopCommands--;
            if (MatrixMode == 0)
            {
                if (ProjMatrixStackPointer > 0) GXStat |= (1<<15);

                memcpy(ProjMatrixStack, ProjMatrix, 16*4);
                ProjMatrixStackPointer++;
                ProjMatrixStackPointer &= 0x1;
            }
            else if (MatrixMode == 3)
            {
                if (TexMatrixStackPointer > 0) GXStat |= (1<<15);

                memcpy(TexMatrixStack, TexMatrix, 16*4);
                TexMatrixStackPointer++;
                TexMatrixStackPointer &= 0x1;
            }
            else
            {
                if (PosMatrixStackPointer > 30) GXStat |= (1<<15);

                memcpy(PosMatrixStack[PosMatrixStackPointer & 0x1F], PosMatrix, 16*4);
                memcpy(VecMatrixStack[PosMatrixStackPointer & 0x1F], VecMatrix, 16*4);
                PosMatrixStackPointer++;
                PosMatrixStackPointer &= 0x3F;
            }
            AddCycles(16);
            break;

        case 0x12: // pop matrix
            if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, &entry.Param, *this);
            VertexPipelineCmdDelayed4();
            NumPushPopCommands--;
            if (MatrixMode == 0)
            {
                if (ProjMatrixStackPointer == 0) GXStat |= (1<<15);

                ProjMatrixStackPointer--;
                ProjMatrixStackPointer &= 0x1;
                memcpy(ProjMatrix, ProjMatrixStack, 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            else if (MatrixMode == 3)
            {
                if (TexMatrixStackPointer == 0) GXStat |= (1<<15);

                TexMatrixStackPointer--;
                TexMatrixStackPointer &= 0x1;
                memcpy(TexMatrix, TexMatrixStack, 16*4);
                AddCycles(17);
            }
            else
            {
                s32 offset = (s32)(entry.Param << 26) >> 26;
                PosMatrixStackPointer -= offset;
                PosMatrixStackPointer &= 0x3F;

                if (PosMatrixStackPointer > 30) GXStat |= (1<<15);

                memcpy(PosMatrix, PosMatrixStack[PosMatrixStackPointer & 0x1F], 16*4);
                memcpy(VecMatrix, VecMatrixStack[PosMatrixStackPointer & 0x1F], 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            break;

        case 0x13: // store matrix
            if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, &entry.Param, *this);
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                memcpy(ProjMatrixStack, ProjMatrix, 16*4);
            }
            else if (MatrixMode == 3)
            {
                memcpy(TexMatrixStack, TexMatrix, 16*4);
            }
            else
            {
                u32 addr = entry.Param & 0x1F;
                if (addr > 30) GXStat |= (1<<15);

                memcpy(PosMatrixStack[addr], PosMatrix, 16*4);
                memcpy(VecMatrixStack[addr], VecMatrix, 16*4);
            }
            AddCycles(16);
            break;

        case 0x14: // restore matrix
            if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, &entry.Param, *this);
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                memcpy(ProjMatrix, ProjMatrixStack, 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            else if (MatrixMode == 3)
            {
                memcpy(TexMatrix, TexMatrixStack, 16*4);
                AddCycles(17);
            }
            else
            {
                u32 addr = entry.Param & 0x1F;
                if (addr > 30) GXStat |= (1<<15);

                memcpy(PosMatrix, PosMatrixStack[addr], 16*4);
                memcpy(VecMatrix, VecMatrixStack[addr], 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            break;

        case 0x15: // identity
            if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, &entry.Param, *this);
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                MatrixLoadIdentity(ProjMatrix);
                ClipMatrixDirty = true;
                AddCycles(18);
            }
            else if (MatrixMode == 3)
                MatrixLoadIdentity(TexMatrix);
            else
            {
                MatrixLoadIdentity(PosMatrix);
                if (MatrixMode == 2)
                    MatrixLoadIdentity(VecMatrix);
                ClipMatrixDirty = true;
                AddCycles(18);
            }
            break;

        case 0x20: // vertex color
            VertexPipelineCmdDelayed6();
            {
                u32 c = entry.Param;
                u32 r = c & 0x1F;
                u32 g = (c >> 5) & 0x1F;
                u32 b = (c >> 10) & 0x1F;
                VertexColor[0] = r;
                VertexColor[1] = g;
                VertexColor[2] = b;
            }
            break;

        case 0x21: // normal
            VertexPipelineCmdDelayed4();
            Normal[0] = (s16)((entry.Param & 0x000003FF) << 6) >> 6;
            Normal[1] = (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
            Normal[2] = (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
            CalculateLighting();
            break;

        case 0x22: // texcoord
            VertexPipelineCmdDelayed4();
            RawTexCoords[0] = entry.Param & 0xFFFF;
            RawTexCoords[1] = entry.Param >> 16;
            if ((TexParam >> 30) == 1)
            {
                TexCoords[0] = (RawTexCoords[0]*TexMatrix[0] + RawTexCoords[1]*TexMatrix[4] + TexMatrix[8] + TexMatrix[12]) >> 12;
                TexCoords[1] = (RawTexCoords[0]*TexMatrix[1] + RawTexCoords[1]*TexMatrix[5] + TexMatrix[9] + TexMatrix[13]) >> 12;
            }
            else
            {
                TexCoords[0] = RawTexCoords[0];
                TexCoords[1] = RawTexCoords[1];
            }
            break;

        case 0x24: // 10-bit vertex
            VertexPipelineSubmitCmd();
            CurVertex[0] = (entry.Param & 0x000003FF) << 6;
            CurVertex[1] = (entry.Param & 0x000FFC00) >> 4;
            CurVertex[2] = (entry.Param & 0x3FF00000) >> 14;
            SubmitVertex();
            break;

        case 0x25: // vertex XY
            VertexPipelineSubmitCmd();
            CurVertex[0] = entry.Param & 0xFFFF;
            CurVertex[1] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x26: // vertex XZ
            VertexPipelineSubmitCmd();
            CurVertex[0] = entry.Param & 0xFFFF;
            CurVertex[2] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x27: // vertex YZ
            VertexPipelineSubmitCmd();
            CurVertex[1] = entry.Param & 0xFFFF;
            CurVertex[2] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x28: // 10-bit delta vertex
            VertexPipelineSubmitCmd();
            CurVertex[0] += (s16)((entry.Param & 0x000003FF) << 6) >> 6;
            CurVertex[1] += (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
            CurVertex[2] += (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
            SubmitVertex();
            break;

        case 0x29: // polygon attributes
            VertexPipelineCmdDelayed8();
            PolygonAttr = entry.Param;
            break;

        case 0x2A: // texture param
            VertexPipelineCmdDelayed8();
            TexParam = entry.Param;
            break;

        case 0x2B: // texture palette
            VertexPipelineCmdDelayed8();
            TexPalette = entry.Param & 0x1FFF;
            break;

        case 0x30: // diffuse/ambient material
            VertexPipelineCmdDelayed6();
            MatDiffuse[0] = entry.Param & 0x1F;
            MatDiffuse[1] = (entry.Param >> 5) & 0x1F;
            MatDiffuse[2] = (entry.Param >> 10) & 0x1F;
            MatAmbient[0] = (entry.Param >> 16) & 0x1F;
            MatAmbient[1] = (entry.Param >> 21) & 0x1F;
            MatAmbient[2] = (entry.Param >> 26) & 0x1F;
            if (entry.Param & 0x8000)
            {
                VertexColor[0] = MatDiffuse[0];
                VertexColor[1] = MatDiffuse[1];
                VertexColor[2] = MatDiffuse[2];
            }
            AddCycles(3);
            break;

        case 0x31: // specular/emission material
            VertexPipelineCmdDelayed6();
            MatSpecular[0] = entry.Param & 0x1F;
            MatSpecular[1] = (entry.Param >> 5) & 0x1F;
            MatSpecular[2] = (entry.Param >> 10) & 0x1F;
            MatEmission[0] = (entry.Param >> 16) & 0x1F;
            MatEmission[1] = (entry.Param >> 21) & 0x1F;
            MatEmission[2] = (entry.Param >> 26) & 0x1F;
            UseShininessTable = (entry.Param & 0x8000) != 0;
            AddCycles(3);
            break;

        case 0x32: // light direction
            StallPolygonPipeline(8 + 1,  2); // 0x32 can run 6 cycles after a vertex
            {
                u32 l = entry.Param >> 30;
                s16 dir[3];
                dir[0] = (s16)((entry.Param & 0x000003FF) << 6) >> 6;
                dir[1] = (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
                dir[2] = (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
                // the order of operations here is very specific: discard bottom 12 bits -> negate -> then sign extend to convert to 11 bit signed int
                // except for when used to calculate the specular reciprocal; then it's: sign extend -> discard lsb -> negate.
                LightDirection[l][0] = (-((dir[0]*VecMatrix[0] + dir[1]*VecMatrix[4] + dir[2]*VecMatrix[8] ) >> 12) << 21) >> 21;
                LightDirection[l][1] = (-((dir[0]*VecMatrix[1] + dir[1]*VecMatrix[5] + dir[2]*VecMatrix[9] ) >> 12) << 21) >> 21;
                LightDirection[l][2] = (-((dir[0]*VecMatrix[2] + dir[1]*VecMatrix[6] + dir[2]*VecMatrix[10]) >> 12) << 21) >> 21;
                s32 den =              -(((dir[0]*VecMatrix[2] + dir[1]*VecMatrix[6] + dir[2]*VecMatrix[10]) << 9) >> 21) + (1<<9);

                if (den == 0) SpecRecip[l] = 0;
                else SpecRecip[l] = (1<<18) / den;
            }
            AddCycles(5);
            break;

        case 0x33: // light color
            VertexPipelineCmdDelayed8();
            {
                u32 l = entry.Param >> 30;
                LightColor[l][0] = entry.Param & 0x1F;
                LightColor[l][1] = (entry.Param >> 5) & 0x1F;
                LightColor[l][2] = (entry.Param >> 10) & 0x1F;
            }
            AddCycles(1);
            break;

        case 0x40: // begin polygons
            StallPolygonPipeline(1, 0);
            // TODO: check if there was a polygon being defined but incomplete
            // such cases seem to freeze the GPU
            PolygonMode = entry.Param & 0x3;
            VertexNum = 0;
            VertexNumInPoly = 0;
            NumConsecutivePolygons = 0;
            LastStripPolygon = NULL;
            NativeStripVerts = 0;
            CurPolygonAttr = PolygonAttr;
            break;

        case 0x41: // end polygons
            VertexPipelineCmdDelayed8();
            // TODO: research this?
            // it doesn't seem to have any effect whatsoever, but
            // its timing characteristics are different from those of other
            // no-op commands
            break;

        case 0x50: // flush
            VertexPipelineCmdDelayed4();
            FlushRequest = 1;
            FlushAttributes = entry.Param & 0x3;
            CycleCount = 325;
            // probably safe to just reset all pipelines
            // but needs checked
            VertexPipeline = 0;
            NormalPipeline = 0;
            PolygonPipeline = 0;
            VertexSlotCounter = 0;
            VertexSlotsFree = 1;
            break;

        case 0x60: // viewport x1,y1,x2,y2
            VertexPipelineCmdDelayed8();
            // note: viewport Y coordinates are upside-down
            Viewport[0] = entry.Param & 0xFF;                             // x0
            Viewport[1] = (191 - ((entry.Param >> 8) & 0xFF)) & 0xFF;     // y0
            Viewport[2] = (entry.Param >> 16) & 0xFF;                     // x1
            Viewport[3] = (191 - (entry.Param >> 24)) & 0xFF;             // y1
            Viewport[4] = (Viewport[2] - Viewport[0] + 1) & 0x1FF;          // width
            Viewport[5] = (Viewport[1] - Viewport[3] + 1) & 0xFF;           // height
            SceneCameraDirty = true;
            break;

        case 0x72: // vec test
            VertexPipelineCmdDelayed6();
            NumTestCommands--;
            VecTest(entry.Param);
            break;

        default:
            VertexPipelineCmdDelayed4();
            //printf("!! UNKNOWN GX COMMAND %02X %08X\n", entry.Command, entry.Param);
            break;
        }
    }
    else
    {
        ExecParams[ExecParamCount] = entry.Param;
        ExecParamCount++;

        if (ExecParamCount == 1)
        {
            // delay the first command entry as needed
            switch (entry.Command)
            {
            // commands that stall the polygon pipeline
            case 0x23: VertexPipelineSubmitCmd(); break;
            case 0x34:
            case 0x71:
                VertexPipelineCmdDelayed8();
                break;
            case 0x70: StallPolygonPipeline(10 + 1, 0); break;
            default: VertexPipelineCmdDelayed4(); break;
            }
        }
        else
        {
            AddCycles(1);

            if (ExecParamCount >= paramsRequiredCount)
            {
                /*printf("[GXS:%08X] 0x%02X,  ", GXStat, entry.Command);
                for (int k = 0; k < ExecParamCount; k++) printf("0x%08X, ", ExecParams[k]);
                printf("\n");*/

                ExecParamCount = 0;

                if (VertexIdLog) VertexIdLog->OnMatrixCommand(entry.Command, ExecParams, *this);

                switch (entry.Command)
                {
                case 0x16: // load 4x4
                    if (MatrixMode == 0)
                    {
                        MatrixLoad4x4(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixLoad4x4(TexMatrix, (s32*)ExecParams);
                        AddCycles(10);
                    }
                    else
                    {
                        MatrixLoad4x4(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                            MatrixLoad4x4(VecMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    break;

                case 0x17: // load 4x3
                    if (MatrixMode == 0)
                    {
                        MatrixLoad4x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixLoad4x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(7);
                    }
                    else
                    {
                        MatrixLoad4x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                            MatrixLoad4x3(VecMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    break;

                case 0x18: // mult 4x4
                    if (MatrixMode == 0)
                    {
                        MatrixMult4x4(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 16);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult4x4(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 16);
                    }
                    else
                    {
                        MatrixMult4x4(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult4x4(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 16);
                        }
                        else AddCycles(35 - 16);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x19: // mult 4x3
                    if (MatrixMode == 0)
                    {
                        MatrixMult4x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 12);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult4x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 12);
                    }
                    else
                    {
                        MatrixMult4x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult4x3(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 12);
                        }
                        else AddCycles(35 - 12);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x1A: // mult 3x3
                    if (MatrixMode == 0)
                    {
                        MatrixMult3x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 9);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult3x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 9);
                    }
                    else
                    {
                        MatrixMult3x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult3x3(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 9);
                        }
                        else AddCycles(35 - 9);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x1B: // scale
                    if (MatrixMode == 0)
                    {
                        MatrixScale(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixScale(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 3);
                    }
                    else
                    {
                        MatrixScale(PosMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    break;

                case 0x1C: // translate
                    if (MatrixMode == 0)
                    {
                        MatrixTranslate(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixTranslate(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 3);
                    }
                    else
                    {
                        MatrixTranslate(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixTranslate(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 3);
                        }
                        else AddCycles(35 - 3);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x23: // full vertex
                    CurVertex[0] = ExecParams[0] & 0xFFFF;
                    CurVertex[1] = ExecParams[0] >> 16;
                    CurVertex[2] = ExecParams[1] & 0xFFFF;
                    SubmitVertex();
                    break;

                case 0x34: // shininess table
                    {
                        for (int i = 0; i < 128; i += 4)
                        {
                            u32 val = ExecParams[i >> 2];
                            ShininessTable[i + 0] = val & 0xFF;
                            ShininessTable[i + 1] = (val >> 8) & 0xFF;
                            ShininessTable[i + 2] = (val >> 16) & 0xFF;
                            ShininessTable[i + 3] = val >> 24;
                        }
                    }
                    break;

                case 0x71: // pos test
                    NumTestCommands -= 2;
                    CurVertex[0] = ExecParams[0] & 0xFFFF;
                    CurVertex[1] = ExecParams[0] >> 16;
                    CurVertex[2] = ExecParams[1] & 0xFFFF;
                    PosTest();
                    break;

                case 0x70: // box test
                    NumTestCommands -= 3;
                    BoxTest(ExecParams);
                    if (WideCheck) { WideCheckBoxTests++; WideCheckBoxHits += (GXStat >> 1) & 1; }
                    if (VertexIdLog) VertexIdLog->OnBoxTest(GXStat & (1<<1));
                    break;

                default:
                    __builtin_unreachable();
                }
            }
        }
    }
}

void GPU3D::ExecuteCommand() noexcept
{
    CmdFIFOEntry entry = CmdFIFORead();
    if (HalfCam) HalfCam->Record(entry.Command, entry.Param);
    RunCommand(entry);
}

void GPU3D::ReplayEntry(u8 command, u32 param) noexcept
{
    CmdFIFOEntry entry {};
    entry.Command = command;
    entry.Param = param;
    RunCommand(entry);
}

u32 GPU3D::NumCommandParams(u8 command) noexcept
{
    return CmdNumParams[command];
}

s32 GPU3D::CyclesToRunFor() const noexcept
{
    if (CycleCount < 0) return 0;
    return CycleCount;
}

void GPU3D::FinishWork(s32 cycles) noexcept
{
    AddCycles(cycles);
    if (NormalPipeline)
        NormalPipeline -= std::min(NormalPipeline, cycles);

    CycleCount = 0;

    if (VertexPipeline || NormalPipeline || PolygonPipeline)
        return;

    GXStat &= ~(1<<27);
}

void GPU3D::Run() noexcept
{
    if (!GeometryEnabled || FlushRequest ||
        (CmdPIPE.IsEmpty() && !(GXStat & (1<<27))))
    {
        Timestamp = NDS.ARM9Timestamp >> NDS.ARM9ClockShift;
        return;
    }

    s32 cycles = (NDS.ARM9Timestamp >> NDS.ARM9ClockShift) - Timestamp;
    CycleCount -= cycles;
    Timestamp = NDS.ARM9Timestamp >> NDS.ARM9ClockShift;

    if (CycleCount <= 0)
    {
        while (CycleCount <= 0 && !CmdPIPE.IsEmpty())
        {
            if (NumPushPopCommands == 0) GXStat &= ~(1<<14);
            if (NumTestCommands == 0)    GXStat &= ~(1<<0);

            ExecuteCommand();
        }
    }

    if (CycleCount <= 0 && CmdPIPE.IsEmpty())
    {
        if (GXStat & (1<<27)) FinishWork(-CycleCount);
        else                  CycleCount = 0;

        if (NumPushPopCommands == 0) GXStat &= ~(1<<14);
        if (NumTestCommands == 0)    GXStat &= ~(1<<0);
    }
}


void GPU3D::CheckFIFOIRQ() noexcept
{
    bool irq = false;
    switch (GXStat >> 30)
    {
    case 1: irq = (CmdFIFO.Level() < 128); break;
    case 2: irq = CmdFIFO.IsEmpty(); break;
    }

    if (irq) NDS.SetIRQ(0, IRQ_GXFIFO);
    else     NDS.ClearIRQ(0, IRQ_GXFIFO);
}

void GPU3D::CheckFIFODMA() noexcept
{
    if (CmdFIFO.Level() < 128)
        NDS.CheckDMAs(0, 0x07);
}

void GPU3D::VCount144(GPU& gpu) noexcept
{
    CurrentRenderer->VCount144(gpu);
}

void GPU3D::RestartFrame(GPU& gpu) noexcept
{
    CurrentRenderer->RestartFrame(gpu);
}

void GPU3D::Stop(const GPU& gpu) noexcept
{
    if (CurrentRenderer)
        CurrentRenderer->Stop(gpu);
}


bool YSort(Polygon* a, Polygon* b)
{
    // polygon sorting rules:
    // * opaque polygons come first
    // * polygons with lower bottom Y come first
    // * upon equal bottom Y, polygons with lower top Y come first
    // * upon equal bottom AND top Y, original ordering is used
    // the SortKey is calculated as to implement these rules

    return a->SortKey < b->SortKey;
}

// whether the registers a render reads are the ones the last render was latched with
bool GPU3D::RenderRegsSame() const noexcept
{
    return RenderDispCnt == DispCnt
        && RenderAlphaRef == AlphaRef
        && RenderClearAttr1 == ClearAttr1
        && RenderClearAttr2 == ClearAttr2
        && RenderFogColor == FogColor
        && RenderFogOffset == FogOffset * 0x200
        && memcmp(RenderEdgeTable, EdgeTable, 8*2) == 0
        && memcmp(RenderFogDensityTable + 1, FogDensityTable, 32) == 0
        && memcmp(RenderToonTable, ToonTable, 32*2) == 0
        // what VBlank derives from those. they only differ if something else wrote the Render fields
        && RenderFogShift == ((DispCnt >> 8) & 0xF)
        && RenderFogDensityTable[0] == FogDensityTable[0]
        && RenderFogDensityTable[33] == FogDensityTable[31];
}

// GPU::ComputeOpt bit 5 (SceneRepeatCheck): compares the scene the renderers are about to draw
// with the copy of the last swap's, and with save makes it the copy. everything a renderer can
// read of a scene is in it: the view width, the polygon count, and for each polygon in list order
// every field of Polygon and, for each of its vertices, every field of Vertex. by value and field
// by field, as the two RAM banks take turns (so the pointers always differ) and the structs have
// padding. vertices past NumVertices and polygons past the count aren't read by anything
bool GPU3D::MatchRenderScene(bool save) noexcept
{
    // a field added to either struct has to be added below
    static_assert(sizeof(Vertex) == 64, "MatchRenderScene compares every field of Vertex");
    static_assert(sizeof(void*) != 8 || sizeof(Polygon) == 224, "MatchRenderScene compares every field of Polygon");
    constexpr u32 PolygonWords = 13, VertexWords = 19;

    u32 item[PolygonWords + 10 * VertexWords];
    bool same = SceneCopyValid;
    const size_t oldWords = SceneCopyWords;
    size_t pos = 0;

    if (!save && !same)
        return false;

    // compares up to the first difference, and with save writes from there on
    auto put = [&](u32 count)
    {
        if (same && (pos + count > oldWords || memcmp(&SceneCopy[pos], item, count * sizeof(u32)) != 0))
            same = false;
        if (save && !same)
        {
            if (SceneCopy.size() < pos + count)
                SceneCopy.resize(std::max<size_t>({pos + count, SceneCopy.size() * 2, 16384}));
            memcpy(&SceneCopy[pos], item, count * sizeof(u32));
        }
        pos += count;
    };

    item[0] = (u32)RenderViewWidth | ((u32)RenderViewMargin << 16);
    item[1] = RenderNumPolygons;
    put(2);

    for (u32 i = 0; i < RenderNumPolygons && (save || same); i++)
    {
        const Polygon* poly = RenderPolygonRAM[i];
        u32 numVertices = std::min<u32>(poly->NumVertices, 10);
        u32* out = item;

        *out++ = poly->NumVertices;
        *out++ = (poly->WBuffer ? 1 : 0) | (poly->Degenerate ? 2 : 0) | (poly->FacingView ? 4 : 0)
            | (poly->Translucent ? 8 : 0) | (poly->IsShadowMask ? 16 : 0) | (poly->IsShadow ? 32 : 0);
        *out++ = poly->Attr;
        *out++ = poly->TexParam;
        *out++ = poly->TexPalette;
        *out++ = (u32)poly->Type;
        *out++ = poly->VTop;
        *out++ = poly->VBottom;
        *out++ = (u32)poly->YTop;
        *out++ = (u32)poly->YBottom;
        *out++ = (u32)poly->XTop;
        *out++ = (u32)poly->XBottom;
        *out++ = poly->SortKey;

        for (u32 j = 0; j < numVertices; j++)
        {
            const Vertex* vertex = poly->Vertices[j];
            *out++ = (u32)poly->FinalZ[j];
            *out++ = (u32)poly->FinalW[j];
            *out++ = (u32)vertex->Position[0];
            *out++ = (u32)vertex->Position[1];
            *out++ = (u32)vertex->Position[2];
            *out++ = (u32)vertex->Position[3];
            *out++ = (u32)vertex->Color[0];
            *out++ = (u32)vertex->Color[1];
            *out++ = (u32)vertex->Color[2];
            *out++ = (u32)(s32)vertex->TexCoords[0];
            *out++ = (u32)(s32)vertex->TexCoords[1];
            *out++ = vertex->Clipped ? 1 : 0;
            *out++ = (u32)vertex->FinalPosition[0];
            *out++ = (u32)vertex->FinalPosition[1];
            *out++ = (u32)vertex->FinalColor[0];
            *out++ = (u32)vertex->FinalColor[1];
            *out++ = (u32)vertex->FinalColor[2];
            *out++ = (u32)vertex->HiresPosition[0];
            *out++ = (u32)vertex->HiresPosition[1];
        }

        put((u32)(out - item));
    }

    // a shorter scene that matches the start of the copy isn't the same either
    same = same && pos == oldWords;
    if (save)
    {
        SceneCopyWords = pos;
        SceneCopyValid = true;
    }
    return same;
}

// debug only (SceneHashLogging): a hash of the sorted polygons and their vertices as the renderers
// read them, by value, as the two RAM banks take turns
u64 GPU3D::HashRenderScene() const noexcept
{
    struct
    {
        u32 NumVertices, Attr, TexParam, TexPalette, VTop, VBottom, SortKey;
        s32 YTop, YBottom, XTop, XBottom, Type;
        u32 Flags;
        s32 FinalZ[10], FinalW[10];
        s32 Position[10][2], HiresPosition[10][2], Color[10][3], TexCoords[10][2];
    } item;

    u64 hash = XXH64(&RenderNumPolygons, sizeof(RenderNumPolygons),
        (u64)((u32)RenderViewWidth | ((u32)RenderViewMargin << 16)) ^ ((u64)FlushAttributes << 32));
    for (u32 i = 0; i < RenderNumPolygons; i++)
    {
        const Polygon* poly = RenderPolygonRAM[i];
        memset(&item, 0, sizeof(item));
        item.NumVertices = poly->NumVertices;
        item.Attr = poly->Attr;
        item.TexParam = poly->TexParam;
        item.TexPalette = poly->TexPalette;
        item.VTop = poly->VTop;
        item.VBottom = poly->VBottom;
        item.SortKey = poly->SortKey;
        item.YTop = poly->YTop;
        item.YBottom = poly->YBottom;
        item.XTop = poly->XTop;
        item.XBottom = poly->XBottom;
        item.Type = poly->Type;
        item.Flags = (poly->WBuffer ? 1 : 0) | (poly->Degenerate ? 2 : 0) | (poly->FacingView ? 4 : 0)
            | (poly->Translucent ? 8 : 0) | (poly->IsShadowMask ? 16 : 0) | (poly->IsShadow ? 32 : 0);
        for (u32 j = 0; j < poly->NumVertices && j < 10; j++)
        {
            const Vertex* vertex = poly->Vertices[j];
            item.FinalZ[j] = poly->FinalZ[j];
            item.FinalW[j] = poly->FinalW[j];
            item.Position[j][0] = vertex->FinalPosition[0];
            item.Position[j][1] = vertex->FinalPosition[1];
            item.HiresPosition[j][0] = vertex->HiresPosition[0];
            item.HiresPosition[j][1] = vertex->HiresPosition[1];
            item.Color[j][0] = vertex->FinalColor[0];
            item.Color[j][1] = vertex->FinalColor[1];
            item.Color[j][2] = vertex->FinalColor[2];
            item.TexCoords[j][0] = vertex->TexCoords[0];
            item.TexCoords[j][1] = vertex->TexCoords[1];
        }
        hash = XXH64(&item, sizeof(item), hash);
    }
    return hash;
}

void GPU3D::VBlank() noexcept
{
    if (VertexIdLog) VertexIdLog->OnVBlank();
    if (WideDrops.Enabled) WideDropsOnVBlank();

    // the in-between scene was last frame's, this one draws the scene itself unless a new one replaces it
    bool isAfterMidScene = HalfCam && HalfCam->OnVBlank();
    RenderFrameChange = 0;
    RenderSceneRepeated = false;

    // a VCOUNT write can jump past line 215 or back below 192, and then nothing was asked to draw
    // what the last VBlank latched: the image is older than that, as after a replaced state, and
    // the scene copy is of a swap no render saw. normally line 215 runs between any two VBlanks
    if (!RenderedSinceVBlank)
    {
        RenderStateReplaced = true;
        SceneCopyValid = false;
    }
    RenderedSinceVBlank = false;

    // the latched state was replaced since the last VBlank (MarkRenderStateReplaced), so whatever
    // this one finds, the frame isn't the last one again
    bool wasReplaced = RenderStateReplaced;
    RenderStateReplaced = false;

    // nothing is latched below while either engine is off, so the frame is drawn from the same
    // state as the last one. it used to leave the flag as it was, which kept an old image up when
    // the rendering engine went off after an identical frame, and drew every frame of a game that
    // never turns it on
    if (!GeometryEnabled || !RenderingEnabled)
        RenderFrameIdentical = !wasReplaced;

    if (GeometryEnabled)
    {
        if (RenderingEnabled)
        {
            if (FlushRequest)
            {
                if (NumPolygons)
                {
                    // separate translucent polygons from opaque ones

                    u32 io = 0, it = NumOpaquePolygons;
                    for (u32 i = 0; i < NumPolygons; i++)
                    {
                        Polygon* poly = &CurPolygonRAM[i];
                        if (poly->Translucent)
                            RenderPolygonRAM[it++] = poly;
                        else
                            RenderPolygonRAM[io++] = poly;
                    }

                    // apply Y-sorting

                    std::stable_sort(RenderPolygonRAM.begin(),
                        RenderPolygonRAM.begin() + ((FlushAttributes & 0x1) ? NumOpaquePolygons : NumPolygons),
                        YSort);
                }

                RenderNumPolygons = NumPolygons;
                RenderViewWidth = ViewWidth;
                RenderViewMargin = ViewMargin;
                RenderWideList = WideListOn;
                RenderFrameIdentical = false;
                // with the list, so a scene drawn twice has its camera both times. the bank swap below starts the next one's
                RenderCamera = PickSceneCamera();
                if (CameraLogData)
                {
                    CameraLogData->Cameras = SceneCameras;
                    CameraLogData->NumCameras = NumSceneCameras;
                    CameraLogData->Other = OtherCameraPolygons;
                    CameraLogData->Polygons = NumPolygons;
                    CameraLogData->Pick = RenderCamera;
                    CameraLogData->Swaps++;
                }

                // SceneRepeatCheck: the swap is the only thing in the way of an identical frame
                // when the registers are the last latch's and the scene is the last swap's again.
                // the copy is only kept up while that can be: after a swap with other registers,
                // or one that wasn't looked at, the next one has nothing to match
                if (SceneRepeatCheck && RenderRegsSame())
                    RenderSceneRepeated = MatchRenderScene(true);
                else
                    SceneCopyValid = false;

                RenderFrameChange = FrameChange_Flush;
                if (SceneHashLogging)
                {
                    u64 hash = HashRenderScene();
                    if (hash == RenderSceneHash && RenderRegsSame())
                        RenderFrameChange |= FrameChange_FlushSame;
                    RenderSceneHash = hash;
                }
            }
            else
            {
                bool regsSame = RenderRegsSame();
                RenderFrameIdentical = regsSame && !wasReplaced;
                if (!regsSame)
                    RenderFrameChange = FrameChange_Regs;
            }

            RenderDispCnt = DispCnt;
            RenderAlphaRef = AlphaRef;

            memcpy(RenderEdgeTable, EdgeTable, 8*2);
            memcpy(RenderToonTable, ToonTable, 32*2);

            RenderFogColor = FogColor;
            RenderFogOffset = FogOffset * 0x200;
            RenderFogShift = (RenderDispCnt >> 8) & 0xF;
            RenderFogDensityTable[0] = FogDensityTable[0];
            memcpy(&RenderFogDensityTable[1], FogDensityTable, 32);
            RenderFogDensityTable[33] = FogDensityTable[31];

            RenderClearAttr1 = ClearAttr1;
            RenderClearAttr2 = ClearAttr2;
        }

        if (FlushRequest)
        {
            if (VertexIdLog) VertexIdLog->OnSwap(*this);
            if (HalfCam) HalfCam->OnSwap();
            if (WideDrops.Enabled) WideDropsOnSwap();
            ResetSceneCameras();

            CurRAMBank = CurRAMBank?0:1;
            CurVertexRAM = &VertexRAM[CurRAMBank ? VertexRAMSize : 0];
            CurPolygonRAM = &PolygonRAM[CurRAMBank ? PolygonRAMSize : 0];

            NumVertices = 0;
            NumPolygons = 0;
            NumOpaquePolygons = 0;
            NativeNumVertices = 0;
            NativeNumPolygons = 0;
            WingNumVertices = 0;
            WingNumPolygons = 0;

            FlushRequest = 0;

            // the width only changes between frames of geometry: right after a swap...
            LatchViewWidth();

            if (HalfCam) HalfCam->OnSceneStart();
        }
    }
    else if (NumVertices == 0 && NumPolygons == 0 && VertexNumInPoly == 0)
    {
        // ...or while the geometry engine is off with nothing half-built
        LatchViewWidth();
    }

    // the renderers skip a frame that's the same as the last one, which would keep the in-between scene up
    if (isAfterMidScene)
    {
        MarkRenderFrameChanged();
        RenderFrameChange |= FrameChange_HalfCam;
    }
}

void GPU3D::VCount215(GPU& gpu) noexcept
{
    if (HalfCam) HalfCam->BeforeRender();

    // debug only, the scene being rendered, whatever the renderer does with it
    if (gpu.SceneStats)
        NoteSceneStats(gpu);

    // the renderer was given what the last VBlank latched (see VBlank). what it does with it, it
    // keeps track of itself
    RenderedSinceVBlank = true;
    CurrentRenderer->RenderFrame(gpu);

    // debug only: this frame's HASH2D number is GetHashFrame() until the frame ends, and the next
    // frame shows this render
    if (gpu.Hash3DLogging)
        CurrentRenderer->QueueHash3D(gpu, gpu.GetHashFrame() + 1);
}

// debug only (GPU::SceneStats), see GPU3D_SceneStats.h for the fields
void GPU3D::NoteSceneStats(const GPU& gpu) noexcept
{
    SceneStatsCounters& sec = SceneStatsSec;
    s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (sec.LogStartNs == 0)
        sec.LogStartNs = now;

    if (now - sec.LogStartNs >= 1000000000 && sec.Frames > 0)
    {
        double perFrame = 1.0 / sec.Frames;
        double od = (sec.OverdrawOpaque + sec.OverdrawTrans) * perFrame;
        Platform::Log(Platform::LogLevel::Info, "SCENESTATS frames=%u identical=%u nopolys=%u bg3d=%u tex=%u hl=%u atest=%u "
            "blend=%u aa=%u edge=%u fog=%u fogalpha=%u bitmap=%u wbuffer=%u polys=%.1f opaque=%.1f trans=%.1f shadow=%.1f "
            "mask=%.1f textured=%.1f untextured=%.1f lines=%.1f od=%.3f od_opaque=%.3f od_trans=%.3f od_mask=%.3f "
            "peakpolys=%u peakod=%.3f width=%d\n",
            sec.Frames, sec.Identical, sec.NoPolys, sec.BG3D, sec.Tex, sec.Highlight, sec.AlphaTest, sec.Blend, sec.AA,
            sec.Edge, sec.Fog, sec.FogAlpha, sec.Bitmap, sec.WBuffer, sec.Polys * perFrame, sec.Opaque * perFrame,
            sec.Trans * perFrame, sec.Shadow * perFrame, sec.Mask * perFrame, sec.Textured * perFrame,
            sec.Untextured * perFrame, sec.Lines * perFrame, od, sec.OverdrawOpaque * perFrame,
            sec.OverdrawTrans * perFrame, sec.OverdrawMask * perFrame, sec.PeakPolys, sec.PeakOverdraw, sec.Width);
        sec = {};
        sec.LogStartNs = now;
    }
    else if (now - sec.LogStartNs >= 1000000000)
        sec.LogStartNs = now;

    u32 dispCnt = RenderDispCnt;
    sec.Frames++;
    sec.Identical += RenderFrameIdentical;
    sec.BG3D += (gpu.GPU2D_A.DispCnt & (1<<3)) != 0;
    sec.Tex += (dispCnt & (1<<0)) != 0;
    sec.Highlight += (dispCnt & (1<<1)) != 0;
    sec.AlphaTest += (dispCnt & (1<<2)) != 0;
    sec.Blend += (dispCnt & (1<<3)) != 0;
    sec.AA += (dispCnt & (1<<4)) != 0;
    sec.Edge += (dispCnt & (1<<5)) != 0;
    sec.FogAlpha += (dispCnt & (1<<6)) != 0;
    sec.Fog += (dispCnt & (1<<7)) != 0;
    sec.Bitmap += (dispCnt & (1<<14)) != 0;
    sec.Width = RenderViewWidth;

    if (RenderNumPolygons == 0)
    {
        sec.NoPolys++;
        return;
    }
    sec.WBuffer += RenderPolygonRAM[0]->WBuffer;

    // twice the area, in native pixels of the (squeezed) view
    s64 area2[3] = {};
    u32 polys = 0;
    for (u32 i = 0; i < RenderNumPolygons; i++)
    {
        const Polygon* polygon = RenderPolygonRAM[i];
        if (polygon->Degenerate)
            continue;
        polys++;

        int kind; // 0 opaque, 1 translucent or shadow, 2 shadow mask
        if (polygon->IsShadowMask)
        {
            kind = 2;
            sec.Mask++;
        }
        else if (polygon->Translucent)
        {
            kind = 1;
            if (polygon->IsShadow) sec.Shadow++;
            else                   sec.Trans++;
        }
        else
        {
            kind = 0;
            sec.Opaque++;
        }

        if ((dispCnt & (1<<0)) && ((polygon->TexParam >> 26) & 0x7) != 0) sec.Textured++;
        else                                                              sec.Untextured++;
        sec.Lines += polygon->Type == 1;

        s64 sum = 0;
        u32 n = polygon->NumVertices;
        for (u32 j = 0; j < n; j++)
        {
            const Vertex* a = polygon->Vertices[j];
            const Vertex* b = polygon->Vertices[(j + 1) < n ? (j + 1) : 0];
            sum += (s64)a->FinalPosition[0] * b->FinalPosition[1] - (s64)b->FinalPosition[0] * a->FinalPosition[1];
        }
        area2[kind] += sum < 0 ? -sum : sum;
    }

    // FinalPosition is the native 256x192 frame, with a wide view squeezed into it by 256/W (only
    // HiresPosition holds the wide X), so the area over 256x192 is the share of the whole view, wide or not
    double screen2 = 2.0 * 256 * 192;
    double od[3] = { area2[0] / screen2, area2[1] / screen2, area2[2] / screen2 };
    sec.Polys += polys;
    sec.OverdrawOpaque += od[0];
    sec.OverdrawTrans += od[1];
    sec.OverdrawMask += od[2];
    sec.PeakPolys = std::max(sec.PeakPolys, polys);
    sec.PeakOverdraw = std::max(sec.PeakOverdraw, od[0] + od[1]);
}

void GPU3D::WideDropsOnSwap() noexcept
{
    u32 drops = DroppedPolygons - WideDrops.SceneStartDrops;
    WideDrops.SceneStartDrops = DroppedPolygons;
    WideDrops.Scenes++;
    WideDrops.DropScenes += drops > 0;
    WideDrops.MaxSceneDrops = std::max(WideDrops.MaxSceneDrops, drops);
    WideDrops.PeakPolygons = std::max(WideDrops.PeakPolygons, NumPolygons);
    WideDrops.PeakVertices = std::max(WideDrops.PeakVertices, NumVertices);
    // the native view's counts are only kept with the view wider or the wide list, otherwise they're the real ones
    bool wide = ViewWidth != WideMelon::NativeWidth || WideListOn;
    WideDrops.PeakNativePolygons = std::max(WideDrops.PeakNativePolygons, wide ? NativeNumPolygons : NumPolygons);
    WideDrops.PeakNativeVertices = std::max(WideDrops.PeakNativeVertices, wide ? NativeNumVertices : NumVertices);
    WideDrops.PeakWingPolygons = std::max(WideDrops.PeakWingPolygons, WingNumPolygons);
}

void GPU3D::WideDropsOnVBlank() noexcept
{
    auto now = std::chrono::steady_clock::now();
    if (now - WideDrops.LastLog < std::chrono::seconds(1))
        return;

    // widelist: GPU::WidePolys's wide list was on for the scene being built, scenes: polygon RAM swaps
    // this second, dropscenes: those that dropped any, drops: polygons dropped as the polygon or vertex
    // RAM (or the wide list or its wing budget) was full, of those native: ones the DS would drop too,
    // wing: ones only the wide view kept, nativefit: ones the native view had room for (the wide view's
    // cost; always 0 at width 256, and should be with the wide list), maxscene: most drops in one scene,
    // peaknpoly and peaknvert: the native view's fullest polygon and vertex RAM at a swap (of 2048 and
    // 6144), peakpoly and peakvert: the stored ones (of 2048 and 6144, or 4096 and 12288 with the wide
    // list), peakwing: the most polygons in the wide list's wing budget (of 2048)
    u32 drops = DroppedPolygons - WideDrops.SecondStartDrops;
    u32 nativefit = DroppedNativeFit - WideDrops.SecondStartNativeFit;
    u32 wing = DroppedWing - WideDrops.SecondStartWing;
    Log(LogLevel::Info, "WIDEDROPS width=%d widelist=%d scenes=%u dropscenes=%u drops=%u native=%u wing=%u nativefit=%u maxscene=%u "
        "peaknpoly=%u peaknvert=%u peakpoly=%u peakvert=%u peakwing=%u margin=%d\n",
        ViewWidth, WideListOn ? 1 : 0, WideDrops.Scenes, WideDrops.DropScenes, drops, drops - nativefit - wing, wing, nativefit,
        WideDrops.MaxSceneDrops, WideDrops.PeakNativePolygons, WideDrops.PeakNativeVertices, WideDrops.PeakPolygons,
        WideDrops.PeakVertices, WideDrops.PeakWingPolygons, ViewMargin);

    WideDrops.LastLog = now;
    WideDrops.Scenes = WideDrops.DropScenes = WideDrops.MaxSceneDrops = WideDrops.PeakPolygons = WideDrops.PeakVertices = 0;
    WideDrops.PeakNativePolygons = WideDrops.PeakNativeVertices = WideDrops.PeakWingPolygons = 0;
    WideDrops.SecondStartDrops = DroppedPolygons;
    WideDrops.SecondStartNativeFit = DroppedNativeFit;
    WideDrops.SecondStartWing = DroppedWing;
}

void GPU3D::SetRenderXPos(u16 xpos) noexcept
{
    if (!RenderingEnabled) return;

    RenderXPos = xpos & 0x01FF;
}


u32* GPU3D::GetLine(int line, u16 xpos, bool abort) noexcept
{
    // the wide check: the same opaque, translucent and clear pixels in both runs (see WideCheck)
    if (WideCheck)
    {
        for (int x = 0; x < 256; x++)
            ScrolledLine[x] = (x & 0x3F) | ((line & 0x3F) << 8) | (((x ^ line) & 0x3F) << 16) | (((x * 3 + line) & 0x1F) << 24);
        return ScrolledLine;
    }

    if (!abort)
    {
        u32* rawline = CurrentRenderer->GetLine(line);

        if (xpos == 0) return rawline;

        // apply X scroll

        if (xpos & 0x100)
        {
            int i = 0, j = xpos;
            for (; j < 512; i++, j++)
                ScrolledLine[i] = 0;
            for (j = 0; i < 256; i++, j++)
                ScrolledLine[i] = rawline[j];
        }
        else
        {
            int i = 0, j = xpos;
            for (; j < 256; i++, j++)
                ScrolledLine[i] = rawline[j];
            for (; i < 256; i++)
                ScrolledLine[i] = 0;
        }
    }
    else
    {
        memset(ScrolledLine, 0, 256*4);
    }

    return ScrolledLine;
}

int GPU3D::RendererViewWidth() const noexcept
{
    return WideCheck ? WideCheck : CurrentRenderer->GetViewWidth();
}

// with the width RendererViewWidth just gave (ViewWidth). the wide check has no margin
int GPU3D::RendererViewMargin() const noexcept
{
    if (WideCheck || !NDS.GPU.WideY)
        return 0;
    return WideMelon::SanitizeMargin(CurrentRenderer->GetViewMargin(), ViewWidth);
}

bool GPU3D::IsRendererAccelerated() const noexcept
{
    return CurrentRenderer && CurrentRenderer->Accelerated;
}

void GPU3D::WriteToGXFIFO(u32 val) noexcept
{
    if (NumCommands == 0)
    {
        NumCommands = 4;
        CurCommand = val;
        ParamCount = 0;
        TotalParams = CmdNumParams[CurCommand & 0xFF];

        if (TotalParams > 0) return;
    }
    else
        ParamCount++;

    for (;;)
    {
        if ((CurCommand & 0xFF) || (NumCommands == 4 && CurCommand == 0))
        {
            // zeroed, padding too: the FIFOs are saved whole, so stack bytes would make states differ
            CmdFIFOEntry entry {};
            entry.Command = CurCommand & 0xFF;
            entry.Param = val;
            CmdFIFOWrite(entry);
        }

        if (ParamCount >= TotalParams)
        {
            CurCommand >>= 8;
            NumCommands--;
            if (NumCommands == 0) break;

            ParamCount = 0;
            TotalParams = CmdNumParams[CurCommand & 0xFF];
        }
        if (ParamCount < TotalParams)
            break;
    }
}


u8 GPU3D::Read8(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000600:
        Run();
        return GXStat & 0xFF;
    case 0x04000601:
        {
            Run();
            return ((GXStat >> 8) & 0xFF) |
                   (PosMatrixStackPointer & 0x1F) |
                   ((ProjMatrixStackPointer & 0x1) << 5);
        }
    case 0x04000602:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return fifolevel & 0xFF;
        }
    case 0x04000603:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return ((GXStat >> 24) & 0xFF) |
                   (fifolevel >> 8) |
                   (fifolevel < 128 ? (1<<1) : 0) |
                   (fifolevel == 0  ? (1<<2) : 0);
        }
    }

    Log(LogLevel::Debug, "unknown GPU3D read8 %08X\n", addr);
    return 0;
}

u16 GPU3D::Read16(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000060:
        return DispCnt;

    case 0x04000320:
        return 46; // TODO, eventually

    case 0x04000600:
        {
            Run();

            return (GXStat & 0xFFFF) |
                   ((PosMatrixStackPointer & 0x1F) << 8) |
                   ((ProjMatrixStackPointer & 0x1) << 13);
        }
    case 0x04000602:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return (GXStat >> 16) |
                   fifolevel |
                   (fifolevel < 128 ? (1<<9) : 0) |
                   (fifolevel == 0  ? (1<<10) : 0);
        }

    case 0x04000604:
        {
            // WideMelon's wide list: the counts the DS would have (see SubmitPolygon). without it never
            // past the DS's limits (a bank over them keeps the wide list until its swap, so this only guards)
            u16 count = WideListOn ? NativeNumPolygons : std::min(NumPolygons, NativePolygonRAMSize);
            if (WideCheck) NoteWideCheckCount(addr, count);
            return count;
        }
    case 0x04000606:
        {
            u16 count = WideListOn ? NativeNumVertices : std::min(NumVertices, NativeVertexRAMSize);
            if (WideCheck) NoteWideCheckCount(addr, count);
            return count;
        }

    case 0x04000630: return VecTestResult[0];
    case 0x04000632: return VecTestResult[1];
    case 0x04000634: return VecTestResult[2];
    }

    Log(LogLevel::Debug, "unknown GPU3D read16 %08X\n", addr);
    return 0;
}

u32 GPU3D::Read32(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000060:
        return DispCnt;

    case 0x04000320:
        return 46; // TODO, eventually

    case 0x04000600:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return GXStat |
                   ((PosMatrixStackPointer & 0x1F) << 8) |
                   ((ProjMatrixStackPointer & 0x1) << 13) |
                   (fifolevel << 16) |
                   (fifolevel < 128 ? (1<<25) : 0) |
                   (fifolevel == 0  ? (1<<26) : 0);
        }

    case 0x04000604:
        {
            u32 count = WideListOn ? (NativeNumPolygons | (NativeNumVertices << 16))
                : (std::min(NumPolygons, NativePolygonRAMSize) | (std::min(NumVertices, NativeVertexRAMSize) << 16));
            if (WideCheck) NoteWideCheckCount(addr, count);
            return count;
        }

    case 0x04000620: return PosTestResult[0];
    case 0x04000624: return PosTestResult[1];
    case 0x04000628: return PosTestResult[2];
    case 0x0400062C: return PosTestResult[3];

    case 0x04000680: return VecMatrix[0];
    case 0x04000684: return VecMatrix[1];
    case 0x04000688: return VecMatrix[2];
    case 0x0400068C: return VecMatrix[4];
    case 0x04000690: return VecMatrix[5];
    case 0x04000694: return VecMatrix[6];
    case 0x04000698: return VecMatrix[8];
    case 0x0400069C: return VecMatrix[9];
    case 0x040006A0: return VecMatrix[10];
    }

    if (addr >= 0x04000640 && addr < 0x04000680)
    {
        UpdateClipMatrix();
        return ClipMatrix[(addr & 0x3C) >> 2];
    }

    //printf("unknown GPU3D read32 %08X\n", addr);
    return 0;
}

// one-shot log to find games that use the 3D clear bitmap (DISP3DCNT bit 14), so the GL and
// compute renderers' handling of it can be checked against the software renderer
static void NoteClearBitmapUse(u32 dispcnt)
{
    static bool logged = false;
    if (logged || !(dispcnt & (1<<14))) return;
    logged = true;
    Log(LogLevel::Debug, "GPU3D: DISP3DCNT bit 14 set, game uses the 3D clear bitmap\n");
}

void GPU3D::Write8(u32 addr, u8 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000601:
        if (val & 0x80)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        return;
    case 0x04000603:
        val &= 0xC0;
        GXStat &= 0x3FFFFFFF;
        GXStat |= (val << 24);
        CheckFIFOIRQ();
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        ((u8*)EdgeTable)[addr - 0x04000330] = val;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        FogDensityTable[addr - 0x04000360] = val & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        ((u8*)ToonTable)[addr - 0x04000380] = val;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write8 %08X %02X\n", addr, val);
}

void GPU3D::Write16(u32 addr, u16 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000060:
        DispCnt = (val & 0x4FFF) | (DispCnt & 0x3000);
        if (val & (1<<12)) DispCnt &= ~(1<<12);
        if (val & (1<<13)) DispCnt &= ~(1<<13);
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        NoteClearBitmapUse(DispCnt);
        return;

    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000350:
        ClearAttr1 = (ClearAttr1 & 0xFFFF0000) | val;
        return;
    case 0x04000352:
        ClearAttr1 = (ClearAttr1 & 0xFFFF) | (val << 16);
        return;
    case 0x04000354:
        ClearAttr2 = (ClearAttr2 & 0xFFFF0000) | val;
        return;
    case 0x04000356:
        ClearAttr2 = (ClearAttr2 & 0xFFFF) | (val << 16);
        return;

    case 0x04000358:
        FogColor = (FogColor & 0xFFFF0000) | val;
        return;
    case 0x0400035A:
        FogColor = (FogColor & 0xFFFF) | (val << 16);
        return;
    case 0x0400035C:
        FogOffset = val & 0x7FFF;
        return;

    case 0x04000600:
        if (val & 0x8000)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        return;
    case 0x04000602:
        val &= 0xC000;
        GXStat &= 0x3FFFFFFF;
        GXStat |= (val << 16);
        CheckFIFOIRQ();
        return;

    case 0x04000610:
        val &= 0x7FFF;
        ZeroDotWLimit = (val * 0x200) + 0x1FF;
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        EdgeTable[(addr - 0x04000330) >> 1] = val;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        addr -= 0x04000360;
        FogDensityTable[addr] = val & 0x7F;
        FogDensityTable[addr+1] = (val >> 8) & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        ToonTable[(addr - 0x04000380) >> 1] = val;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write16 %08X %04X\n", addr, val);
}

void GPU3D::Write32(u32 addr, u32 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000060:
        DispCnt = (val & 0x4FFF) | (DispCnt & 0x3000);
        if (val & (1<<12)) DispCnt &= ~(1<<12);
        if (val & (1<<13)) DispCnt &= ~(1<<13);
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        NoteClearBitmapUse(DispCnt);
        return;

    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000350:
        ClearAttr1 = val;
        return;
    case 0x04000354:
        ClearAttr2 = val;
        return;

    case 0x04000358:
        FogColor = val;
        return;
    case 0x0400035C:
        FogOffset = val & 0x7FFF;
        return;

    case 0x04000600:
        if (val & 0x8000)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        val &= 0xC0000000;
        GXStat &= 0x3FFFFFFF;
        GXStat |= val;
        CheckFIFOIRQ();
        return;

    case 0x04000610:
        val &= 0x7FFF;
        ZeroDotWLimit = (val * 0x200) + 0x1FF;
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000440)
    {
        WriteToGXFIFO(val);
        return;
    }

    if (addr >= 0x04000440 && addr < 0x040005CC)
    {
        CmdFIFOEntry entry {};
        entry.Command = (addr & 0x1FC) >> 2;
        entry.Param = val;
        CmdFIFOWrite(entry);
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        addr = (addr - 0x04000330) >> 1;
        EdgeTable[addr] = val & 0xFFFF;
        EdgeTable[addr+1] = val >> 16;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        addr -= 0x04000360;
        FogDensityTable[addr] = val & 0x7F;
        FogDensityTable[addr+1] = (val >> 8) & 0x7F;
        FogDensityTable[addr+2] = (val >> 16) & 0x7F;
        FogDensityTable[addr+3] = (val >> 24) & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        addr = (addr - 0x04000380) >> 1;
        ToonTable[addr] = val & 0xFFFF;
        ToonTable[addr+1] = val >> 16;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write32 %08X %08X\n", addr, val);
}

void GPU3D::Blit(const GPU& gpu) noexcept
{
    if (CurrentRenderer)
        CurrentRenderer->Blit(gpu);
}

Renderer3D::Renderer3D(bool Accelerated)
: Accelerated(Accelerated)
{ }

}

