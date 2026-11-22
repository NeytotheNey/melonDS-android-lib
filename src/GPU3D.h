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

#ifndef GPU3D_H
#define GPU3D_H

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "Savestate.h"
#include "FIFO.h"
#include "GPU3D_VertexId.h"
#include "GPU3D_HalfCamera.h"
#include "GPU3D_SceneStats.h"

namespace melonDS
{
class GPU;

struct Vertex
{
    s32 Position[4];
    s32 Color[3];
    s16 TexCoords[2];

    bool Clipped;

    // final vertex attributes.
    // allows them to be reused in polygon strips.

    s32 FinalPosition[2];
    s32 FinalColor[3];

    // hi-res position (4-bit fractional part)
    // TODO maybe: hi-res color? (that survives clipping)
    s32 HiresPosition[2];

    void DoSavestate(Savestate* file) noexcept;
};

// a vertex with only what clipping looks at, for WideMelon's native view (see GPU3D::NativeClipVertices)
struct ClipPosition
{
    s32 Position[4];
    bool Clipped;
};

struct Polygon
{
    Vertex* Vertices[10];
    u32 NumVertices;

    s32 FinalZ[10];
    s32 FinalW[10];
    bool WBuffer;
    // FinalZ is GPU::PreciseDepth's (Z-buffer only), for the OpenGL renderer's vertex packing. not in
    // savestates: a loaded scene has it off
    bool PreciseDepth;

    u32 Attr;
    u32 TexParam;
    u32 TexPalette;

    bool Degenerate;

    bool FacingView;
    bool Translucent;

    bool IsShadowMask;
    bool IsShadow;

    int Type; // 0=regular 1=line

    u32 VTop, VBottom; // vertex indices
    s32 YTop, YBottom; // Y coords
    s32 XTop, XBottom; // associated X coords

    u32 SortKey;

    // which of its scene's cameras it was built under (GPU3D::SceneCameras), GPU3D::NoSceneCamera when the table was full or the
    // polygon came from a savestate, which doesn't hold it
    u8 Camera;

    void DoSavestate(Savestate* file) noexcept;
};

class Renderer3D;
class NDS;

class GPU3D
{
public:
    GPU3D(melonDS::NDS& nds, std::unique_ptr<Renderer3D>&& renderer = nullptr) noexcept;
    // a GPU3D that only runs replayed commands for HalfCamera: no renderer, and nothing reaches the NDS
    struct ShadowTag {};
    GPU3D(melonDS::NDS& nds, ShadowTag) noexcept;
    ~GPU3D() noexcept = default;
    void Reset() noexcept;

    void DoSavestate(Savestate* file) noexcept;

    void SetEnabled(bool geometry, bool rendering) noexcept;

    void ExecuteCommand() noexcept;

    s32 CyclesToRunFor() const noexcept;
    void Run() noexcept;
    void CheckFIFOIRQ() noexcept;
    void CheckFIFODMA() noexcept;

    void VCount144(GPU& gpu) noexcept;
    void VBlank() noexcept;
    void VCount215(GPU& gpu) noexcept;

    void RestartFrame(GPU& gpu) noexcept;
    void Stop(const GPU& gpu) noexcept;

    void SetRenderXPos(u16 xpos) noexcept;
    [[nodiscard]] u16 GetRenderXPos() const noexcept { return RenderXPos; }

    // width of the 3D view in native pixels (256 = stock DS) that geometry is being built for.
    // it follows the renderer's GetViewWidth(), latched when the polygon RAM is swapped (or at
    // VBlank while the geometry engine is off)
    [[nodiscard]] int GetViewWidth() const noexcept { return ViewWidth; }
    // and the vertical margin, the 3D rows above and below the 192 (0 unless the width is wide),
    // latched with it from the renderer's GetViewMargin() (0 with GPU::WideY off)
    [[nodiscard]] int GetViewMargin() const noexcept { return ViewMargin; }
    u32* GetLine(int line) noexcept { return GetLine(line, RenderXPos, AbortFrame); }
    // with the X scroll and abort state a line had when it was built, for a display capture
    // line that is drawn later (see GPU::Begin2DJob)
    u32* GetLine(int line, u16 xpos, bool abort) noexcept;

    void WriteToGXFIFO(u32 val) noexcept;

    [[nodiscard]] bool IsRendererAccelerated() const noexcept;
    // the accelerated 2D line format (planes + control word): the GL renderers, or the wide check
    [[nodiscard]] bool UsesAccelLines() const noexcept { return IsRendererAccelerated() || WideCheck; }

    // the PC wide check (tools/swi-harness --wide-check), 0 = off, as everywhere else. geometry is
    // built for this width whatever the renderer says (256 = wide off), 2D writes the accelerated
    // line format (with the wings bits above 256), and GetLine hands display capture a fixed
    // pattern, since software 3D can't draw the wide middle like the GL renderers. polygons are
    // timed alike (unless not WideCheckEqualTiming) and box tests answered as stock (unless
    // WideCheckSqueezeBoxTest), as both are
    // game-visible by design (see SubmitPolygon, BoxTest). set it before SetRenderer3D, which
    // sizes the framebuffers. the counts are for its report
    int WideCheck = 0;
    bool WideCheckSqueezeBoxTest = false;
    // false: polygons timed as WideMelon times them (by the native view), not alike in both runs
    bool WideCheckEqualTiming = true;
    u32 WideCheckBoxTests = 0, WideCheckBoxHits = 0, WideCheckCountReads = 0;
    u32 WideCheckBoxRetryHits = 0; // box tests that missed natively and hit on the wide retry
    u32 WideCheckCountHash = 0; // of the values the polygon RAM count reads returned, in order
    void NoteWideCheckCount(u32 addr, u32 value) noexcept
    {
        WideCheckCountReads++;
        WideCheckCountHash = (WideCheckCountHash ^ value ^ (addr << 24)) * 16777619u;
    }

    [[nodiscard]] Renderer3D& GetCurrentRenderer() noexcept { return *CurrentRenderer; }
    [[nodiscard]] const Renderer3D& GetCurrentRenderer() const noexcept { return *CurrentRenderer; }
    void SetCurrentRenderer(std::unique_ptr<Renderer3D>&& renderer) noexcept;

    u8 Read8(u32 addr) noexcept;
    u16 Read16(u32 addr) noexcept;
    u32 Read32(u32 addr) noexcept;
    void Write8(u32 addr, u8 val) noexcept;
    void Write16(u32 addr, u16 val) noexcept;
    void Write32(u32 addr, u32 val) noexcept;
    void Blit(const GPU& gpu) noexcept;

    // debug only: logs VERTEXID lines (see GPU3D_VertexId.h). 0 is off, 1 a line per second, 2 a line
    // per scene as well. set before the game runs
    void SetVertexIdLogging(int mode) noexcept;
    [[nodiscard]] int GetVertexIdLogging() const noexcept { return VertexIdLog ? VertexIdLog->GetMode() : 0; }

    // a scene's cameras: each projection and viewport its polygons were built under, with how many of them, counted as they're built
    // (SubmitPolygon) so the camera a frontend's effects get with the depth is the one most of the scene used, whatever is current at
    // the swap. a polygon's Camera is its index here
    static constexpr u32 MaxSceneCameras = 16;
    static constexpr u8 NoSceneCamera = 0xFF;
    struct CameraCount
    {
        s32 Proj[16];
        u32 Viewport[6];
        u32 Polygons;
    };
    // the camera picked for a scene at the swap (PickSceneCamera): the one with the most polygons, raw as the DS has it
    struct SceneCamera
    {
        s32 Proj[16] {};
        u32 Viewport[6] {};
        // its index in the scene's cameras (Polygon::Camera), NoSceneCamera for a scene with none
        u8 Index = NoSceneCamera;
        // whether positions can be rebuilt from the depth with it: the scene is Z-buffered, the projection is a perspective one, at
        // least half the scene's polygons use it, and every polygon was counted (not after a savestate load mid-scene)
        bool Valid = false;
    };
    // the render list's (RenderPolygonRAM), picked when the list was latched, or HalfCamera's in-between scene's while that's shown.
    // cleared with the rendering state
    SceneCamera RenderCamera {};
    // a scene's cameras as they were counted, so a renderer can look up a polygon's own projection by its Camera (the OpenGL
    // renderer's sun shadows). RenderCameras is the render list's, latched, replaced and cleared with RenderCamera
    struct CameraTable
    {
        std::array<CameraCount, MaxSceneCameras> Cameras {};
        u32 Count = 0;
    };
    CameraTable RenderCameras {};

    // debug only: each scene's cameras as they were counted, for the desktop's --camera-dump. costs a null check per swap while off
    struct CameraLog
    {
        std::array<CameraCount, MaxSceneCameras> Cameras {};
        u32 NumCameras = 0;
        // polygons built while the table was full, and the scene's polygons
        u32 Other = 0, Polygons = 0;
        // what the swap picked from them (RenderCamera then)
        SceneCamera Pick {};
        // swaps latched since the logging went on, so a reader can tell a new scene
        u32 Swaps = 0;
    };
    // set before the game runs
    void SetCameraLogging(bool enable) noexcept { CameraLogData = enable ? std::make_unique<CameraLog>() : nullptr; }
    [[nodiscard]] const CameraLog* GetCameraLog() const noexcept { return CameraLogData.get(); }

    // polygons dropped because the polygon or vertex RAM was full, since the last reset, and of those,
    // with WideMelon on, the ones the native view would still have had room for, so the wide view cost
    // them (an estimate, see SubmitPolygon), and the ones only the wide view kept (wing). not serialized
    u32 DroppedPolygons = 0, DroppedNativeFit = 0, DroppedWing = 0;
    // the polygon and vertex RAM counts the native view would have so far, with WideMelon on (for the wide check)
    [[nodiscard]] u32 GetNativeNumPolygons() const noexcept { return NativeNumPolygons; }
    [[nodiscard]] u32 GetNativeNumVertices() const noexcept { return NativeNumVertices; }
    // debug only: logs a WIDEDROPS line once a second on those. set before the game runs
    void SetWideDropLogging(bool enable) noexcept { WideDrops = {}; WideDrops.Enabled = enable; }
    [[nodiscard]] bool GetWideDropLogging() const noexcept { return WideDrops.Enabled; }

    // the debug switch for an in-between 3D scene from a halfway camera (see GPU3D_HalfCamera.h). 0 is
    // off, 1 the camera only, 2 characters too, 3 logs the camera per scene as well, 4 also moves
    // billboards with the camera, 5 also logs every pair. set before the game runs
    void SetHalfCamera(int mode) noexcept;
    // the setting: the behavior of mode 4 with no logging, between frames at any time. nothing is
    // allocated until the first time it's on, and after that off only pauses it. not with the debug switch
    void SetHalfCameraSmoothing(bool isEnabled) noexcept;
    [[nodiscard]] int GetHalfCamera() const noexcept { return HalfCam ? HalfCam->GetMode() : 0; }
    // from GPU::StartScanline
    void NoteDisplayCapture(u32 captureCnt) noexcept { if (HalfCam) HalfCam->NoteCapture(captureCnt); }
    [[nodiscard]] bool NeedsRealSceneRedraw() const noexcept { return HalfCam && HalfCam->NeedsRedraw(); }
    void RedrawRealScene(GPU& gpu) noexcept { HalfCam->Redraw(gpu); }
    // GPU::ComputePace 2: whether leaving this frame's render out can't disturb HalfCamera (see
    // HalfCamera::CanSkipRender). true without one
    [[nodiscard]] bool CanHalfCameraSkipRender() const noexcept { return !HalfCam || HalfCam->CanSkipRender(); }
private:
    friend class HalfCamera;

    melonDS::NDS& NDS;
    typedef union
    {
        u64 _contents;
        struct
        {
            u32 Param;
            u8 Command;
        };

    } CmdFIFOEntry;

    void UpdateClipMatrix() noexcept;
    void ResetRenderingState() noexcept;
    // the latched state (the Render fields) was replaced outside of VBlank: by a reset, a loaded
    // state or the rendering engine going off. the image the renderers hold isn't of it, so the
    // frame isn't identical until one VBlank has gone by with it (RenderStateReplaced), and the
    // scene copy of MatchRenderScene isn't the last latched scene any more
    void MarkRenderStateReplaced() noexcept;
    bool RenderStateReplaced = true;
    // line 215 ran since the last VBlank (VCount215). not serialized: a loaded state marks the
    // latched state replaced anyway, and a wrong value here only costs one render or one match
    bool RenderedSinceVBlank = false;
    // MatchRenderScene's copy of the last swap's scene: the view width, the polygon count, then
    // each polygon's fields and its vertices'. SceneCopy only grows, SceneCopyWords is what's used
    std::vector<u32> SceneCopy;
    size_t SceneCopyWords = 0;
    bool SceneCopyValid = false;
    void AddCycles(s32 num) noexcept;
    void NextVertexSlot() noexcept;
    void StallPolygonPipeline(s32 delay, s32 nonstalldelay) noexcept;
    void SubmitPolygon() noexcept;
    void SubmitVertex() noexcept;
    void CalculateLighting() noexcept;
    void BoxTest(const u32* params) noexcept;
    void PosTest() noexcept;
    void VecTest(u32 param) noexcept;
    void CmdFIFOWrite(const CmdFIFOEntry& entry) noexcept;
    CmdFIFOEntry CmdFIFORead() noexcept;
    void RunCommand(CmdFIFOEntry entry) noexcept;
    // one FIFO word of a replayed scene. only for the shadow, it does what ExecuteCommand does without the FIFO
    void ReplayEntry(u8 command, u32 param) noexcept;
    [[nodiscard]] static u32 NumCommandParams(u8 command) noexcept;
    void FinishWork(s32 cycles) noexcept;
    void VertexPipelineSubmitCmd() noexcept
    {
        // vertex commands 0x24, 0x25, 0x26, 0x27, 0x28
        if (!(VertexSlotsFree & 0x1)) NextVertexSlot();
        else                          AddCycles(1);
        NormalPipeline = 0;
    }

    void VertexPipelineCmdDelayed6() noexcept
    {
        // commands 0x20, 0x30, 0x31, 0x72 that can run 6 cycles after a vertex
        if (VertexPipeline > 2) AddCycles((VertexPipeline - 2) + 1);
        else                    AddCycles(NormalPipeline + 1);
        NormalPipeline = 0;
    }

    void VertexPipelineCmdDelayed8() noexcept
    {
        // commands 0x29, 0x2A, 0x2B, 0x33, 0x34, 0x41, 0x60, 0x71 that can run 8 cycles after a vertex
        if (VertexPipeline > 0) AddCycles(VertexPipeline + 1);
        else                    AddCycles(NormalPipeline + 1);
        NormalPipeline = 0;
    }

    void VertexPipelineCmdDelayed4() noexcept
    {
        // all other commands can run 4 cycles after a vertex
        // no need to do much here since that is the minimum
        AddCycles(NormalPipeline + 1);
        NormalPipeline = 0;
    }

    std::unique_ptr<Renderer3D> CurrentRenderer = nullptr;

    u16 RenderXPos = 0;
    int ViewWidth = 256;
    int RendererViewWidth() const noexcept;
    // WideMelon camera stage 2: the margin, 0..128 rows above and below the 192, so the view is
    // 192 + 2 * ViewMargin rows tall (ViewHeight). only with ViewWidth wide, latched with it
    int ViewMargin = 0;
    int RendererViewMargin() const noexcept;
    [[nodiscard]] int ViewHeight() const noexcept { return 192 + 2 * ViewMargin; }
    // WideMelon's wide list (GPU::WidePolys): with the view wider than 256, a bank holds up to
    // PolygonRAMSize polygons and VertexRAMSize vertices, the native view's counts take the DS's limits
    // and only they set DISP3DCNT bit 13 and feed the count registers, and the polygons only the wide view
    // keeps get a budget of their own (WingNumPolygons). latched with ViewWidth (LatchViewWidth), but kept
    // through a renderer switch mid-scene, and a state's GP3W brings its scene's. off, everything is as before
    bool WideListOn = false;
    // GPU::PreciseDepth for the scene being built, latched with ViewWidth (LatchViewWidth)
    bool PreciseDepthOn = false;
    void LatchViewWidth() noexcept;

    // WideMelon: the polygon and vertex RAM counts the native view would have (NumPolygons and NumVertices
    // without the wide view), see NativeClipVertices. derived again on a state load, not serialized
    u32 NativeNumPolygons = 0, NativeNumVertices = 0;
    // and its last strip polygon, as LastStripPolygon is for the real one: its vertex count (0 = none) and
    // which of its vertices clipping made (bit per vertex), for whether the next one shares two vertices
    u32 NativeStripVerts = 0, NativeStripClipped = 0;
    int NativeClipVertices(int sentverts, int wideverts, const Vertex* wideclipped, u32& clippedmask) noexcept;
    bool NativeZeroDot(int sentverts) noexcept;
    // whether the last NativeClipVertices came out as the squeezed view's clip (no native X plane in the way),
    // and if not, the polygon it clipped, for NativeZeroDot
    bool NativeClipSame = false;
    ClipPosition NativeClipped[10];
    int NativeClippedCount = 0;
    // the wide list: the polygons in it the native view doesn't keep (clipped away, or zero-dot there), and
    // their vertices, of NativePolygonRAMSize and NativeVertexRAMSize. in the GP3W section of a savestate
    u32 WingNumPolygons = 0, WingNumVertices = 0;
    // a polygon was dropped because the wide list or the wing budget was full, which bit 13 doesn't
    // show (HalfCamera::Replay). not serialized
    bool WideListOverflow = false;
    // the GP3W section: what GP3D's upstream layout can't hold (see DoSavestate)
    void DoWideSavestate(Savestate* file, const RenderList* renderList, u32 renderNumPolygons) noexcept;
    void WideSavestateHighWater(const RenderList* renderList, u32 renderNumPolygons, u32 polygons[2], u32 vertices[2]) const noexcept;
    [[nodiscard]] bool WideSavestateExtra(const RenderList* renderList, u32 renderNumPolygons) const noexcept;

    // the WIDEDROPS line, per scene (polygon RAM swap) and once a second
    struct WideDropStats
    {
        bool Enabled = false;
        std::chrono::steady_clock::time_point LastLog {};
        u32 Scenes = 0, DropScenes = 0, MaxSceneDrops = 0, PeakPolygons = 0, PeakVertices = 0;
        u32 PeakNativePolygons = 0, PeakNativeVertices = 0, PeakWingPolygons = 0;
        u32 SceneStartDrops = 0, SecondStartDrops = 0, SecondStartNativeFit = 0, SecondStartWing = 0;
    } WideDrops;
    void WideDropsOnSwap() noexcept;
    void WideDropsOnVBlank() noexcept;

    // the SCENESTATS line (GPU::SceneStats), at each line 215 and once a second
    SceneStatsCounters SceneStatsSec {};
    void NoteSceneStats(const GPU& gpu) noexcept;

    // the scene being built's cameras (see CameraCount), not serialized: a state load starts them again, and the pick at its swap then
    // sees polygons it didn't count. CurSceneCamera is the one the next polygon gets, looked up again once SceneCameraDirty says the
    // projection or the viewport may have changed (UpdateClipMatrix, VIEWPORT). OtherCameraPolygons were built with the table full
    std::array<CameraCount, MaxSceneCameras> SceneCameras {};
    u32 NumSceneCameras = 0;
    u32 OtherCameraPolygons = 0;
    u8 CurSceneCamera = NoSceneCamera;
    bool SceneCameraDirty = true;
    void NoteSceneCamera() noexcept;
    void ResetSceneCameras() noexcept;
    [[nodiscard]] SceneCamera PickSceneCamera() const noexcept;
    [[nodiscard]] CameraTable SceneCameraTable() const noexcept;
    // HalfCamera's: the real scene's RenderCamera and RenderCameras while the in-between scene's own are there (it keeps the real
    // list the same way)
    SceneCamera RealRenderCamera {};
    CameraTable RealRenderCameras {};

    // only allocated while the logging is on, not serialized
    std::unique_ptr<VertexIdLogger> VertexIdLog;
    std::unique_ptr<CameraLog> CameraLogData;
    std::unique_ptr<HalfCamera> HalfCam;

public:
    FIFO<CmdFIFOEntry, 256> CmdFIFO {};
    FIFO<CmdFIFOEntry, 4> CmdPIPE {};

    FIFO<CmdFIFOEntry, 64> CmdStallQueue {};

    u32 ZeroDotWLimit = 0xFFFFFF;

    u32 GXStat = 0;

    u32 ExecParams[32] {};
    u32 ExecParamCount = 0;

    s32 CycleCount = 0;
    s32 VertexPipeline = 0;
    s32 NormalPipeline = 0;
    s32 PolygonPipeline = 0;
    s32 VertexSlotCounter = 0;
    u32 VertexSlotsFree = 0;

    u32 NumPushPopCommands = 0;
    u32 NumTestCommands = 0;


    u32 MatrixMode = 0;

    s32 ProjMatrix[16] {};
    s32 PosMatrix[16] {};
    s32 VecMatrix[16] {};
    s32 TexMatrix[16] {};

    s32 ClipMatrix[16] {};
    bool ClipMatrixDirty = false;

    u32 Viewport[6] {};

    s32 ProjMatrixStack[16] {};
    s32 PosMatrixStack[32][16] {};
    s32 VecMatrixStack[32][16] {};
    s32 TexMatrixStack[16] {};
    s32 ProjMatrixStackPointer = 0;
    s32 PosMatrixStackPointer = 0;
    s32 TexMatrixStackPointer = 0;

    u32 NumCommands = 0;
    u32 CurCommand = 0;
    u32 ParamCount = 0;
    u32 TotalParams = 0;

    bool GeometryEnabled = false;
    bool RenderingEnabled = false;

    u32 DispCnt = 0;
    u8 AlphaRefVal = 0;
    u8 AlphaRef = 0;

    u16 ToonTable[32] {};
    u16 EdgeTable[8] {};

    u32 FogColor = 0;
    u32 FogOffset = 0;
    u8 FogDensityTable[32] {};

    u32 ClearAttr1 = 0;
    u32 ClearAttr2 = 0;

    u32 RenderDispCnt = 0;
    u8 RenderAlphaRef = 0;

    u16 RenderToonTable[32] {};
    u16 RenderEdgeTable[8] {};

    u32 RenderFogColor = 0;
    u32 RenderFogOffset = 0;
    u32 RenderFogShift = 0;
    u8 RenderFogDensityTable[34] {};

    u32 RenderClearAttr1 = 0;
    u32 RenderClearAttr2 = 0;

    bool RenderFrameIdentical = false; // not part of the hardware state, don't serialize
    // debug only, for the compute renderer's COMPWHY log (GPU::ComputeWhy), not serialized: what
    // the last VBlank found, which is 0 when it left RenderFrameIdentical alone. with
    // SceneHashLogging a swap also hashes what the renderers draw the scene from, and one that
    // brought the same scene as the swap before, with the same registers, gets FrameChange_FlushSame
    enum : u8
    {
        FrameChange_Flush = 1 << 0,
        FrameChange_FlushSame = 1 << 1,
        FrameChange_Regs = 1 << 2,
        FrameChange_HalfCam = 1 << 3,
    };
    u8 RenderFrameChange = 0;
    bool SceneHashLogging = false;
    u64 RenderSceneHash = 0;
    [[nodiscard]] bool RenderRegsSame() const noexcept;
    [[nodiscard]] u64 HashRenderScene() const noexcept;

    // the compute renderer's (GPU::ComputeOpt bit 5), not serialized. RenderFrameIdentical is
    // false for every swap, as upstream has it and as the other renderers count on. with
    // SceneRepeatCheck, which the compute renderer sets at each RenderFrame and a renderer switch
    // clears, a swap whose registers are the last latch's also compares its scene with a copy of
    // the last swap's, by value (MatchRenderScene), and RenderSceneRepeated is set from VBlank to
    // VBlank when that swap changed nothing a render reads. it then means what
    // RenderFrameIdentical does, and everything that clears that one outside of VBlank clears both
    // (MarkRenderFrameChanged)
    bool SceneRepeatCheck = false;
    bool RenderSceneRepeated = false;
    // for the COMPWHY log: HalfCamera's in-between scenes that came out as the real one and
    // weren't shown (HalfCamera::ShowReplayedScene), since the start
    u32 MidScenesSame = 0;
    void MarkRenderFrameChanged() noexcept
    {
        RenderFrameIdentical = false;
        RenderSceneRepeated = false;
    }
    // whether RenderPolygonRAM and RenderViewWidth are what the copy has, field by field with the
    // vertices by value. with save the copy becomes this scene, without it nothing changes
    bool MatchRenderScene(bool save) noexcept;
    int RenderViewWidth = 256; // the view width RenderPolygonRAM was built for, not serialized either
    int RenderViewMargin = 0;  // and its margin
    // and whether it was built into WideMelon's wide list (WideListOn at the swap), for the renderers'
    // list sizes. derived on a state load
    bool RenderWideList = false;

    bool AbortFrame = false;

    u64 Timestamp = 0;


    u32 PolygonMode = 0;
    s16 CurVertex[3] {};
    u8 VertexColor[3] {};
    s16 TexCoords[2] {};
    s16 RawTexCoords[2] {};
    s16 Normal[3] {};

    s16 LightDirection[4][3] {};
    s32 SpecRecip[4] {};
    u8 LightColor[4][3] {};
    u8 MatDiffuse[3] {};
    u8 MatAmbient[3] {};
    u8 MatSpecular[3] {};
    u8 MatEmission[3] {};

    bool UseShininessTable = false;
    u8 ShininessTable[128] {};

    u32 PolygonAttr = 0;
    u32 CurPolygonAttr = 0;

    u32 TexParam = 0;
    u32 TexPalette = 0;

    s32 PosTestResult[4] {};
    s16 VecTestResult[3] {};

    Vertex TempVertexBuffer[4] {};
    // the clip space X and Y of each of them before WideMelon's squeeze (the same with it off). derived
    // again on a state load, or from GP3W
    s32 TempVertexNativeX[4] {};
    s32 TempVertexNativeY[4] {};
    u32 VertexNum = 0;
    u32 VertexNumInPoly = 0;
    u32 NumConsecutivePolygons = 0;
    Polygon* LastStripPolygon = nullptr;
    u32 NumOpaquePolygons = 0;

    // two banks of VertexRAMSize and PolygonRAMSize, the DS's sizes but for WideMelon's wide list
    Vertex VertexRAM[VertexRAMSize * 2] {};
    Polygon PolygonRAM[PolygonRAMSize * 2] {};

    Vertex* CurVertexRAM = nullptr;
    Polygon* CurPolygonRAM = nullptr;
    u32 NumVertices = 0;
    u32 NumPolygons = 0;
    u32 CurRAMBank = 0;

    RenderList RenderPolygonRAM {};
    u32 RenderNumPolygons = 0;

    u32 FlushRequest = 0;
    u32 FlushAttributes = 0;
    u32 ScrolledLine[256]; // not part of the hardware state, don't serialize
};

// the camera the top screen's depth (Renderer3D::SetOutputDepthTexture) was drawn with, for a frontend that rebuilds view-space
// positions from it. a pixel at (x, y) in DS pixels from the native top screen's top-left corner, rows going down (a pixel's center is
// at +0.5; the wings and the margin rows are outside 0..256 and 0..192) with a Z-buffered 24-bit depth d is at NDC
// x = (x - Viewport[0]) * 2 / Viewport[2] - 1, y = 1 - (y - Viewport[1]) * 2 / Viewport[3], z = (d - 0x7FFE00) / 2^23, and its
// view-space position is inverse(Proj) times (x, y, z, 1), divided by its w. a game that puts its camera in the projection gets
// world space instead
struct DepthCamera
{
    // the projection most of the scene's polygons were built under, as real numbers in the DS's order, which is OpenGL's column-major
    // one: GLSL's mat4 of these 16 floats times (view x, y, z, 1) is the clip position, as the DS's row vector times its matrix is
    std::array<float, 16> Proj {};
    // its viewport: left, top, width and height in DS pixels
    std::array<float, 4> Viewport {};
    // false when the scene is W-buffered, the projection is orthographic, it covers under half the scene's polygons, or the frame
    // has no depth. Proj and Viewport still hold the scene's pick then, for logs (0 for a scene without polygons)
    bool Valid = false;
};

// a frontend's sun shadows (Renderer3D::SetSunShadows), for the characters' own: while On, the frontend wants the depth and the scene's
// camera is valid, the OpenGL renderer draws each polygon under another camera that writes depth (the depth's flag 8) a second time,
// stood upright about its bottom edge and flattened onto the ground away from the sun, and sets flag 16 where it lands. Direction
// (towards the sun) and Up (the world's up) are unit vectors in the view space of DepthCamera's projection
struct SunShadows
{
    bool On = false;
    std::array<float, 3> Direction {};
    std::array<float, 3> Up {};

    bool operator==(const SunShadows& other) const noexcept
    {
        return On == other.On && Direction == other.Direction && Up == other.Up;
    }
    bool operator!=(const SunShadows& other) const noexcept { return !(*this == other); }
};

// a frontend's hidden textures (Renderer3D::SetHiddenTextures) hold at most this many hashes
constexpr u32 MaxHiddenTextures = 64;
// and its light textures (Renderer3D::SetLightTextures) as many
constexpr u32 MaxLightTextures = 64;

// debug: the textures of the last frame the OpenGL renderer drew (Renderer3D::SetTextureListing), one entry per TexParam (but its
// repeat, flip and transform bits) and TexPalette the frame's textured polygons used, in the order they first came. Hash is the
// texture's content hash (GLTexHashes), Box the polygons' bounds (left, top, right, bottom, in the frame's 3D pixels, the native top
// screen's 256 x 192 when it's not wide), Alpha and PolygonId the first polygon's, Hidden whether the hidden textures skipped them,
// Light whether the light textures marked them
struct TextureListEntry
{
    u32 Hash;
    u32 TexParam;
    u32 TexPalette;
    u32 Polygons;
    u32 Translucent;
    s32 Box[4];
    u8 Alpha;
    u8 PolygonId;
    bool Hidden;
    bool Light;
};
struct TextureList
{
    // the frames listed so far, so a frontend can tell a new listing
    u32 Frames = 0;
    std::vector<TextureListEntry> Entries;
    // the textures past this many in a frame aren't listed
    static constexpr size_t MaxEntries = 1024;
};

// GPU::Comp1x (experiment K2c): what the compositor drew into an output texture. HalvesAt1x bit h
// (0 the top screen, 1 the bottom one) is a half drawn at 1x: its image is the first ViewWidth x 192
// texels from the half's first row (h * 194 * Scale, the rows the full draw starts it at), and the
// rest of the half's rows hold whatever they held before. Scale is the output's
//
// HalfScale (experiment K3) is the scale those halves were drawn at: 1, or with filtered 2D the
// filter map's, which Scale is then a multiple of. Their image is then the first
// ViewWidth * HalfScale x 192 * HalfScale texels from the same first row, shown nearest-upscaled by
// Scale / HalfScale. Every half in HalvesAt1x has the same one, since the filter is on or off for
// the whole frame
struct OutputLayout
{
    int HalvesAt1x = 0;
    int Scale = 1;
    int HalfScale = 1;
    // WideMelon camera: the top screen's lines with 3D wings, its own (control word bit 18) or
    // held from an earlier frame by the dual-screen wing hold
    int TopWingLines = 0;
    // WideMelon camera stage 2: the output's vertical margin (Renderer3D::GetOutputMargin when it
    // was drawn). the top half is (192 + 2 * TopMargin) * Scale rows, then 2 * Scale padding rows,
    // then the bottom screen's 192 * Scale, and the top screen's DS line y is at row
    // (y + TopMargin) * Scale. 0 is the old 386-row layout
    int TopMargin = 0;
    // whether the depth texture given with Renderer3D::SetOutputDepthTexture holds this draw's
    // top-half depth (see GLCompositor::RenderDepth), and the camera it was drawn with
    bool HasDepth = false;
    DepthCamera Camera {};
};

class Renderer3D
{
public:
    virtual ~Renderer3D() = default;

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    virtual void Reset(GPU& gpu) = 0;

    // This "Accelerated" flag currently communicates if the framebuffer should
    // be allocated differently and other little misc handlers. Ideally there
    // are more detailed "traits" that we can ask of the Renderer3D type
    const bool Accelerated;

    virtual void VCount144(GPU& gpu) {};
    virtual void Stop(const GPU& gpu) {}
    virtual void RenderFrame(GPU& gpu) = 0;
    virtual void RestartFrame(GPU& gpu) {};
    virtual u32* GetLine(int line) = 0;
    virtual void Blit(const GPU& gpu) {};

    virtual void SetupAccelFrame() {}
    virtual void PrepareCaptureFrame() {}
    // submits the queued GPU work without waiting for it, so the GPU starts on what display
    // capture will read back (see GPU::StartHBlank at line 215 and SoftRenderer::VBlankEnd)
    virtual void FlushCommands() {}
    // whether RenderFrame can be given polygons from HalfCamera's buffers: it only reads them during the
    // call, on the emulator thread, so they can change after it returns
    [[nodiscard]] virtual bool CanDrawReplayedScene() const noexcept { return false; }
    // whether GPU::BlitFlushCapture flushes right after the line-192 Blit in capture frames. only
    // the compute renderer gains from it; with classic OpenGL it cost ~9% more GPU cycles for no fps
    virtual bool FlushAfterCaptureBlit() const { return false; }
    // GPU::BlitAfterReadback, which only runs with a renderer that has these. at line 215, before
    // RenderFrame: true when the render can keep the 3D frame it replaces for the Blit after it
    // (SetupAccelFrame binds that one until the Blit is done), false when the Blit has to go first
    [[nodiscard]] virtual bool SupportsBlitAfterRender() const noexcept { return false; }
    virtual bool KeepFrameForBlit(const GPU& gpu) { return false; }
    // PrepareCaptureFrame right after the render at line 215, for the next frame's capture, into a
    // buffer of its own. at line 0, with use set (where PrepareCaptureFrame would run), the take
    // makes it the one GetLine maps and returns true, if it still holds what PrepareCaptureFrame
    // would read back (nothing drew into the 3D frame or resized it since). either way it's dropped,
    // and one that isn't used leaves GetLine's buffer as the old path would
    virtual void PrepareEarlyCaptureFrame() {}
    virtual bool TakeEarlyCaptureFrame(bool use) { return false; }
    virtual void SetOutputTexture(int buffer, u32 texture) {}
    virtual void BindOutputTexture(int buffer) {}
    // a frontend's post effects (depth of field): an RGBA8 texture the output's width by its top
    // half's rows ((192 + 2 * margin) * scale) for output buffer [buffer], which the compositor
    // fills with the top screen's 24-bit depth and flags (kCompositorDepthFS), or 0 for none. given before each frame like the output
    // texture; while none is given the renderer keeps no depth and nothing costs more. only the
    // OpenGL renderer has one, the others never set OutputLayout::HasDepth
    virtual void SetOutputDepthTexture(int buffer, u32 texture) {}
    // for the compositor's depth pass: binds the 3D depth of the frame SetupAccelFrame binds to the
    // active texture unit, with the camera it was drawn with in [camera], and returns false when there's none to bind
    virtual bool SetupAccelDepth(DepthCamera& camera) { return false; }
    // the frontend's sun shadows (see SunShadows), given before each frame like the depth texture. only the OpenGL renderer draws them
    virtual void SetSunShadows(const SunShadows& sun) {}
    // the frontend's hidden textures, given before each frame like the sun shadows: [count] content hashes (GLTexHashes, at most
    // MaxHiddenTextures are taken), whose polygons the OpenGL renderer doesn't draw at all, as if the game hadn't sent them, while the
    // game's own state stays as it is. 0 shows them all and costs nothing. the other renderers draw everything
    virtual void SetHiddenTextures(const u32* hashes, u32 count) {}
    // the frontend's light textures, given the same way: [count] content hashes (at most MaxLightTextures are taken), whose
    // polygons' pixels get flag 32 in the depth's flags (kDepthResolveFS) where they write depth, so effects can make them glow.
    // they're drawn as they are. 0 marks none and costs nothing. only the OpenGL renderer marks them
    virtual void SetLightTextures(const u32* hashes, u32 count) {}
    // debug: while on, the OpenGL renderer lists each frame's textures it draws (TextureList), nullptr from the others or while off
    virtual void SetTextureListing(bool on) {}
    [[nodiscard]] virtual const TextureList* GetTextureList() const { return nullptr; }
    // GPU::Comp1x (experiment K2c): how the compositor last drew into output buffer [buffer]. true,
    // with [layout], the first time it's asked after a draw into the buffer, false when nothing drew
    // into it since then (or since SetOutputTexture gave it a texture), so the texture still holds
    // whatever it held, and the layout that went with that
    virtual bool TakeOutputLayout(int buffer, OutputLayout& layout) { return false; }
    // after a savestate is loaded, for what the renderer keeps from earlier frames' output
    virtual void SavestateLoaded() {}

    // WideMelon, in native pixels: the width new geometry should be built for, and the width of
    // the output right now (it can lag behind for the polygons still being drawn).
    // only the GL and compute renderers go wider than 256
    [[nodiscard]] virtual int GetViewWidth() const noexcept { return 256; }
    [[nodiscard]] virtual int GetOutputWidth() const noexcept { return 256; }
    // and the vertical margin (camera stage 2): the 3D rows above and below the top screen's 192
    // new geometry is built for, and the margin of the output right now, which lags like the width.
    // 0 unless the width is wide
    [[nodiscard]] virtual int GetViewMargin() const noexcept { return 0; }
    [[nodiscard]] virtual int GetOutputMargin() const noexcept { return 0; }

    // hi-res display capture (GPU::HiresCapture): whether the GL compositor's capture pass would
    // run with this frame's capture (GLCompositor::RunsHiresCapture). off without a compositor
    [[nodiscard]] virtual bool RunsHiresCapture(int debug) const noexcept { return false; }

    // debug only (GPU::ComputeTimers and ComputeSync): the GL compositor times its passes
    // (ComputePass) through these, with the compute renderer's timers. a begin ends the pass before it
    virtual void BeginGPUTimer(int pass) {}
    virtual void EndGPUTimer() {}

    // debug only (GPU::Hash3DLogging), right after RenderFrame at line 215: logs, now or some frames
    // later, a HASH3D line with a checksum of the 3D image this render left (LogHash3D, in GetLine's
    // pixel format, at native resolution), as frame, the HASH2D frame that shows it. nothing may wait
    // on the GPU for it. without an override no line is logged
    virtual void QueueHash3D(GPU& gpu, u32 frame) {}

    // GPU::ComputePace 2: the last render was left out to let the GPU catch up, so the 3D image
    // is older than the scene. GPU then has it drawn after all (RedrawSkippedFrame) at line 0
    // before a display capture reads it back, and before the 3D state it's drawn from is cleared.
    // the redraw is the render that was left out: same latched state, same textures
    [[nodiscard]] virtual bool NeedsCaptureRedraw() const noexcept { return false; }
    virtual void RedrawSkippedFrame(GPU& gpu) {}

#ifdef MELONADE_DEBUG_TOOLS
    // debug builds only, for the frontend's CAPWAIT log, compute renderer only: summed over the
    // capture readback maps since the frontend last zeroed them. the wait is the time in
    // glMapBufferRange, the spin the CaptureSpinUs poll before it, and the span the time from
    // RenderFrame's start to the map's return
    s64 CaptureMapCount = 0;
    s64 CaptureMapWaitNs = 0;
    s64 CaptureSpinNs = 0;
    s64 CaptureSpanNs = 0;
#endif

    virtual bool NeedsShaderCompile() { return false; }
    virtual void ShaderCompileStep(int& current, int& count) {}

protected:
    Renderer3D(bool Accelerated);
};

}

#endif
