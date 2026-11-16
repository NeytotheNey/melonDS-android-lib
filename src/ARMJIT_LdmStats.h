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

#ifndef ARMJIT_LDMSTATS_H
#define ARMJIT_LDMSTATS_H

#ifdef JIT_ENABLED

#include "types.h"

// melonade: where the JIT's slow multi-register loads and stores (SlowBlockTransfer9/7) go, and why a block transfer was
// compiled without the fast path (debug.melonade.ldmstats in the app). Off unless turned on; while off, each counted spot
// costs a load and a branch on Enabled, and nothing changes in the generated code. Everything here runs on the emulator
// thread (both CPUs, the compiler and the fault handler), so the counters are plain.
// Two lines about every 5 s, with the counts since the last ones (EndFrame):
//   LDMSTATS run secs=<s> <stream>=<calls>/<words>[,nofast=<calls>,patched=<calls>][,cross=<calls>][,<region>=<calls>/<words>...] ...
//   LDMSTATS jit secs=<s> <stream>=<outcome>:<sites>,... [<stream>.rgn=<region>:<sites>,...] patch=9r:<n>,9w:<n>,7r:<n>,7w:<n>,single:<n>
// with streams 9r 9w 7r 7w (CPU, read or write). run: the slow calls by the region of their start address; nofast are calls
// from transfers compiled without a fast path, patched from fast paths the fault handler turned into a slow call (ARM64 only);
// cross are transfers whose last word is in another region than the first. jit: the block transfers compiled, by outcome
// (fast: the predicted region is fastmem compatible, fastcond: an ARM conditional one, fastthumb: a Thumb one, both of which take
// the fast path whatever the region; single: one loaded register, compiled as a single LDR/STR; nofastmem, ldmfast, user, region:
// the reason for no fast path). rgn: the predicted regions of the "region" ones. patch: fast paths rewritten to the slow call.
namespace melonDS::LdmStats
{

extern bool Enabled;

enum
{
    Compile_Fast,
    Compile_FastCond,
    Compile_FastThumb,
    Compile_Single,
    Compile_NoFastmem,
    Compile_LdmFast,
    Compile_UserMode,
    Compile_Region,
    Compile_Count
};

void SetEnabled(bool enabled) noexcept;
// SlowBlockTransfer9/7, returnAddress is the JIT code that called it
void NoteTransfer(u32 num, bool write, u32 addr, u32 words, const void* returnAddress) noexcept;
// Comp_MemAccessBlock; predictedAddr is the address the block's first run touched (FetchedInstr::DataRegion)
void NoteCompile(u32 num, bool store, int outcome, u32 predictedAddr) noexcept;
// RewriteMemAccess: block is num * 2 + store for a block transfer's fast path, -1 for a single load or store
void NotePatch(int block) noexcept;
// NDS::RunFrame, once a frame
void EndFrame() noexcept;

}

#endif

#endif
