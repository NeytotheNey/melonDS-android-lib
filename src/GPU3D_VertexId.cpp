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

#include "GPU3D_VertexId.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include "GPU3D.h"
#include "Platform.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

static constexpr u64 HashSeed = 0x243F6A8885A308D3ULL;
static constexpr u64 IdentityLoad = 0x13198A2E03707344ULL;

static u64 Mix(u64 hash, u64 value)
{
    hash = (hash ^ value) * 0x9E3779B97F4A7C15ULL;
    return hash ^ (hash >> 29);
}

// the parameter words of a command that loads or changes a matrix
static u32 MatrixCommandWords(u32 command)
{
    switch (command)
    {
    case 0x16: return 16; // load 4x4
    case 0x17: return 12; // load 4x3
    case 0x18: return 16; // mult 4x4
    case 0x19: return 12; // mult 4x3
    case 0x1A: return 9;  // mult 3x3
    case 0x1B: return 3;  // scale
    case 0x1C: return 3;  // translate
    default: return 0;
    }
}

static u64 MixWords(u64 hash, const u32* words, u32 count)
{
    for (u32 i = 0; i < count; i++)
        hash = Mix(hash, words[i]);
    return hash;
}

// the old polygon matcher, from the shelved geometry interpolation (docs/frame-interpolation.md).
// all distances are in 1/16 pixels, like the hi-res positions
static constexpr s32 Pixel = 16;
static constexpr u32 SearchWindow = 32;
static constexpr s32 MaxMatchError = 12 * Pixel;
static constexpr s32 MaxResyncError = 4 * Pixel;
static constexpr s32 MaxCenterDistance = 64 * Pixel;
static constexpr s32 SkipCost = Pixel;
static constexpr s32 StillCost = 2 * Pixel;
static constexpr s32 CellSize = 16 * Pixel;
static constexpr u32 MaxCellCandidates = 16;
static constexpr u32 ShiftSamples = 16;
static constexpr s32 MaxShiftOffset = 3;
static constexpr u32 MinShiftSamples = 8;
static constexpr s32 ShiftTolerance = Pixel;
static constexpr s32 ShiftOffsetCost = 2 * Pixel;

static u64 PolygonSignature(const Polygon& poly)
{
    u32 flags = (poly.FacingView ? 1 : 0)
        | (poly.IsShadowMask ? 2 : 0)
        | (poly.IsShadow ? 4 : 0)
        | (poly.WBuffer ? 8 : 0)
        | ((poly.Type & 0xF) << 4);

    u64 hash = Mix(HashSeed, ((u64)(poly.Attr & ~0x001F0000u) << 32) | flags);
    return Mix(hash, ((u64)poly.TexParam << 32) | poly.TexPalette);
}

static s32 Distance(s32 ax, s32 ay, s32 bx, s32 by)
{
    return std::max(std::abs(ax - bx), std::abs(ay - by));
}

static s32 PairError(s32 oldX, s32 oldY, s32 newX, s32 newY, s32 shiftX, s32 shiftY)
{
    s32 shifted = Distance(oldX, oldY, newX - shiftX, newY - shiftY);
    s32 still = Distance(oldX, oldY, newX, newY) + StillCost;
    return std::min(shifted, still);
}

static s32 CellOf(s32 v)
{
    return v >= 0 ? v / CellSize : -((CellSize - 1 - v) / CellSize);
}

static u64 CellKey(u64 signature, s32 cellX, s32 cellY)
{
    return Mix(signature, ((u64)(u32)cellX << 32) | (u32)cellY);
}

VertexIdLogger::VertexIdLogger(int mode) : Mode(mode), LastLog(Clock::now())
{
    Invalidate();
}

// clocking every call would distort what it measures
template <typename Body>
void VertexIdLogger::Sampled(Body&& body)
{
    if ((HookCalls++ & 0xFF) != 0)
    {
        body();
        return;
    }

    Clock::time_point start = Clock::now();
    body();
    SampledNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    SampledCalls++;
}

void VertexIdLogger::OnMatrixCommand(u32 command, const u32* params, const GPU3D& gpu)
{
    if (command < 0x11 || command > 0x1C || gpu.MatrixMode == 3)
        return;

    if (gpu.MatrixMode == 0)
    {
        // a camera could be sent in the projection matrix. what's on its stack doesn't matter here,
        // this is only what the scene sent
        if (command >= 0x15)
        {
            Sampled([&]()
            {
                u64& hash = command <= 0x17 ? ProjectionLoadHash : ProjectionMultHash;
                hash = MixWords(Mix(hash, command), params, MatrixCommandWords(command));
            });
        }
        return;
    }

    Sampled([&]()
    {
        switch (command)
        {
        case 0x11: // push
            MatrixStack[gpu.PosMatrixStackPointer & 0x1F] = Matrix;
            return;

        case 0x12: // pop
            Matrix = MatrixStack[(gpu.PosMatrixStackPointer - ((s32)(params[0] << 26) >> 26)) & 0x1F];
            return;

        case 0x13: // store
            MatrixStack[params[0] & 0x1F] = Matrix;
            return;

        case 0x14: // restore
            Matrix = MatrixStack[params[0] & 0x1F];
            return;

        case 0x15: // identity, which replaces the matrix just like a load
            Matrix.Placement = HashSeed;
            Matrix.Load = IdentityLoad;
            Matrix.LoadScene = SceneNumber;
            Scene.Identity++;
            return;
        }

        u32 numWords = MatrixCommandWords(command);
        if (command == 0x16 || command == 0x17)
        {
            Matrix.Placement = HashSeed;
            Matrix.Load = MixWords(HashSeed + command, params, numWords);
            Matrix.LoadScene = SceneNumber;
            if (VBlanksInScene == 0)
                Scene.LoadsEarly++;
            else
                Scene.LoadsLate++;
        }
        else
        {
            Matrix.Placement = MixWords(Mix(Matrix.Placement, command), params, numWords);
        }
    });
}

void VertexIdLogger::OnVertex(const GPU3D& gpu)
{
    Sampled([&]()
    {
        Scene.Submitted++;

        // whether this vertex completes a polygon, as GPU3D::SubmitVertex decides it
        u32 inPolygon = gpu.VertexNumInPoly + 1;
        switch (gpu.PolygonMode)
        {
        case 0: Scene.Attempted += inPolygon == 3; break;
        case 1: Scene.Attempted += inPolygon == 4; break;
        case 2: Scene.Attempted += (gpu.NumConsecutivePolygons & 1) || inPolygon == 3; break;
        case 3: Scene.Attempted += inPolygon == 4; break;
        }

        if (NumRecords >= MaxVertices)
        {
            Scene.Overflow++;
            return;
        }

        VertexRecord& record = Records[NumRecords++];
        record.Placement = Matrix.Placement;
        record.Load = Matrix.Load;
        record.HasLoad = Matrix.LoadScene == SceneNumber;

        u64 position = (u64)(u16)gpu.CurVertex[0] | ((u64)(u16)gpu.CurVertex[1] << 16) | ((u64)(u16)gpu.CurVertex[2] << 32);
        u64 texture = (u64)(u16)gpu.RawTexCoords[0] | ((u64)(u16)gpu.RawTexCoords[1] << 16) | ((u64)gpu.TexParam << 32);
        record.Hash = Mix(Mix(HashSeed, position), texture);
    });
}

void VertexIdLogger::OnPolygon(u32 index, const GPU3D& gpu)
{
    if (index >= MaxPolygons)
        return;

    // a polygon is always the last vertices submitted, for strips too, whatever the swaps in
    // TempVertexBuffer and the clipping did to the copies in the vertex RAM
    Sampled([&]()
    {
        PolygonSources[index] = { Scene.Overflow ? 0 : NumRecords, (gpu.PolygonMode & 1) ? 4u : 3u };
    });
}

void VertexIdLogger::OnPolygonRejected()
{
    Scene.RamRejected++;
}

void VertexIdLogger::OnBoxTest(bool passed)
{
    Scene.BoxTests++;
    if (passed)
        Scene.BoxPassed++;
}

void VertexIdLogger::OnVBlank()
{
    VBlanksInScene++;
    TotalVBlanks++;

    Clock::time_point now = Clock::now();
    if (now - LastLog >= std::chrono::seconds(1))
        LogSecond(now);
}

void VertexIdLogger::OnSwap(const GPU3D& gpu)
{
    Clock::time_point start = Clock::now();

    if (IsSceneValid)
    {
        AnalyzeScene(gpu);
    }
    else
    {
        TotalDropped++;
        HasPrevious = false;
    }

    Scene = {};
    NumRecords = 0;
    VBlanksInScene = 0;
    ProjectionLoadHash = ProjectionMultHash = HashSeed;
    SceneNumber++;
    IsSceneValid = true;

    TimedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

void VertexIdLogger::Invalidate()
{
    IsSceneValid = false;
    Matrix = { HashSeed, 0, 0 };
    MatrixStack.fill(Matrix);
    NumRecords = 0;
    VBlanksInScene = 0;
    ProjectionLoadHash = ProjectionMultHash = HashSeed;
    Scene = {};
}

void VertexIdLogger::AnalyzeScene(const GPU3D& gpu)
{
    AnalyzeVertices();
    AnalyzePolygons(gpu);

    bool isPair = HasPrevious;
    if (isPair)
    {
        CompareWithSignatures();

        bool isOrderChanged = OrderHash != PreviousOrderHash;
        TotalPairs++;
        TotalDominantChanged += DominantLoad != PreviousDominantLoad;
        TotalOrderChanged += isOrderChanged;
        TotalReordered += isOrderChanged && !Scene.PlacementsNew && !Scene.PlacementsGone;
        TotalSplitChanged += Scene.LoadsEarly != PreviousLoadsEarly || Scene.LoadsLate != PreviousLoadsLate;
        TotalProjectionLoadChanged += ProjectionLoadHash != PreviousProjectionLoadHash;
        TotalProjectionMultChanged += ProjectionMultHash != PreviousProjectionMultHash;
    }

    constexpr u32 numCounts = sizeof(SceneCounts) / sizeof(u32);
    const u32* sceneCounts = reinterpret_cast<const u32*>(&Scene);
    u32* totalCounts = reinterpret_cast<u32*>(&Total);
    for (u32 i = 0; i < numCounts; i++)
        totalCounts[i] += sceneCounts[i];
    TotalScenes++;

    if (Mode >= 2)
        LogScene(isPair);

    PreviousDominantLoad = DominantLoad;
    PreviousOrderHash = OrderHash;
    PreviousLoadsEarly = Scene.LoadsEarly;
    PreviousLoadsLate = Scene.LoadsLate;
    PreviousProjectionLoadHash = ProjectionLoadHash;
    PreviousProjectionMultHash = ProjectionMultHash;
    HasPrevious = true;
    Bank ^= 1;
}

void VertexIdLogger::AnalyzeVertices()
{
    VertexSlots& keys = KeySets[Bank];
    VertexSlots& placements = PlacementSets[Bank];
    const VertexSlots& previousKeys = KeySets[Bank ^ 1];
    const VertexSlots& previousPlacements = PlacementSets[Bank ^ 1];

    keys.Clear();
    placements.Clear();
    PairCounts.Clear();
    LoadCounts.Clear();

    OrderHash = HashSeed;
    DominantLoad = 0;

    for (u32 i = 0; i < NumRecords; i++)
    {
        const VertexRecord& record = Records[i];
        bool isNew;

        u64 pair = Mix(Mix(HashSeed, record.Placement), record.Hash);
        auto& pairSlot = PairCounts.FindOrAdd(pair, isNew);
        u32 pairSlotIndex = (u32)(&pairSlot - PairCounts.Slots.data());
        u32 occurrence = pairSlot.Value++;
        if (occurrence)
            Scene.Duplicates++;
        else
            PairFirstRecords[pairSlotIndex] = i;

        u64 key = Mix(pair, occurrence);
        VertexKeys[i] = key;
        keys.FindOrAdd(key, isNew);

        if (HasPrevious)
        {
            if (previousKeys.Find(key))
            {
                Scene.Matched++;
                if (occurrence)
                {
                    Scene.MatchedByOccurrence++;
                    if (record.Load != Records[PairFirstRecords[pairSlotIndex]].Load)
                        Scene.MatchedOtherLoad++;
                }
            }
            else
                Scene.UnmatchedNew++;
        }

        placements.FindOrAdd(record.Placement, isNew);
        if (isNew)
        {
            OrderHash = Mix(OrderHash, record.Placement);
            Scene.Placements++;
            if (HasPrevious && !previousPlacements.Find(record.Placement))
                Scene.PlacementsNew++;
        }

        // neither of these says anything about a camera: with an identity load, it's in the placement
        // hash, and a load of 0 means the matrix isn't known
        if (record.Load == IdentityLoad)
        {
            Scene.IdentityVertices++;
        }
        else if (record.Load == 0)
        {
            Scene.UnknownVertices++;
        }
        else
        {
            auto& loadSlot = LoadCounts.FindOrAdd(record.Load, isNew);
            if (isNew)
                Scene.Cameras++;
            loadSlot.Value++;
            if (loadSlot.Value > Scene.DominantVertices)
            {
                Scene.DominantVertices = loadSlot.Value;
                DominantLoad = record.Load;
            }
        }

        if (!record.HasLoad)
            Scene.NoLoad++;
    }

    if (HasPrevious)
    {
        Scene.UnmatchedOld = previousKeys.Count - std::min(previousKeys.Count, Scene.Matched);
        u32 placementsKept = Scene.Placements - Scene.PlacementsNew;
        Scene.PlacementsGone = previousPlacements.Count - std::min(previousPlacements.Count, placementsKept);
    }
}

// CurPolygonRAM has them in the order the game submitted them, unlike the Y-sorted RenderPolygonRAM
void VertexIdLogger::AnalyzePolygons(const GPU3D& gpu)
{
    u32 numPolygons = std::min(gpu.NumPolygons, MaxPolygons);
    PolygonSlots& keySet = PolygonKeySets[Bank];
    keySet.Clear();

    Scene.Polygons = numPolygons;
    Scene.RamVertices = gpu.NumVertices;

    for (u32 i = 0; i < numPolygons; i++)
    {
        const Polygon& poly = gpu.CurPolygonRAM[i];
        PolygonRecord& record = PolygonRecords[Bank][i];

        record.Signature = PolygonSignature(poly);
        record.NumVertices = (u16)poly.NumVertices;
        record.ClipMask = 0;

        s64 sumX = 0, sumY = 0;
        for (u32 j = 0; j < poly.NumVertices; j++)
        {
            const Vertex& vertex = *poly.Vertices[j];
            sumX += vertex.HiresPosition[0];
            sumY += vertex.HiresPosition[1];
            if (vertex.Clipped)
            {
                record.ClipMask |= 1u << j;
                Scene.ClippedVertices++;
            }
        }
        record.X = poly.NumVertices ? (s32)(sumX / poly.NumVertices) : 0;
        record.Y = poly.NumVertices ? (s32)(sumY / poly.NumVertices) : 0;
        if (record.ClipMask)
            Scene.ClippedPolygons++;

        // the polygon's key is made of the keys of all its submitted vertices, in order
        const PolygonSource& source = PolygonSources[i];
        record.Key = 0;
        if (source.End >= source.NumVertices && source.End > 0)
        {
            u64 key = HashSeed;
            for (u32 j = source.End - source.NumVertices; j < source.End; j++)
                key = Mix(key, VertexKeys[j]);
            record.Key = key ? key : 1;

            bool isNew;
            auto& slot = keySet.FindOrAdd(record.Key, isNew);
            if (isNew)
                slot.Value = i;
        }
        else
            Scene.Unmapped++;
    }

    NumPolygonRecords[Bank] = numPolygons;
}

// walks the new scene against the previous one the way the shelved GeometryInterpolator::Build paired
// polygons (signature, 32 polygon window, resync past it, last pair's shift), and counts where that
// pairing differs from the one the keys give. it pairs them like the old one did before its later
// per-vertex distance check, which didn't affect the walk
void VertexIdLogger::CompareWithSignatures()
{
    const PolygonRecord* oldScene = PolygonRecords[Bank ^ 1].data();
    const PolygonRecord* newScene = PolygonRecords[Bank].data();
    u32 numOld = NumPolygonRecords[Bank ^ 1];
    u32 numNew = NumPolygonRecords[Bank];
    const PolygonSlots& oldKeys = PolygonKeySets[Bank ^ 1];

    IsOldCellsValid = false;

    u32 cursor = 0;
    s32 shiftX, shiftY;
    EstimateShift(oldScene, numOld, newScene, numNew, shiftX, shiftY);

    for (u32 j = 0; j < numNew; j++)
    {
        const PolygonRecord& polygon = newScene[j];

        s32 best = -1;
        s32 bestCost = INT_MAX;
        u32 end = std::min(cursor + SearchWindow, numOld);
        for (u32 k = cursor; k < end; k++)
        {
            const PolygonRecord& old = oldScene[k];
            if (old.Signature != polygon.Signature)
                continue;
            if (Distance(old.X, old.Y, polygon.X, polygon.Y) >= MaxCenterDistance)
                continue;

            s32 error = PairError(old.X, old.Y, polygon.X, polygon.Y, shiftX, shiftY);
            if (error >= MaxMatchError)
                continue;

            s32 cost = error + (s32)(k - cursor) * SkipCost;
            if (cost < bestCost)
            {
                bestCost = cost;
                best = (s32)k;
            }
        }

        if (best < 0 && end < numOld)
            best = FindBeyondWindow(oldScene, polygon, end, numOld, shiftX, shiftY);

        s32 keyPartner = -1;
        if (polygon.Key)
        {
            const auto* slot = oldKeys.Find(polygon.Key);
            if (slot)
                keyPartner = (s32)slot->Value;
        }

        bool isClipDifferent = false;
        if (keyPartner >= 0)
        {
            const PolygonRecord& keyOld = oldScene[keyPartner];
            isClipDifferent = keyOld.NumVertices != polygon.NumVertices || keyOld.ClipMask != polygon.ClipMask;
            Scene.PolygonsMatched++;
        }

        if (best < 0)
        {
            if (keyPartner < 0)
                Scene.AgreeNone++;
            else if (isClipDifferent)
                Scene.ClipDifferent++;
            else
                Scene.KeyOnly++;
            continue;
        }

        if (keyPartner < 0)
            Scene.SignatureOnly++;
        else if (keyPartner != best)
            Scene.Wrong++;
        else if (isClipDifferent)
            Scene.ClipDifferent++;
        else
            Scene.Agree++;

        cursor = (u32)best + 1;

        // only from pairs of the same shape, clipping moves the center
        const PolygonRecord& partner = oldScene[best];
        if (partner.NumVertices == polygon.NumVertices && partner.ClipMask == polygon.ClipMask)
        {
            shiftX = polygon.X - partner.X;
            shiftY = polygon.Y - partner.Y;
        }
    }

    Scene.PolygonsCompared = numNew;
    Scene.PolygonsOldOnly = numOld - std::min(numOld, Scene.PolygonsMatched);
}

void VertexIdLogger::EstimateShift(const PolygonRecord* oldScene, u32 numOld, const PolygonRecord* newScene, u32 numNew, s32& shiftX, s32& shiftY) const
{
    shiftX = 0;
    shiftY = 0;

    s32 bestCost = INT_MAX;
    for (s32 offset = -MaxShiftOffset; offset <= MaxShiftOffset; offset++)
    {
        s32 dx[ShiftSamples], dy[ShiftSamples];
        u32 count = 0;
        for (u32 i = 0; i < ShiftSamples && i < numNew; i++)
        {
            s32 k = (s32)i + offset;
            if (k < 0 || k >= (s32)numOld)
                continue;

            const PolygonRecord& old = oldScene[k];
            const PolygonRecord& polygon = newScene[i];
            if (old.Signature != polygon.Signature || Distance(old.X, old.Y, polygon.X, polygon.Y) >= MaxCenterDistance)
                continue;
            dx[count] = polygon.X - old.X;
            dy[count] = polygon.Y - old.Y;
            count++;
        }
        if (count < MinShiftSamples)
            continue;

        s32 sortedX[ShiftSamples], sortedY[ShiftSamples];
        std::copy_n(dx, count, sortedX);
        std::copy_n(dy, count, sortedY);
        std::nth_element(sortedX, sortedX + count / 2, sortedX + count);
        std::nth_element(sortedY, sortedY + count / 2, sortedY + count);
        s32 medianX = sortedX[count / 2], medianY = sortedY[count / 2];

        u32 consistent = 0;
        for (u32 i = 0; i < count; i++)
        {
            if (Distance(dx[i], dy[i], medianX, medianY) <= ShiftTolerance)
                consistent++;
        }
        if (consistent * 4 < count * 3)
            continue;

        s32 cost = Distance(medianX, medianY, 0, 0) + std::abs(offset) * ShiftOffsetCost;
        if (cost < bestCost)
        {
            bestCost = cost;
            shiftX = medianX;
            shiftY = medianY;
        }
    }
}

s32 VertexIdLogger::FindBeyondWindow(const PolygonRecord* oldScene, const PolygonRecord& polygon, u32 start, u32 numOld, s32 shiftX, s32 shiftY)
{
    auto isBefore = [](const CellEntry& a, const CellEntry& b)
    {
        return a.Key != b.Key ? a.Key < b.Key : a.Index < b.Index;
    };

    if (!IsOldCellsValid)
    {
        for (u32 i = 0; i < numOld; i++)
            OldCells[i] = { CellKey(oldScene[i].Signature, CellOf(oldScene[i].X), CellOf(oldScene[i].Y)), i };
        std::sort(OldCells.begin(), OldCells.begin() + numOld, isBefore);
        IsOldCellsValid = true;
    }

    s32 best = -1;
    s32 bestError = MaxResyncError;
    for (int hypothesis = 0; hypothesis < 2; hypothesis++)
    {
        if (hypothesis == 1 && shiftX == 0 && shiftY == 0)
            break;

        s32 cellX = CellOf(polygon.X - (hypothesis == 0 ? shiftX : 0));
        s32 cellY = CellOf(polygon.Y - (hypothesis == 0 ? shiftY : 0));
        for (s32 y = cellY - 1; y <= cellY + 1; y++)
        {
            for (s32 x = cellX - 1; x <= cellX + 1; x++)
            {
                CellEntry first = { CellKey(polygon.Signature, x, y), start };
                auto it = std::lower_bound(OldCells.begin(), OldCells.begin() + numOld, first, isBefore);

                for (u32 tried = 0; it != OldCells.begin() + numOld && it->Key == first.Key && tried < MaxCellCandidates; ++it, tried++)
                {
                    const PolygonRecord& old = oldScene[it->Index];
                    if (old.Signature != polygon.Signature || Distance(old.X, old.Y, polygon.X, polygon.Y) >= MaxCenterDistance)
                        continue;

                    s32 error = PairError(old.X, old.Y, polygon.X, polygon.Y, shiftX, shiftY);
                    if (error < bestError || (error == bestError && best >= 0 && (s32)it->Index < best))
                    {
                        bestError = error;
                        best = (s32)it->Index;
                    }
                }
            }
        }
    }

    return best;
}

void VertexIdLogger::LogScene(bool isPair)
{
    u32 recorded = Scene.Submitted - Scene.Overflow;
    double dominantShare = recorded ? Scene.DominantVertices * 100.0 / recorded : 0.0;
    double identityShare = recorded ? Scene.IdentityVertices * 100.0 / recorded : 0.0;
    double polygonCoverage = Scene.PolygonsCompared ? Scene.PolygonsMatched * 100.0 / Scene.PolygonsCompared : 0.0;
    u32 projectionChanged = isPair ? ((ProjectionLoadHash != PreviousProjectionLoadHash ? 1 : 0) | (ProjectionMultHash != PreviousProjectionMultHash ? 2 : 0)) : 0;

    Log(LogLevel::Info, "VERTEXID scene n=%u pair=%d vb=%u sub=%u att=%u poly=%u ramrej=%u clip=%u clipv=%u ramv=%u ovf=%u unmap=%u "
        "match=%u occ=%u occload=%u newv=%u oldv=%u dup=%u agree=%u none=%u wrong=%u sigonly=%u keyonly=%u clipdiff=%u pcov=%.1f pold=%u "
        "cam=%u dom=%.1f identv=%.1f domhash=%08X noload=%u unwr=%u early=%u late=%u ident=%u order=%08X "
        "projload=%08X projmult=%08X projchg=%u "
        "box=%u pass=%u fail=%u plc=%u plcnew=%u plcgone=%u\n",
        SceneNumber, isPair ? 1 : 0, VBlanksInScene, Scene.Submitted, Scene.Attempted, Scene.Polygons, Scene.RamRejected, Scene.ClippedPolygons,
        Scene.ClippedVertices, Scene.RamVertices, Scene.Overflow, Scene.Unmapped,
        Scene.Matched, Scene.MatchedByOccurrence, Scene.MatchedOtherLoad, Scene.UnmatchedNew, Scene.UnmatchedOld, Scene.Duplicates,
        Scene.Agree, Scene.AgreeNone, Scene.Wrong, Scene.SignatureOnly, Scene.KeyOnly, Scene.ClipDifferent, polygonCoverage, Scene.PolygonsOldOnly,
        Scene.Cameras, dominantShare, identityShare, (u32)DominantLoad, Scene.NoLoad, Scene.UnknownVertices, Scene.LoadsEarly, Scene.LoadsLate, Scene.Identity, (u32)OrderHash,
        (u32)ProjectionLoadHash, (u32)ProjectionMultHash, projectionChanged,
        Scene.BoxTests, Scene.BoxPassed, Scene.BoxTests - Scene.BoxPassed, Scene.Placements, Scene.PlacementsNew, Scene.PlacementsGone);
}

void VertexIdLogger::LogSecond(Clock::time_point now)
{
    double scenes = TotalScenes ? TotalScenes : 1;
    double pairs = TotalPairs ? TotalPairs : 1;
    u32 recorded = Total.Submitted - Total.Overflow;
    double dominantShare = recorded ? Total.DominantVertices * 100.0 / recorded : 0.0;
    double identityShare = recorded ? Total.IdentityVertices * 100.0 / recorded : 0.0;
    double polygonCoverage = Total.PolygonsCompared ? Total.PolygonsMatched * 100.0 / Total.PolygonsCompared : 0.0;
    double hookNs = SampledCalls ? (double)SampledNs / SampledCalls * HookCalls : 0.0;
    u32 wallMs = (u32)std::chrono::duration_cast<std::chrono::milliseconds>(now - LastLog).count();

    Log(LogLevel::Info, "VERTEXID sec scenes=%u pairs=%u drop=%u vb=%u wallms=%u "
        "sub=%.1f att=%.1f poly=%.1f ramrej=%.1f clip=%.1f clipv=%.1f ramv=%.1f ovf=%u unmap=%.1f "
        "match=%.1f occ=%.1f occload=%.1f newv=%.1f oldv=%.1f dup=%.1f "
        "agree=%.1f none=%.1f wrong=%.1f sigonly=%.1f keyonly=%.1f clipdiff=%.1f pcov=%.1f pold=%.1f "
        "cam=%.1f dom=%.1f identv=%.1f domchg=%u noload=%.1f unwr=%.1f loads=%.1f ident=%.1f "
        "ordchg=%u reorder=%u early=%.1f late=%.1f splitchg=%u projloadchg=%u projmultchg=%u "
        "box=%.1f pass=%.1f fail=%.1f plc=%.1f plcnew=%.1f plcgone=%.1f us=%.0f hookus=%.0f\n",
        TotalScenes, TotalPairs, TotalDropped, TotalVBlanks, wallMs,
        Total.Submitted / scenes, Total.Attempted / scenes, Total.Polygons / scenes, Total.RamRejected / scenes, Total.ClippedPolygons / scenes,
        Total.ClippedVertices / scenes, Total.RamVertices / scenes, Total.Overflow, Total.Unmapped / scenes,
        Total.Matched / pairs, Total.MatchedByOccurrence / pairs, Total.MatchedOtherLoad / pairs, Total.UnmatchedNew / pairs, Total.UnmatchedOld / pairs, Total.Duplicates / scenes,
        Total.Agree / pairs, Total.AgreeNone / pairs, Total.Wrong / pairs, Total.SignatureOnly / pairs, Total.KeyOnly / pairs,
        Total.ClipDifferent / pairs, polygonCoverage, Total.PolygonsOldOnly / pairs,
        Total.Cameras / scenes, dominantShare, identityShare, TotalDominantChanged, Total.NoLoad / scenes, Total.UnknownVertices / scenes, (Total.LoadsEarly + Total.LoadsLate) / scenes, Total.Identity / scenes,
        TotalOrderChanged, TotalReordered, Total.LoadsEarly / scenes, Total.LoadsLate / scenes, TotalSplitChanged, TotalProjectionLoadChanged, TotalProjectionMultChanged,
        Total.BoxTests / scenes, Total.BoxPassed / scenes, (Total.BoxTests - Total.BoxPassed) / scenes,
        Total.Placements / scenes, Total.PlacementsNew / pairs, Total.PlacementsGone / pairs,
        TimedNs / 1000.0, hookNs / 1000.0);

    Total = {};
    TotalScenes = TotalPairs = TotalDropped = TotalVBlanks = 0;
    TotalDominantChanged = TotalOrderChanged = TotalReordered = TotalSplitChanged = 0;
    TotalProjectionLoadChanged = TotalProjectionMultChanged = 0;
    TimedNs = 0;
    HookCalls = SampledCalls = 0;
    SampledNs = 0;
    LastLog = Clock::now();
    // the time this line took goes into the next second's count
    TimedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(LastLog - now).count();
}

}
