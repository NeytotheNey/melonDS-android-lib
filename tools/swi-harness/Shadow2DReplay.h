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
    melonDS::u64 WindowLines = 0;   // of them, lines with windows on
    melonDS::u64 ObjWindowOnLines = 0; // of them, lines with the OBJ window on
    melonDS::u64 ObjWindowLines = 0; // and lines where an OBJ window sprite drew into it
    melonDS::u64 Mismatches = 0;    // plane words that differed
    melonDS::u64 MemMismatches = 0; // MemTex words the incremental uploads left stale
    melonDS::u64 AffineLines = 0;   // of the lines compared, those with an affine BG
    melonDS::u64 ExtLines = 0;      // and with a tiled extended BG
    // the affine composite (lines with a rotated or scaled BG, Shadow2D::HiresBGs, as at 2x and up):
    // its planes at each native pixel's first sub-pixel, which have to be the planes pass's, and over
    // every sub-pixel at SubScale on every 8th frame, how many differ from the native pixel's (the
    // sharpness hi-res adds, only counted)
    melonDS::u64 HiresLines = 0;
    melonDS::u64 AffineRuns = 0;    // the affine composite's draws (runs of lines sharing constants)
    melonDS::u64 PerLineRuns = 0;   // of them, runs whose fragments read each line's constants
    melonDS::u64 StackMismatches = 0;
    melonDS::u64 SubPixels = 0, SubDiffers = 0;
    int SubScale = 4;
    melonDS::u32 Frames = 0;
    // the rotated or scaled BGs seen (unit, BG, kind, A-D), each logged once, the first 64
    melonDS::u64 SeenRot[64] {};
    int SeenRotCount = 0;
    FILE* Log = nullptr;            // the first mismatches go here

    // Shadow2DRecorder::FrameHook
    static void Frame(const melonDS::GPU& gpu, int backbuf, void* user);
};

#endif
