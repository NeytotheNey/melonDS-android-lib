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

// Headless SWI logging harness.
//
//   swi-harness <rom.nds> --bios official <dir> | --bios file <dir> | --bios free
//               --frames N --out <dir> [--shot-every N] [--blob-max BYTES]
//               [--blob-total BYTES] [--jit]
//
// Both BIOS modes use the same generated firmware, the same RTC date and
// direct boot, so only the BIOS differs between runs. Writes swi.jsonl,
// outputs.bin (raw output buffers of the memory-writing SWIs), frames.tsv
// (white/color stats per screenshot), core.log and BMP screenshots of both
// screens (top above bottom).

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_set>
#include <filesystem>

#include "NDS.h"
#include "DSi.h"
#include "DSi_NAND.h"
#include "NDSCart.h"
#include "Args.h"
#include "SPI_Firmware.h"
#include "GPU.h"
#include "Platform.h"
#include "HarnessLog.h"

using namespace melonDS;

namespace
{
FILE* CoreLog = nullptr;
bool StopRequested = false;
}

namespace melonDS::Platform
{

void SignalStop(StopReason reason, void* userdata)
{
    fprintf(stderr, "emulator stopped, reason %d\n", (int)reason);
    StopRequested = true;
}

static std::string ModeString(FileMode mode, bool exists)
{
    std::string str;
    if (mode & FileMode::Append)
        str = (mode & FileMode::Read) ? "a+" : "a";
    else if (mode & FileMode::Write)
    {
        if ((mode & FileMode::Preserve) && exists)
            str = "r+";
        else
            str = (mode & FileMode::Read) ? "w+" : "w";
    }
    else
        str = "r";

    if (!(mode & FileMode::Text))
        str += 'b';
    return str;
}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;

    bool exists = FileExists(path);
    if ((mode & FileMode::NoCreate) && !exists && (mode & FileMode::Write))
        return nullptr;

    FILE* f = fopen(path.c_str(), ModeString(mode, exists).c_str());
    return reinterpret_cast<FileHandle*>(f);
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    FILE* f = fopen(name.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool LocalFileExists(const std::string& name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath)
{
    FILE* f = fopen(filepath.c_str(), "ab");
    if (!f) return false;
    fclose(f);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file)
{
    return fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return (u64)ftello64(reinterpret_cast<FILE*>(file));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int stdorigin = SEEK_SET;
    switch (origin)
    {
    case FileSeekOrigin::Start: stdorigin = SEEK_SET; break;
    case FileSeekOrigin::Current: stdorigin = SEEK_CUR; break;
    case FileSeekOrigin::End: stdorigin = SEEK_END; break;
    }
    return fseeko64(reinterpret_cast<FILE*>(file), offset, stdorigin) == 0;
}

void FileRewind(FileHandle* file)
{
    rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file)
{
    return fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return ret < 0 ? 0 : (u64)ret;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    s64 pos = ftello64(f);
    fseeko64(f, 0, SEEK_END);
    s64 len = ftello64(f);
    fseeko64(f, pos, SEEK_SET);
    return (u64)len;
}

void Log(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(CoreLog ? CoreLog : stderr, fmt, args);
    va_end(args);
}

struct Thread
{
    std::thread Handle;
};

Thread* Thread_Create(std::function<void()> func, const char* name)
{
    Thread* thread = new Thread();
    thread->Handle = std::thread(std::move(func));
    return thread;
}

void Thread_Free(Thread* thread)
{
    if (thread->Handle.joinable()) thread->Handle.detach();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread->Handle.joinable()) thread->Handle.join();
}

struct Semaphore
{
    std::mutex Lock;
    std::condition_variable Cond;
    int Count = 0;
};

Semaphore* Semaphore_Create()
{
    return new Semaphore();
}

void Semaphore_Free(Semaphore* sema)
{
    delete sema;
}

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> lock(sema->Lock);
    sema->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> lock(sema->Lock);
    sema->Cond.wait(lock, [sema]() { return sema->Count > 0; });
    sema->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(sema->Lock);
    if (timeout_ms == 0)
    {
        if (sema->Count <= 0) return false;
    }
    else if (!sema->Cond.wait_for(lock, std::chrono::milliseconds(timeout_ms), [sema]() { return sema->Count > 0; }))
        return false;
    sema->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    {
        std::lock_guard<std::mutex> lock(sema->Lock);
        sema->Count += count;
    }
    sema->Cond.notify_all();
}

struct Mutex
{
    std::mutex Lock;
};

Mutex* Mutex_Create()
{
    return new Mutex();
}

void Mutex_Free(Mutex* mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    mutex->Lock.lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    mutex->Lock.unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return mutex->Lock.try_lock();
}

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

u64 GetUSCount()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// saves are thrown away so both runs start from the same blank save
void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {}
void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {}
void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata) {}
void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata) {}

void MP_Begin(void* userdata) {}
void MP_End(void* userdata) {}
int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_RecvPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata) { return 0; }
int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata) { return 0; }

int Net_SendPacket(u8* data, int len, void* userdata) { return 0; }
int Net_RecvPacket(u8* data, void* userdata) { return 0; }

void Camera_Start(int num, void* userdata) {}
void Camera_Stop(int num, void* userdata) {}
void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata)
{
    memset(frame, 0, width * height * (yuv ? 2 : 4));
}

void Mic_Start(void* userdata) {}
void Mic_Stop(void* userdata) {}
int Mic_ReadInput(s16* data, int maxlength, void* userdata) { return 0; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder* dec) {}
bool AAC_Configure(AACDecoder* dec, int frequency, int channels) { return false; }
bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen) { return false; }

bool Addon_KeyDown(KeyType type, void* userdata) { return false; }
void Addon_RumbleStart(u32 len, void* userdata) {}
void Addon_RumbleStop(void* userdata) {}
float Addon_MotionQuery(MotionQueryType type, void* userdata) { return 0; }

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary* lib) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return nullptr; }

}

namespace
{

void Usage()
{
    fprintf(stderr,
        "usage: swi-harness <rom.nds> (--bios official <dir> | --bios file <dir> | --bios free)\n"
        "                   --frames N --out <dir>\n"
        "                   [--shot-every N] [--blob-max BYTES] [--blob-total BYTES] [--jit]\n"
        "  <dir> holds bios9.bin and bios7.bin; \"file\" works like \"official\" and is meant\n"
        "  for other BIOS images (for example an older FreeBIOS build)\n"
        "  --shot-every   screenshot interval in frames (default 60, 0 = last frame only)\n"
        "  --blob-max     largest SWI output saved to outputs.bin (default 4194304, 0 = none)\n"
        "  --blob-total   cap for outputs.bin (default 2147483648)\n"
        "  --jit          use the JIT (needs a build with -DSWI_HARNESS_JIT=ON)\n"
        "  --firmware     load this firmware image instead of the generated one\n"
        "                 (still direct boot; for experiments, the BIOS diff uses the generated one)\n"
        "  --tap KEY@FRAME  hold a key for 6 frames from FRAME (A B SELECT START RIGHT LEFT UP DOWN R L X Y),\n"
        "                 can be repeated\n"
        "  --console dsi  DSi mode, set up like the app: --bios picks the DS-mode (NTR) BIOS, and\n"
        "                 --dsi <dir> holds the DSi bios9.bin, bios7.bin and firmware.bin\n"
        "  --nand <file>  DSi NAND image; it is written to (touch calibration patch), so pass a copy\n"
        "  --rtc Y,M,D,h,m,s  RTC date and time (default 2026,1,1,12,0,0)\n"
        "  --dump-mainram   write main RAM (game memory only) to mainram.bin at the end\n");
}

const char* ConsoleModeName(u16 scfgbios)
{
    // SCFG_BIOS bit 1 (ARM9) / bit 9 (ARM7) select the NTR BIOS, as set up by DSi::SetupDirectBoot
    return (scfgbios & (1<<1)) ? "NTR" : "TWL";
}

// SCFG state of a DSi, as one tab-separated line
std::string SCFGState(DSi& dsi)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "SCFG_BIOS=%04X\tSCFG_EXT9=%08X\tSCFG_EXT7=%08X\tSCFG_Clock9=%04X\tSCFG_Clock7=%04X\tSCFG_MC=%08X\tSCFG_RST=%04X\tmode=%s",
        dsi.SCFG_BIOS, dsi.SCFG_EXT[0], dsi.SCFG_EXT[1], dsi.SCFG_Clock9, dsi.SCFG_Clock7, dsi.SCFG_MC, dsi.SCFG_RST,
        ConsoleModeName(dsi.SCFG_BIOS));
    return buf;
}

bool LoadDSiBIOS(const std::string& path, DSiBIOSImage& out)
{
    // handed to the emulator only, never inspected
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);

    // same as the app with the boot screen off: park the reset vector
    *(u32*)out.data() = 0xEAFFFFFE;
    return ok;
}

// opens the NAND like the app's loadNAND, including the touch calibration patch
std::optional<DSi_NAND::NANDImage> LoadNAND(const std::string& path, const DSiBIOSImage& arm7ibios)
{
    Platform::FileHandle* nandfile = Platform::OpenFile(path, Platform::FileMode::ReadWriteExisting);
    if (!nandfile)
        return std::nullopt;

    // the emulator takes the ES key from the ARM7i BIOS here, as in the app
    DSi_NAND::NANDImage nandimage(nandfile, &arm7ibios[0x8308]);
    if (!nandimage)
        return std::nullopt;

    {
        auto mount = DSi_NAND::NANDMount(nandimage);
        if (!mount)
            return std::nullopt;

        DSi_NAND::DSiFirmwareSystemSettings settings {};
        if (!mount.ReadUserData(settings))
            return std::nullopt;

        settings.TouchCalibrationADC1 = {0, 0};
        settings.TouchCalibrationPixel1 = {0, 0};
        settings.TouchCalibrationADC2 = {255 << 4, 191 << 4};
        settings.TouchCalibrationPixel2 = {255, 191};
        settings.UpdateHash();

        if (!mount.ApplyUserData(settings))
            return std::nullopt;
    }

    return nandimage;
}

struct Tap
{
    u32 Frame;
    u32 Key;
};

int KeyIndex(const std::string& name)
{
    static const char* names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    for (int i = 0; i < 12; i++)
        if (name == names[i]) return i;
    return -1;
}

bool ReadFile(const std::string& path, std::vector<u8>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseeko64(f, 0, SEEK_END);
    s64 len = ftello64(f);
    fseeko64(f, 0, SEEK_SET);
    out.resize(len);
    bool ok = fread(out.data(), 1, len, f) == (size_t)len;
    fclose(f);
    return ok;
}

struct ScreenStats
{
    double White[2];
    u32 Colors[2];
};

// top screen above the bottom one, 32-bit BMP
ScreenStats WriteScreenshot(NDS& nds, const std::string& path)
{
    ScreenStats stats {};
    int front = nds.GPU.FrontBuffer;
    const u32* screens[2] = {nds.GPU.Framebuffer[front][0].get(), nds.GPU.Framebuffer[front][1].get()};

    const int w = 256, h = 384;
    std::vector<u32> pixels(w * h, 0);
    for (int s = 0; s < 2; s++)
    {
        if (!screens[s]) continue;
        memcpy(&pixels[s * 256 * 192], screens[s], 256 * 192 * 4);

        u32 white = 0;
        std::unordered_set<u32> colors;
        for (int i = 0; i < 256 * 192; i++)
        {
            u32 c = screens[s][i] & 0xFFFFFF;
            if ((c & 0xF0F0F0) == 0xF0F0F0) white++;
            if (colors.size() < 100000) colors.insert(c);
        }
        stats.White[s] = white / (256.0 * 192.0);
        stats.Colors[s] = (u32)colors.size();
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return stats;

    u32 datasize = w * h * 4;
    u8 header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *(u32*)&header[2] = 54 + datasize;
    *(u32*)&header[10] = 54;
    *(u32*)&header[14] = 40;
    *(s32*)&header[18] = w;
    *(s32*)&header[22] = -h; // top-down rows
    *(u16*)&header[26] = 1;
    *(u16*)&header[28] = 32;
    *(u32*)&header[34] = datasize;
    fwrite(header, 1, sizeof(header), f);

    for (auto& p : pixels) p |= 0xFF000000;
    fwrite(pixels.data(), 4, pixels.size(), f);
    fclose(f);
    return stats;
}

}

int main(int argc, char** argv)
{
    std::string rompath, biosmode, biosdir, outdir, firmwarepath;
    u32 frames = 0, shotevery = 60;
    u32 blobmax = 4 * 1024 * 1024;
    u64 blobtotal = 2ull * 1024 * 1024 * 1024;
    bool jit = false;
    std::vector<Tap> taps;
    std::string console = "ds", dsidir, nandpath;
    int rtc[6] = {2026, 1, 1, 12, 0, 0};
    bool dumpmainram = false;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { Usage(); exit(1); }
            return argv[++i];
        };

        if (arg == "--bios")
        {
            biosmode = next();
            if (biosmode == "official" || biosmode == "file") biosdir = next();
        }
        else if (arg == "--frames") frames = strtoul(next().c_str(), nullptr, 0);
        else if (arg == "--out") outdir = next();
        else if (arg == "--shot-every") shotevery = strtoul(next().c_str(), nullptr, 0);
        else if (arg == "--blob-max") blobmax = strtoul(next().c_str(), nullptr, 0);
        else if (arg == "--blob-total") blobtotal = strtoull(next().c_str(), nullptr, 0);
        else if (arg == "--jit") jit = true;
        else if (arg == "--firmware") firmwarepath = next();
        else if (arg == "--tap")
        {
            std::string tap = next();
            size_t at = tap.find('@');
            int key = (at == std::string::npos) ? -1 : KeyIndex(tap.substr(0, at));
            if (key < 0)
            {
                fprintf(stderr, "bad --tap %s\n", tap.c_str());
                return 1;
            }
            taps.push_back({(u32)strtoul(tap.substr(at + 1).c_str(), nullptr, 0), (u32)key});
        }
        else if (arg == "--console") console = next();
        else if (arg == "--dump-mainram") dumpmainram = true;
        else if (arg == "--dsi") dsidir = next();
        else if (arg == "--nand") nandpath = next();
        else if (arg == "--rtc")
        {
            std::string val = next();
            if (sscanf(val.c_str(), "%d,%d,%d,%d,%d,%d", &rtc[0], &rtc[1], &rtc[2], &rtc[3], &rtc[4], &rtc[5]) != 6)
            {
                fprintf(stderr, "bad --rtc %s\n", val.c_str());
                return 1;
            }
        }
        else if (arg == "-h" || arg == "--help") { Usage(); return 0; }
        else if (arg[0] == '-') { fprintf(stderr, "unknown option %s\n", arg.c_str()); Usage(); return 1; }
        else rompath = arg;
    }

    if (rompath.empty() || outdir.empty() || frames == 0 || (biosmode != "official" && biosmode != "file" && biosmode != "free")
        || (console != "ds" && console != "dsi") || (console == "dsi" && (dsidir.empty() || nandpath.empty())))
    {
        Usage();
        return 1;
    }
    bool dsimode = (console == "dsi");

    std::filesystem::create_directories(outdir);
    CoreLog = fopen((outdir + "/core.log").c_str(), "w");

    std::vector<u8> rom;
    if (!ReadFile(rompath, rom))
    {
        fprintf(stderr, "can't read ROM %s\n", rompath.c_str());
        return 1;
    }

    NDSArgs args {};
#ifdef JIT_ENABLED
    // same defaults as the app
    args.JIT = jit ? std::make_optional(JITArgs()) : std::nullopt;
#else
    if (jit)
    {
        fprintf(stderr, "--jit needs a build with -DSWI_HARNESS_JIT=ON\n");
        return 1;
    }
    args.JIT = std::nullopt;
#endif
    args.GDB = std::nullopt;
    if (dsimode && firmwarepath.empty())
        firmwarepath = dsidir + "/firmware.bin"; // the app loads the DSi firmware dump in DSi mode

    if (firmwarepath.empty())
        args.Firmware = Firmware(0); // generated DS firmware, same for both runs
    else
    {
        Platform::FileHandle* f = Platform::OpenFile(firmwarepath, Platform::FileMode::Read);
        if (!f)
        {
            fprintf(stderr, "can't read firmware %s\n", firmwarepath.c_str());
            return 1;
        }
        args.Firmware = Firmware(f);
        Platform::CloseFile(f);
        args.Firmware.UpdateChecksums(); // what the app's customizeFirmware ends with
    }

    if (biosmode == "official" || biosmode == "file")
    {
        // the files are only handed to the emulator, never inspected
        std::vector<u8> bios9, bios7;
        if (!ReadFile(biosdir + "/bios9.bin", bios9) || bios9.size() != ARM9BIOSSize)
        {
            fprintf(stderr, "can't read %s/bios9.bin (%u bytes expected)\n", biosdir.c_str(), ARM9BIOSSize);
            return 1;
        }
        if (!ReadFile(biosdir + "/bios7.bin", bios7) || bios7.size() != ARM7BIOSSize)
        {
            fprintf(stderr, "can't read %s/bios7.bin (%u bytes expected)\n", biosdir.c_str(), ARM7BIOSSize);
            return 1;
        }
        memcpy(args.ARM9BIOS->data(), bios9.data(), ARM9BIOSSize);
        memcpy(args.ARM7BIOS->data(), bios7.data(), ARM7BIOSSize);
    }

    std::unique_ptr<NDS> nds;
    if (dsimode)
    {
        auto arm9ibios = std::make_unique<DSiBIOSImage>();
        auto arm7ibios = std::make_unique<DSiBIOSImage>();
        if (!LoadDSiBIOS(dsidir + "/bios9.bin", *arm9ibios) || !LoadDSiBIOS(dsidir + "/bios7.bin", *arm7ibios))
        {
            fprintf(stderr, "can't read the DSi BIOS files in %s\n", dsidir.c_str());
            return 1;
        }

        auto nand = LoadNAND(nandpath, *arm7ibios);
        if (!nand)
        {
            fprintf(stderr, "can't open or patch the DSi NAND %s\n", nandpath.c_str());
            return 1;
        }

        // same order and values as the app: no SD card, no full BIOS boot, DSP HLE on
        DSiArgs dsiargs {
            std::move(args),
            std::move(arm9ibios),
            std::move(arm7ibios),
            std::move(*nand),
            std::nullopt,
            false,
            true,
        };
        nds = std::make_unique<DSi>(std::move(dsiargs), nullptr);
    }
    else
        nds = std::make_unique<NDS>(std::move(args), nullptr);

    printf("console %s, NTR BIOS: %s %s (ARM9 native: %d, ARM7 native: %d), JIT %s\n", console.c_str(),
        biosmode.c_str(), biosdir.c_str(),
        nds->IsLoadedARM9BIOSKnownNative(), nds->IsLoadedARM7BIOSKnownNative(), jit ? "on" : "off");

    u32 romlen = (u32)rom.size();
    auto romdata = std::make_unique<u8[]>(romlen);
    memcpy(romdata.get(), rom.data(), romlen);
    rom.clear();
    rom.shrink_to_fit();

    NDSCart::NDSCartArgs cartargs {};
    auto cart = NDSCart::ParseROM(std::move(romdata), romlen, nullptr, std::move(cartargs));
    if (!cart)
    {
        fprintf(stderr, "failed to parse ROM\n");
        return 1;
    }

    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->RTC.SetDateTime(rtc[0], rtc[1], rtc[2], rtc[3], rtc[4], rtc[5]);
    std::string romname = std::filesystem::path(rompath).filename().string();
    nds->SetupDirectBoot(romname);

    FILE* scfglog = nullptr;
    if (dsimode)
    {
        DSi& dsi = *(DSi*)nds.get();
        const NDSHeader& header = nds->NDSCartSlot.GetCart()->GetHeader();
        scfglog = fopen((outdir + "/scfg.tsv").c_str(), "w");
        if (scfglog)
        {
            fprintf(scfglog, "# cart unit code %02X (%s cart), game runs in %s mode after direct boot\n",
                header.UnitCode, (header.UnitCode & 0x02) ? "DSi-enhanced/DSi" : "DS-only",
                ConsoleModeName(dsi.SCFG_BIOS));
            fprintf(scfglog, "frame\tstate\n0\t%s\n", SCFGState(dsi).c_str());
            fflush(scfglog);
        }
        printf("cart unit code %02X, after direct boot: %s\n", header.UnitCode, SCFGState(dsi).c_str());
    }

    if (!SWILog::Open(outdir, blobmax, blobtotal))
    {
        fprintf(stderr, "can't open log files in %s\n", outdir.c_str());
        return 1;
    }

    FILE* framelog = fopen((outdir + "/frames.tsv").c_str(), "w");
    if (framelog) fprintf(framelog, "frame\ttop_white\tbottom_white\ttop_colors\tbottom_colors\tswi9\tswi7\tfile\n");

    nds->Start();

    auto t0 = std::chrono::steady_clock::now();
    for (u32 frame = 0; frame < frames && !StopRequested; frame++)
    {
        SWILog::SetFrame(frame);

        u32 keymask = 0xFFF;
        for (const Tap& tap : taps)
            if (frame >= tap.Frame && frame < tap.Frame + 6)
                keymask &= ~(1 << tap.Key);
        nds->SetKeyMask(keymask);

        nds->RunFrame();

        bool last = (frame + 1 == frames);
        if (last || (shotevery && ((frame + 1) % shotevery) == 0))
        {
            char name[64];
            snprintf(name, sizeof(name), "frame_%05u.bmp", frame + 1);
            ScreenStats stats = WriteScreenshot(*nds, outdir + "/" + name);
            if (framelog)
            {
                fprintf(framelog, "%u\t%.3f\t%.3f\t%u\t%u\t%llu\t%llu\t%s\n", frame + 1,
                    stats.White[0], stats.White[1], stats.Colors[0], stats.Colors[1],
                    (unsigned long long)SWILog::GetCallCount(0), (unsigned long long)SWILog::GetCallCount(1), name);
                fflush(framelog);
            }
            if (scfglog)
            {
                fprintf(scfglog, "%u\t%s\n", frame + 1, SCFGState(*(DSi*)nds.get()).c_str());
                fflush(scfglog);
            }
            printf("frame %u: top white %.0f%% (%u colors), bottom white %.0f%% (%u colors), SWIs ARM9 %llu ARM7 %llu\n",
                frame + 1, stats.White[0] * 100, stats.Colors[0], stats.White[1] * 100, stats.Colors[1],
                (unsigned long long)SWILog::GetCallCount(0), (unsigned long long)SWILog::GetCallCount(1));
            fflush(stdout);
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("ran %u frames in %.1f s\n", frames, secs);

    SWILog::Close(&nds->ARM9, &nds->ARM7);
    if (dumpmainram)
    {
        FILE* f = fopen((outdir + "/mainram.bin").c_str(), "wb");
        if (f)
        {
            fwrite(nds->MainRAM, 1, nds->MainRAMMask + 1, f);
            fclose(f);
        }
    }
    if (framelog) fclose(framelog);
    if (scfglog) fclose(scfglog);
    nds->Stop();
    if (CoreLog) fclose(CoreLog);
    return 0;
}
