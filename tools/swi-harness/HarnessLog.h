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

#ifndef SWIHARNESS_HARNESSLOG_H
#define SWIHARNESS_HARNESSLOG_H

#include <string>
#include "types.h"

namespace melonDS { class ARM; }

namespace melonDS::SWILog
{

// opens swi.jsonl and outputs.bin in outdir. maxblob caps the output bytes
// saved per call (0 = save none)
bool Open(const std::string& outdir, u32 maxblob, u64 maxblobtotal);

// writes the SWIs that never returned and closes the files
void Close(ARM* arm9, ARM* arm7);

// the frame number written with each call
void SetFrame(u32 frame);

// calls logged so far, per CPU
u64 GetCallCount(int num);

}

#endif // SWIHARNESS_HARNESSLOG_H
