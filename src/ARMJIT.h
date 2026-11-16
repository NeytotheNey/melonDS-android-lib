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

#ifndef ARMJIT_H
#define ARMJIT_H

#include <algorithm>
#include <optional>
#include <memory>
#include "types.h"
#include "MemConstants.h"
#include "Args.h"
#include "ARMJIT_Memory.h"

#ifdef JIT_ENABLED
#include "JitBlock.h"

#if defined(__APPLE__) && defined(__aarch64__)
    #include <pthread.h>
#endif

#include "ARMJIT_Compiler.h"

namespace melonDS
{
class ARM;

class JitBlock;
class ARMJIT
{
public:
    ARMJIT(melonDS::NDS& nds, std::optional<JITArgs> jit) noexcept;
    ~ARMJIT() noexcept;
    void InvalidateByAddr(u32) noexcept;
    void CheckAndInvalidateWVRAM(int) noexcept;
    void CheckAndInvalidateITCM() noexcept;
    void Reset() noexcept;
    void JitEnableWrite() noexcept;
    void JitEnableExecute() noexcept;
    void CompileBlock(ARM* cpu) noexcept;
    void ResetBlockCache() noexcept;

    template <u32 num, int region>
    void CheckAndInvalidate(u32 addr) noexcept
    {
        u32 localAddr = Memory.LocaliseAddress(region, num, addr);
        if (CodeMemRegions[region][(localAddr & 0x7FFFFFF) / 512].Code & (1 << ((localAddr & 0x1FF) / 16)))
            InvalidateByAddr(localAddr);
    }
    JitBlockEntry LookUpBlock(u32 num, u64* entries, u32 offset, u32 addr) noexcept;
    bool SetupExecutableRegion(u32 num, u32 blockAddr, u64*& entry, u32& start, u32& size) noexcept;
    u32 LocaliseCodeAddress(u32 num, u32 addr) const noexcept;

    ARMJIT_Memory Memory;
private:
    int MaxBlockSize {};
    bool LiteralOptimizations = false;
    bool BranchOptimizations = false;
    bool FastMemory = false;
    // melonade: multi-register loads (LDM, POP, Thumb LDMIA) take the fastmem path on ARM64 as well as stores.
    // Not part of JITArgs, so the app's JITArgs stay as they are; an A/B switch (debug.melonade.ldmfast)
    bool FastBlockLoads = true;
    // melonade: block transfers on the slow path done straight in host memory when every word is in one plain region
    // (DirectBlockTransfer9/7 in ARMJIT.cpp); an A/B switch (debug.melonade.ldmdirect)
    bool DirectBlockTransfers = true;
    // melonade: ARM64, a block exit looks up and jumps to the next block itself (Compiler::Gen_DispatchStubs) instead of
    // going back to the Execute loop for it; same emulation either way. An A/B switch (debug.melonade.jitdispatch)
    bool JitDispatch = false;

public:
    melonDS::NDS& NDS;
    TinyVector<u32> InvalidLiterals {};
    friend class ARMJIT_Memory;
    void blockSanityCheck(u32 num, u32 blockAddr, JitBlockEntry entry) noexcept;
    void RetireJitBlock(JitBlock* block) noexcept;

    int GetMaxBlockSize() const noexcept { return MaxBlockSize; }
    bool LiteralOptimizationsEnabled() const noexcept { return LiteralOptimizations; }
    bool BranchOptimizationsEnabled() const noexcept { return BranchOptimizations; }
    bool FastMemoryEnabled() const noexcept { return FastMemory; }
    bool FastBlockLoadsEnabled() const noexcept { return FastBlockLoads; }
    bool DirectBlockTransfersEnabled() const noexcept { return DirectBlockTransfers; }
    bool JitDispatchEnabled() const noexcept { return JitDispatch; }

    void SetJITArgs(JITArgs args) noexcept;
    void SetMaxBlockSize(int size) noexcept;
    void SetLiteralOptimizations(bool enabled) noexcept;
    void SetBranchOptimizations(bool enabled) noexcept;
    void SetFastMemory(bool enabled) noexcept;
    // ARM64 only (x64 always has loads on the fast path). Blocks already compiled are thrown away when it changes
    void SetFastBlockLoads(bool enabled) noexcept;
    // read on every slow block transfer, so it can change at any time; nothing compiled depends on it
    void SetDirectBlockTransfers(bool enabled) noexcept { DirectBlockTransfers = enabled; }
    // ARM64 only (no effect on x64). Read when a block compiles, so blocks already compiled are thrown away when it changes
    void SetJitDispatch(bool enabled) noexcept;
    // melonade: LDMSTATS lines on the slow block transfers (ARMJIT_LdmStats.h, debug.melonade.ldmstats); clears the counts
    void SetLdmStats(bool enabled) noexcept;
    bool LdmStatsEnabled() const noexcept;

    Compiler JITCompiler;
    std::unordered_map<u32, JitBlock*> JitBlocks9 {};
    std::unordered_map<u32, JitBlock*> JitBlocks7 {};

    std::unordered_map<u32, JitBlock*> RestoreCandidates {};


    AddressRange CodeIndexITCM[ITCMPhysicalSize / 512] {};
    AddressRange CodeIndexMainRAM[MainRAMMaxSize / 512] {};
    AddressRange CodeIndexSWRAM[SharedWRAMSize / 512] {};
    AddressRange CodeIndexVRAM[0x100000 / 512] {};
    AddressRange CodeIndexARM9BIOS[ARM9BIOSSize / 512] {};
    AddressRange CodeIndexARM7BIOS[ARM7BIOSSize / 512] {};
    AddressRange CodeIndexARM7WRAM[ARM7WRAMSize / 512] {};
    AddressRange CodeIndexARM7WVRAM[0x40000 / 512] {};
    AddressRange CodeIndexBIOS9DSi[0x10000 / 512] {};
    AddressRange CodeIndexBIOS7DSi[0x10000 / 512] {};
    AddressRange CodeIndexNWRAM_A[NWRAMSize / 512] {};
    AddressRange CodeIndexNWRAM_B[NWRAMSize / 512] {};
    AddressRange CodeIndexNWRAM_C[NWRAMSize / 512] {};

    u64 FastBlockLookupITCM[ITCMPhysicalSize / 2] {};
    u64 FastBlockLookupMainRAM[MainRAMMaxSize / 2] {};
    u64 FastBlockLookupSWRAM[SharedWRAMSize / 2] {};
    u64 FastBlockLookupVRAM[0x100000 / 2] {};
    u64 FastBlockLookupARM9BIOS[ARM9BIOSSize / 2] {};
    u64 FastBlockLookupARM7BIOS[ARM7BIOSSize / 2] {};
    u64 FastBlockLookupARM7WRAM[ARM7WRAMSize / 2] {};
    u64 FastBlockLookupARM7WVRAM[0x40000 / 2] {};
    u64 FastBlockLookupBIOS9DSi[0x10000 / 2] {};
    u64 FastBlockLookupBIOS7DSi[0x10000 / 2] {};
    u64 FastBlockLookupNWRAM_A[NWRAMSize / 2] {};
    u64 FastBlockLookupNWRAM_B[NWRAMSize / 2] {};
    u64 FastBlockLookupNWRAM_C[NWRAMSize / 2] {};

    AddressRange* const CodeMemRegions[ARMJIT_Memory::memregions_Count] =
    {
        NULL,
        CodeIndexITCM,
        NULL,
        CodeIndexARM9BIOS,
        CodeIndexMainRAM,
        CodeIndexSWRAM,
        NULL,
        CodeIndexVRAM,
        CodeIndexARM7BIOS,
        CodeIndexARM7WRAM,
        NULL,
        NULL,
        CodeIndexARM7WVRAM,
        CodeIndexBIOS9DSi,
        CodeIndexBIOS7DSi,
        CodeIndexNWRAM_A,
        CodeIndexNWRAM_B,
        CodeIndexNWRAM_C
    };

    u64* const FastBlockLookupRegions[ARMJIT_Memory::memregions_Count] =
    {
        NULL,
        FastBlockLookupITCM,
        NULL,
        FastBlockLookupARM9BIOS,
        FastBlockLookupMainRAM,
        FastBlockLookupSWRAM,
        NULL,
        FastBlockLookupVRAM,
        FastBlockLookupARM7BIOS,
        FastBlockLookupARM7WRAM,
        NULL,
        NULL,
        FastBlockLookupARM7WVRAM,
        FastBlockLookupBIOS9DSi,
        FastBlockLookupBIOS7DSi,
        FastBlockLookupNWRAM_A,
        FastBlockLookupNWRAM_B,
        FastBlockLookupNWRAM_C
    };
};
}

// Defined in assembly
extern "C" void ARM_Dispatch(melonDS::ARM* cpu, melonDS::JitBlockEntry entry);
#else
namespace melonDS
{
class ARM;

// This version is a stub; the methods all do nothing,
// but there's still a Memory member.
class ARMJIT
{
public:
    ARMJIT(melonDS::NDS& nds, std::optional<JITArgs>) noexcept : Memory(nds) {}
    ~ARMJIT() noexcept {}
    void InvalidateByAddr(u32) noexcept {}
    void CheckAndInvalidateWVRAM(int) noexcept {}
    void CheckAndInvalidateITCM() noexcept {}
    void Reset() noexcept {}
    void JitEnableWrite() noexcept {}
    void JitEnableExecute() noexcept {}
    void CompileBlock(ARM*) noexcept {}
    void ResetBlockCache() noexcept {}
    bool FastBlockLoadsEnabled() const noexcept { return false; }
    void SetFastBlockLoads(bool) noexcept {}
    bool DirectBlockTransfersEnabled() const noexcept { return false; }
    void SetDirectBlockTransfers(bool) noexcept {}
    bool JitDispatchEnabled() const noexcept { return false; }
    void SetJitDispatch(bool) noexcept {}
    void SetLdmStats(bool) noexcept {}
    bool LdmStatsEnabled() const noexcept { return false; }
    template <u32, int>
    void CheckAndInvalidate(u32 addr) noexcept {}

    ARMJIT_Memory Memory;
};
}
#endif // JIT_ENABLED

#endif // ARMJIT_H

