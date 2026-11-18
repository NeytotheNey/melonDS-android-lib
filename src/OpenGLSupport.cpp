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

#include "OpenGLSupport.h"
#include "Utils.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_STATIC_LINKING_ONLY
#include "xxhash/xxhash.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

namespace OpenGL
{

// the compute program cache. a program's key is a 128 bit hash of the cache's version, the driver
// (its vendor, renderer and version strings) and the program's full source, so neither a driver
// update nor an app update that changes the source can hand back a binary built for something
// else. the file is read whole at the first lookup and checked end to end, and written again whole
// (to a temp file that then replaces it) after a batch of compiles that added programs. it's only a
// cache: a missing, short or bad file, or a binary the driver turns down, means compiling as before
namespace
{

struct ShaderCacheKey
{
    u64 Low, High;
    bool operator==(const ShaderCacheKey& other) const noexcept { return Low == other.Low && High == other.High; }
};

struct ShaderCacheKeyHash
{
    size_t operator()(const ShaderCacheKey& key) const noexcept { return (size_t)(key.Low ^ (key.High * 0x9E3779B97F4A7C15ull)); }
};

struct ShaderCacheEntry
{
    u32 BinaryFormat;
    // the cache's clock when the program was last stored or loaded, to drop the least recently used
    u64 LastUsed;
    std::vector<u8> Data;
};

// "MCSC". bump the version whenever the layout or what goes into the key changes
constexpr u32 ShaderCacheMagic = 0x4353434D;
constexpr u32 ShaderCacheVersion = 2;
// the most the file holds. each scale, WideMelon width and computeopt layout is 33 programs of its
// own, a few MB, so this keeps the last several configurations
constexpr u64 ShaderCacheMaxBytes = 24 << 20;
// a file or a binary bigger than these isn't one this wrote
constexpr u64 ShaderCacheMaxFileBytes = ShaderCacheMaxBytes * 2;
constexpr u32 ShaderCacheMaxEntryBytes = 4 << 20;
constexpr u32 ShaderCacheMaxEntries = 4096;
// how far behind the clock a loaded program's stamp may be before a hit writes the file again:
// the clock moves one a program stored or loaded, so this is about two batches of 33
constexpr u64 ShaderCacheStaleStamps = 64;
// magic, version, count, 0, clock, checksum of the rest
constexpr size_t ShaderCacheHeaderBytes = 4 * 4 + 8 + 8;
// key (2), last used, binary format, length
constexpr size_t ShaderCacheEntryHeaderBytes = 8 * 3 + 4 * 2;

std::mutex ShaderCacheMutex;
std::string ShaderCachePath;
std::unordered_map<ShaderCacheKey, ShaderCacheEntry, ShaderCacheKeyHash> ShaderCache;
// whether the file was read (or found missing or bad) since the path was set or the cache released
bool ShaderCacheLoaded = false;
// programs were added or replaced since the file was read or written, or hits moved stale stamps
bool ShaderCacheDirty = false;
u64 ShaderCacheClock = 0;
// -1 until the first lookup asks the driver, then whether it gives program binaries back at all
int ShaderCacheSupported = -1;
// the driver strings that go into every key
std::string ShaderCacheDriver;

// only the 1st, 2nd, 4th, 8th... problem, so a driver that turns every binary down or a full disk
// can't flood the log
void LogShaderCacheProblem(const char* fmt, ...)
{
    static u32 problems = 0;
    problems++;
    if ((problems & (problems - 1)) != 0)
        return;

    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    Log(LogLevel::Warn, "%s (%u so far)\n", message, problems);
}

// glProgramBinary with a format the driver doesn't know raises an error, and the renderer reads
// glGetError right after some of its own calls, so none of the cache's may be left behind
bool ClearGLErrors()
{
    bool clean = true;
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++)
        clean = false;
    return clean;
}

ShaderCacheKey ShaderCacheKeyFor(const std::string& source)
{
    XXH3_state_t state;
    XXH3_INITSTATE(&state);
    XXH3_128bits_reset(&state);
    u32 version = ShaderCacheVersion;
    XXH3_128bits_update(&state, &version, sizeof(version));
    // with its terminator, so the driver strings can't run into the source
    XXH3_128bits_update(&state, ShaderCacheDriver.c_str(), ShaderCacheDriver.size() + 1);
    XXH3_128bits_update(&state, source.data(), source.size());
    XXH128_hash_t hash = XXH3_128bits_digest(&state);
    return { hash.low64, hash.high64 };
}

u32 Read32(const std::vector<u8>& buffer, size_t at)
{
    u32 value;
    memcpy(&value, &buffer[at], sizeof(value));
    return value;
}

u64 Read64(const std::vector<u8>& buffer, size_t at)
{
    u64 value;
    memcpy(&value, &buffer[at], sizeof(value));
    return value;
}

void Write32(std::vector<u8>& buffer, u32 value)
{
    const u8* bytes = (const u8*)&value;
    buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
}

void Write64(std::vector<u8>& buffer, u64 value)
{
    const u8* bytes = (const u8*)&value;
    buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
}

// every length is checked against what's left before it's used, so a bad file can only be turned down
bool ParseShaderCache(const std::vector<u8>& buffer)
{
    if (buffer.size() < ShaderCacheHeaderBytes
        || Read32(buffer, 0) != ShaderCacheMagic || Read32(buffer, 4) != ShaderCacheVersion)
        return false;
    u32 count = Read32(buffer, 8);
    u64 clock = Read64(buffer, 16);
    u64 checksum = Read64(buffer, 24);
    if (count > ShaderCacheMaxEntries
        || XXH3_64bits(buffer.data() + ShaderCacheHeaderBytes, buffer.size() - ShaderCacheHeaderBytes) != checksum)
        return false;

    size_t at = ShaderCacheHeaderBytes;
    for (u32 i = 0; i < count; i++)
    {
        if (buffer.size() - at < ShaderCacheEntryHeaderBytes)
            return false;
        ShaderCacheKey key = { Read64(buffer, at), Read64(buffer, at + 8) };
        u64 lastUsed = Read64(buffer, at + 16);
        u32 format = Read32(buffer, at + 24);
        u32 length = Read32(buffer, at + 28);
        at += ShaderCacheEntryHeaderBytes;
        if (length == 0 || length > ShaderCacheMaxEntryBytes || buffer.size() - at < length || lastUsed > clock)
            return false;
        ShaderCache[key] = ShaderCacheEntry { format, lastUsed, std::vector<u8>(buffer.begin() + at, buffer.begin() + at + length) };
        at += length;
    }
    if (at != buffer.size())
        return false;

    ShaderCacheClock = clock;
    return true;
}

// with the mutex held
void LoadShaderCache(ShaderCacheStats& stats)
{
    ShaderCacheLoaded = true;
    ShaderCache.clear();
    ShaderCacheDirty = false;
    ShaderCacheClock = 0;

    Platform::FileHandle* file = Platform::OpenFile(ShaderCachePath, Platform::FileMode::Read);
    // none yet
    if (!file)
        return;

    u64 length = Platform::FileLength(file);
    std::vector<u8> buffer;
    bool read = false;
    if (length >= ShaderCacheHeaderBytes && length <= ShaderCacheMaxFileBytes)
    {
        buffer.resize(length);
        read = Platform::FileRead(buffer.data(), length, 1, file) == 1;
    }
    Platform::CloseFile(file);

    if (read && ParseShaderCache(buffer))
    {
        stats.BytesLoaded += length;
        return;
    }

    ShaderCache.clear();
    ShaderCacheClock = 0;
    LogShaderCacheProblem("OpenGL: the compute program cache (%llu bytes) is unreadable or from another version, starting a new one",
        (unsigned long long)length);
}

// with the mutex held: whether programs can go through the cache, which is read the first time
bool ShaderCacheReady(ShaderCacheStats& stats)
{
    if (ShaderCachePath.empty())
        return false;

    if (ShaderCacheSupported < 0)
    {
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        const char* vendor = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version = (const char*)glGetString(GL_VERSION);
        const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        ClearGLErrors();
        ShaderCacheSupported = formats > 0 && vendor && renderer && version && glsl;
        if (ShaderCacheSupported)
        {
            // the pointer size too, as a 32 and a 64 bit app can get different builds of a driver
            ShaderCacheDriver = std::string(vendor) + '\n' + renderer + '\n' + version + '\n' + glsl + '\n' + std::to_string(sizeof(void*));
        }
        else
            Log(LogLevel::Warn, "OpenGL: the driver has no program binary formats, compute programs aren't cached\n");
    }
    if (!ShaderCacheSupported)
        return false;

    if (!ShaderCacheLoaded)
        LoadShaderCache(stats);
    return true;
}

// with the mutex held, after the program linked with GL_PROGRAM_BINARY_RETRIEVABLE_HINT
void StoreProgram(GLuint program, const ShaderCacheKey& key, const std::string& name)
{
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length > 0 && (u32)length <= ShaderCacheMaxEntryBytes)
    {
        ShaderCacheEntry entry { 0, 0, std::vector<u8>(length) };
        GLsizei written = 0;
        GLenum format = 0;
        glGetProgramBinary(program, length, &written, &format, entry.Data.data());
        if (ClearGLErrors() && written > 0 && written <= length)
        {
            entry.Data.resize(written);
            entry.BinaryFormat = format;
            entry.LastUsed = ++ShaderCacheClock;
            ShaderCache[key] = std::move(entry);
            ShaderCacheDirty = true;
            return;
        }
    }
    ClearGLErrors();
    LogShaderCacheProblem("OpenGL: no binary of compute program %s (%d bytes) to cache", name.c_str(), length);
}

}

void SetShaderCachePath(const std::string& path)
{
    std::lock_guard lock(ShaderCacheMutex);
    if (path == ShaderCachePath)
        return;
    ShaderCachePath = path;
    decltype(ShaderCache)().swap(ShaderCache);
    ShaderCacheLoaded = false;
    ShaderCacheDirty = false;
}

void SaveShaderCache(ShaderCacheStats& stats)
{
    std::lock_guard lock(ShaderCacheMutex);
    if (!ShaderCacheDirty || ShaderCachePath.empty())
        return;
    // a write that fails isn't tried again until another program is added
    ShaderCacheDirty = false;

    // the most recently used first, as many as fit. the rest leave memory too
    std::vector<std::pair<u64, ShaderCacheKey>> order;
    order.reserve(ShaderCache.size());
    for (const auto& [key, entry] : ShaderCache)
        order.emplace_back(entry.LastUsed, key);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    u64 size = ShaderCacheHeaderBytes;
    size_t kept = 0;
    for (; kept < order.size() && kept < ShaderCacheMaxEntries; kept++)
    {
        u64 entrySize = ShaderCacheEntryHeaderBytes + ShaderCache.at(order[kept].second).Data.size();
        if (size - ShaderCacheHeaderBytes + entrySize > ShaderCacheMaxBytes)
            break;
        size += entrySize;
    }
    for (size_t i = kept; i < order.size(); i++)
        ShaderCache.erase(order[i].second);

    std::vector<u8> buffer;
    buffer.reserve(size);
    Write32(buffer, ShaderCacheMagic);
    Write32(buffer, ShaderCacheVersion);
    Write32(buffer, (u32)kept);
    Write32(buffer, 0);
    Write64(buffer, ShaderCacheClock);
    // the checksum, filled in below
    Write64(buffer, 0);
    for (size_t i = 0; i < kept; i++)
    {
        const ShaderCacheEntry& entry = ShaderCache.at(order[i].second);
        Write64(buffer, order[i].second.Low);
        Write64(buffer, order[i].second.High);
        Write64(buffer, entry.LastUsed);
        Write32(buffer, entry.BinaryFormat);
        Write32(buffer, (u32)entry.Data.size());
        buffer.insert(buffer.end(), entry.Data.begin(), entry.Data.end());
    }
    u64 checksum = XXH3_64bits(buffer.data() + ShaderCacheHeaderBytes, buffer.size() - ShaderCacheHeaderBytes);
    memcpy(&buffer[24], &checksum, sizeof(checksum));

    if (!ReplaceFile(ShaderCachePath, buffer.data(), buffer.size()))
    {
        LogShaderCacheProblem("OpenGL: couldn't write the compute program cache (%zu bytes)", buffer.size());
        return;
    }
    stats.BytesSaved += buffer.size();
}

void ReleaseShaderCache()
{
    std::lock_guard lock(ShaderCacheMutex);
    decltype(ShaderCache)().swap(ShaderCache);
    ShaderCacheLoaded = false;
    ShaderCacheDirty = false;
}

// see LastShaderError
static std::string LastShaderErrorText;

const std::string& LastShaderError()
{
    return LastShaderErrorText;
}

bool CompilerShader(GLuint& id, const std::string& source, const std::string& name, const std::string& type)
{
    int res;

    if (!glCreateShader)
    {
        Log(LogLevel::Error, "OpenGL: Cannot build shader program, OpenGL hasn't been loaded\n");
        return false;
    }

    const char* sourceC = source.c_str();
    int len = source.length();
    glShaderSource(id, 1, &sourceC, &len);

    glCompileShader(id);

    glGetShaderiv(id, GL_COMPILE_STATUS, &res);
    if (res != GL_TRUE)
    {
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &res);
        if (res < 1) res = 1024;
        char* log = new char[res+1];
        glGetShaderInfoLog(id, res+1, NULL, log);
        Log(LogLevel::Error, "OpenGL: failed to compile %s shader %s: %s\n", type.c_str(), name.c_str(), log);
        LastShaderErrorText = log;
        Log(LogLevel::Debug, "shader source:\n--\n%s\n--\n", source.c_str());
        delete[] log;

        glDeleteShader(id);

        return false;
    }

    return true;
}

bool LinkProgram(GLuint& result, GLuint* ids, int numIds)
{
    int res;

    if (!glLinkProgram)
    {
        Log(LogLevel::Error, "OpenGL: Cannot link shader program, OpenGL hasn't been loaded\n");
        return false;
    }

    for (int i = 0; i < numIds; i++)
    {
        glAttachShader(result, ids[i]);
    }

    glLinkProgram(result);

    for (int i = 0; i < numIds; i++)
        glDetachShader(result, ids[i]);

    glGetProgramiv(result, GL_LINK_STATUS, &res);
    if (res != GL_TRUE)
    {
        glGetProgramiv(result, GL_INFO_LOG_LENGTH, &res);
        if (res < 1) res = 1024;
        char* log = new char[res+1];
        glGetProgramInfoLog(result, res+1, NULL, log);
        Log(LogLevel::Error, "OpenGL: failed to link shader program: %s\n", log);
        LastShaderErrorText = log;
        delete[] log;

        return false;
    }

    return true;
}

static bool BuildComputeProgram(GLuint& result, const std::string& source, const std::string& name, bool retrievable)
{
    // an older failure's message isn't this one's
    LastShaderErrorText.clear();
    result = glCreateProgram();
    // without it glGetProgramBinary may have nothing to give back
    if (retrievable)
        glProgramParameteri(result, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

    GLuint shader = 0;
    bool linkingSucess = false;

    if (!glCreateShader || !glDeleteShader)
        goto error;

    shader = glCreateShader(GL_COMPUTE_SHADER);

    if (!CompilerShader(shader, source, name, "compute"))
    {
        // CompilerShader already deleted it
        shader = 0;
        goto error;
    }

    linkingSucess = LinkProgram(result, &shader, 1);

error:
    // 0 if glCreateShader isn't loaded, and glDeleteShader might not be either
    if (shader != 0)
        glDeleteShader(shader);

    if (!linkingSucess)
    {
        glDeleteProgram(result);
        // Program names are shared with other contexts, so don't leave a deleted name behind for the caller to delete again
        result = 0;
    }

    return linkingSucess;
}

bool CompileComputeProgram(GLuint& result, const std::string& source, const std::string& name, ShaderCacheStats* cache)
{
    if (!cache)
        return BuildComputeProgram(result, source, name, false);

    // held through the GL calls too: only one renderer compiles at a time, and the entry being
    // loaded from can't go away under glProgramBinary
    std::lock_guard lock(ShaderCacheMutex);
    cache->Programs++;
    if (!ShaderCacheReady(*cache))
    {
        cache->Uncached++;
        return BuildComputeProgram(result, source, name, false);
    }

    ShaderCacheKey key = ShaderCacheKeyFor(source);
    auto it = ShaderCache.find(key);
    if (it == ShaderCache.end())
        cache->Misses++;
    else
    {
        result = glCreateProgram();
        glProgramBinary(result, it->second.BinaryFormat, it->second.Data.data(), (GLsizei)it->second.Data.size());
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(result, GL_LINK_STATUS, &linkStatus);
        ClearGLErrors();
        if (linkStatus == GL_TRUE)
        {
            // the file's stamps only move when it's written. a program that's behind the clock by
            // more than about two batches (other configurations were stored or used since) is written
            // again with its new stamp, so the configuration played every day isn't the first to go.
            // one that's still among the newest isn't, so a launch with the same settings writes nothing
            if (ShaderCacheClock - it->second.LastUsed > ShaderCacheStaleStamps)
                ShaderCacheDirty = true;
            it->second.LastUsed = ++ShaderCacheClock;
            cache->Hits++;
            return true;
        }

        // the driver changed under the same strings, or turns the binary down for its own reasons.
        // compile it as if it wasn't there, and the new binary replaces it
        glDeleteProgram(result);
        result = 0;
        ShaderCache.erase(it);
        ShaderCacheDirty = true;
        cache->Rejected++;
        LogShaderCacheProblem("OpenGL: the driver turned the cached compute program %s down, compiling it", name.c_str());
    }

    if (!BuildComputeProgram(result, source, name, true))
        return false;
    StoreProgram(result, key, name);
    return true;
}

bool CompileVertexFragmentProgram(GLuint& result,
    const std::string& vs, const std::string& fs,
    const std::string& name,
    const std::initializer_list<AttributeTarget>& vertexInAttrs,
    const std::initializer_list<AttributeTarget>& fragmentOutAttrs)
{
    GLuint shaders[2] =
    {
        glCreateShader(GL_VERTEX_SHADER),
        glCreateShader(GL_FRAGMENT_SHADER)
    };
    result = glCreateProgram();

    bool linkingSucess = false;

    // CompilerShader deletes a shader that failed, deleting 0 below does nothing
    if (!CompilerShader(shaders[0], vs, name, "vertex"))
    {
        shaders[0] = 0;
        goto error;
    }

    if (!CompilerShader(shaders[1], fs, name, "fragment"))
    {
        shaders[1] = 0;
        goto error;
    }


    for (const AttributeTarget& target : vertexInAttrs)
    {
        glBindAttribLocation(result, target.Location, target.Name);
    }
    // ANDROID: Not supported by GLES. Shaders explicitly specify frag data location
    // for (const AttributeTarget& target : fragmentOutAttrs)
    // {
    //     glBindFragDataLocation(result, target.Location, target.Name);
    // }

    linkingSucess = LinkProgram(result, shaders, 2);

error:
    glDeleteShader(shaders[1]);
    glDeleteShader(shaders[0]);

    if (!linkingSucess)
    {
        glDeleteProgram(result);
        // Program names are shared with other contexts, so don't leave a deleted name behind for the caller to delete again
        result = 0;
    }

    return linkingSucess;
}

}

}
