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

#ifndef GPU3D_VERTEXID_H
#define GPU3D_VERTEXID_H

#include <array>
#include <chrono>

#include "types.h"

namespace melonDS
{
class GPU3D;
struct Polygon;

// debug only, logging only (debug.melonade.vertexid, see DebugProperties.h): measures whether 3D
// vertices have a stable identity between consecutive scenes, and whether the camera matrix can be
// told apart. it only ever reads the GPU3D, runs on the emulator thread, and isn't in savestates.
//
// a scene runs from one polygon RAM swap to the next, not from VBlank to VBlank. the vertex key is
// (placement hash, vertex hash, occurrence of that pair in the scene):
// * placement hash: the raw words of the position matrix multiplies, translates and scales since the
//   last load. it leaves out the load itself, which is the camera if the game sends it that way
// * vertex hash: the object space position after the vertex commands resolved it, the raw UV and
//   TexParam
//
// VERTEXID sec, once a second, averages per scene (sc) or per pair of consecutive scenes (pr):
//   scenes pairs drop  scenes, compared pairs, and scenes left out after a reset or a state load
//   vb wallms          VBlanks and milliseconds this line covers
//   sub att            sc: vertices and polygons submitted, before culling and clipping
//   poly               sc: polygons in CurPolygonRAM
//   ramrej             sc: polygons rejected because the polygon or vertex RAM was full
//   clip clipv         sc: polygons with a clipped vertex, clipped vertices
//   ramv               sc: vertices in CurVertexRAM
//   ovf                total vertices that didn't fit the side table, so they have no key
//   unmap              sc: polygons without a key, as their vertices overflowed or span a swap
//   match occ          pr: new vertices whose key is in the previous scene, and how many of those
//                      needed an occurrence above 0
//   occload            pr: of occ, those whose load hash differs from occurrence 0's in that scene,
//                      so objects told apart only by order, which a culled one shifts. it shows the
//                      exposure, not how many really got swapped. shared vertices don't count
//   newv oldv          pr: vertices only in the new scene, and only in the previous one
//   dup                sc: vertices whose (placement, vertex) pair already came up in the scene
//   agree none wrong sigonly keyonly clipdiff
//                      pr: the old signature match per polygon against the key match: same partner,
//                      neither found one, different partners, only signature found one, only key
//                      did, and clipdiff: agree or keyonly cases where the key partner has another
//                      clip shape (NumVertices or ClipMask), taken out of those two, which the old
//                      code rejected. the six add up to poly
//   pcov pold          pr: percent of the polygons in the RAM with a key partner in the previous
//                      scene's RAM, and that scene's polygons without one in the new scene
//   cam dom            sc: distinct load hashes under the vertices, and the percent of the vertices
//                      under the most common one, leaving out MTX_IDENTITY and unknown (load 0)
//   identv             percent of the vertices whose matrix comes from MTX_IDENTITY
//   domchg             pairs where the most common load hash changed
//   noload             sc: vertices whose matrix comes from no load in this scene
//   unwr               sc: vertices with an unknown matrix: from a stack slot not written since the
//                      last reset or state load, or the matrix as it was then. included in noload
//   loads ident        sc: MTX_LOAD_4x3/4x4 and MTX_IDENTITY commands in the position modes
//   ordchg reorder     pairs where the order of first appearance of the placement hashes changed,
//                      and those of them with the same set of placement hashes
//   early late splitchg  sc: matrix loads before and after the scene's first VBlank, and the pairs
//                      where that split changed
//   projloadchg projmultchg  pairs where the projection hashes changed, see projload and projmult
//   box pass fail      sc: BoxTest commands and their results
//   plc plcnew plcgone sc: distinct placement hashes; pr: only in the new scene, only in the previous
//   us hookus          microseconds this second in this code: scene analysis and log lines by clock,
//                      and the per-command hooks estimated from one call in 256 (an upper bound, it
//                      includes a clock read)
//
// VERTEXID scene, per scene with debug.melonade.vertexid=2, has the same fields as plain numbers for
// that scene, plus n (scene number, shows lines over the log budget), pair (1 if compared with the
// previous scene), domhash (low 32 bits of the most common load hash), order (low 32 bits of the
// first appearance hash), projload and projmult (low 32 bits of the hash of the projection matrix
// loads, and of its multiplies, translates and scales, sent in this scene, in order, so a scene
// without any has a fixed value) and projchg (bit 0: projload changed, bit 1: projmult changed)
class VertexIdLogger
{
public:
    // [mode] 1 for the line per second, 2 for the line per scene as well
    explicit VertexIdLogger(int mode);

    [[nodiscard]] int GetMode() const noexcept { return Mode; }

    // before the command runs, so the stack pointer is still the old one. ignores other commands
    void OnMatrixCommand(u32 command, const u32* params, const GPU3D& gpu);
    // when a vertex is submitted, before the polygon it may complete
    void OnVertex(const GPU3D& gpu);
    // when a polygon is added to CurPolygonRAM at [index]
    void OnPolygon(u32 index, const GPU3D& gpu);
    // when a polygon doesn't fit in the polygon or vertex RAM
    void OnPolygonRejected();
    void OnBoxTest(bool passed);
    void OnVBlank();
    // just before the polygon RAM banks swap
    void OnSwap(const GPU3D& gpu);
    // after a reset or a state load: the scene in progress can't be trusted
    void Invalidate();

private:
    using Clock = std::chrono::steady_clock;

    // a scene can submit more than fits in the 6144 vertices of the vertex RAM, as culled polygons
    // don't take any and a strip's polygons share them
    static constexpr u32 MaxVertices = 32768;
    static constexpr u32 MaxPolygons = 2048;
    static constexpr u32 NumMatrixStackEntries = 32;

    // open addressing, cleared in constant time. the sizes below keep them at most half full
    template <u32 Bits>
    struct HashSlots
    {
        struct Slot
        {
            u64 Key;
            u32 Value;
            u32 Generation;
        };

        std::array<Slot, 1u << Bits> Slots {};
        u32 Generation = 1;
        u32 Count = 0;

        void Clear() noexcept
        {
            Generation++;
            Count = 0;
        }

        [[nodiscard]] static u32 IndexOf(u64 key) noexcept
        {
            return (u32)((key * 0x9E3779B97F4A7C15ULL) >> (64 - Bits));
        }

        [[nodiscard]] const Slot* Find(u64 key) const noexcept
        {
            u32 index = IndexOf(key);
            while (Slots[index].Generation == Generation)
            {
                if (Slots[index].Key == key)
                    return &Slots[index];
                index = (index + 1) & ((1u << Bits) - 1);
            }
            return nullptr;
        }

        Slot& FindOrAdd(u64 key, bool& isNew) noexcept
        {
            u32 index = IndexOf(key);
            while (Slots[index].Generation == Generation)
            {
                if (Slots[index].Key == key)
                {
                    isNew = false;
                    return Slots[index];
                }
                index = (index + 1) & ((1u << Bits) - 1);
            }

            Slot& slot = Slots[index];
            slot.Key = key;
            slot.Value = 0;
            slot.Generation = Generation;
            Count++;
            isNew = true;
            return slot;
        }
    };

    using VertexSlots = HashSlots<16>;
    using PolygonSlots = HashSlots<13>;

    // what the position matrix is made of, with a copy per matrix stack entry
    struct MatrixId
    {
        u64 Placement;
        u64 Load;
        u32 LoadScene;
    };

    struct VertexRecord
    {
        u64 Placement;
        u64 Hash;
        u64 Load;
        bool HasLoad;
    };

    struct PolygonSource
    {
        u32 End; // the side table index after its last submitted vertex
        u32 NumVertices;
    };

    struct PolygonRecord
    {
        u64 Signature;
        u64 Key; // 0 if it has none
        s32 X, Y;
        u16 ClipMask;
        u16 NumVertices;
    };

    struct CellEntry
    {
        u64 Key;
        u32 Index;
    };

    // u32s only, added up as an array for the line per second
    struct SceneCounts
    {
        u32 Submitted, Attempted, Polygons, RamRejected, ClippedPolygons, ClippedVertices, RamVertices, Overflow, Unmapped;
        u32 Matched, MatchedByOccurrence, MatchedOtherLoad, UnmatchedNew, UnmatchedOld, Duplicates;
        u32 Agree, AgreeNone, Wrong, SignatureOnly, KeyOnly, ClipDifferent;
        u32 PolygonsCompared, PolygonsMatched, PolygonsOldOnly;
        u32 Cameras, DominantVertices, IdentityVertices, NoLoad, UnknownVertices, LoadsEarly, LoadsLate, Identity;
        u32 Placements, PlacementsNew, PlacementsGone, BoxTests, BoxPassed;
    };

    template <typename Body>
    void Sampled(Body&& body);

    void AnalyzeScene(const GPU3D& gpu);
    void AnalyzeVertices();
    void AnalyzePolygons(const GPU3D& gpu);
    void CompareWithSignatures();
    void EstimateShift(const PolygonRecord* oldScene, u32 numOld, const PolygonRecord* newScene, u32 numNew, s32& shiftX, s32& shiftY) const;
    s32 FindBeyondWindow(const PolygonRecord* oldScene, const PolygonRecord& polygon, u32 start, u32 numOld, s32 shiftX, s32 shiftY);
    void LogScene(bool isPair);
    void LogSecond(Clock::time_point now);

    int Mode;

    bool IsSceneValid = false;
    u32 SceneNumber = 1;
    u32 VBlanksInScene = 0;

    MatrixId Matrix;
    std::array<MatrixId, NumMatrixStackEntries> MatrixStack;

    u32 NumRecords = 0;
    std::array<VertexRecord, MaxVertices> Records {};
    std::array<PolygonSource, MaxPolygons> PolygonSources {};
    std::array<u64, MaxVertices> VertexKeys {};

    // the scene being built and the previous one, by [Bank] and [Bank ^ 1]
    u32 Bank = 0;
    bool HasPrevious = false;
    std::array<VertexSlots, 2> KeySets {};
    std::array<VertexSlots, 2> PlacementSets {};
    std::array<PolygonSlots, 2> PolygonKeySets {};
    std::array<std::array<PolygonRecord, MaxPolygons>, 2> PolygonRecords {};
    std::array<u32, 2> NumPolygonRecords {};

    VertexSlots PairCounts {};
    // the record of each pair's occurrence 0, by the index of its slot in PairCounts
    std::array<u32, 1u << 16> PairFirstRecords {};
    VertexSlots LoadCounts {};

    std::array<CellEntry, MaxPolygons> OldCells {};
    bool IsOldCellsValid = false;

    SceneCounts Scene {};
    u64 DominantLoad = 0, PreviousDominantLoad = 0;
    u64 OrderHash = 0, PreviousOrderHash = 0;
    u32 PreviousLoadsEarly = 0, PreviousLoadsLate = 0;
    u64 ProjectionLoadHash = 0, PreviousProjectionLoadHash = 0;
    u64 ProjectionMultHash = 0, PreviousProjectionMultHash = 0;

    SceneCounts Total {};
    u32 TotalScenes = 0, TotalPairs = 0, TotalDropped = 0, TotalVBlanks = 0;
    u32 TotalDominantChanged = 0, TotalOrderChanged = 0, TotalReordered = 0, TotalSplitChanged = 0;
    u32 TotalProjectionLoadChanged = 0, TotalProjectionMultChanged = 0;

    Clock::time_point LastLog;
    s64 TimedNs = 0;
    u32 HookCalls = 0;
    u32 SampledCalls = 0;
    s64 SampledNs = 0;
};

}

#endif
