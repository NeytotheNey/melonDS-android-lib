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

#ifndef GPU3D_HALFCAMERA_H
#define GPU3D_HALFCAMERA_H

#include <array>
#include <chrono>
#include <memory>

#include "types.h"

namespace melonDS
{
class GPU;
class GPU3D;
struct Polygon;

// experiment, debug only (debug.melonade.halfcam, see DebugProperties.h). for games that draw a new
// 3D scene every other frame: the frame a new scene B arrives shows scene B's own geometry seen from a
// camera halfway between scene A's and B's, and the next frame shows real B, so only the 3D is a frame
// late. the shelved polygon matching (docs/shelved/frame-interpolation.md) isn't needed for geometry
// that doesn't move, and clipping and culling run again for the polygons scene B sent. it can't bring
// back what the game itself culled for B (BoxTest per block and per prop), so geometry leaving the view
// can be gone from the in-between scene, which is a frame early at that screen edge.
//
// the scene's commands are recorded as they execute (the FIFO words between two swaps), plus the
// geometry state at its start. that was picked over a per-vertex record because then lighting, texture
// coordinates, matrix stacks, strips and anything else come out of the same code as the real scene, and
// a matrix pushed or multiplied after the camera load picks up the halfway camera by itself. all of it
// runs in a second GPU3D (Shadow), which has its own matrices, polygon RAM and vertex RAM and is never
// serialized. the commands don't touch anything outside it: the FIFOs, the IRQs and the DMAs are in
// ExecuteCommand's FIFO read, which replay doesn't go through.
//
// the camera is found from the data: the position-mode MTX_LOAD_4x3 with the most vertices under it,
// counted over the recorded stream at the swap. every load of it in scene B is replaced by the average
// with scene A's, which puts each vertex at the midpoint of its two view space positions. other 4x3
// loads close to the camera are moved by the same amount, as the character scene sends its own load.
//
// characters (mode 2 and up): BillboardActorScene_Draw (White 2 0x0204ebdc) sends, per actor, PUSH,
// MTX_MULT_4x3 (billboard rotation and position), MTX_MULT_3x3, SCALE, material, BEGIN, a quad by the
// 0x488 and 0x48C ports, END, POP. a "batch" is therefore the few vertices drawn after an MTX_MULT_4x3 in
// a position mode, under a camera load. batches whose words aren't in the other scene are moving ones,
// which are paired by order of appearance, by shape (primitive type and vertex count), within a distance
// on screen and with close rotation words, and the paired ones get the average of the MTX_MULT_4x3's 12
// words. their scale and vertices stay B's. the others stay at B's placement. static geometry is never
// paired, its words are the same in both scenes.
//
// HALFCAM sec, once a second:
//   vb scenes interp      VBlanks, swaps, in-between scenes drawn
//   nolock ...            scenes that showed the real one instead, by reason:
//     nolock   the swaps aren't on every other frame
//     render   the renderer can't draw it (only the compute renderer does)
//     cap      display capture ran recently, is armed, or was armed by line 215 (capab) or line 0 (capre).
//              a line 0 redraw reads VRAM later than line 215 would, so with capre above 0, or loadre (the
//              redraw after a state load or reset that interrupted an in-between frame), a run can't be
//              treated as bit-exact
//     stream   no full stream for A or B: after a reset or state load, or an overflow (recovf)
//     share    A or B has no 4x3 load with at least 90% of the vertices
//     far      the cameras are too far apart (a cut or transition, see MaxRotationStep)
//     still    same camera hash and nothing paired can move, so it would draw the same picture
//     empty    no polygons
//     full     the replay ran out of polygon or vertex RAM
//   sharepct minshare     vertices under the top load over this second, and the lowest scene's
//   near nearstatic       commands of other 4x3 loads close to the camera: moved with it, and left alone
//                         because the same load is in the previous scene. added up
//   paired unpaired       moving batches of B with and without a partner, added up
//   pairrot               of unpaired, those that only failed on the rotation words (a mirrored sprite)
//   batchovf              batches over the table size
//   anaus replayus maxus  average analysis time per scene, average and worst replay time
//   subv poly             average vertices submitted and polygons kept per replay
//
// HALFCAM cam, per scene in mode 3: the top load's translation, its delta from the last scene's, the
// largest change in a rotation word, its hash and share, and what was done with the scene
class HalfCamera
{
public:
    HalfCamera(GPU3D& gpu, int mode);
    ~HalfCamera();

    [[nodiscard]] int GetMode() const noexcept { return Mode; }

    // a FIFO word as it starts executing
    void Record(u8 command, u32 param) noexcept
    {
        if (NumRecords < MaxRecords)
            Records[NumRecords++] = { param, command };
        else
            IsOverflowed = true;
    }

    // first thing at VBlank: goes back to the real scene's list. returns whether the last frame drew
    // the in-between scene, which means this one must draw even if nothing changed
    bool OnVBlank();
    // with a finished scene in the stream, just before the polygon RAM banks swap
    void OnSwap();
    // just after the swap, the next scene's commands start here
    void OnSceneStart();

    // display capture ran (GPU::StartScanline) or is armed. capturing only source B doesn't count
    void NoteCapture(u32 captureCnt) noexcept;
    // before the render at line 215: a capture armed by now gets the real scene instead
    void BeforeRender();
    // at the start of line 0, the last moment before GPU2D's VBlankEnd reads the 3D back: a capture
    // armed after line 215 needs the frame drawn again without the in-between scene, and so does
    // anything after a load that interrupted an in-between frame
    [[nodiscard]] bool NeedsRedraw() const noexcept;
    void Redraw(GPU& gpu);

    // a reset or a state load: nothing recorded so far can be trusted
    void Invalidate();
    // what a savestate has to contain: never the in-between scene
    void GetRealRenderList(const std::array<Polygon*, 2048>*& list, u32& numPolygons) const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    static constexpr u32 MaxRecords = 1u << 18;

    // the swaps are on every other frame, 8 VBlanks in a row, ending with this one
    static constexpr u32 LockMask = 0xFF;
    static constexpr u32 LockPattern = 0x55;
    // VBlanks after a capture with no in-between scenes, as a game that captures will likely do it again
    static constexpr u32 CaptureHoldFrames = 16;

    struct RecordEntry
    {
        u32 Param;
        u8 Command;
    };

    enum Reason : u32
    {
        Reason_None,
        Reason_NotLocked,
        Reason_Renderer,
        Reason_Capture,
        Reason_Stream,
        Reason_Share,
        Reason_Far,
        Reason_Still,
        Reason_Empty,
        Reason_Full,
        Reason_Count,
    };

    struct GeometryState;
    struct SceneInfo;

    struct Totals
    {
        u32 VBlanks, Scenes, Replays, Interpolated;
        std::array<u32, Reason_Count> Skipped;
        u32 CaptureAborted, CaptureRedrawn, LoadRedrawn, RecordOverflows, BatchOverflows;
        u64 Vertices, TopVertices;
        float MinShare;
        u32 NearLoads, NearStatic, Paired, Unpaired, PairRotation;
        s64 AnalysisNs, ReplayNs, MaxReplayNs;
        u64 ReplayVertices, ReplayPolygons;
    };

    void Analyze(SceneInfo& scene);
    void FindMoving(const SceneInfo& current, const SceneInfo& previous);
    [[nodiscard]] Reason Decide(const SceneInfo& current, const SceneInfo& previous) const;
    bool Replay(const SceneInfo& current, const SceneInfo& previous);
    void PatchCamera(const SceneInfo& current, const SceneInfo& previous);
    void PairBatch(u32 movingIndex, const SceneInfo& current, const SceneInfo& previous);
    [[nodiscard]] bool IsNearOnScreen(const s32* wordsA, const s32* wordsB) const;
    void ShowReplayedScene();
    void RestoreRealScene();
    void LogScene(const SceneInfo& current, const SceneInfo& previous, Reason reason) const;
    void LogSecond(Clock::time_point now);

    GPU3D& Main;
    int Mode;

    // only ever used on the emulator thread, and not freed until the emulator goes away or the switch
    // is set again before a game starts, never from VBlank
    std::unique_ptr<GPU3D> Shadow;

    std::unique_ptr<RecordEntry[]> Records;
    u32 NumRecords = 0;
    bool IsOverflowed = false;

    std::unique_ptr<GeometryState> Start;
    bool HasStart = false;

    // the scene being finished and the last one, by [Bank] and [Bank ^ 1]
    std::array<std::unique_ptr<SceneInfo>, 2> Scenes;
    u32 Bank = 0;
    // moving batches, as indices into the scene's batches. the previous scene's are only moving compared
    // to the current scene, so they aren't kept with it
    std::unique_ptr<u16[]> MovingCurrent;
    std::unique_ptr<u16[]> MovingPrevious;
    u32 NumMovingCurrent = 0;
    u32 NumMovingPrevious = 0;
    u32 PairCursor = 0;

    u32 History = 0;
    u32 FramesSinceCapture = 0;

    // RenderPolygonRAM holds the in-between scene, RealList what it had
    bool IsMidActive = false;
    // Invalidate ended an in-between frame, and the renderer's output can still be that picture
    bool IsMidPictureStale = false;
    u32 MidNumPolygons = 0;
    u32 RealNumPolygons = 0;
    std::array<Polygon*, 2048> RealList {};

    u32 SceneNumber = 0;
    Totals Total {};
    Clock::time_point LastLog;
};

}

#endif
