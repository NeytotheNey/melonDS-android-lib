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

#ifndef GPU3D_COMPUTEROOMS_H
#define GPU3D_COMPUTEROOMS_H

#include <string>

#include "types.h"

// the compute renderer's work buffers start small and grow (ComputeRenderer::GrowWorkBuffers),
// and a game that needs more grows on the same frame every time, often mid-animation (White 2's
// title at 8x: 140-180 ms). this remembers the room each game grew to, per everything that changes
// what a room means, so the next start allocates it up front, at the first frame. a small file
// next to the program cache (OpenGL::SetShaderCachePath): a bad one is just no rooms
namespace melonDS::ComputeWorkRooms
{

// all u32 so the file entry has no padding and compares as bytes
struct Key
{
    // from the cart's header. 0 is no key: no cart, or homebrew without a game code
    u32 GameCode;
    u32 HeaderCRC;
    u32 Scale;
    u32 TileSize;
    u32 TileLayers;
    u32 TargetWidth;
    u32 MaxWorkTiles;
    u32 MaxSpans;
};

// the file, empty (the default) for none. read at the first Find or Remember
void SetPath(const std::string& path);

// the rooms remembered for key, which are whole steps (a layer of work tiles, a 64th of the spans)
// no bigger than the key's most. a stamp that fell behind is written back, so a game that never
// grows again isn't the first one dropped
bool Find(const Key& key, int& workTiles, int& spans);

// after a grow: keeps the larger of what's remembered and each of these (0, a buffer that has no
// storage, is none), and writes the file if that grew
void Remember(const Key& key, int workTiles, int spans);

}

#endif
