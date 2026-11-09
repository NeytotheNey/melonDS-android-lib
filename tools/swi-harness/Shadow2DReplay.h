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

#ifndef SWIHARNESS_SHADOW2DREPLAY_H
#define SWIHARNESS_SHADOW2DREPLAY_H

#include <stdio.h>
#include "types.h"

namespace melonDS { class GPU; }

// the GPU 2D shadow renderer's shaders replayed on the CPU at line 192 of every frame (see the .cpp)
struct Shadow2DReplay
{
    melonDS::u64 Lines = 0;         // lines compared
    melonDS::u64 Mismatches = 0;    // plane words that differed
    melonDS::u64 MemMismatches = 0; // MemTex words the incremental uploads left stale
    FILE* Log = nullptr;            // the first mismatches go here

    // Shadow2DRecorder::FrameHook
    static void Frame(const melonDS::GPU& gpu, int backbuf, void* user);
};

#endif
