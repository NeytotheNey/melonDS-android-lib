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
#include <vector>

#include "types.h"

namespace melonDS
{
class GPU;
class GPU3D;
struct Polygon;

// a bank of the polygon and vertex RAM as the DS has it, and as GPU3D keeps it: WideMelon's wide list
// (GPU::WidePolys, see GPU3D::SubmitPolygon) can hold twice that. here as GPU3D.h includes this first
constexpr u32 NativePolygonRAMSize = 2048, NativeVertexRAMSize = 6144;
constexpr u32 PolygonRAMSize = 4096, VertexRAMSize = 12288;
// a bank's sorted polygons (GPU3D::RenderPolygonRAM)
using RenderList = std::array<Polygon*, PolygonRAMSize>;

// experiment, behind the Smooth 30 FPS setting (mode 4, quiet) and, in debug builds, debug.melonade.halfcam
// (see DebugProperties.h). for games that draw a new
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
// the lines below are the logging of modes 1 to 5. a quiet HalfCamera, which is what the setting uses, logs none of
// them, and otherwise behaves the same as that mode.
//
// HALFCAM sec, once a second:
//   vb scenes interp      VBlanks, swaps, in-between scenes drawn
//   nolock ...            scenes that showed the real one instead, by reason:
//     nolock   the swaps aren't on every other frame
//     render   the renderer can't draw it (only the compute and OpenGL renderers do)
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
// HALFCAM cam, per scene in mode 3 and up: the top load's translation, its delta from the last scene's, the
// largest change in a rotation word, its hash and share, and what was done with the scene
//
// billboards (mode 4 is mode 3 plus this): NitroSystem's SBC BB and BBY commands read the position matrix back
// on the CPU and send it as an MTX_LOAD_4x3 (see NextReadbackStep). replay can't see that read, so those words
// carry scene B's camera, and modes 1 to 3 draw the object at B's view position in a world that's halfway: half
// a camera step off, every in-between frame. an object the camera follows, like the player, hardly moves in view
// space, so it doesn't show on those. mode 4 reads the shadow's matrix at that point instead of using the sent
// translation, and keeps the batch open through that load, so walking objects are paired by their placement as
// well. the loads themselves aren't paired. their rotation and scale stay B's. a pair now moves the object for
// real, so mode 4 doesn't pair by order either (see MatchMoving): in modes 2 and 3 the load hides a wrong pair.
//
// HALFCAM diag, once a second in mode 3 and up, as batches/vertices:
//   dom same chg noload   runs of vertices under the top load, under another load also in the last scene, under
//                         another load that isn't; and vertices under no load of this scene
//   pair unp              moving batches paired and not
//   pairid pairpos        mode 4: of pair, with the same palette and texture format, and with only the format
//   pairfar pairmax       mode 4: the nearest partner was still too far on screen, and the longest pair, in the
//                         units of the batch's matrix (world units if nothing scaled it)
//   bb                    readback loads
//   bbcam bbstill bbother of those matched by order in the last scene with a moved camera: moved exactly like the
//                         camera (a world position from B's camera, which is what mode 4 fixes), didn't move
//                         (following the camera), or anything else (walking, or culling shifted the order)
//   bbfix bbmax           mode 4: translations replaced, and the largest change to a word, in view units. it
//                         should be about half the camera step plus half the object's own step, and it's capped
//                         at half the camera step plus 6
//   bbunp bbrej           mode 4: left at B's because the object moved and has no partner, and because they were
//                         over that cap, which means it probably isn't what was read back
//   bbflush bbovf         the billboard path of the other flush types, where the load isn't the read back matrix
//                         and nothing is fixed, and loads over the table size
//
// HALFCAM pair, mode 5, per in-between scene with anything moving: n=<scene> m=<moving in B>/<moving in A>, then per
// moving batch of B (at most 8): v<vertices>g<billboard loads>@<x,y,z of its placement, in its matrix's units>, then
// :<tier><index in A>,d<distance> for a pair, tier a same palette and texture address, p same palette, i only the same
// texture format, or :far :rot :none for none. then after | the moving batches of A nobody took (at most 4), the ones
// that vanished: <index in A>v<vertices>g<billboard loads>@<x,y,z>
class HalfCamera
{
public:
    HalfCamera(GPU3D& gpu, int mode, bool isQuiet);
    ~HalfCamera();

    [[nodiscard]] int GetMode() const noexcept { return Mode; }

    // a FIFO word as it starts executing
    void Record(u8 command, u32 param) noexcept
    {
        if (IsPaused)
            return;
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
    // at line 215: whether the renderer can leave this render out to let the GPU catch up
    [[nodiscard]] bool CanSkipRender() const noexcept;

    // a reset or a state load: nothing recorded so far can be trusted
    void Invalidate();
    // between frames: while paused, nothing is recorded or analyzed and only real scenes are shown. pausing goes
    // through Invalidate, so it ends an in-between frame the same way a state load does
    void SetPaused(bool isPaused);
    // what a savestate has to contain: never the in-between scene
    void GetRealRenderList(const RenderList*& list, u32& numPolygons) const noexcept;

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
        u32 TopGroups, SameLoadGroups, SameLoadVertices, ChangedLoadGroups, ChangedLoadVertices, NoLoadVerticesTotal;
        u32 PairedVertices, UnpairedVertices;
        u32 ReadbackLoads, ReadbackWithCamera, ReadbackStill, ReadbackOther, ReadbackFixed, ReadbackUnpaired, ReadbackRejected, ReadbackFlushPath, ReadbackOverflows;
        s64 ReadbackMaxDistance;
        u32 PairedByIdentity, PairedByPosition, PairTooFar;
        s64 PairMaxDistance;
    };

    void Analyze(SceneInfo& scene);
    void FindMoving(const SceneInfo& current, const SceneInfo& previous);
    [[nodiscard]] Reason Decide(const SceneInfo& current, const SceneInfo& previous) const;
    bool Replay(const SceneInfo& current, const SceneInfo& previous);
    void PatchCamera(const SceneInfo& current, const SceneInfo& previous);
    u32 FindExtraBatches(const SceneInfo& scene, const SceneInfo& other, u16* moving);
    void ResetShadow();
    void CaptureBatchClips(const SceneInfo& current);
    void LogPairs(const SceneInfo& current, const SceneInfo& previous) const;
    void MatchMoving(const SceneInfo& current, const SceneInfo& previous);
    bool PairBatch(u32 movingIndex, const SceneInfo& current, const SceneInfo& previous);
    void FixReadbackLoad(u32 recordIndex);
    void CountDiagnostics(const SceneInfo& current, const SceneInfo& previous);
    [[nodiscard]] bool IsNearOnScreen(const s32* clip, const s32* wordsA, const s32* wordsB, double maxX, double maxY) const;
    bool ShowReplayedScene();
    void RestoreRealScene();
    void LogScene(const SceneInfo& current, const SceneInfo& previous, Reason reason) const;
    void LogSecond(Clock::time_point now);

    GPU3D& Main;
    int Mode;
    // no logging, no timers and no CountDiagnostics. the plain counters in Total still run, spread through the
    // analysis and the replay, but nothing reads them, and the 32-bit ones just wrap
    bool IsQuiet;
    bool IsPaused = false;

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
    // mode 4: per moving batch of the current scene, its partner in MovingPrevious or -1, the tier that found it, and
    // whether a candidate was only left out for its rotation. worked out before each replay
    struct PairCandidate
    {
        s64 Distance;
        u16 Current;
        u16 Previous;
    };
    std::unique_ptr<s16[]> PartnerCurrent;
    std::unique_ptr<u8[]> PartnerTier;
    std::unique_ptr<bool[]> PartnerRotationFar;
    std::unique_ptr<bool[]> PartnerTooFar;
    std::unique_ptr<std::array<s32, 16>[]> BatchClips;
    struct KeyEntry
    {
        u64 Key;
        u16 Index;
    };
    std::vector<KeyEntry> KeyOrder;
    std::unique_ptr<bool[]> IsExtra;
    std::unique_ptr<bool[]> IsPartnerUsed;
    std::vector<PairCandidate> PairCandidates;
    s64 ReadbackBound = 0;

    u32 History = 0;
    u32 FramesSinceCapture = 0;

    // RenderPolygonRAM holds the in-between scene, RealList what it had
    bool IsMidActive = false;
    // Invalidate ended an in-between frame, and the renderer's output can still be that picture
    bool IsMidPictureStale = false;
    // what the last OnVBlank returned: this frame's render replaces an in-between picture
    bool IsAfterMid = false;
    u32 MidNumPolygons = 0;
    u32 RealNumPolygons = 0;
    RenderList RealList {};

    u32 SceneNumber = 0;
    Totals Total {};
    Clock::time_point LastLog;
};

}

#endif
