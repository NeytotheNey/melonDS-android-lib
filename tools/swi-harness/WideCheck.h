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

#ifndef SWIHARNESS_WIDECHECK_H
#define SWIHARNESS_WIDECHECK_H

#include <string>
#include <utility>
#include <vector>
#include "types.h"

struct WideCheckOptions
{
    std::string RomPath, OutDir;
    std::string StatePath; // a melonDS savestate (.mlN) loaded after boot, or empty
    melonDS::u32 Frames = 0;
    int Width = 0;         // the wide run's view width, even, 256..768 (256 = both runs off, a sanity run)
    std::vector<std::pair<melonDS::u32, melonDS::u32>> Taps; // (first frame, key bit), held for 6 frames
    int RTC[6] {};
    bool SqueezeBoxTest = false; // box tests as WideMelon does them: memory may differ, only reported
    bool EqualTiming = true;     // every polygon timed alike in both runs, false = as WideMelon times them
};

// runs the game twice in lockstep, WideMelon off and on, and compares what has to match;
// returns the exit code (0 = pass, 1 = fail, 2 = couldn't run, 3 = inconclusive: a wide run that
// never drew 3D or never kept a different polygon count, so it didn't exercise the wide path)
int RunWideCheck(const WideCheckOptions& opts);

#endif // SWIHARNESS_WIDECHECK_H
