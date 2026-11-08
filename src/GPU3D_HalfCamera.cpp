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

#include "GPU3D_HalfCamera.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "GPU.h"
#include "GPU3D.h"
#include "NDS.h"
#include "Platform.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

static constexpr u32 MaxLoads = 64;
static constexpr u32 MaxLoadUses = 4096;
static constexpr u32 MaxBatches = 1024;
static constexpr u32 MaxReadbackLoads = 256;
// a character is one quad, this leaves room for a sprite made of a few
static constexpr u32 MaxBatchVertices = 32;

// the two cameras have to be close for a lerp of the words to mean anything. a rotation word (4.12) moving
// by 1/8 is about 7 degrees in one scene, 200 degrees a second, more than any camera in play turns. it also
// keeps the lerp's shrink, cos^2(angle/2), under 0.4%, about half a native pixel at the screen edge. a
// translation word (20.12) moving by 64 is 4 tiles of 16 units in one scene. the logs have walking at 2 units a
// scene and running at 4, and no log has the bike yet. it's a guess, for a cut. the HALFCAM cam lines show real values
static constexpr s32 MaxRotationStep = 0x200;
static constexpr s32 MaxTranslationStep = 0x40000;

// how far a readback load's position can be from what the replay reads back: half the camera's step, worked out
// per scene, plus half the object's step, which the pairing limits below keep to about 5 units, and a unit spare.
// anything more isn't what the game read back, and the real position is a far smaller error than a wrong one
static constexpr s32 MaxReadbackObjectStep = 6 * 0x1000;
// words of rounding allowed when checking that a readback load moved exactly like the camera did
static constexpr s32 MaxReadbackRounding = 8;

// a load counts as the camera too if its rotation is this close to the camera's
static constexpr s32 MaxNearRotation = 0x200;

static constexpr u32 PairWindow = 8;
// modes 2 and 3: how far a character can move on screen between two scenes, in native pixels of the 256x192 view.
// the pixels per unit of a real game haven't been measured
static constexpr double MaxPairX = 24.0 / 128.0;
static constexpr double MaxPairY = 24.0 / 96.0;

// mode 4: what a walking object can do in one scene, in native pixels. known: a tile is 16 units, walking is 2 units a
// scene and running 4 in the logs. assumed, not measured: about a pixel per unit. no log has the bike, and if it does 8
// units a scene it's right at this limit. Y is narrower as the tilted camera squeezes it
static constexpr double MaxWalkX = 10.0 / 128.0;
static constexpr double MaxWalkY = 8.0 / 96.0;
// mode 4: pairs to consider per scene, as all of a scene's moving batches against the last one's
static constexpr u32 MaxPairCandidates = 1u << 14;

// 90% of the scene's vertices have to be under one load
static constexpr u32 MinSharePercent = 90;

static constexpr u64 HashSeed = 0x243F6A8885A308D3ULL;

static u64 Mix(u64 hash, u64 value)
{
    hash = (hash ^ value) * 0x9E3779B97F4A7C15ULL;
    return hash ^ (hash >> 29);
}

// the words can be anything a game sends, so these don't fit in 32 bits for sure
static s64 WordDistance(s32 a, s32 b)
{
    return std::abs((s64)a - b);
}

static s32 HalfWayFromTo(s32 from, s32 to)
{
    return (s32)(((s64)to - from) >> 1);
}

// NitroSystem's billboard commands (SBC BB and BBY, in Platinum at 0x020b0d7c and 0x020b1084) send MTX_MODE 0,
// PUSH, IDENTITY, read CLIPMTX_RESULT on the CPU to get the position matrix, then send POP, MTX_MODE 2 and an
// MTX_LOAD_4x3 whose translation is that matrix's. this walks those commands, step 5 being the load. the packed
// write leaves NOPs in between
static u32 NextReadbackStep(u32 step, u8 command, u32 param)
{
    if (command == 0x00)
        return step;

    u32 matrixMode = param & 0x3;
    if (step == 1 && command == 0x11)
        return 2;
    if (step == 2 && command == 0x15)
        return 3;
    if (step == 3 && command == 0x12)
        return 4;
    if (step == 4 && command == 0x10 && (matrixMode == 1 || matrixMode == 2))
        return 5;
    return command == 0x10 && matrixMode == 0 ? 1 : 0;
}

// what replay needs from the GPU3D at the start of a scene. the same names as there, for CopyGeometry
struct HalfCamera::GeometryState
{
    u32 MatrixMode;
    s32 ProjMatrix[16], PosMatrix[16], VecMatrix[16], TexMatrix[16];
    u32 Viewport[6];
    s32 ProjMatrixStack[16], PosMatrixStack[32][16], VecMatrixStack[32][16], TexMatrixStack[16];
    s32 ProjMatrixStackPointer, PosMatrixStackPointer, TexMatrixStackPointer;

    u32 PolygonMode;
    s16 CurVertex[3];
    u8 VertexColor[3];
    s16 TexCoords[2], RawTexCoords[2], Normal[3];

    s16 LightDirection[4][3];
    s32 SpecRecip[4];
    u8 LightColor[4][3];
    u8 MatDiffuse[3], MatAmbient[3], MatSpecular[3], MatEmission[3];

    bool UseShininessTable;
    u8 ShininessTable[128];

    u32 PolygonAttr, CurPolygonAttr, TexParam, TexPalette;

    Vertex TempVertexBuffer[4];
    s32 TempVertexNativeX[4];
    u32 VertexNum, VertexNumInPoly, NumConsecutivePolygons;
    u32 ExecParams[32];
    u32 ExecParamCount;
    // what the polygons of a scene are built with is the attributes of the last flush, not its own
    u32 FlushAttributes;
};

template <typename Dst, typename Src>
static void CopyGeometry(Dst& dst, const Src& src)
{
#define COPY_FIELD(name) \
    static_assert(sizeof(dst.name) == sizeof(src.name)); \
    memcpy(&dst.name, &src.name, sizeof(dst.name))

    COPY_FIELD(MatrixMode);
    COPY_FIELD(ProjMatrix); COPY_FIELD(PosMatrix); COPY_FIELD(VecMatrix); COPY_FIELD(TexMatrix);
    COPY_FIELD(Viewport);
    COPY_FIELD(ProjMatrixStack); COPY_FIELD(PosMatrixStack); COPY_FIELD(VecMatrixStack); COPY_FIELD(TexMatrixStack);
    COPY_FIELD(ProjMatrixStackPointer); COPY_FIELD(PosMatrixStackPointer); COPY_FIELD(TexMatrixStackPointer);
    COPY_FIELD(PolygonMode);
    COPY_FIELD(CurVertex); COPY_FIELD(VertexColor); COPY_FIELD(TexCoords); COPY_FIELD(RawTexCoords); COPY_FIELD(Normal);
    COPY_FIELD(LightDirection); COPY_FIELD(SpecRecip); COPY_FIELD(LightColor);
    COPY_FIELD(MatDiffuse); COPY_FIELD(MatAmbient); COPY_FIELD(MatSpecular); COPY_FIELD(MatEmission);
    COPY_FIELD(UseShininessTable); COPY_FIELD(ShininessTable);
    COPY_FIELD(PolygonAttr); COPY_FIELD(CurPolygonAttr); COPY_FIELD(TexParam); COPY_FIELD(TexPalette);
    COPY_FIELD(TempVertexBuffer); COPY_FIELD(TempVertexNativeX);
    COPY_FIELD(VertexNum); COPY_FIELD(VertexNumInPoly); COPY_FIELD(NumConsecutivePolygons);
    COPY_FIELD(ExecParams); COPY_FIELD(ExecParamCount);
    COPY_FIELD(FlushAttributes);

#undef COPY_FIELD
}

struct HalfCamera::SceneInfo
{
    struct Load
    {
        u64 Hash;
        u32 Vertices;
        u32 Groups; // runs of vertices under it, for the diagnostics
        s32 Words[12];
        bool Is4x3;
        bool IsCameraLike;
        bool IsReadback;
    };

    struct ReadbackLoad
    {
        u32 Index; // into the records
        s32 Translation[3];
    };

    struct LoadUse
    {
        u32 Index; // into the records
        u32 Load;
    };

    struct Batch
    {
        u64 Key; // the words and the shape
        u64 Shape;
        u32 Index; // of the MTX_MULT_4x3's first word in the records
        u32 EndIndex; // of the record that closed it
        s32 Words[12];
        s32 Load;
        u32 Vertices;
        // mode 4: what it's drawn with, as of its first vertex. the polygon attributes leave out the alpha, which
        // can fade. the texture leaves out its VRAM address, which an animation frame changes
        u32 Attr;
        u32 TexFormat;
        u32 TexAddress;
        u32 Palette;
        u32 ReadbackLoads;
    };

    bool IsValid = false;
    u32 NumVertices = 0;
    s32 TopLoad = -1;
    u32 NumLoads = 0;
    u32 NumLoadUses = 0;
    u32 NumBatches = 0;
    u32 NumReadbackLoads = 0;
    u32 NoLoadVertices = 0;
    std::array<ReadbackLoad, MaxReadbackLoads> ReadbackLoads {};
    std::array<Load, MaxLoads> Loads {};
    std::array<LoadUse, MaxLoadUses> LoadUses {};
    std::array<Batch, MaxBatches> Batches {};
    // sorted
    std::array<u64, MaxBatches> BatchKeys {};

    [[nodiscard]] const Load* Top() const noexcept { return TopLoad >= 0 ? &Loads[TopLoad] : nullptr; }
    [[nodiscard]] bool HasLoad(u64 hash) const noexcept
    {
        for (u32 i = 0; i < NumLoads; i++)
        {
            if (Loads[i].Hash == hash)
                return true;
        }
        return false;
    }
    [[nodiscard]] bool IsShareEnough() const noexcept
    {
        const Load* top = Top();
        return top && top->Is4x3 && (u64)top->Vertices * 100 >= (u64)NumVertices * MinSharePercent;
    }
};

HalfCamera::HalfCamera(GPU3D& gpu, int mode, bool isQuiet) :
    Main(gpu),
    Mode(mode),
    IsQuiet(isQuiet),
    Shadow(new GPU3D(gpu.NDS, GPU3D::ShadowTag {})),
    Records(new RecordEntry[MaxRecords]),
    Start(new GeometryState),
    MovingCurrent(new u16[MaxBatches]),
    MovingPrevious(new u16[MaxBatches]),
    PartnerCurrent(new s16[MaxBatches]),
    PartnerTier(new u8[MaxBatches]),
    PartnerRotationFar(new bool[MaxBatches]),
    PartnerTooFar(new bool[MaxBatches]),
    BatchClips(new std::array<s32, 16>[MaxBatches]),
    IsExtra(new bool[MaxBatches]),
    IsPartnerUsed(new bool[MaxBatches]),
    LastLog(Clock::now())
{
    Scenes[0] = std::make_unique<SceneInfo>();
    Scenes[1] = std::make_unique<SceneInfo>();
    Total.MinShare = 100.0f;
}

HalfCamera::~HalfCamera() = default;

void HalfCamera::NoteCapture(u32 captureCnt) noexcept
{
    if (((captureCnt >> 29) & 0x3) != 1)
        FramesSinceCapture = 0;
}

bool HalfCamera::OnVBlank()
{
    bool wasMid = IsMidActive || IsMidPictureStale;
    if (IsMidActive)
        RestoreRealScene();

    if (Main.GeometryEnabled && Main.RenderingEnabled)
    {
        History = (History << 1) | (Main.FlushRequest ? 1 : 0);

        // a capture already armed for the next frame (GPU::StartScanline reports the ones that ran)
        u32 captureCnt = Main.NDS.GPU.GPU2D_A.CaptureCnt;
        if (captureCnt & (1u << 31))
            NoteCapture(captureCnt);
        if (FramesSinceCapture < CaptureHoldFrames)
            FramesSinceCapture++;
    }
    else
        History = 0;

    Total.VBlanks++;
    if (!IsQuiet)
    {
        Clock::time_point now = Clock::now();
        if (now - LastLog >= std::chrono::seconds(1))
            LogSecond(now);
    }

    return wasMid;
}

void HalfCamera::OnSwap()
{
    if (IsPaused)
        return;

    // the timers are only for the HALFCAM sec line
    Clock::time_point start {};
    if (!IsQuiet)
        start = Clock::now();
    SceneInfo& current = *Scenes[Bank];
    const SceneInfo& previous = *Scenes[Bank ^ 1];

    SceneNumber++;
    Total.Scenes++;

    Analyze(current);
    if (Mode >= 2 && current.IsValid && previous.IsValid)
        FindMoving(current, previous);
    else
        NumMovingCurrent = NumMovingPrevious = 0;

    if (current.IsValid && current.NumVertices)
    {
        Total.Vertices += current.NumVertices;
        const SceneInfo::Load* top = current.Top();
        u32 topVertices = top ? top->Vertices : 0;
        Total.TopVertices += topVertices;
        Total.MinShare = std::min(Total.MinShare, topVertices * 100.0f / current.NumVertices);
    }

    Clock::time_point replayStart {};
    if (!IsQuiet)
    {
        replayStart = Clock::now();
        Total.AnalysisNs += std::chrono::duration_cast<std::chrono::nanoseconds>(replayStart - start).count();
    }

    Reason reason = Decide(current, previous);
    if (reason == Reason_None)
    {
        Total.Replays++;
        if (Replay(current, previous))
        {
            ShowReplayedScene();
            Total.Interpolated++;
        }
        else
            reason = Reason_Full;

        if (!IsQuiet)
        {
            s64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - replayStart).count();
            Total.ReplayNs += ns;
            Total.MaxReplayNs = std::max(Total.MaxReplayNs, ns);
        }
    }

    if (reason != Reason_None)
        Total.Skipped[reason]++;

    if (!IsQuiet && Mode >= 3)
    {
        CountDiagnostics(current, previous);
        LogScene(current, previous, reason);
    }
    if (!IsQuiet && Mode >= 5 && Total.Replays != 0 && (reason == Reason_None || reason == Reason_Full))
        LogPairs(current, previous);

    Bank ^= 1;
}

void HalfCamera::OnSceneStart()
{
    if (IsPaused)
        return;

    CopyGeometry(*Start, Main);
    HasStart = true;
    NumRecords = 0;
    IsOverflowed = false;
}

// the matrix a vertex is under comes from a load in this scene, or it isn't counted as any load's. a load
// sent before the scene (left in the current matrix or on the stack) isn't in the stream, so it can't be
// replaced, and it has to count against the share
void HalfCamera::Analyze(SceneInfo& scene)
{
    scene.IsValid = false;
    scene.NumVertices = 0;
    scene.TopLoad = -1;
    scene.NumLoads = 0;
    scene.NumLoadUses = 0;
    scene.NumBatches = 0;
    scene.NumReadbackLoads = 0;
    scene.NoLoadVertices = 0;

    if (IsOverflowed)
        Total.RecordOverflows++;
    if (!HasStart || IsOverflowed || Start->ExecParamCount != 0)
        return;

    constexpr s32 NoLoad = -1;
    s32 currentLoad = NoLoad;
    s32 lastLoad = NoLoad;
    std::array<s32, 32> stack;
    stack.fill(NoLoad);
    u32 matrixMode = Start->MatrixMode;
    s32 stackPointer = Start->PosMatrixStackPointer;

    u32 readbackStep = 0;
    bool isGroupOpen = false;
    // the load just seen looks like a readback, which the command after it decides
    bool isReadbackPending = false;
    bool isPendingListed = false;
    s32 pendingLoad = NoLoad;

    u32 currentAttr = Start->PolygonAttr;
    u32 currentTexParam = Start->TexParam;
    u32 currentPalette = Start->TexPalette;

    u32 i = 0;

    bool isBatchOpen = false;
    SceneInfo::Batch batch {};

    auto closeBatch = [&]()
    {
        if (isBatchOpen && batch.Vertices > 0 && batch.Vertices <= MaxBatchVertices)
        {
            if (scene.NumBatches < MaxBatches)
            {
                batch.Shape = Mix(batch.Shape, batch.Vertices);
                batch.EndIndex = i;
                batch.Key = batch.Shape;
                for (s32 word : batch.Words)
                    batch.Key = Mix(batch.Key, (u32)word);
                // mode 4: what's drawn counts too, so a character on the exact spot of a tuft isn't taken for it
                if (Mode >= 4)
                    batch.Key = Mix(Mix(Mix(Mix(batch.Key, batch.Attr), batch.TexFormat), batch.TexAddress), batch.Palette);
                scene.Batches[scene.NumBatches++] = batch;
            }
            else
                Total.BatchOverflows++;
        }
        isBatchOpen = false;
    };

    auto confirmReadback = [&]()
    {
        if (isReadbackPending && pendingLoad != NoLoad)
            scene.Loads[pendingLoad].IsReadback = true;
        isReadbackPending = false;
    };

    while (i < NumRecords)
    {
        u8 command = Records[i].Command;
        u32 numWords = std::max<u32>(GPU3D::NumCommandParams(command), 1);
        if (i + numWords > NumRecords)
            return;
        // the words of a command are together in the FIFO, unless the game wrote something odd
        for (u32 j = 1; j < numWords; j++)
        {
            if (Records[i + j].Command != command)
                return;
        }

        bool isPositionMode = matrixMode == 1 || matrixMode == 2;
        u32 param = Records[i].Param;

        bool isReadbackLoad = readbackStep == 5 && command == 0x17;
        readbackStep = NextReadbackStep(readbackStep, command, param);

        // the billboard's other two flush paths (Platinum 0x020b0f44) send some other matrix in this load, and
        // the read back translation in an MTX_MULT_4x3 after it, which is left alone: only counted
        if (isReadbackPending && command != 0x00)
        {
            if (command == 0x19 && isPositionMode)
            {
                isReadbackPending = false;
                if (isPendingListed)
                    scene.NumReadbackLoads--;
                Total.ReadbackFlushPath++;
            }
            else
                confirmReadback();
        }

        // anything that can change which load the matrix comes from starts a new run of vertices
        if (command == 0x12 || command == 0x14 || command == 0x15 || command == 0x16 || command == 0x17)
            isGroupOpen = false;

        switch (command)
        {
        case 0x10:
            matrixMode = param & 0x3;
            break;

        case 0x11:
            if (isPositionMode)
            {
                stack[stackPointer & 0x1F] = currentLoad;
                stackPointer = (stackPointer + 1) & 0x3F;
            }
            break;

        case 0x12:
            if (isPositionMode)
            {
                closeBatch();
                stackPointer = (stackPointer - ((s32)(param << 26) >> 26)) & 0x3F;
                currentLoad = stack[stackPointer & 0x1F];
            }
            break;

        case 0x13:
            if (isPositionMode)
                stack[param & 0x1F] = currentLoad;
            break;

        case 0x14:
            if (isPositionMode)
            {
                closeBatch();
                currentLoad = stack[param & 0x1F];
            }
            break;

        case 0x15:
            if (isPositionMode)
            {
                closeBatch();
                currentLoad = NoLoad;
            }
            break;

        case 0x16:
        case 0x17:
            if (isPositionMode)
            {
                // in mode 4 a billboard's load is part of the batch its placement started. that matrix is
                // what the load is really made from, and it's what gets paired
                if (!(isReadbackLoad && Mode >= 4))
                    closeBatch();
                else if (isBatchOpen)
                    batch.ReadbackLoads++;

                isPendingListed = isReadbackLoad && scene.NumReadbackLoads < MaxReadbackLoads;
                if (isPendingListed)
                {
                    SceneInfo::ReadbackLoad& readback = scene.ReadbackLoads[scene.NumReadbackLoads++];
                    readback.Index = i;
                    for (u32 j = 0; j < 3; j++)
                        readback.Translation[j] = (s32)Records[i + 9 + j].Param;
                }
                else if (isReadbackLoad)
                    Total.ReadbackOverflows++;

                u64 hash = HashSeed + command;
                for (u32 j = 0; j < numWords; j++)
                    hash = Mix(hash, Records[i + j].Param);

                // the same load comes over and over, and only a few differ
                s32 found = NoLoad;
                if (lastLoad != NoLoad && scene.Loads[lastLoad].Hash == hash)
                    found = lastLoad;
                for (u32 j = 0; found == NoLoad && j < scene.NumLoads; j++)
                {
                    if (scene.Loads[j].Hash == hash)
                        found = (s32)j;
                }
                if (found == NoLoad && scene.NumLoads < MaxLoads)
                {
                    found = (s32)scene.NumLoads++;
                    SceneInfo::Load& load = scene.Loads[found];
                    load.Hash = hash;
                    load.Vertices = 0;
                    load.Groups = 0;
                    load.Is4x3 = command == 0x17;
                    load.IsCameraLike = false;
                    load.IsReadback = false;
                    if (command == 0x17)
                    {
                        for (u32 j = 0; j < 12; j++)
                            load.Words[j] = (s32)Records[i + j].Param;
                    }
                }

                currentLoad = found;
                lastLoad = found;
                if (isReadbackLoad)
                {
                    isReadbackPending = true;
                    pendingLoad = found;
                }

                if (found != NoLoad && command == 0x17)
                {
                    // too many to replace them all, so it can't be interpolated
                    if (scene.NumLoadUses >= MaxLoadUses)
                        return;
                    scene.LoadUses[scene.NumLoadUses++] = { i, (u32)found };
                }
            }
            break;

        case 0x19:
            if (isPositionMode && Mode >= 2)
            {
                closeBatch();
                isBatchOpen = true;
                batch = {};
                batch.Index = i;
                batch.Load = currentLoad;
                batch.Shape = HashSeed;
                for (u32 j = 0; j < 12; j++)
                    batch.Words[j] = (s32)Records[i + j].Param;
            }
            break;

        case 0x29:
            currentAttr = param;
            break;

        case 0x2A:
            currentTexParam = param;
            break;

        case 0x2B:
            currentPalette = param;
            break;

        case 0x40:
            if (isBatchOpen)
                batch.Shape = Mix(batch.Shape, param);
            break;

        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x27:
        case 0x28:
            scene.NumVertices++;
            if (currentLoad != NoLoad)
            {
                scene.Loads[currentLoad].Vertices++;
                if (!isGroupOpen)
                    scene.Loads[currentLoad].Groups++;
                isGroupOpen = true;
            }
            else
                scene.NoLoadVertices++;
            // the shape is the primitive type and vertex count, not the scale or the vertex words: an
            // animation frame can change the size, and replay keeps B's size and vertices anyway. White 2
            // takes the quad's corners from a table per scene and sizes each actor by its SCALE
            if (isBatchOpen)
            {
                if (batch.Vertices == 0)
                {
                    batch.Attr = currentAttr & ~0x001F0000u;
                    batch.TexFormat = currentTexParam & 0xFFFF0000u;
                    batch.TexAddress = currentTexParam & 0xFFFFu;
                    batch.Palette = currentPalette;
                }
                batch.Vertices++;
            }
            break;

        default:
            break;
        }

        i += numWords;
    }
    closeBatch();
    confirmReadback();

    for (u32 j = 0; j < scene.NumLoads; j++)
    {
        if (scene.TopLoad < 0 || scene.Loads[j].Vertices > scene.Loads[scene.TopLoad].Vertices)
            scene.TopLoad = (s32)j;
    }

    if (const SceneInfo::Load* top = scene.Top(); top && top->Is4x3)
    {
        for (u32 j = 0; j < scene.NumLoads; j++)
        {
            SceneInfo::Load& load = scene.Loads[j];
            if (!load.Is4x3)
                continue;

            load.IsCameraLike = true;
            for (u32 k = 0; k < 9; k++)
                load.IsCameraLike = load.IsCameraLike && WordDistance(load.Words[k], top->Words[k]) <= MaxNearRotation;
            for (u32 k = 9; k < 12; k++)
                load.IsCameraLike = load.IsCameraLike && WordDistance(load.Words[k], top->Words[k]) <= MaxTranslationStep;
        }
    }

    scene.IsValid = true;

    if (Mode >= 2)
    {
        // only batches over the camera can be characters
        u32 kept = 0;
        for (u32 j = 0; j < scene.NumBatches; j++)
        {
            const SceneInfo::Batch& candidate = scene.Batches[j];
            if (candidate.Load != NoLoad && scene.Loads[candidate.Load].IsCameraLike)
            {
                scene.Batches[kept] = candidate;
                scene.BatchKeys[kept] = candidate.Key;
                kept++;
            }
        }
        scene.NumBatches = kept;
        std::sort(scene.BatchKeys.begin(), scene.BatchKeys.begin() + kept);
    }
}

// a batch with the same words and shape in the other scene didn't move, and pairing it would let a static
// prop be moved towards a similar one nearby if something else was culled
void HalfCamera::FindMoving(const SceneInfo& current, const SceneInfo& previous)
{
    if (Mode >= 4)
    {
        NumMovingCurrent = FindExtraBatches(current, previous, MovingCurrent.get());
        NumMovingPrevious = FindExtraBatches(previous, current, MovingPrevious.get());
        return;
    }

    auto findIn = [](const SceneInfo& scene, u64 key)
    {
        return std::binary_search(scene.BatchKeys.begin(), scene.BatchKeys.begin() + scene.NumBatches, key);
    };

    NumMovingCurrent = 0;
    for (u32 i = 0; i < current.NumBatches; i++)
    {
        if (!findIn(previous, current.Batches[i].Key))
            MovingCurrent[NumMovingCurrent++] = (u16)i;
    }

    NumMovingPrevious = 0;
    for (u32 i = 0; i < previous.NumBatches; i++)
    {
        if (!findIn(current, previous.Batches[i].Key))
            MovingPrevious[NumMovingPrevious++] = (u16)i;
    }
}

// mode 4: by counts, not just presence. of the batches with one key, the ones beyond how many the other scene has
// are moving: a character arriving on the spot of a tuft that's still there is one more of that key. the earliest
// ones count as the ones that stayed. returns how many, in the order they were sent
u32 HalfCamera::FindExtraBatches(const SceneInfo& scene, const SceneInfo& other, u16* moving)
{
    KeyOrder.resize(scene.NumBatches);
    for (u32 i = 0; i < scene.NumBatches; i++)
    {
        KeyOrder[i] = { scene.Batches[i].Key, (u16)i };
        IsExtra[i] = false;
    }
    std::sort(KeyOrder.begin(), KeyOrder.end(), [](const KeyEntry& a, const KeyEntry& b)
    {
        return a.Key != b.Key ? a.Key < b.Key : a.Index < b.Index;
    });

    const u64* otherBegin = other.BatchKeys.data();
    const u64* otherEnd = otherBegin + other.NumBatches;
    for (u32 i = 0; i < scene.NumBatches;)
    {
        u32 end = i;
        while (end < scene.NumBatches && KeyOrder[end].Key == KeyOrder[i].Key)
            end++;

        auto range = std::equal_range(otherBegin, otherEnd, KeyOrder[i].Key);
        u32 inOther = (u32)(range.second - range.first);
        for (u32 j = i + inOther; j < end; j++)
            IsExtra[KeyOrder[j].Index] = true;
        i = end;
    }

    u32 count = 0;
    for (u32 i = 0; i < scene.NumBatches; i++)
    {
        if (IsExtra[i])
            moving[count++] = (u16)i;
    }
    return count;
}

HalfCamera::Reason HalfCamera::Decide(const SceneInfo& current, const SceneInfo& previous) const
{
    if ((History & LockMask) != LockPattern)
        return Reason_NotLocked;
    if (!Main.CurrentRenderer->CanDrawReplayedScene())
        return Reason_Renderer;
    // the next frame could capture the 3D into VRAM, which is emulated state: the game can read it back
    // or draw with it, and it's in savestates
    if (FramesSinceCapture < CaptureHoldFrames)
        return Reason_Capture;
    if (!current.IsValid || !previous.IsValid)
        return Reason_Stream;
    if (Main.NumPolygons == 0)
        return Reason_Empty;
    if (!current.IsShareEnough() || !previous.IsShareEnough())
        return Reason_Share;

    const SceneInfo::Load& a = *previous.Top();
    const SceneInfo::Load& b = *current.Top();
    if (a.Hash == b.Hash)
    {
        // nothing would come out differently
        if (NumMovingCurrent == 0)
            return Reason_Still;
    }
    else
    {
        for (u32 i = 0; i < 12; i++)
        {
            if (WordDistance(a.Words[i], b.Words[i]) > (i < 9 ? MaxRotationStep : MaxTranslationStep))
                return Reason_Far;
        }
    }

    return Reason_None;
}

void HalfCamera::PatchCamera(const SceneInfo& current, const SceneInfo& previous)
{
    const SceneInfo::Load& a = *previous.Top();
    const SceneInfo::Load& b = *current.Top();
    if (a.Hash == b.Hash)
        return;

    // half of the way from B to A. for the camera itself it's the average of the two
    s32 delta[12];
    for (u32 i = 0; i < 12; i++)
        delta[i] = HalfWayFromTo(b.Words[i], a.Words[i]);

    u32 topIndex = (u32)current.TopLoad;
    for (u32 i = 0; i < current.NumLoadUses; i++)
    {
        const SceneInfo::LoadUse& use = current.LoadUses[i];
        const SceneInfo::Load& load = current.Loads[use.Load];
        if (!load.IsCameraLike)
            continue;

        if (use.Load != topIndex)
        {
            // with a camera close to its rotation it would look like another camera load, but FixReadbackLoad
            // has the real answer for its translation, and its rotation doesn't follow the camera
            if (Mode >= 4 && load.IsReadback)
                continue;

            // the same words in both scenes aren't following the camera (a HUD or an effect), and moving
            // them would shake them. the exception is last scene's camera itself, sent a scene late
            if (load.Hash != a.Hash && previous.HasLoad(load.Hash))
            {
                Total.NearStatic++;
                continue;
            }
            Total.NearLoads++;
        }

        for (u32 j = 0; j < 12; j++)
            Records[use.Index + j].Param = (u32)load.Words[j] + (u32)delta[j];
    }
}

bool HalfCamera::IsNearOnScreen(const s32* clip, const s32* wordsA, const s32* wordsB, double maxX, double maxY) const
{
    // where each position ends up: the batch's matrix is the translation in the last 3 words of the
    // multiply on top of the current clip matrix, so these are the batches' origins in clip space
    auto project = [clip](const s32* words, double& x, double& y)
    {
        double px = words[9], py = words[10], pz = words[11];
        double clipX = px * clip[0] + py * clip[4] + pz * clip[8] + 4096.0 * clip[12];
        double clipY = px * clip[1] + py * clip[5] + pz * clip[9] + 4096.0 * clip[13];
        double clipW = px * clip[3] + py * clip[7] + pz * clip[11] + 4096.0 * clip[15];
        if (clipW <= 0.0)
            return false;
        x = clipX / clipW;
        y = clipY / clipW;
        return true;
    };

    double ax, ay, bx, by;
    if (!project(wordsA, ax, ay) || !project(wordsB, bx, by))
        return false;

    return std::abs(ax - bx) <= maxX && std::abs(ay - by) <= maxY;
}

// mode 4. the matrix each moving batch's multiply goes on top of, by running only the matrix commands. the replay
// proper starts over after this. a billboard's translation isn't fixed here, but the game sends the camera again
// before the next placement
void HalfCamera::CaptureBatchClips(const SceneInfo& current)
{
    GPU3D& shadow = *Shadow;
    u32 nextMoving = 0;
    u32 i = 0;
    while (i < NumRecords && nextMoving < NumMovingCurrent)
    {
        u8 command = Records[i].Command;
        u32 numWords = std::max<u32>(GPU3D::NumCommandParams(command), 1);

        if (i == current.Batches[MovingCurrent[nextMoving]].Index)
        {
            shadow.UpdateClipMatrix();
            memcpy(BatchClips[nextMoving].data(), shadow.ClipMatrix, sizeof(shadow.ClipMatrix));
            nextMoving++;
        }

        if (command >= 0x10 && command <= 0x1C)
        {
            for (u32 j = 0; j < numWords; j++)
                shadow.ReplayEntry(command, Records[i + j].Param);
        }

        i += numWords;
    }
}

// mode 4. the order of appearance says nothing: an effect that comes or goes next to a character shifts it, and a
// grass tuft is the same quad with the same attributes as the character walking through it, so a character would be
// lerped towards it. so the closest pair goes first, among the ones with the same palette and texture, then the same
// palette (an animation frame only changes the texture's address), and only then among the ones that differ in the
// palette. whatever is too far on screen can't take part, so that it doesn't use up a tier
void HalfCamera::MatchMoving(const SceneInfo& current, const SceneInfo& previous)
{
    for (u32 i = 0; i < NumMovingCurrent; i++)
    {
        PartnerCurrent[i] = -1;
        PartnerRotationFar[i] = false;
        PartnerTooFar[i] = false;
    }
    for (u32 i = 0; i < NumMovingPrevious; i++)
        IsPartnerUsed[i] = false;

    if ((u64)NumMovingCurrent * NumMovingPrevious > MaxPairCandidates)
    {
        Total.BatchOverflows++;
        return;
    }

    for (u32 tier = 0; tier < 3; tier++)
    {
        PairCandidates.clear();
        for (u32 i = 0; i < NumMovingCurrent; i++)
        {
            if (PartnerCurrent[i] >= 0)
                continue;

            const SceneInfo::Batch& b = current.Batches[MovingCurrent[i]];
            for (u32 k = 0; k < NumMovingPrevious; k++)
            {
                if (IsPartnerUsed[k])
                    continue;

                const SceneInfo::Batch& a = previous.Batches[MovingPrevious[k]];
                if (a.Shape != b.Shape || a.Attr != b.Attr || a.TexFormat != b.TexFormat)
                    continue;
                if (tier == 0 && a.TexAddress != b.TexAddress)
                    continue;
                if (tier <= 1 && a.Palette != b.Palette)
                    continue;

                // a sprite mirrored by its matrix goes from 1 to -1 in a word, which lerps to a flat quad
                bool isRotationClose = true;
                for (u32 j = 0; j < 9; j++)
                    isRotationClose = isRotationClose && WordDistance(a.Words[j], b.Words[j]) <= MaxRotationStep;
                if (!isRotationClose)
                {
                    PartnerRotationFar[i] = true;
                    continue;
                }

                if (!IsNearOnScreen(BatchClips[i].data(), a.Words, b.Words, MaxWalkX, MaxWalkY))
                {
                    PartnerTooFar[i] = true;
                    continue;
                }

                s64 distance = 0;
                for (u32 j = 9; j < 12; j++)
                    distance = std::max(distance, WordDistance(a.Words[j], b.Words[j]));
                PairCandidates.push_back({ distance, (u16)i, (u16)k });
            }
        }

        std::sort(PairCandidates.begin(), PairCandidates.end(), [](const PairCandidate& x, const PairCandidate& y)
        {
            if (x.Distance != y.Distance)
                return x.Distance < y.Distance;
            return x.Current != y.Current ? x.Current < y.Current : x.Previous < y.Previous;
        });

        for (const PairCandidate& candidate : PairCandidates)
        {
            if (PartnerCurrent[candidate.Current] >= 0 || IsPartnerUsed[candidate.Previous])
                continue;

            PartnerCurrent[candidate.Current] = (s16)candidate.Previous;
            PartnerTier[candidate.Current] = (u8)tier;
            IsPartnerUsed[candidate.Previous] = true;
        }
    }
}

// by the time the replay gets here, the clip matrix is the halfway camera and whatever the scene applied
// on it, which is what the batch's multiply goes on top of. returns whether it found a partner
bool HalfCamera::PairBatch(u32 movingIndex, const SceneInfo& current, const SceneInfo& previous)
{
    const SceneInfo::Batch& b = current.Batches[MovingCurrent[movingIndex]];

    Shadow->UpdateClipMatrix();

    if (Mode >= 4)
    {
        s32 partner = PartnerCurrent[movingIndex];
        if (partner >= 0)
        {
            const SceneInfo::Batch& a = previous.Batches[MovingPrevious[partner]];
            s64 distance = 0;
            for (u32 j = 0; j < 12; j++)
                Records[b.Index + j].Param = (u32)b.Words[j] + (u32)HalfWayFromTo(b.Words[j], a.Words[j]);
            for (u32 j = 9; j < 12; j++)
                distance = std::max(distance, WordDistance(a.Words[j], b.Words[j]));

            Total.Paired++;
            Total.PairedVertices += b.Vertices;
            if (PartnerTier[movingIndex] <= 1)
                Total.PairedByIdentity++;
            else
                Total.PairedByPosition++;
            Total.PairMaxDistance = std::max(Total.PairMaxDistance, distance);
            return true;
        }

        Total.Unpaired++;
        Total.UnpairedVertices += b.Vertices;
        if (PartnerTooFar[movingIndex])
            Total.PairTooFar++;
        if (PartnerRotationFar[movingIndex])
            Total.PairRotation++;
        return false;
    }

    bool isRotationTooFar = false;
    u32 end = std::min(PairCursor + PairWindow, NumMovingPrevious);
    for (u32 k = PairCursor; k < end; k++)
    {
        const SceneInfo::Batch& a = previous.Batches[MovingPrevious[k]];
        if (a.Shape != b.Shape || !IsNearOnScreen(Shadow->ClipMatrix, a.Words, b.Words, MaxPairX, MaxPairY))
            continue;

        // a sprite mirrored by its matrix goes from 1 to -1 in a word, which lerps to a flat quad
        bool isRotationClose = true;
        for (u32 j = 0; j < 9; j++)
            isRotationClose = isRotationClose && WordDistance(a.Words[j], b.Words[j]) <= MaxRotationStep;
        if (!isRotationClose)
        {
            isRotationTooFar = true;
            continue;
        }

        for (u32 j = 0; j < 12; j++)
            Records[b.Index + j].Param = (u32)b.Words[j] + (u32)HalfWayFromTo(b.Words[j], a.Words[j]);

        PairCursor = k + 1;
        Total.Paired++;
        Total.PairedVertices += b.Vertices;
        return true;
    }

    Total.Unpaired++;
    Total.UnpairedVertices += b.Vertices;
    if (isRotationTooFar)
        Total.PairRotation++;
    return false;
}

// mode 4. the game worked out this translation on the CPU, from the position matrix it read back, so the
// words carry B's camera and everything the scene applied after it. the shadow has that same matrix at this
// point with the halfway camera and the paired placement, so reading it here gives what the game would have
// sent. the rotation and the scale that follow stay B's
void HalfCamera::FixReadbackLoad(u32 recordIndex)
{
    const s32* matrix = Shadow->PosMatrix;
    const u32 wordIndex = recordIndex + 9;

    s64 distance = 0;
    for (u32 j = 0; j < 3; j++)
        distance = std::max(distance, WordDistance((s32)Records[wordIndex + j].Param, matrix[12 + j]));

    if (distance > ReadbackBound)
    {
        Total.ReadbackRejected++;
        return;
    }

    for (u32 j = 0; j < 3; j++)
        Records[wordIndex + j].Param = (u32)matrix[12 + j];
    Total.ReadbackFixed++;
    Total.ReadbackMaxDistance = std::max(Total.ReadbackMaxDistance, distance);
}

void HalfCamera::CountDiagnostics(const SceneInfo& current, const SceneInfo& previous)
{
    if (!current.IsValid)
        return;

    Total.NoLoadVerticesTotal += current.NoLoadVertices;
    for (u32 i = 0; i < current.NumLoads; i++)
    {
        const SceneInfo::Load& load = current.Loads[i];
        if ((s32)i == current.TopLoad)
        {
            Total.TopGroups += load.Groups;
        }
        else if (previous.IsValid && previous.HasLoad(load.Hash))
        {
            Total.SameLoadGroups += load.Groups;
            Total.SameLoadVertices += load.Vertices;
        }
        else
        {
            Total.ChangedLoadGroups += load.Groups;
            Total.ChangedLoadVertices += load.Vertices;
        }
    }

    Total.ReadbackLoads += current.NumReadbackLoads;

    // does a readback load move exactly as the camera did? then it's a world position seen from B's camera.
    // by order, so a load culled in one of the scenes counts as other
    const SceneInfo::Load* a = previous.IsValid ? previous.Top() : nullptr;
    const SceneInfo::Load* b = current.Top();
    if (!a || !b || !a->Is4x3 || !b->Is4x3 || a->Hash == b->Hash)
        return;

    u32 count = std::min(current.NumReadbackLoads, previous.NumReadbackLoads);
    for (u32 i = 0; i < count; i++)
    {
        const SceneInfo::ReadbackLoad& oldLoad = previous.ReadbackLoads[i];
        const SceneInfo::ReadbackLoad& newLoad = current.ReadbackLoads[i];

        bool isSameAsCamera = true;
        bool isStill = true;
        for (u32 j = 0; j < 3; j++)
        {
            s64 moved = (s64)newLoad.Translation[j] - oldLoad.Translation[j];
            s64 cameraMoved = (s64)b->Words[9 + j] - a->Words[9 + j];
            isSameAsCamera = isSameAsCamera && std::abs(moved - cameraMoved) <= MaxReadbackRounding;
            isStill = isStill && std::abs(moved) <= MaxReadbackRounding;
        }

        if (isSameAsCamera)
            Total.ReadbackWithCamera++;
        else if (isStill)
            Total.ReadbackStill++;
        else
            Total.ReadbackOther++;
    }
}

void HalfCamera::ResetShadow()
{
    GPU3D& shadow = *Shadow;

    CopyGeometry(shadow, *Start);
    // what the scene was built for: it's only updated after this, at the swap
    shadow.ViewWidth = Main.ViewWidth;
    shadow.ZeroDotWLimit = Main.ZeroDotWLimit;
    shadow.ClipMatrixDirty = true;
    shadow.LastStripPolygon = nullptr;
    shadow.DispCnt = 0;

    // the same as a flush leaves them, there's no timing to keep here
    shadow.CycleCount = 0;
    shadow.VertexPipeline = 0;
    shadow.NormalPipeline = 0;
    shadow.PolygonPipeline = 0;
    shadow.VertexSlotCounter = 0;
    shadow.VertexSlotsFree = 1;

    shadow.CurVertexRAM = &shadow.VertexRAM[shadow.CurRAMBank ? 6144 : 0];
    shadow.CurPolygonRAM = &shadow.PolygonRAM[shadow.CurRAMBank ? 2048 : 0];
    shadow.NumVertices = 0;
    shadow.NumPolygons = 0;
    shadow.NumOpaquePolygons = 0;
    shadow.NativeNumVertices = 0;
    shadow.NativeNumPolygons = 0;
}

bool HalfCamera::Replay(const SceneInfo& current, const SceneInfo& previous)
{
    GPU3D& shadow = *Shadow;

    // the other bank each time, like the real thing, so a picture being read is never the one written
    shadow.CurRAMBank ^= 1;
    ResetShadow();

    PatchCamera(current, previous);

    if (Mode >= 4)
    {
        if (NumMovingCurrent)
        {
            CaptureBatchClips(current);
            ResetShadow();
        }
        MatchMoving(current, previous);

        s64 halfCameraStep = 0;
        for (u32 i = 9; i < 12; i++)
            halfCameraStep = std::max(halfCameraStep, WordDistance(previous.Top()->Words[i], current.Top()->Words[i]) / 2);
        ReadbackBound = halfCameraStep + MaxReadbackObjectStep;
    }

    PairCursor = 0;
    u32 nextMoving = 0;
    u32 nextBatchIndex = NumMovingCurrent ? current.Batches[MovingCurrent[0]].Index : UINT32_MAX;
    u32 nextReadback = 0;
    u32 nextReadbackIndex = Mode >= 4 && current.NumReadbackLoads ? current.ReadbackLoads[0].Index : UINT32_MAX;
    u32 submitted = 0;
    // a moving billboard with no partner keeps its placement from B. the halfway camera on that placement
    // would put an object the camera follows half a step off, while the sent words have it steady in view
    u32 keepReadbackUntil = 0;

    u32 i = 0;
    while (i < NumRecords)
    {
        u8 command = Records[i].Command;
        u32 numWords = std::max<u32>(GPU3D::NumCommandParams(command), 1);

        if (i == nextBatchIndex)
        {
            if (!PairBatch(nextMoving, current, previous))
                keepReadbackUntil = current.Batches[MovingCurrent[nextMoving]].EndIndex;
            nextMoving++;
            nextBatchIndex = nextMoving < NumMovingCurrent ? current.Batches[MovingCurrent[nextMoving]].Index : UINT32_MAX;
        }

        if (i == nextReadbackIndex)
        {
            if (i < keepReadbackUntil)
                Total.ReadbackUnpaired++;
            else
                FixReadbackLoad(i);
            nextReadback++;
            nextReadbackIndex = nextReadback < current.NumReadbackLoads ? current.ReadbackLoads[nextReadback].Index : UINT32_MAX;
        }

        switch (command)
        {
        case 0x50: // the shadow never swaps
        case 0x70: // box test, and the vector test. their results are the game's, it already has them
        case 0x72:
            break;

        case 0x71: // position test: its result is the game's too, but it does set the vertex
            shadow.CurVertex[0] = (s16)(Records[i].Param & 0xFFFF);
            shadow.CurVertex[1] = (s16)(Records[i].Param >> 16);
            shadow.CurVertex[2] = (s16)(Records[i + 1].Param & 0xFFFF);
            break;

        default:
            if (command >= 0x23 && command <= 0x28)
                submitted++;
            for (u32 j = 0; j < numWords; j++)
                shadow.ReplayEntry(command, Records[i + j].Param);
            break;
        }

        i += numWords;
    }

    Total.ReplayVertices += submitted;
    Total.ReplayPolygons += shadow.NumPolygons;

    // a hole in the in-between scene would be worse than not having it. bit 13 is the RAM overflow flag
    return shadow.NumPolygons > 0 && !(shadow.DispCnt & (1 << 13));
}

void HalfCamera::ShowReplayedScene()
{
    GPU3D& shadow = *Shadow;

    RealList = Main.RenderPolygonRAM;
    RealNumPolygons = Main.RenderNumPolygons;

    // the same steps as GPU3D::VBlank: translucent polygons after the opaque ones, then Y-sorting
    auto& list = Main.RenderPolygonRAM;
    u32 io = 0, it = shadow.NumOpaquePolygons;
    for (u32 i = 0; i < shadow.NumPolygons; i++)
    {
        Polygon* poly = &shadow.CurPolygonRAM[i];
        if (poly->Translucent)
            list[it++] = poly;
        else
            list[io++] = poly;
    }
    std::stable_sort(list.begin(), list.begin() + ((Main.FlushAttributes & 0x1) ? shadow.NumOpaquePolygons : shadow.NumPolygons),
        [](const Polygon* a, const Polygon* b) { return a->SortKey < b->SortKey; });

    Main.RenderNumPolygons = shadow.NumPolygons;
    MidNumPolygons = shadow.NumPolygons;
    IsMidActive = true;
}

void HalfCamera::RestoreRealScene()
{
    Main.RenderPolygonRAM = RealList;
    // unless something else set the count since (SetEnabled turning rendering off clears it)
    if (Main.RenderNumPolygons == MidNumPolygons)
        Main.RenderNumPolygons = RealNumPolygons;
    IsMidActive = false;
}

void HalfCamera::BeforeRender()
{
    // this render replaces the picture, OnVBlank made sure it isn't skipped
    IsMidPictureStale = false;

    u32 captureCnt = Main.NDS.GPU.GPU2D_A.CaptureCnt;
    if (!IsMidActive || !(captureCnt & (1u << 31)) || ((captureCnt >> 29) & 0x3) == 1)
        return;

    RestoreRealScene();
    Main.RenderFrameIdentical = false;
    FramesSinceCapture = 0;
    Total.CaptureAborted++;
    if (Total.Interpolated)
        Total.Interpolated--;
    Total.Skipped[Reason_Capture]++;
}

bool HalfCamera::NeedsRedraw() const noexcept
{
    if (IsMidPictureStale)
        return true;

    u32 captureCnt = Main.NDS.GPU.GPU2D_A.CaptureCnt;
    return IsMidActive && (captureCnt & (1u << 31)) && ((captureCnt >> 29) & 0x3) != 1;
}

void HalfCamera::Redraw(GPU& gpu)
{
    if (IsMidPictureStale)
    {
        // what's left after a load. a renderer switched since has no such picture
        IsMidPictureStale = false;
        Total.LoadRedrawn++;
        Main.RenderFrameIdentical = false;
        if (Main.CurrentRenderer->CanDrawReplayedScene())
            Main.CurrentRenderer->RenderFrame(gpu);
        return;
    }

    RestoreRealScene();
    Main.RenderFrameIdentical = false;
    FramesSinceCapture = 0;
    Total.CaptureRedrawn++;
    if (Total.Interpolated)
        Total.Interpolated--;
    Total.Skipped[Reason_Capture]++;

    Main.CurrentRenderer->RenderFrame(gpu);
}

void HalfCamera::Invalidate()
{
    if (IsMidActive)
    {
        RestoreRealScene();
        Main.RenderFrameIdentical = false;
        // the renderer's output can still be the in-between picture, which a capture armed in a loaded
        // state would read back. it isn't drawn here, as this isn't always where GL can be used
        IsMidPictureStale = true;
    }

    History = 0;
    HasStart = false;
    NumRecords = 0;
    IsOverflowed = false;
    Scenes[0]->IsValid = false;
    Scenes[1]->IsValid = false;
    NumMovingCurrent = 0;
    NumMovingPrevious = 0;
    // a game that was capturing before may well be again
    FramesSinceCapture = 0;
}

void HalfCamera::SetPaused(bool isPaused)
{
    if (isPaused && !IsPaused)
        Invalidate();
    IsPaused = isPaused;
}

void HalfCamera::GetRealRenderList(const std::array<Polygon*, 2048>*& list, u32& numPolygons) const noexcept
{
    if (!IsMidActive)
        return;

    list = &RealList;
    if (numPolygons == MidNumPolygons)
        numPolygons = RealNumPolygons;
}

void HalfCamera::LogScene(const SceneInfo& current, const SceneInfo& previous, Reason reason) const
{
    static const char* const ReasonNames[Reason_Count] = { "mid", "nolock", "render", "cap", "stream", "share", "far", "still", "empty", "full" };

    const SceneInfo::Load* top = current.IsValid ? current.Top() : nullptr;
    if (!top || !top->Is4x3)
    {
        Log(LogLevel::Info, "HALFCAM cam n=%u nocam res=%s\n", SceneNumber, ReasonNames[reason]);
        return;
    }

    float share = current.NumVertices ? top->Vertices * 100.0f / current.NumVertices : 0.0f;
    const SceneInfo::Load* last = previous.IsValid ? previous.Top() : nullptr;
    bool hasDelta = last && last->Is4x3;

    s32 maxRotation = 0;
    float dx = 0, dy = 0, dz = 0;
    if (hasDelta)
    {
        for (u32 i = 0; i < 9; i++)
            maxRotation = std::max(maxRotation, std::abs(top->Words[i] - last->Words[i]));
        dx = (top->Words[9] - last->Words[9]) / 4096.0f;
        dy = (top->Words[10] - last->Words[10]) / 4096.0f;
        dz = (top->Words[11] - last->Words[11]) / 4096.0f;
    }

    Log(LogLevel::Info, "HALFCAM cam n=%u t=%.3f,%.3f,%.3f dt=%.3f,%.3f,%.3f drot=%d delta=%d hash=%08X share=%.1f res=%s\n",
        SceneNumber, top->Words[9] / 4096.0f, top->Words[10] / 4096.0f, top->Words[11] / 4096.0f, dx, dy, dz, maxRotation, hasDelta ? 1 : 0,
        (u32)top->Hash, share, ReasonNames[reason]);
}

// mode 5: one line per in-between scene with anything moving, so that a minute of walking fits the log's budgets. see
// the header for how to read it
void HalfCamera::LogPairs(const SceneInfo& current, const SceneInfo& previous) const
{
    if (NumMovingCurrent == 0 && NumMovingPrevious == 0)
        return;

    char line[512];
    int length = snprintf(line, sizeof(line), "n=%u m=%u/%u", SceneNumber, NumMovingCurrent, NumMovingPrevious);

#define HALFCAM_ADD(...) \
    do { if (length >= 0 && (size_t)length < sizeof(line)) length += snprintf(line + length, sizeof(line) - length, __VA_ARGS__); } while (0)

    constexpr u32 MaxTraceCurrent = 8;
    constexpr u32 MaxTracePrevious = 4;
    for (u32 i = 0; i < std::min(NumMovingCurrent, MaxTraceCurrent); i++)
    {
        const SceneInfo::Batch& b = current.Batches[MovingCurrent[i]];
        s32 partner = PartnerCurrent[i];
        HALFCAM_ADD(" v%ug%u@%.2f,%.2f,%.2f", b.Vertices, b.ReadbackLoads, b.Words[9] / 4096.0, b.Words[10] / 4096.0, b.Words[11] / 4096.0);
        if (partner >= 0)
        {
            const SceneInfo::Batch& a = previous.Batches[MovingPrevious[partner]];
            s64 distance = 0;
            for (u32 j = 9; j < 12; j++)
                distance = std::max(distance, WordDistance(a.Words[j], b.Words[j]));
            HALFCAM_ADD(":%c%d,d%.2f", "api"[PartnerTier[i]], partner, distance / 4096.0);
        }
        else
            HALFCAM_ADD(":%s", PartnerTooFar[i] ? "far" : PartnerRotationFar[i] ? "rot" : "none");
    }
    if (NumMovingCurrent > MaxTraceCurrent)
        HALFCAM_ADD(" +%u", NumMovingCurrent - MaxTraceCurrent);

    HALFCAM_ADD(" |");
    u32 shown = 0, vanished = 0;
    for (u32 k = 0; k < NumMovingPrevious; k++)
    {
        if (IsPartnerUsed[k])
            continue;
        vanished++;
        if (shown < MaxTracePrevious)
        {
            const SceneInfo::Batch& a = previous.Batches[MovingPrevious[k]];
            HALFCAM_ADD(" %uv%ug%u@%.2f,%.2f,%.2f", k, a.Vertices, a.ReadbackLoads, a.Words[9] / 4096.0, a.Words[10] / 4096.0, a.Words[11] / 4096.0);
            shown++;
        }
    }
    if (vanished > shown)
        HALFCAM_ADD(" +%u", vanished - shown);

#undef HALFCAM_ADD

    Log(LogLevel::Info, "HALFCAM pair %s\n", line);
}

void HalfCamera::LogSecond(Clock::time_point now)
{
    double replayDiv = Total.Replays ? Total.Replays : 1;
    double sceneDiv = Total.Scenes ? Total.Scenes : 1;
    double share = Total.Vertices ? Total.TopVertices * 100.0 / Total.Vertices : 0.0;

    Log(LogLevel::Info, "HALFCAM sec mode=%d vb=%u scenes=%u interp=%u nolock=%u render=%u cap=%u capab=%u capre=%u loadre=%u stream=%u recovf=%u share=%u far=%u still=%u empty=%u full=%u "
        "sharepct=%.1f minshare=%.1f near=%u nearstatic=%u paired=%u unpaired=%u pairrot=%u batchovf=%u anaus=%.0f replayus=%.0f maxus=%.0f subv=%.0f poly=%.0f\n",
        Mode, Total.VBlanks, Total.Scenes, Total.Interpolated,
        Total.Skipped[Reason_NotLocked], Total.Skipped[Reason_Renderer], Total.Skipped[Reason_Capture], Total.CaptureAborted, Total.CaptureRedrawn, Total.LoadRedrawn,
        Total.Skipped[Reason_Stream], Total.RecordOverflows, Total.Skipped[Reason_Share], Total.Skipped[Reason_Far], Total.Skipped[Reason_Still],
        Total.Skipped[Reason_Empty], Total.Skipped[Reason_Full],
        share, (double)Total.MinShare, Total.NearLoads, Total.NearStatic, Total.Paired, Total.Unpaired, Total.PairRotation, Total.BatchOverflows,
        Total.AnalysisNs / sceneDiv / 1000.0, Total.ReplayNs / replayDiv / 1000.0, Total.MaxReplayNs / 1000.0,
        Total.ReplayVertices / replayDiv, Total.ReplayPolygons / replayDiv);

    if (Mode >= 3)
    {
        Log(LogLevel::Info, "HALFCAM diag dom=%u/%llu same=%u/%u chg=%u/%u noload=%u pair=%u/%u unp=%u/%u pairid=%u pairpos=%u pairfar=%u pairmax=%.3f "
            "bb=%u bbcam=%u bbstill=%u bbother=%u bbfix=%u bbmax=%.3f bbunp=%u bbrej=%u bbflush=%u bbovf=%u\n",
            Total.TopGroups, (unsigned long long)Total.TopVertices, Total.SameLoadGroups, Total.SameLoadVertices, Total.ChangedLoadGroups, Total.ChangedLoadVertices,
            Total.NoLoadVerticesTotal, Total.Paired, Total.PairedVertices, Total.Unpaired, Total.UnpairedVertices,
            Total.PairedByIdentity, Total.PairedByPosition, Total.PairTooFar, Total.PairMaxDistance / 4096.0,
            Total.ReadbackLoads, Total.ReadbackWithCamera, Total.ReadbackStill, Total.ReadbackOther, Total.ReadbackFixed, Total.ReadbackMaxDistance / 4096.0,
            Total.ReadbackUnpaired, Total.ReadbackRejected, Total.ReadbackFlushPath, Total.ReadbackOverflows);
    }

    Total = {};
    Total.MinShare = 100.0f;
    LastLog = Clock::now();
}

}
