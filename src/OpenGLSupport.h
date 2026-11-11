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

#ifndef OPENGLSUPPORT_H
#define OPENGLSUPPORT_H

#include <stdio.h>
#include <string.h>

#include "Platform.h"
#include "PlatformOGL.h"

namespace melonDS::OpenGL
{

// what the compute program cache did for a batch of CompileComputeProgram calls
struct ShaderCacheStats
{
    u32 Programs = 0;
    // loaded from a binary, compiled as there was none, compiled as the driver turned its binary
    // down, compiled as there's no cache (no path, or a driver without binary formats)
    u32 Hits = 0, Misses = 0, Rejected = 0, Uncached = 0;
    u64 BytesLoaded = 0, BytesSaved = 0;
};

// the file the compute program cache lives in, which the frontend picks (private storage it may
// lose, like Android's cache dir). empty, the default, is no cache. the file is read at the first
// CompileComputeProgram that asks for the cache
void SetShaderCachePath(const std::string& path);
// writes the file again if programs were added since it was read or written (or hits moved stale
// last-used stamps, see CompileComputeProgram), dropping the least
// recently used past its cap. once at the end of a batch of compiles, not after each program
void SaveShaderCache(ShaderCacheStats& stats);
// frees the programs held in memory, which the next lookup reads from the file again
void ReleaseShaderCache();

struct AttributeTarget
{
    const char* Name;
    u32 Location;
};


bool CompileVertexFragmentProgram(GLuint& result,
    const std::string& vs, const std::string& fs,
    const std::string& name,
    const std::initializer_list<AttributeTarget>& vertexInAttrs,
    const std::initializer_list<AttributeTarget>& fragmentOutAttrs);

// with a cache, a program compiled before from the same source on the same driver is loaded from
// its binary instead, and a new one is kept for next time (counted in it)
bool CompileComputeProgram(GLuint& result, const std::string& source, const std::string& name, ShaderCacheStats* cache = nullptr);

}

#endif // OPENGLSUPPORT_H
