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

#include "GPU3D_ComputeRooms.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "Platform.h"
#include "Utils.h"
#include "xxhash/xxhash.h"

namespace melonDS::ComputeWorkRooms
{

namespace
{

struct Entry
{
    Key RoomKey;
    u32 WorkTiles;
    u32 Spans;
    // the clock when it was last found or grown, to drop the least recently used
    u64 LastUsed;
};
static_assert(sizeof(Entry) == 48, "the file's entries are the struct's bytes");

// "MCRM". bump the version whenever Key or Entry changes
constexpr u32 RoomsMagic = 0x4D52434D;
constexpr u32 RoomsVersion = 1;
// games times scales, widths and layouts. 24 KB at most
constexpr u32 RoomsMaxEntries = 512;
// magic, version, count, 0, clock, checksum of the entries
constexpr size_t RoomsHeaderBytes = 4 * 4 + 8 + 8;
// how far behind the clock a found entry's stamp may be before it's written back. the clock moves
// one a find or grow, so a game started within the last 32 or so isn't
constexpr u64 RoomsStaleStamps = 32;

std::mutex RoomsMutex;
std::string RoomsPath;
std::vector<Entry> Rooms;
bool RoomsLoaded = false;
u64 RoomsClock = 0;

bool SameKey(const Key& a, const Key& b)
{
    return memcmp(&a, &b, sizeof(Key)) == 0;
}

// a room that GrowWorkBuffers could have made for this key: whole steps up to the most
bool ValidEntry(const Entry& entry)
{
    const Key& key = entry.RoomKey;
    return key.GameCode != 0 && key.MaxWorkTiles >= 16 && key.MaxWorkTiles % 16 == 0
        && key.MaxSpans >= 64 && key.MaxSpans % 64 == 0
        && entry.WorkTiles > 0 && entry.WorkTiles <= key.MaxWorkTiles && entry.WorkTiles % (key.MaxWorkTiles / 16) == 0
        && entry.Spans > 0 && entry.Spans <= key.MaxSpans && entry.Spans % (key.MaxSpans / 64) == 0
        && entry.LastUsed <= RoomsClock;
}

void LogRoomsProblem(const char* what)
{
    static u32 problems = 0;
    problems++;
    if ((problems & (problems - 1)) == 0)
        Platform::Log(Platform::LogLevel::Warn, "Compute renderer: %s (%u so far)\n", what, problems);
}

// with the mutex held. every count and room is checked, so a bad file can only be turned down
void Load()
{
    RoomsLoaded = true;
    Rooms.clear();
    RoomsClock = 0;

    Platform::FileHandle* file = Platform::OpenFile(RoomsPath, Platform::FileMode::Read);
    // none yet
    if (!file)
        return;

    u64 length = Platform::FileLength(file);
    std::vector<u8> buffer;
    bool read = false;
    if (length >= RoomsHeaderBytes && length <= RoomsHeaderBytes + (u64)RoomsMaxEntries * sizeof(Entry))
    {
        buffer.resize(length);
        read = Platform::FileRead(buffer.data(), length, 1, file) == 1;
    }
    Platform::CloseFile(file);

    u32 header[4] = {};
    u64 clock = 0, checksum = 0;
    if (read)
    {
        memcpy(header, buffer.data(), sizeof(header));
        memcpy(&clock, buffer.data() + 16, 8);
        memcpy(&checksum, buffer.data() + 24, 8);
    }
    if (!read || header[0] != RoomsMagic || header[1] != RoomsVersion || header[2] > RoomsMaxEntries
        || length != RoomsHeaderBytes + (u64)header[2] * sizeof(Entry)
        || XXH3_64bits(buffer.data() + RoomsHeaderBytes, length - RoomsHeaderBytes) != checksum)
    {
        LogRoomsProblem("the work buffer rooms file is unreadable or from another version, starting a new one");
        return;
    }

    RoomsClock = clock;
    Rooms.resize(header[2]);
    memcpy(Rooms.data(), buffer.data() + RoomsHeaderBytes, length - RoomsHeaderBytes);
    if (!std::all_of(Rooms.begin(), Rooms.end(), ValidEntry))
    {
        Rooms.clear();
        RoomsClock = 0;
        LogRoomsProblem("the work buffer rooms file has a bad entry, starting a new one");
    }
}

// with the mutex held: the most recently used first, as many as fit
void Save()
{
    std::sort(Rooms.begin(), Rooms.end(), [](const Entry& a, const Entry& b) { return a.LastUsed > b.LastUsed; });
    if (Rooms.size() > RoomsMaxEntries)
        Rooms.resize(RoomsMaxEntries);

    std::vector<u8> buffer(RoomsHeaderBytes + Rooms.size() * sizeof(Entry));
    u32 header[4] = { RoomsMagic, RoomsVersion, (u32)Rooms.size(), 0 };
    memcpy(buffer.data(), header, sizeof(header));
    memcpy(buffer.data() + 16, &RoomsClock, 8);
    memcpy(buffer.data() + RoomsHeaderBytes, Rooms.data(), Rooms.size() * sizeof(Entry));
    u64 checksum = XXH3_64bits(buffer.data() + RoomsHeaderBytes, buffer.size() - RoomsHeaderBytes);
    memcpy(buffer.data() + 24, &checksum, 8);

    if (!ReplaceFile(RoomsPath, buffer.data(), buffer.size()))
        LogRoomsProblem("couldn't write the work buffer rooms file");
}

// with the mutex held
Entry* Lookup(const Key& key)
{
    if (RoomsPath.empty() || key.GameCode == 0)
        return nullptr;
    if (!RoomsLoaded)
        Load();
    for (Entry& entry : Rooms)
        if (SameKey(entry.RoomKey, key))
            return &entry;
    return nullptr;
}

}

void SetPath(const std::string& path)
{
    std::lock_guard lock(RoomsMutex);
    if (path == RoomsPath)
        return;
    RoomsPath = path;
    Rooms.clear();
    RoomsLoaded = false;
}

bool Find(const Key& key, int& workTiles, int& spans)
{
    std::lock_guard lock(RoomsMutex);
    Entry* entry = Lookup(key);
    if (!entry)
        return false;

    workTiles = (int)entry->WorkTiles;
    spans = (int)entry->Spans;
    bool stale = RoomsClock - entry->LastUsed > RoomsStaleStamps;
    entry->LastUsed = ++RoomsClock;
    // at the targets' setup, which is a hitch already (a compile or the first frame), and only
    // once a game was left alone for a while, so not at every start
    if (stale)
        Save();
    return true;
}

void Remember(const Key& key, int workTiles, int spans)
{
    // each on its own: a buffer whose allocation failed has 0, which is never stored, but the
    // other one's room still is
    u32 tiles = (u32)std::max(workTiles, 0);
    u32 spanRoom = (u32)std::max(spans, 0);
    if (tiles == 0 && spanRoom == 0)
        return;

    std::lock_guard lock(RoomsMutex);
    if (RoomsPath.empty() || key.GameCode == 0 || key.MaxWorkTiles < 16 || key.MaxSpans < 64)
        return;
    Entry* entry = Lookup(key);
    if (entry && entry->WorkTiles >= tiles && entry->Spans >= spanRoom)
        return;

    // a new entry without one of them gets a single step, below where the renderer starts, so
    // that buffer starts as it always does
    Entry grown { key, std::max(tiles, key.MaxWorkTiles / 16), std::max(spanRoom, key.MaxSpans / 64), RoomsClock + 1 };
    if (entry)
    {
        grown.WorkTiles = std::max(entry->WorkTiles, tiles);
        grown.Spans = std::max(entry->Spans, spanRoom);
    }
    // one bad entry would make the whole file be turned down at the next start
    RoomsClock++;
    if (!ValidEntry(grown))
        return;
    if (entry)
        *entry = grown;
    else
        Rooms.push_back(grown);
    Save();
}

}
