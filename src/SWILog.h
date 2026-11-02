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

#ifndef MELONDS_SWILOG_H
#define MELONDS_SWILOG_H

// SWI boundary logging hooks, only built with MELONADE_SWI_LOG (the PC test
// harness in tools/swi-harness). The core only reports SWI entries and jumps;
// the harness implements these functions and decides what to record.
//
// Only state visible at the SWI boundary is passed on: the SVC instruction in
// the caller's code, the caller's registers, and jumps that land back on the
// caller's return address. Nothing executed inside the BIOS is reported.

#ifdef MELONADE_SWI_LOG

#include "types.h"

namespace melonDS
{
class ARM;

namespace SWILog
{

// number of SWIs waiting for their return, per CPU (0 = ARM9, 1 = ARM7)
extern u32 PendingCount[2];

// called by the SVC handlers before the exception is taken
void OnEntry(ARM* cpu, u32 comment, bool thumb);

// called by JumpTo once the target address and the mode are final
void OnJumpSlow(ARM* cpu, u32 addr);

inline void OnJump(ARM* cpu, u32 num, u32 addr)
{
    if (PendingCount[num]) OnJumpSlow(cpu, addr);
}

}
}

#endif

#endif // MELONDS_SWILOG_H
