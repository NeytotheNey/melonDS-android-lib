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

// SWI boundary logger for the test harness.
//
// Clean-room rule: this file only looks at what the calling game can see.
// On entry it records the SVC instruction's comment field and the caller's
// registers. The return is detected when a jump lands on the caller's return
// address in the caller's mode with the caller's stack pointer, and then the
// caller's registers and the memory the SWI was asked to write are recorded.
// It never records addresses, instructions or memory inside the BIOS ranges
// (ARM9 0xFFFF0000+, ARM7 0x00000000-0x0000FFFF, which covers the DSi BIOSes
// too), and SVCs issued from inside
// those ranges are ignored.

#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

#include "NDS.h"
#include "ARM.h"
#include "SWILog.h"
#include "HarnessLog.h"
#include "xxhash/xxhash.h"

namespace melonDS::SWILog
{

u32 PendingCount[2] = {0, 0};

namespace
{

constexpr u32 MaxPending = 64;

struct PendingCall
{
    u64 Seq;
    u32 SWI;
    u32 Comment;
    bool Thumb;
    u32 Caller;     // address of the SVC instruction
    u32 RetAddr;    // where the SWI should come back to
    u32 CPSR;       // caller's CPSR
    u32 SP;         // caller's sp
    u32 In[4];
    u32 InHigh[9];  // r4-r12
    u32 LR;
    u64 T0;
    u32 Frame0;

    // memory the SWI is expected to write (0 size = none or unknown)
    u32 Dst;
    u32 Size;
    std::string Extra;  // pre-formatted JSON fields describing the inputs

    // IntrWait/VBlankIntrWait/Halt: the IRQ check flags word in game RAM
    u32 FlagsAddr;
    u32 Flags0;
};

FILE* LogFile = nullptr;
FILE* BlobFile = nullptr;
u64 BlobOffset = 0;
u64 BlobTotalMax = 0;
u32 BlobMax = 0;
u32 CurFrame = 0;
u64 Seq[2] = {0, 0};
std::vector<PendingCall> Pending[2];
std::vector<u8> Buffer;

const char* SWIName(int num, u32 swi)
{
    switch (swi)
    {
    case 0x00: return "SoftReset";
    case 0x03: return "WaitByLoop";
    case 0x04: return "IntrWait";
    case 0x05: return "VBlankIntrWait";
    case 0x06: return "Halt";
    case 0x07: return num ? "Sleep" : "invalid07";
    case 0x08: return num ? "SoundBias" : "invalid08";
    case 0x09: return "Div";
    case 0x0B: return "CpuSet";
    case 0x0C: return "CpuFastSet";
    case 0x0D: return "Sqrt";
    case 0x0E: return "GetCRC16";
    case 0x0F: return "IsDebugger";
    case 0x10: return "BitUnPack";
    case 0x11: return "LZ77UnCompWram";
    case 0x12: return "LZ77UnCompVram";
    case 0x13: return "HuffUnComp";
    case 0x14: return "RLUnCompWram";
    case 0x15: return "RLUnCompVram";
    case 0x16: return num ? "invalid16" : "Diff8bitUnFilterWram";
    case 0x18: return num ? "invalid18" : "Diff16bitUnFilter";
    case 0x1A: return num ? "GetSineTable" : "invalid1A";
    case 0x1B: return num ? "GetPitchTable" : "invalid1B";
    case 0x1C: return num ? "GetVolumeTable" : "invalid1C";
    case 0x1D: return num ? "GetBootProcs" : "invalid1D";
    case 0x1F: return num ? "CustomHalt" : "CustomPost";
    default: return "unknown";
    }
}

bool InBIOS(int num, u32 addr)
{
    if (num == 0) return addr >= 0xFFFF0000;
    return addr < 0x10000; // NTR BIOS is 0-3FFF, the ARM7i BIOS 0-FFFF
}

// only plain RAM-like regions the game owns, never the BIOS or I/O
bool Readable(ARM* cpu, u32 addr)
{
    u32 region = addr >> 24;
    if (cpu->Num == 0)
    {
        ARMv5* arm9 = (ARMv5*)cpu;
        if (addr < arm9->ITCMSize) return true;
        if ((addr & arm9->DTCMMask) == arm9->DTCMBase) return true;
        return region == 0x02 || region == 0x03 || region == 0x05 || region == 0x06 || region == 0x07;
    }
    else
    {
        return region == 0x02 || region == 0x03 || region == 0x06;
    }
}

bool ReadableRange(ARM* cpu, u32 addr, u32 len)
{
    if (len == 0) return Readable(cpu, addr);
    u32 end = addr + len - 1;
    if (end < addr) return false;
    // ranges are checked at both ends and every 16 MB step in between
    for (u64 a = addr; a <= end; a += 0x1000000)
        if (!Readable(cpu, (u32)a)) return false;
    return Readable(cpu, end);
}

u8 Peek8(ARM* cpu, u32 addr)
{
    if (cpu->Num == 0)
    {
        ARMv5* arm9 = (ARMv5*)cpu;
        if (addr < arm9->ITCMSize)
            return arm9->ITCM[addr & (ITCMPhysicalSize - 1)];
        if ((addr & arm9->DTCMMask) == arm9->DTCMBase)
            return arm9->DTCM[addr & (DTCMPhysicalSize - 1)];
        return cpu->NDS.ARM9Read8(addr);
    }
    return cpu->NDS.ARM7Read8(addr);
}

u32 Peek32(ARM* cpu, u32 addr)
{
    return Peek8(cpu, addr) | (Peek8(cpu, addr+1) << 8) | (Peek8(cpu, addr+2) << 16) | ((u32)Peek8(cpu, addr+3) << 24);
}

std::string HexBytes(ARM* cpu, u32 addr, u32 len)
{
    std::string out;
    char tmp[4];
    for (u32 i = 0; i < len; i++)
    {
        snprintf(tmp, sizeof(tmp), "%02X", Peek8(cpu, addr + i));
        out += tmp;
    }
    return out;
}

u64 Timestamp(ARM* cpu)
{
    if (cpu->Num == 0) return cpu->NDS.ARM9Timestamp + cpu->Cycles;
    return cpu->NDS.ARM7Timestamp + cpu->Cycles;
}

u32 CallerSP(ARM* cpu)
{
    return cpu->R[13];
}

bool IsWaitSWI(u32 swi)
{
    return swi == 0x04 || swi == 0x05 || swi == 0x06 || swi == 0x1F;
}

// works out which memory a data SWI writes, from its documented arguments
void SetupOutput(ARM* cpu, PendingCall& call)
{
    char tmp[160];
    u32 r0 = call.In[0], r1 = call.In[1], r2 = call.In[2];

    call.Dst = 0;
    call.Size = 0;

    switch (call.SWI)
    {
    case 0x0B: // CpuSet
        {
            u32 count = r2 & 0x1FFFFF;
            u32 unit = (r2 & (1<<26)) ? 4 : 2;
            call.Dst = r1;
            call.Size = count * unit;
            snprintf(tmp, sizeof(tmp), ",\"mode\":\"%s%s\"", (r2 & (1<<24)) ? "fill" : "copy", unit == 4 ? "32" : "16");
            call.Extra += tmp;
        }
        break;

    case 0x0C: // CpuFastSet
        {
            u32 count = r2 & 0x1FFFFF;
            call.Dst = r1;
            call.Size = ((count + 7) & ~7) * 4;
            snprintf(tmp, sizeof(tmp), ",\"mode\":\"%s\"", (r2 & (1<<24)) ? "fill" : "copy");
            call.Extra += tmp;
        }
        break;

    case 0x10: // BitUnPack: r2 points to {u16 srclen, u8 srcwidth, u8 dstwidth, u32 offset}
        if (ReadableRange(cpu, r2, 8))
        {
            u32 srclen = Peek8(cpu, r2) | (Peek8(cpu, r2+1) << 8);
            u32 srcw = Peek8(cpu, r2+2);
            u32 dstw = Peek8(cpu, r2+3);
            u32 offset = Peek32(cpu, r2+4);
            snprintf(tmp, sizeof(tmp), ",\"unpack\":{\"srclen\":%u,\"srcw\":%u,\"dstw\":%u,\"offset\":\"%08X\"}",
                srclen, srcw, dstw, offset);
            call.Extra += tmp;
            if (srcw && dstw && srcw <= 8 && dstw <= 32)
            {
                u64 bits = ((u64)srclen * 8 / srcw) * dstw;
                call.Dst = r1;
                call.Size = (u32)(((bits + 31) / 32) * 4);
            }
        }
        break;

    case 0x11: case 0x12: // LZ77
    case 0x13:            // Huffman
    case 0x14: case 0x15: // RLE
    case 0x16: case 0x18: // diff filters
        if (ReadableRange(cpu, r0, 16))
        {
            u32 header = Peek32(cpu, r0);
            u32 type = (header >> 4) & 0xF;
            u32 want = 0;
            switch (call.SWI)
            {
            case 0x11: case 0x12: want = 1; break;
            case 0x13: want = 2; break;
            case 0x14: case 0x15: want = 3; break;
            case 0x16: case 0x18: want = 8; break;
            }

            snprintf(tmp, sizeof(tmp), ",\"header\":\"%08X\",\"src_head\":\"%s\"", header, HexBytes(cpu, r0, 16).c_str());
            call.Extra += tmp;

            u32 size = header >> 8;
            if (type == want && size <= 0x1000000)
            {
                if (call.SWI == 0x13) size = (size + 3) & ~3;
                call.Dst = r1;
                call.Size = size;
            }
            else
                call.Extra += ",\"header_unrecognized\":true";
        }
        else
            call.Extra += ",\"src_unreadable\":true";
        break;
    }

    if (call.Size && !ReadableRange(cpu, call.Dst, call.Size))
    {
        call.Extra += ",\"dst_unreadable\":true";
        call.Size = 0;
    }
}

void WriteRegs(const char* key, const u32* regs, int count, std::string& out)
{
    char tmp[16];
    out += ",\"";
    out += key;
    out += "\":[";
    for (int i = 0; i < count; i++)
    {
        snprintf(tmp, sizeof(tmp), "%s\"%08X\"", i ? "," : "", regs[i]);
        out += tmp;
    }
    out += "]";
}

void WriteRecord(ARM* cpu, const PendingCall& call, bool returned)
{
    if (!LogFile) return;

    int num = cpu->Num;
    std::string line;
    char tmp[256];

    snprintf(tmp, sizeof(tmp),
        "{\"cpu\":%d,\"seq\":%llu,\"swi\":%u,\"name\":\"%s\",\"comment\":\"%06X\",\"thumb\":%d,"
        "\"caller\":\"%08X\",\"cpsr_in\":\"%08X\",\"sp\":\"%08X\",\"frame\":%u",
        num ? 7 : 9, (unsigned long long)call.Seq, call.SWI, SWIName(num, call.SWI), call.Comment, call.Thumb ? 1 : 0,
        call.Caller, call.CPSR, call.SP, call.Frame0);
    line += tmp;

    WriteRegs("in", call.In, 4, line);
    WriteRegs("in_r4_r12", call.InHigh, 9, line);
    snprintf(tmp, sizeof(tmp), ",\"lr_in\":\"%08X\"", call.LR);
    line += tmp;
    line += call.Extra;

    if (!returned)
    {
        line += ",\"returned\":false}\n";
        fputs(line.c_str(), LogFile);
        return;
    }

    u32 out[13];
    for (int i = 0; i < 13; i++) out[i] = cpu->R[i];
    WriteRegs("out", out, 4, line);
    WriteRegs("out_r4_r12", &out[4], 9, line);

    snprintf(tmp, sizeof(tmp), ",\"cpsr_out\":\"%08X\",\"frame_end\":%u,\"cycles\":%llu",
        cpu->CPSR, CurFrame, (unsigned long long)(Timestamp(cpu) - call.T0));
    line += tmp;

    if (IsWaitSWI(call.SWI))
    {
        u32 flags = call.FlagsAddr ? Peek32(cpu, call.FlagsAddr) : 0;
        snprintf(tmp, sizeof(tmp), ",\"flags_in\":\"%08X\",\"flags_out\":\"%08X\",\"ime\":%u,\"ie\":\"%08X\",\"if\":\"%08X\"",
            call.Flags0, flags, cpu->NDS.IME[num], cpu->NDS.IE[num], cpu->NDS.IF[num]);
        line += tmp;
    }

    if (call.Size)
    {
        Buffer.resize(call.Size);
        for (u32 i = 0; i < call.Size; i++)
            Buffer[i] = Peek8(cpu, call.Dst + i);

        u64 hash = XXH64(Buffer.data(), Buffer.size(), 0);
        snprintf(tmp, sizeof(tmp), ",\"dst\":\"%08X\",\"size\":%u,\"hash\":\"%016llX\",\"out_head\":\"%s\"",
            call.Dst, call.Size, (unsigned long long)hash, HexBytes(cpu, call.Dst, std::min<u32>(call.Size, 16)).c_str());
        line += tmp;

        if (BlobFile && call.Size <= BlobMax && BlobOffset + call.Size <= BlobTotalMax)
        {
            fwrite(Buffer.data(), 1, Buffer.size(), BlobFile);
            snprintf(tmp, sizeof(tmp), ",\"blob\":%llu", (unsigned long long)BlobOffset);
            line += tmp;
            BlobOffset += call.Size;
        }
    }

    line += "}\n";
    fputs(line.c_str(), LogFile);
}

}

void OnEntry(ARM* cpu, u32 comment, bool thumb)
{
    int num = cpu->Num;
    u32 ret = cpu->R[15] - (thumb ? 2 : 4);
    u32 caller = ret - (thumb ? 2 : 4);

    // SVCs from inside the BIOS are none of our business
    if (InBIOS(num, caller)) return;

    PendingCall call {};
    call.Seq = Seq[num]++;
    call.Comment = comment;
    call.SWI = thumb ? (comment & 0xFF) : ((comment >> 16) & 0xFF);
    call.Thumb = thumb;
    call.Caller = caller;
    call.RetAddr = ret;
    call.CPSR = cpu->CPSR;
    call.SP = CallerSP(cpu);
    for (int i = 0; i < 4; i++) call.In[i] = cpu->R[i];
    for (int i = 0; i < 9; i++) call.InHigh[i] = cpu->R[4 + i];
    call.LR = cpu->R[14];
    call.T0 = Timestamp(cpu);
    call.Frame0 = CurFrame;

    SetupOutput(cpu, call);

    call.FlagsAddr = 0;
    call.Flags0 = 0;
    if (IsWaitSWI(call.SWI))
    {
        if (num == 0)
            call.FlagsAddr = ((ARMv5*)cpu)->DTCMBase + 0x3FF8;
        else
            call.FlagsAddr = 0x0380FFF8;
        call.Flags0 = Peek32(cpu, call.FlagsAddr);
    }

    if (Pending[num].size() >= MaxPending)
    {
        WriteRecord(cpu, Pending[num].front(), false);
        Pending[num].erase(Pending[num].begin());
    }

    Pending[num].push_back(std::move(call));
    PendingCount[num] = (u32)Pending[num].size();
}

void OnJumpSlow(ARM* cpu, u32 addr)
{
    int num = cpu->Num;
    bool thumb = addr & 0x1;
    u32 target = addr & ~0x1;
    u32 mode = cpu->CPSR & 0x1F;

    auto& list = Pending[num];
    // newest first: nested SWIs (from IRQ handlers) come back before the outer ones
    for (int i = (int)list.size() - 1; i >= 0; i--)
    {
        PendingCall& call = list[i];
        if (call.RetAddr != target) continue;
        if (call.Thumb != thumb) continue;
        if ((call.CPSR & 0x1F) != mode) continue;
        if (call.SP != CallerSP(cpu)) continue;

        WriteRecord(cpu, call, true);
        list.erase(list.begin() + i);
        PendingCount[num] = (u32)list.size();
        return;
    }
}

bool Open(const std::string& outdir, u32 maxblob, u64 maxblobtotal)
{
    std::string logpath = outdir + "/swi.jsonl";
    LogFile = fopen(logpath.c_str(), "wb");
    if (!LogFile) return false;
    setvbuf(LogFile, nullptr, _IOFBF, 1 << 20);

    BlobMax = maxblob;
    BlobTotalMax = maxblobtotal;
    BlobOffset = 0;
    if (maxblob)
    {
        std::string blobpath = outdir + "/outputs.bin";
        BlobFile = fopen(blobpath.c_str(), "wb");
        if (BlobFile) setvbuf(BlobFile, nullptr, _IOFBF, 1 << 20);
    }

    return true;
}

void Close(ARM* arm9, ARM* arm7)
{
    ARM* cpus[2] = {arm9, arm7};
    for (int num = 0; num < 2; num++)
    {
        for (auto& call : Pending[num])
            WriteRecord(cpus[num], call, false);
        Pending[num].clear();
        PendingCount[num] = 0;
    }

    if (LogFile) fclose(LogFile);
    if (BlobFile) fclose(BlobFile);
    LogFile = nullptr;
    BlobFile = nullptr;
}

void SetFrame(u32 frame)
{
    CurFrame = frame;
}

u64 GetCallCount(int num)
{
    return Seq[num];
}

}
