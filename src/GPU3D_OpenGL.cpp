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

#include "GPU3D_OpenGL.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "NDS.h"
#include "GPU.h"
#include "GPU3D_OpenGL_shaders.h"
#include "GPU3D_ComputeStats.h"
#include "Platform.h"
#include "WideMelon.h"

namespace melonDS
{

// debug only: GLTIME's passes (GPU::GLTimers, a pass runs up to the next one) and SCENEFRAGS'
// counters (GPU::SceneStats 2). nothing while their switches are off, and nothing in release builds
#ifdef MELONADE_DEBUG_TOOLS
#define GL_TIMER(pass) do { if (Timers) Timers->Begin(GLPass::pass); } while (0)
#define FRAG_PASS(pass) do { if (FragCounter) SetFragPass(GLFragCounter::Frag_##pass); } while (0)
#define TRANS_SKIPPED(field) do { if (TransStats) TransStats->Sec.field++; } while (0)
#else
#define GL_TIMER(pass) do {} while (0)
#define FRAG_PASS(pass) do {} while (0)
#define TRANS_SKIPPED(field) do {} while (0)
#endif

// GPU::GLPace's clock
static s64 PaceNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool GLRenderer::BuildRenderShader(bool wbuffer, bool fragCount, bool fetch, bool nodiscard, bool full, int edge)
{
    char shadername[80];
    char probename[16] = "";
    if (TexProbe)
        snprintf(probename, sizeof(probename), "TexProbe%d", TexProbe);
    // GPU::GLTexCache 256: the color-only build is only the cache-only programs', never the twins'
    bool colorOnly = TexCache && TexColorPlane && TexColorOnly && TexCacheOnly && !full;
    snprintf(shadername, sizeof(shadername), "RenderShader%c%s%s%s%s%s%s%s%s", wbuffer?'W':'Z', fetch ? "Fetch" : "",
        nodiscard ? "NoDiscard" : "", fragCount ? "Count" : "", probename,
        TexCache ? (TexCacheOnly && !full ? "TexCacheOnly" : "TexCache") : "", TexCache && TexWrapVS ? "WrapVS" : "",
        TexCache && TexColorPlane ? (colorOnly ? "ColorOnly" : "Color") : "", edge == 2 ? "EdgeLines" : edge ? "Edge" : "");

    // kShaderHeader has no newline of its own, the common sources start with one
    const char* wbufdef = wbuffer ? "\n#define WBuffer" : "";
    // GPU::GLTexCache 64 (T2 step 3): the heap lookup's per-polygon wrap constants come from the
    // vertex shader, in a flat varying both stages declare under this
    const char* wrapdef = TexCache && TexWrapVS ? "\n#define TexWrapVS" : "";
    // GPU::GLEdgeMark 1 (E1): the opaque fragments set the edge flag from their polygon's outline,
    // which the vertex shader passes on where to find (kRenderVS, kRenderFS's EdgeFlag)
    // GPU::GLEdgeFast bit 1: and up to 4 of those edges come as flat varyings (EdgeLines)
    const char* edgedef = edge == 2 ? "\n#define EdgeMark\n#define EdgeLines" : edge ? "\n#define EdgeMark" : "";

    std::string vsbuf;
    vsbuf += kShaderHeader;
    vsbuf += wbufdef;
    vsbuf += wrapdef;
    vsbuf += edgedef;
    vsbuf += kRenderVSCommon;
    vsbuf += kRenderVS;

    std::string fsbuf;
    fsbuf += kShaderHeader;
    // GPU::GLTransSkip bit 2: only built when the extension is listed (PrepareFetchShaders)
    if (fetch)
        fsbuf += "\n#extension GL_EXT_shader_framebuffer_fetch : require\n#define FramebufferFetch";
    // GPU::GLNoDiscard: kRenderFS's opaque-only main, without discard
    if (nodiscard)
        fsbuf += "\n#define NoDiscard";
    // GPU::GLTexProbe (T2 measurement probes, render wrong): kRenderFSCommon's texture lookups
    // become one fetch (1), none (2), the wrap and one fetch (3), or the wrap and two dependent
    // fetches (4), in every build of the render programs
    if (TexProbe == 1)
        fsbuf += "\n#define TexProbeFetch";
    else if (TexProbe == 2)
        fsbuf += "\n#define TexProbeConst";
    else if (TexProbe == 3)
        fsbuf += "\n#define TexProbeWrap";
    else if (TexProbe == 4)
        fsbuf += "\n#define TexProbeChain";
    // GPU::GLTexCache (T2 step 1): kRenderFSCommon's nearest lookup reads the decoded texel heap for
    // the polygons AssignTexCache put in it, in every build of the render programs. never together
    // with the probe (UpdateRenderVariant)
    if (TexCache)
        fsbuf += "\n#define TexCache";
    // GPU::GLTexCache step 2: the nearest lookup without the old decode path, except in the twins
    // (full) the heap's fallback polygons draw with
    if (TexCache && TexCacheOnly && !full)
        fsbuf += "\n#define TexCacheOnly";
    // GPU::GLTexCache 128 (T2 step 4a, measurement only): the heap lookup reads the color plane for
    // the polygons AssignTexCache marked, in every build with the lookup. 256 (debug): the
    // cache-only programs without the record path
    if (TexCache && TexColorPlane)
        fsbuf += "\n#define TexColorPlane";
    if (colorOnly)
        fsbuf += "\n#define TexColorOnly";
    fsbuf += wrapdef;
#ifdef MELONADE_DEBUG_TOOLS
    // debug only, GPU::GLEdgeFast 4: the EdgeLines builds fail, to test the fallback to the texture ones
    if (edge == 2 && FailEdgeLines)
        fsbuf += "\n#error gledgefast 4 makes the EdgeLines programs fail on purpose";
    // debug only, GPU::GLTexCache 32: a twin that can't build, to test TexCacheOff
    if (full && FailTwins)
        fsbuf += "\n#error gltexcache 32 makes the twins fail on purpose";
#endif
    fsbuf += wbufdef;
    fsbuf += edgedef;
    fsbuf += kRenderFSCommon;
#ifdef MELONADE_DEBUG_TOOLS
    if (fragCount)
    {
        // debug only: the counter declarations, and the count first thing in main
        fsbuf += kRenderFSFragCount;
        std::string fs = kRenderFS;
        // the last main: kRenderFS's first one is the NoDiscard build's (GPU::GLNoDiscard), which
        // the counting builds don't compile, and the normal one comes after it under #else
        size_t mainPos = fs.rfind("void main()");
        size_t brace = mainPos == std::string::npos ? std::string::npos : fs.find('{', mainPos);
        if (brace == std::string::npos)
            return false;
        fs.insert(brace + 1, kRenderFSFragCountHit);
        fsbuf += fs;
    }
    else
#endif
    fsbuf += kRenderFS;

    GLuint prog;
    bool ret = OpenGL::CompileVertexFragmentProgram(prog,
        vsbuf, fsbuf,
        shadername,
        {{"vPosition", 0}, {"vColor", 1}, {"vTexcoord", 2}, {"vPolygonAttr", 3}, {"vEdgeInfo", 4},
         {"vEdgeLine0", 5}, {"vEdgeLine1", 6}, {"vEdgeLine2", 7}},
        {{"oColor", 0}, {"oAttr", 1}});

    if (!ret) return false;

    GLint uni_id = glGetUniformBlockIndex(prog, "uConfig");
    glUniformBlockBinding(prog, uni_id, 0);

    glUseProgram(prog);

    uni_id = glGetUniformLocation(prog, "TexMem");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(prog, "TexPalMem");
    glUniform1i(uni_id, 1);
    if (TexCache)
    {
        glUniform1i(glGetUniformLocation(prog, "TexHeap"), 2);
        glUniform1i(glGetUniformLocation(prog, "TexPalLinear"), 3);
        if (TexColorPlane)
            glUniform1i(glGetUniformLocation(prog, "TexColor"), 4);
    }
    if (edge)
        glUniform1i(glGetUniformLocation(prog, "EdgeTex"), 5);

    int id = (nodiscard ? 4 : (int)wbuffer + (fetch ? 2 : 0)) + (full ? FullShader : 0) + (edge == 2 ? EdgeLineShader : edge ? EdgeShader : 0);
    DepthBiasLoc[id] = glGetUniformLocation(prog, "uDepthBias");
    glUniform1f(DepthBiasLoc[id], 0.0f);
    DepthBias[id] = 0.0f;

    RenderShader[id] = prog;
    RenderModeULoc[id] = glGetUniformLocation(prog, "uRenderMode");
#ifdef MELONADE_DEBUG_TOOLS
    FragPassLoc[id] = fragCount ? glGetUniformLocation(prog, "uFragPass") : -1;
#endif

    return true;
}

// GPU::GLTransSkip bit 2: RenderShader[2] and [3], built the first time a frame wants them (so a
// session that never sets the bit compiles nothing more), as counting builds while SCENEFRAGS
// counts. false without the extension, or when they don't build (logged once)
bool GLRenderer::PrepareFetchShaders()
{
    if (!HasFramebufferFetch)
        return false;
    if (RenderShader[2] && RenderShader[3])
        return true;
    if (FetchShadersTried)
        return false;
    FetchShadersTried = true;

    bool fragCount = false;
#ifdef MELONADE_DEBUG_TOOLS
    fragCount = FragCounter != nullptr;
#endif
    if (!BuildRenderShader(false, fragCount, true) || !BuildRenderShader(true, fragCount, true))
    {
        glDeleteProgram(RenderShader[2]);
        glDeleteProgram(RenderShader[3]);
        RenderShader[2] = RenderShader[3] = 0;
        Platform::Log(Platform::LogLevel::Warn, "GLTRANS: the framebuffer fetch programs didn't build, gltrans 4 stays off\n");
        CurShaderID = -1;
        return false;
    }

    // BuildRenderShader leaves its program in use
    CurShaderID = -1;
    CurRenderMode = -1;
    return true;
}

// GPU::GLNoDiscard: RenderShader[4], built the first time a frame wants it (one program, and the
// shader cache has it from the second run on), like PrepareFetchShaders. false when it doesn't
// build (logged once), and the frame draws as with the switch off
bool GLRenderer::PrepareNoDiscardShader()
{
    if (RenderShader[4])
        return true;
    if (NoDiscardTried)
        return false;
    NoDiscardTried = true;

    if (!BuildRenderShader(false, false, false, true))
    {
        glDeleteProgram(RenderShader[4]);
        RenderShader[4] = 0;
        Platform::Log(Platform::LogLevel::Warn, "GLNODISCARD: the no-discard program didn't build, glnodiscard stays off\n");
        CurShaderID = -1;
        return false;
    }

    // BuildRenderShader leaves its program in use
    CurShaderID = -1;
    CurRenderMode = -1;
    return true;
}

// GPU::GLTexProbe (T2 measurement probe, renders wrong) and GPU::GLTexCache (T2 step 1): RenderShader[0]
// and [1] are built again with the probe or the heap lookup (as counting builds while SCENEFRAGS
// counts), and the fetch and no-discard builds are dropped, to be made again with it the first frame
// that wants them. when [0] or [1] doesn't build, they're built again as they were (logged), and the
// variant stays off until a switch changes
void GLRenderer::SetRenderVariant(int probe, bool cache, bool only, bool wrapvs, bool color, bool colorOnly)
{
    only = cache && only;
    wrapvs = cache && wrapvs;
    color = cache && color;
    colorOnly = color && only && colorOnly;
    if (probe == TexProbe && cache == TexCache && only == TexCacheOnly && wrapvs == TexWrapVS
        && color == TexColorPlane && colorOnly == TexColorOnly)
        return;

    bool fragCount = false;
#ifdef MELONADE_DEBUG_TOOLS
    fragCount = FragCounter != nullptr;
#endif
    int oldProbe = TexProbe;
    bool oldCache = TexCache, oldOnly = TexCacheOnly, oldWrapVS = TexWrapVS;
    bool oldColor = TexColorPlane, oldColorOnly = TexColorOnly;
    GLuint prev[2] = { RenderShader[0], RenderShader[1] };
    TexProbe = probe;
    TexCache = cache;
    TexCacheOnly = only;
    TexWrapVS = wrapvs;
    TexColorPlane = color;
    TexColorOnly = colorOnly;
    bool cacheVariantChanged = cache != oldCache || only != oldOnly || wrapvs != oldWrapVS
        || color != oldColor || colorOnly != oldColorOnly;
    bool built0 = BuildRenderShader(false, fragCount);
    bool built1 = built0 && BuildRenderShader(true, fragCount);
    if (!built1)
    {
        if (built0)
            glDeleteProgram(RenderShader[0]);
        if (probe != oldProbe)
            Platform::Log(Platform::LogLevel::Warn, "GLTEXPROBE: the probe %d render programs didn't build, gltexprobe stays at %d\n", probe, oldProbe);
        if (cacheVariantChanged)
            Platform::Log(Platform::LogLevel::Warn, "GLTEXCACHE: the render programs %s the texel heap lookup%s%s%s didn't build, gltexcache stays %s\n",
                cache ? "with" : "without", only ? " only" : "", wrapvs ? " and the vertex shader wrap" : "",
                color ? (colorOnly ? " and the color plane only" : " and the color plane") : "", oldCache ? "on" : "off");
        TexProbe = oldProbe;
        TexCache = oldCache;
        TexCacheOnly = oldOnly;
        TexWrapVS = oldWrapVS;
        TexColorPlane = oldColor;
        TexColorOnly = oldColorOnly;
        BuildRenderShader(false, fragCount);
        BuildRenderShader(true, fragCount);
    }
    for (int i = 0; i < 2; i++)
        if (RenderShader[i] != prev[i])
            glDeleteProgram(prev[i]);

    glDeleteProgram(RenderShader[2]);
    glDeleteProgram(RenderShader[3]);
    glDeleteProgram(RenderShader[4]);
    RenderShader[2] = RenderShader[3] = RenderShader[4] = 0;
#ifdef MELONADE_DEBUG_TOOLS
    FragPassLoc[2] = FragPassLoc[3] = FragPassLoc[4] = -1;
#endif
    DropFullShaders();
    DropEdgeShaders();
    FetchShadersTried = false;
    NoDiscardTried = false;
    CurShaderID = -1;
    CurRenderMode = -1;
    if (built1 && probe != oldProbe)
        Platform::Log(Platform::LogLevel::Info, "GLTEXPROBE: render programs built with texture probe %d (renders wrong on purpose)\n", TexProbe);
    if (built1 && cacheVariantChanged)
        Platform::Log(Platform::LogLevel::Info, "GLTEXCACHE: render programs built %s the texel heap lookup%s%s%s\n", TexCache ? "with" : "without",
            TexCacheOnly ? " only (step 2, the old decode path is in twins made when a polygon needs it)" : "",
            TexWrapVS ? ", its wrap constants made in the vertex shader (step 3)" : "",
            TexColorPlane ? (TexColorOnly ? ", the color plane only, no record path (step 4, 256)"
                                          : ", the color plane (step 4, palette-tracked)") : "");
}

// RenderFrame, when GPU::GLTexProbe or GPU::GLTexCache differs from what was asked last: the probe
// wins (the heap lookup is left out while it's nonzero). any change of GPU::GLTexCache starts the
// heap over, and the heap only exists while the programs have the lookup
void GLRenderer::UpdateRenderVariant(const GPU& gpu)
{
    bool cacheChanged = gpu.GLTexCache != TexCacheAsked;
    TexProbeAsked = gpu.GLTexProbe;
    TexCacheAsked = gpu.GLTexCache;
    int probe = (TexProbeAsked >= 1 && TexProbeAsked <= 4) ? TexProbeAsked : 0;
    bool cache = TexCacheAsked != 0 && probe == 0;
    if (cacheChanged)
    {
        CacheFailed = false;
        // made again for the new value when a polygon needs them
        DropFullShaders();
    }
    // off after a twin didn't build (TexCacheOff), until the switch changes
    cache = cache && !CacheFailed;
    // step 2 unless bit 4 (16) asks for step 1
    bool only = !(TexCacheAsked & 16);
    // step 3, bit 6 (64): the heap lookup's wrap constants from the vertex shader
    bool wrapvs = (TexCacheAsked & 64) != 0;
    // step 4, bit 7 (128): the color plane (4a, with 4b's palette tracking); bit 8 (256, debug): its cache-only
    // programs without the record path (needs 128 and step 2, ignored otherwise)
    bool color = (TexCacheAsked & 128) != 0;
    bool colorOnly = color && only && (TexCacheAsked & 256) != 0;
#ifdef MELONADE_DEBUG_TOOLS
    // debug only, GPU::GLTexCache 32: the twins' source gets an #error, to test TexCacheOff
    FailTwins = (TexCacheAsked & 32) != 0;
#endif

    if (TexHeap && (cacheChanged || !cache))
        DeleteTexCache();
    if (cache && !TexHeap && !CreateTexCache(TexCacheAsked))
        cache = false;
    SetRenderVariant(probe, cache, only, wrapvs, color, colorOnly);
    // a build that failed left the programs as they were, which can be the heap lookup ones with the
    // heap already gone: make it again, or drop to the programs without the lookup. if even that
    // fails, RenderFrame only runs AssignTexCache with a heap, so every polygon takes the old path
    if (TexCache && !TexHeap && !CreateTexCache(TexCacheAsked))
        SetRenderVariant(TexProbe, false, false, false, false, false);
    if (TexHeap && !TexCache)
        DeleteTexCache();
}

// GPU::GLTexCache: the heap texture (smaller in the tiny mode, 4) and TexPalLinear, filled from
// TexPalConvBuffer, which holds all of palette RAM once a frame has run (TexMemDirtyAll converts
// every row; on the first frame that upload fills both). false when GL can't make them
bool GLRenderer::CreateTexCache(int mode)
{
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    u32 rows = maxTextureSize >= 4096 ? 4096 : 2048;
    if (mode & 4)
        rows = GLTexHeap::TinyRows;

    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
    glActiveTexture(GL_TEXTURE2);
    glGenTextures(1, &TexHeapID);
    glBindTexture(GL_TEXTURE_2D, TexHeapID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 1024, rows);

    glActiveTexture(GL_TEXTURE3);
    glGenTextures(1, &TexPalLinearID);
    glBindTexture(GL_TEXTURE_2D, TexPalLinearID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 256, 192, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, TexPalConvBuffer);

    // GPU::GLTexCache 128 (T2 step 4a, measurement only): the color plane, the heap's size, the
    // internal format, format and type of TexPalLinear (the same 16-bit words). only the blocks of
    // entries with one are ever written or read
    if (mode & 128)
    {
        glActiveTexture(GL_TEXTURE4);
        glGenTextures(1, &TexColorID);
        glBindTexture(GL_TEXTURE_2D, TexColorID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, rows, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
    }
    GLenum error = glGetError();
    glActiveTexture(GL_TEXTURE0);
    if (error != GL_NO_ERROR)
    {
        Platform::Log(Platform::LogLevel::Warn, "GLTEXCACHE: the heap textures (1024 x %u RGBA8UI, 256 x 192 RGB5_A1%s) failed "
            "with GL error 0x%X, gltexcache stays off\n", rows, (mode & 128) ? ", the RGB5_A1 color plane" : "", error);
        DeleteTexCache();
        return false;
    }

    TexHeapRows = rows;
    TexHeap = std::make_unique<GLTexHeap>();
    TexHeap->Configure(mode, rows, TexPalConvBuffer);
    Platform::Log(Platform::LogLevel::Info, "GLTEXCACHE: on, mode %d, heap 1024 x %u texels%s\n", mode, rows,
        (mode & 128) ? ", with a color plane (step 4)" : "");
    return true;
}

void GLRenderer::DeleteTexCache()
{
    glDeleteTextures(1, &TexHeapID);
    glDeleteTextures(1, &TexPalLinearID);
    glDeleteTextures(1, &TexColorID);
    TexHeapID = TexPalLinearID = TexColorID = 0;
    TexHeapRows = 0;
    TexHeap.reset();
#ifdef MELONADE_DEBUG_TOOLS
    TexCacheStats.reset();
#endif
}

// GPU::GLTexCache step 2: the twins with the old decode path of the programs this frame uses
// (Z and W always, fetch with FetchClear0, no-discard with NoDiscard), as counting builds while
// SCENEFRAGS counts, made the first frame a polygon needs them (AssignTexCache marked one). false
// when one doesn't build: RenderFrame then turns the cache off (TexCacheOff)
bool GLRenderer::PrepareFullShaders()
{
    bool fragCount = false;
#ifdef MELONADE_DEBUG_TOOLS
    fragCount = FragCounter != nullptr;
#endif
    auto build = [&](bool wbuffer, bool fetch, bool nodiscard)
    {
        int id = (nodiscard ? 4 : (int)wbuffer + (fetch ? 2 : 0)) + FullShader;
        if (RenderShader[id])
            return true;
        if (BuildRenderShader(wbuffer, fragCount && !nodiscard, fetch, nodiscard, true))
            return true;
        glDeleteProgram(RenderShader[id]);
        RenderShader[id] = 0;
        return false;
    };
    bool ok = build(false, false, false) && build(true, false, false);
    if (ok && FetchClear0)
        ok = build(false, true, false) && build(true, true, false);
    if (ok && NoDiscard)
        ok = build(false, false, true);
    // BuildRenderShader leaves its program in use
    CurShaderID = -1;
    CurRenderMode = -1;
    return ok;
}

void GLRenderer::DropFullShaders()
{
    // and their edge-marking builds (GPU::GLEdgeMark 1), made again the next edge-marking frame
    for (int base : {FullShader, EdgeShader + FullShader, EdgeLineShader + FullShader})
    {
        for (int i = base; i < base + 5; i++)
        {
            glDeleteProgram(RenderShader[i]);
            RenderShader[i] = 0;
#ifdef MELONADE_DEBUG_TOOLS
            FragPassLoc[i] = -1;
#endif
        }
    }
}

// GPU::GLEdgeMark 1 (E1): the edge-marking builds of the opaque draws' programs this frame has
// (Z and W, no-discard with NoDiscard, the twins once PrepareFullShaders made them), each made the
// first edge-marking frame that has it, after the frame's other programs are settled. false when
// one doesn't build (logged once): that frame and the ones after it draw without the edge flag
// until the render programs are made again (DropEdgeShaders)
bool GLRenderer::PrepareEdgeShaders()
{
    if (EdgeShadersFailed)
        return false;

    bool fragCount = false;
#ifdef MELONADE_DEBUG_TOOLS
    fragCount = FragCounter != nullptr;
#endif
    bool built = false;
    auto build = [&](bool wbuffer, bool nodiscard, bool full)
    {
        int id = (nodiscard ? 4 : (int)wbuffer) + (full ? FullShader : 0);
        if (!RenderShader[id] || RenderShader[EdgeSet + id])
            return true;
        built = true;
        if (BuildRenderShader(wbuffer, fragCount && !nodiscard, false, nodiscard, full, EdgeSet == EdgeLineShader ? 2 : 1))
            return true;
        glDeleteProgram(RenderShader[EdgeSet + id]);
        RenderShader[EdgeSet + id] = 0;
        return false;
    };
    // the no-discard ones only in frames that use them (NoDiscard)
    bool ok = build(false, false, false) && build(true, false, false) && (!NoDiscard || build(false, true, false))
        && build(false, false, true) && build(true, false, true) && (!NoDiscard || build(false, true, true));
    if (!built)
        return true;

    // BuildRenderShader leaves its program in use
    CurShaderID = -1;
    CurRenderMode = -1;
    if (!ok && EdgeSet == EdgeLineShader)
    {
        // GPU::GLEdgeFast bit 1: the texture builds instead, for the rest of the renderer's life
        for (int i = EdgeLineShader; i < EdgeLineShader + 10; i++)
        {
            glDeleteProgram(RenderShader[i]);
            RenderShader[i] = 0;
#ifdef MELONADE_DEBUG_TOOLS
            FragPassLoc[i] = -1;
#endif
        }
        EdgeLinesFailed = true;
        EdgeSet = EdgeShader;
        Platform::Log(Platform::LogLevel::Warn, "GLEDGEMARK: the edge-marking programs with the outlines as varyings didn't build, "
            "gledgefast 2 draws them from the texture\n");
        return PrepareEdgeShaders();
    }
    if (!ok)
    {
        DropEdgeShaders();
        EdgeShadersFailed = true;
        Platform::Log(Platform::LogLevel::Warn, "GLEDGEMARK: the edge-marking render programs didn't build, gledgemark 1 draws as 0\n");
    }
    return ok;
}

void GLRenderer::DropEdgeShaders()
{
    for (int i = EdgeShader; i < EdgeLineShader + 10; i++)
    {
        glDeleteProgram(RenderShader[i]);
        RenderShader[i] = 0;
#ifdef MELONADE_DEBUG_TOOLS
        FragPassLoc[i] = -1;
#endif
    }
    EdgeShadersFailed = false;
    CurShaderID = -1;
}

// GPU::GLTexCache step 2, when a twin didn't build: logged once, the programs are built again with
// both paths (step 1) for this frame on, and the polygons lose RenderKey_TexFull. this frame's
// fetch and no-discard programs are made again for it (they were dropped); a no-discard one that
// doesn't build takes its key bit off the polygons too, so the frame stays exact
void GLRenderer::TexCacheOff()
{
    Platform::Log(Platform::LogLevel::Warn, "GLTEXCACHE: the render programs with the old decode path (step 2's twins) didn't build, "
        "gltexcache %d stays off until it changes\n", TexCacheAsked);
    CacheFailed = true;
    // the programs without the heap lookup, which the frames with the switch at 0 use
    SetRenderVariant(TexProbe, false, false, false, false, false);
    if (TexCache)
        Platform::Log(Platform::LogLevel::Warn, "GLTEXCACHE: the render programs without the heap lookup didn't build either\n");
    for (int i = 0; i < NumFinalPolys; i++)
    {
        PolygonList[i].TexWord = PolygonList[i].PolyData->TexParam & 0xFFFF;
        PolygonList[i].RenderKey &= ~RenderKey_TexFull;
    }
    DeleteTexCache();
    // SetRenderVariant dropped this frame's fetch and no-discard programs. the fetch ones are those
    // of a frame with the switch at 0 now; a no-discard one that doesn't build takes its key bit off
    if (FetchClear0 && !PrepareFetchShaders())
        Platform::Log(Platform::LogLevel::Warn, "GLTEXCACHE: the framebuffer fetch programs didn't build again, this frame's translucent draws are lost\n");
    if (NoDiscard && !PrepareNoDiscardShader())
    {
        NoDiscard = false;
        for (int i = 0; i < NumFinalPolys; i++)
            PolygonList[i].RenderKey &= ~RenderKey_NoDiscard;
    }
}

// nodiscard: RenderShader[4] (GPU::GLNoDiscard), Z-buffer and opaque only, which has no uRenderMode.
// full: the twin with the old decode path (GPU::GLTexCache step 2, RenderKey_TexFull)
void GLRenderer::UseRenderShader(bool wbuffer, int mode, bool fetch, bool nodiscard, bool full)
{
    // GPU::GLEdgeMark 1: the opaque draws of an edge-marking frame use the builds that write the edge
    // flag (PrepareEdgeShaders made them)
    int twin = (full ? FullShader : 0) + (EdgeMark != 0 && mode == RenderMode_Opaque && !fetch ? EdgeSet : 0);
    if (nodiscard)
    {
        if (CurShaderID != 4 + twin)
        {
            glUseProgram(RenderShader[4 + twin]);
            CurShaderID = 4 + twin;
            CurRenderMode = -1;
        }
        return;
    }

    int id = (int)wbuffer + (fetch ? 2 : 0) + twin;
    if (CurShaderID != id)
    {
        glUseProgram(RenderShader[id]);
        CurShaderID = id;
        CurRenderMode = -1;
    }
    if (CurRenderMode != mode)
    {
        glUniform1i(RenderModeULoc[id], mode);
        CurRenderMode = mode;
    }
}

// GPU::GLNoDiscard (R1 step 1): NoDiscard_Yes when an opaque-pass polygon's fragments always come
// out of FinalColor at alpha 31, so kRenderFS's opaque discard (col.a < 30.5/31) can't fire, without
// looking at its texture: polygon alpha 31 (the vertex alpha the shader gets), and texturing off (DISP3DCNT
// bit 0, uDispCnt in the shader) or no texture, or decal (the vertex alpha alone; mode 3 is a
// shadow or mask, left out), or a palette texture (I2/I4/I8) whose color 0 isn't transparent
// (TexParam bit 29), whose texels all come out at 1.0. no alpha test in these shaders. translucent
// polygons, need-opaque ones included, and shadows aren't drawn with it. GLOPAQUE's untex, decal and
// texpal classes (CountOpaqueR1). NoDiscard_Scan (R1 step 2, GPU::GLNoDiscard bit 1): the same
// polygon with modulate or toon/highlight and a texture that can return alpha 0 texels, so the
// texture decides (OpaqueTexClass): direct, 4x4, or a palette with color 0 transparent. A3I5 and
// A5I3 polygons are always translucent, so they never get here (T1's classes cover them)
enum
{
    NoDiscard_No,
    NoDiscard_Yes,
    NoDiscard_Scan,
};
static int NoDiscardClass(const Polygon* polygon, bool texturing)
{
    if (polygon->Translucent || polygon->IsShadowMask || polygon->IsShadow)
        return NoDiscard_No;
    u32 attr = polygon->Attr;
    if (((attr >> 16) & 0x1F) != 31)
        return NoDiscard_No;
    u32 format = (polygon->TexParam >> 26) & 0x7;
    if (!texturing || format == 0)
        return NoDiscard_Yes;
    if (((attr >> 4) & 0x3) == 1)
        return NoDiscard_Yes;
    if (format >= 2 && format <= 4)
        return (polygon->TexParam & (1u << 29)) ? NoDiscard_Scan : NoDiscard_Yes;
    if (format == 5 || format == 7)
        return NoDiscard_Scan;
    return NoDiscard_No;
}

void SetupDefaultTexParams(GLuint tex)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

GLRenderer::GLRenderer(GLCompositor&& compositor) noexcept :
    Renderer3D(true),
    CurGLCompositor(std::move(compositor))
{
    // GLRenderer::New() will be used to actually initialize the renderer;
    // The various glDelete* functions silently ignore invalid IDs,
    // so we can just let the destructor clean up a half-initialized renderer.
    ClearOpaqueTex();
}

std::unique_ptr<GLRenderer> GLRenderer::New() noexcept
{
    assert(glEnable != nullptr);

    std::optional<GLCompositor> compositor =  GLCompositor::New();
    if (!compositor)
        return nullptr;

    // Will be returned if the initialization succeeds,
    // or cleaned up via RAII if it fails.
    std::unique_ptr<GLRenderer> result = std::unique_ptr<GLRenderer>(new GLRenderer(std::move(*compositor)));
    compositor = std::nullopt;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);

    GLint maxTextureSize = 0;
    GLint maxViewportDims[2] = {};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);
    result->MaxTargetWidth = std::min(maxTextureSize, maxViewportDims[0]);
    result->MaxTargetHeight = std::min(maxTextureSize, maxViewportDims[1]);

    glDepthRangef(0.0f, 1.0f);
    glClearDepthf(1.0f);

    if (!OpenGL::CompileVertexFragmentProgram(result->ClearShaderPlain,
            kClearVS, kClearFS,
            "ClearShader",
            {{"vPosition", 0}},
            {{"oColor", 0}, {"oAttr", 1}}))
        return nullptr;

    result->ClearUniformLoc[0] = glGetUniformLocation(result->ClearShaderPlain, "uColor");
    result->ClearUniformLoc[1] = glGetUniformLocation(result->ClearShaderPlain, "uDepth");
    result->ClearUniformLoc[2] = glGetUniformLocation(result->ClearShaderPlain, "uOpaquePolyID");
    result->ClearUniformLoc[3] = glGetUniformLocation(result->ClearShaderPlain, "uFogFlag");

    if (!OpenGL::CompileVertexFragmentProgram(result->ClearShaderBitmap,
            kClearBitmapVS, kClearBitmapFS,
            "ClearShaderBitmap",
            {{"vPosition", 0}},
            {{"oColor", 0}, {"oAttr", 1}}))
        return nullptr;

    result->ClearBitmapULoc[0] = glGetUniformLocation(result->ClearShaderBitmap, "uScale");
    result->ClearBitmapULoc[1] = glGetUniformLocation(result->ClearShaderBitmap, "uOffset");
    result->ClearBitmapULoc[2] = glGetUniformLocation(result->ClearShaderBitmap, "uOpaquePolyID");

    glUseProgram(result->ClearShaderBitmap);
    glUniform1i(glGetUniformLocation(result->ClearShaderBitmap, "TexMem"), 0);

    memset(result->RenderShader, 0, sizeof(RenderShader));

    if (!result->BuildRenderShader(false))
        return nullptr;

    if (!result->BuildRenderShader(true))
        return nullptr;

    if (!OpenGL::CompileVertexFragmentProgram(result->FinalPassEdgeShader,
            kFinalPassVS, kFinalPassEdgeFS,
            "FinalPassEdgeShader",
            {{"vPosition", 0}},
            {{"oColor", 0}}))
        return nullptr;
    if (!OpenGL::CompileVertexFragmentProgram(result->FinalPassFogShader,
            kFinalPassVS, kFinalPassFogFS,
            "FinalPassFogShader",
            {{"vPosition", 0}},
            {{"oColor", 0}}))
        return nullptr;

    GLuint uni_id = glGetUniformBlockIndex(result->FinalPassEdgeShader, "uConfig");
    glUniformBlockBinding(result->FinalPassEdgeShader, uni_id, 0);

    glUseProgram(result->FinalPassEdgeShader);
    uni_id = glGetUniformLocation(result->FinalPassEdgeShader, "DepthBuffer");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(result->FinalPassEdgeShader, "AttrBuffer");
    glUniform1i(uni_id, 1);
    result->EdgeULoc[0] = glGetUniformLocation(result->FinalPassEdgeShader, "uEdgeScale");
    result->EdgeULoc[1] = glGetUniformLocation(result->FinalPassEdgeShader, "uDepthUnbias");
    result->EdgeULoc[2] = glGetUniformLocation(result->FinalPassEdgeShader, "uClearPolyID");
    result->EdgeULoc[3] = glGetUniformLocation(result->FinalPassEdgeShader, "uClearDepth");
    result->EdgeULoc[4] = glGetUniformLocation(result->FinalPassEdgeShader, "uEdgeFast");

    uni_id = glGetUniformBlockIndex(result->FinalPassFogShader, "uConfig");
    glUniformBlockBinding(result->FinalPassFogShader, uni_id, 0);

    glUseProgram(result->FinalPassFogShader);
    uni_id = glGetUniformLocation(result->FinalPassFogShader, "DepthBuffer");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(result->FinalPassFogShader, "AttrBuffer");
    glUniform1i(uni_id, 1);


    memset(&result->ShaderConfig, 0, sizeof(ShaderConfig));

    glGenBuffers(1, &result->ShaderConfigUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, result->ShaderConfigUBO);
    static_assert((sizeof(ShaderConfig) & 15) == 0);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ShaderConfig), &result->ShaderConfig, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, result->ShaderConfigUBO);


    float clearvtx[6*2] =
    {
        -1.0, -1.0,
        1.0, 1.0,
        -1.0, 1.0,

        -1.0, -1.0,
        1.0, -1.0,
        1.0, 1.0
    };

    glGenBuffers(1, &result->ClearVertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->ClearVertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(clearvtx), clearvtx, GL_STATIC_DRAW);

    glGenVertexArrays(1, &result->ClearVertexArrayID);
    glBindVertexArray(result->ClearVertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)(0));


    glGenBuffers(1, &result->VertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->VertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VertexBuffer), nullptr, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &result->VertexArrayID);
    glBindVertexArray(result->VertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_SHORT, 7*4, (void*)(0));
    glEnableVertexAttribArray(1); // color
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, 7*4, (void*)(2*4));
    glEnableVertexAttribArray(2); // texcoords
    glVertexAttribIPointer(2, 2, GL_SHORT, 7*4, (void*)(3*4));
    glEnableVertexAttribArray(3); // attrib
    glVertexAttribIPointer(3, 3, GL_UNSIGNED_INT, 7*4, (void*)(4*4));

    // GPU::GLEdgeMark 1: where a vertex's polygon's outline is in EdgeTex, its own buffer, only
    // written in edge-marking frames and only read by their programs (the others don't have it)
    glGenBuffers(1, &result->EdgeInfoBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->EdgeInfoBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(EdgeInfoBuffer), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(4); // edge info
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, 4, (void*)(0));

    // GPU::GLEdgeFast bit 1: up to 4 edges of a vertex's polygon, the same way, only written in the
    // frames that use them and only read by their programs
    glGenBuffers(1, &result->EdgeLineBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->EdgeLineBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(EdgeLineBuffer), nullptr, GL_DYNAMIC_DRAW);
    for (int i = 0; i < 3; i++)
    {
        glEnableVertexAttribArray(5 + i); // edge lines
        glVertexAttribPointer(5 + i, 4, GL_FLOAT, GL_FALSE, 12*4, (void*)(uintptr_t)(i*16));
    }

    glGenBuffers(1, &result->IndexBufferID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, result->IndexBufferID);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(IndexBuffer), nullptr, GL_DYNAMIC_DRAW);

    glGenFramebuffers(1, &result->MainFramebuffer);
    glGenFramebuffers(1, &result->DownscaleFramebuffer);

    // color buffers
    glGenTextures(1, &result->ColorBufferTex);
    SetupDefaultTexParams(result->ColorBufferTex);

    // depth/stencil buffer
    glGenTextures(1, &result->DepthBufferTex);
    SetupDefaultTexParams(result->DepthBufferTex);

    // attribute buffer
    // R: opaque polyID (for edgemarking)
    // G: edge flag
    // B: fog flag
    glGenTextures(1, &result->AttrBufferTex);
    SetupDefaultTexParams(result->AttrBufferTex);

    // downscale framebuffer for display capture (always 256x192)
    glGenTextures(1, &result->DownScaleBufferTex);
    SetupDefaultTexParams(result->DownScaleBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);

    glGenBuffers(1, &result->PixelbufferID);
    glGenBuffers(1, &result->EarlyPixelbufferID);

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &result->TexMemID);
    glBindTexture(GL_TEXTURE_2D, result->TexMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);

    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &result->TexPalMemID);
    glBindTexture(GL_TEXTURE_2D, result->TexPalMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 48, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // GPU::GLTransSkip bit 2 (T1c, docs/plan-gpu-discovery.md): whether the shader can read the
    // color target, coherently. one name at a time with glGetStringi, which GLES 3 and a desktop core
    // profile both have (glGetString's list is GL_INVALID_ENUM on the latter), compared whole, since
    // the EXT one is a prefix of its non-coherent sibling. only the EXT one is used: the ARM one reads
    // color only and has its own rules with more than one color target
    {
        GLint numExtensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
        auto hasExtension = [numExtensions](const char* name)
        {
            for (GLint e = 0; e < numExtensions; e++)
            {
                const char* extension = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)e);
                if (extension && strcmp(extension, name) == 0)
                    return true;
            }
            return false;
        };
        result->HasFramebufferFetch = hasExtension("GL_EXT_shader_framebuffer_fetch");
#ifdef MELONADE_DEBUG_TOOLS
        Platform::Log(Platform::LogLevel::Info, "GLTRANS: framebuffer fetch GL_EXT_shader_framebuffer_fetch=%d "
            "GL_ARM_shader_framebuffer_fetch=%d GL_EXT_shader_framebuffer_fetch_non_coherent=%d\n",
            result->HasFramebufferFetch ? 1 : 0,
            hasExtension("GL_ARM_shader_framebuffer_fetch") ? 1 : 0,
            hasExtension("GL_EXT_shader_framebuffer_fetch_non_coherent") ? 1 : 0);
#endif
    }

    return result;
}

GLRenderer::~GLRenderer()
{
    assert(glDeleteTextures != nullptr);

    glDeleteTextures(1, &TexMemID);
    glDeleteTextures(1, &TexPalMemID);
    DeleteTexCache();

    glDeleteFramebuffers(1, &MainFramebuffer);
    glDeleteFramebuffers(1, &DownscaleFramebuffer);
    glDeleteTextures(1, &ColorBufferTex);
    glDeleteTextures(1, &KeptColorTex);
    glDeleteTextures(1, &SpareColorTex);
    glDeleteTextures(1, &DepthBufferTex);
    glDeleteTextures(1, &AttrBufferTex);
    glDeleteTextures(1, &DownScaleBufferTex);
    glDeleteBuffers(1, &PixelbufferID);
    glDeleteBuffers(1, &EarlyPixelbufferID);

    glDeleteVertexArrays(1, &VertexArrayID);
    glDeleteBuffers(1, &VertexBufferID);
    glDeleteBuffers(1, &IndexBufferID);
    glDeleteBuffers(1, &EdgeInfoBufferID);
    glDeleteBuffers(1, &EdgeLineBufferID);
    glDeleteTextures(1, &EdgeTexID);
    glDeleteVertexArrays(1, &ClearVertexArrayID);
    glDeleteBuffers(1, &ClearVertexBufferID);

    glDeleteBuffers(1, &ShaderConfigUBO);

    for (int i = 0; i < NumRenderShaders; i++)
    {
        if (!RenderShader[i]) continue;
        glDeleteProgram(RenderShader[i]);
    }
    glDeleteProgram(ClearShaderPlain);
    glDeleteProgram(ClearShaderBitmap);
    glDeleteProgram(FinalPassEdgeShader);
    glDeleteProgram(FinalPassFogShader);

    if (ShadowLogQueries[0]) glDeleteQueries(64, ShadowLogQueries);
    DropPaceFences();

#ifdef MELONADE_DEBUG_TOOLS
    // their GL objects, while the context is still current
    glDeleteProgram(CoverShader);
    glDeleteProgram(CoverAlphaShader);
    Timers.reset();
    FragCounter.reset();
    Hash3D.reset();
#endif
}

void GLRenderer::Reset(GPU& gpu)
{
    // held WideMelon wings and hi-res capture copies belong to the old scene, and so does the
    // filtered 2D map
    CurGLCompositor.ResetWingHold();
    CurGLCompositor.ResetHiresCapture();
    CurGLCompositor.ResetFilter2D();
    CurGLCompositor.ResetShadow2D();
    TexMemDirtyAll = true;
    // TexMemDirtyAll drops them at the next frame anyway (every chunk counts as changed)
    ClearOpaqueTex();
    // GPU::GLTexCache: the same, the heap would only rehash everything at its next use
    if (TexHeap)
        TexHeap->Clear();
    TargetsChanged = true;
    EarlyCapture = false;
    // pacing starts over with the new console
    DropPaceFences();
}

void GLRenderer::SetBetterPolygons(bool betterpolygons) noexcept
{
    SetRenderSettings(betterpolygons, ScaleFactor);
}

void GLRenderer::SetScaleFactor(int scale) noexcept
{
    SetRenderSettings(BetterPolygons, scale);
}


bool GLRenderer::TargetFits(int width, int scale) const noexcept
{
    return width == WideMelon::NativeWidth || MaxTargetWidth <= 0 || width * scale <= MaxTargetWidth;
}

// with a margin the output is (384+2+2 x margin) x scale tall, which has to fit too. the compositor's
// output can be at a lower scale than this (GPU::OutputCap), so the 3D's is the one checked
bool GLRenderer::MarginFits(int margin, int scale) const noexcept
{
    return margin == 0 || MaxTargetHeight <= 0 || (384 + 2 + 2 * margin) * scale <= MaxTargetHeight;
}

// the native targets (256 x 192 at the scale) and the compositor's output, and the frontend's
// textures made like it, (384+2) x scale tall, have to fit in a texture and a viewport. at 16x
// that's 4096 x 6176, past a GPU whose limit is 4096. a limit that wasn't read counts as no limit
bool GLRenderer::ScaleFits(int scale) const noexcept
{
    return (MaxTargetWidth <= 0 || WideMelon::NativeWidth * scale <= MaxTargetWidth) &&
           (MaxTargetHeight <= 0 || (384 + 2) * scale <= MaxTargetHeight);
}

// the largest scale up to the one asked for that fits the GPU's limits, logged when it's smaller
int GLRenderer::FitScale(int scale) const noexcept
{
    int fit = std::max(scale, 1);
    while (fit > 1 && !ScaleFits(fit))
        fit--;
    if (fit < scale)
        Platform::Log(Platform::LogLevel::Warn, "OpenGL renderer: %dx doesn't fit the GPU's %dx%d texture and viewport limit, using %dx\n",
            scale, MaxTargetWidth, MaxTargetHeight, fit);
    return fit;
}

int GLRenderer::ResolveViewWidth(int width, int scale) noexcept
{
    if (width == WideMelon::NativeWidth)
        return width;

    if (!TargetFits(width, scale))
    {
        Platform::Log(Platform::LogLevel::Warn, "WideMelon: %dx%d does not fit in %d, using native width\n",
            width, scale, MaxTargetWidth);
        return WideMelon::NativeWidth;
    }

    if (!CurGLCompositor.PrepareWide())
        return WideMelon::NativeWidth;

    return width;
}

int GLRenderer::ResolveViewMargin(int margin, int width, int scale) noexcept
{
    margin = WideMelon::SanitizeMargin(margin, width);
    if (!MarginFits(margin, scale))
    {
        Platform::Log(Platform::LogLevel::Warn, "WideMelon: a margin of %d at %dx does not fit in %d rows, using none\n",
            margin, scale, MaxTargetHeight);
        return 0;
    }
    return margin;
}

void GLRenderer::SetViewWidth(int width) noexcept
{
    RequestedViewWidth = WideMelon::SanitizeWidth(width);

    // the scale decides whether it fits, the first SetRenderSettings resolves it otherwise.
    // the targets follow in RenderFrame. the margin goes with the width
    if (ScaleFactor > 0)
    {
        ViewWidth = ResolveViewWidth(RequestedViewWidth, ScaleFactor);
        ViewMargin = ResolveViewMargin(RequestedViewMargin, ViewWidth, ScaleFactor);
    }
}

void GLRenderer::SetViewMargin(int margin) noexcept
{
    RequestedViewMargin = std::clamp(margin, 0, WideMelon::MaxMargin);
    if (ScaleFactor > 0)
        ViewMargin = ResolveViewMargin(RequestedViewMargin, ViewWidth, ScaleFactor);
}

void GLRenderer::SetRenderSettings(bool betterpolygons, int scale) noexcept
{
    // GetScaleFactor is the scale that's used, which the frontend sizes its textures by
    scale = FitScale(scale);
    ViewWidth = ResolveViewWidth(RequestedViewWidth, scale);
    ViewMargin = ResolveViewMargin(RequestedViewMargin, ViewWidth, scale);

    int targetWidth = TargetWidth;
    if (!TargetFits(targetWidth, scale))
        targetWidth = ViewWidth;
    int targetMargin = WideMelon::SanitizeMargin(TargetMargin, targetWidth);
    if (!MarginFits(targetMargin, scale))
        targetMargin = WideMelon::SanitizeMargin(ViewMargin, targetWidth);

    ResizeTargets(betterpolygons, scale, targetWidth, targetMargin);
}

// GPU::OutputCap: the targets stay as they are, only the compositor's output follows the need
void GLRenderer::SetOutputNeed(const GPU& gpu, int frontendNeed) noexcept
{
    // first, so a cap that starts here builds GPU::OutputCap3D's programs (and the capture pass's) along with the exact one
    CurGLCompositor.SetOutputCap3D(gpu);

    int need = CurGLCompositor.OutputNeed(gpu, frontendNeed);
    bool anyRatio = GLCompositor::OutputAnyRatio(gpu);
    if (need != OutputNeed || anyRatio != OutputAnyRatio)
    {
        OutputNeed = need;
        OutputAnyRatio = anyRatio;
        // before the targets' first setup, that one caps it
        if (ScaleFactor > 0)
            CurGLCompositor.SetRenderSettings(GLCompositor::CappedScale(ScaleFactor, need, anyRatio), ScaleFactor, TargetWidth, TargetMargin);
    }

    // debug: a line when the cap's outcome or its reason changes
    CurGLCompositor.LogOutputCap(gpu, frontendNeed);
}

// true when anything changed
bool GLRenderer::ResizeTargets(bool betterpolygons, int scale, int targetWidth, int targetMargin) noexcept
{
    if (betterpolygons == BetterPolygons && scale == ScaleFactor && targetWidth == TargetWidth && targetMargin == TargetMargin)
        return false;

    CurGLCompositor.SetRenderSettings(GLCompositor::CappedScale(scale, OutputNeed, OutputAnyRatio), scale, targetWidth, targetMargin);
    ScaleFactor = scale;
    TargetWidth = targetWidth;
    TargetMargin = targetMargin;
    BetterPolygons = betterpolygons;
    TargetsChanged = true;
    // the pixel buffers are reallocated below
    EarlyCapture = false;

    ScreenW = targetWidth * scale;
    ScreenH = (192 + 2 * targetMargin) * scale;

    // the other color buffers are at the old size. nothing is kept here: a kept frame only lives
    // from the render at line 215 to the Blit right after it, and KeepFrameForBlit refuses a render
    // that resizes
    glDeleteTextures(1, &SpareColorTex);
    glDeleteTextures(1, &KeptColorTex);
    SpareColorTex = KeptColorTex = 0;

    glBindTexture(GL_TEXTURE_2D, ColorBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, DepthBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, ScreenW, ScreenH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glBindTexture(GL_TEXTURE_2D, AttrBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, ScreenW, ScreenH, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, DownscaleFramebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, DownScaleBufferTex, 0);

    GLenum fbassign[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

    glBindFramebuffer(GL_FRAMEBUFFER, MainFramebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ColorBufferTex, 0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, DepthBufferTex, 0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, AttrBufferTex, 0);
    glDrawBuffers(2, fbassign);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
    glBufferData(GL_PIXEL_PACK_BUFFER, 256*192*4, NULL, GL_DYNAMIC_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, EarlyPixelbufferID);
    glBufferData(GL_PIXEL_PACK_BUFFER, 256*192*4, NULL, GL_DYNAMIC_READ);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    //glLineWidth(scale);
    //glLineWidth(1.5);
    return true;
}

// the width RenderFrame sizes the targets for: the polygons match them, so after a width change
// they only shrink once the game has built geometry at the new width
int GLRenderer::FrameTargetWidth(const GPU& gpu) noexcept
{
    int targetWidth = std::max(ViewWidth, gpu.GPU3D.RenderViewWidth);
    if (!TargetFits(targetWidth, ScaleFactor) ||
        (targetWidth != WideMelon::NativeWidth && !CurGLCompositor.PrepareWide()))
        targetWidth = ViewWidth;
    return targetWidth;
}

// and the margin, the same way: the larger of the view's and the polygons', on a wide target
int GLRenderer::FrameTargetMargin(const GPU& gpu, int targetWidth) const noexcept
{
    if (targetWidth == WideMelon::NativeWidth)
        return 0;
    int viewMargin = gpu.WideY ? ViewMargin : 0;
    int targetMargin = std::max(viewMargin, gpu.GPU3D.RenderViewMargin);
    if (!MarginFits(targetMargin, ScaleFactor))
        targetMargin = viewMargin;
    return targetMargin;
}

// GPU::BlitAfterReadback, at line 215: the compositor's size follows the targets, so a render that
// resizes them gets the Blit first. otherwise the render draws into the spare color buffer and the
// one it replaces stays for the Blit (see KeepColorBuffer). a render that's skipped keeps nothing,
// and the Blit reads the color buffer as it was
bool GLRenderer::KeepFrameForBlit(const GPU& gpu)
{
    int targetWidth = FrameTargetWidth(gpu);
    if (targetWidth != TargetWidth || FrameTargetMargin(gpu, targetWidth) != TargetMargin)
        return false;

    KeepRequested = true;
    return true;
}

// the render about to start draws into the spare color buffer, and the frame it replaces is kept
// for the Blit. same size and format, so MainFramebuffer stays complete
void GLRenderer::KeepColorBuffer() noexcept
{
    if (!SpareColorTex)
    {
        glGenTextures(1, &SpareColorTex);
        SetupDefaultTexParams(SpareColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    KeptColorTex = ColorBufferTex;
    ColorBufferTex = SpareColorTex;
    SpareColorTex = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, MainFramebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ColorBufferTex, 0);
}


void GLRenderer::SetupPolygon(GLRenderer::RendererPolygon* rp, Polygon* polygon) const
{
    rp->PolyData = polygon;
    rp->TexWord = polygon->TexParam & 0xFFFF;

    // render key: depending on what we're drawing
    // opaque polygons:
    // - depthfunc
    // -- alpha=0
    // regular translucent polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID
    // ---- need opaque
    // shadow mask polygons:
    // - depthfunc?????
    // shadow polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID

    rp->RenderKey = (polygon->Attr >> 14) & 0x1; // bit14 - depth func
    if (!polygon->IsShadowMask)
    {
        if (polygon->Translucent)
        {
            if (polygon->IsShadow) rp->RenderKey |= 0x20000;
            else                   rp->RenderKey |= 0x10000;
            rp->RenderKey |= (polygon->Attr >> 10) & 0x2; // bit11 - depth write
            rp->RenderKey |= (polygon->Attr >> 13) & 0x4; // bit15 - fog
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16; // polygon ID
            if ((polygon->Attr & 0x001F0000) == 0x001F0000) // need opaque
                rp->RenderKey |= 0x4000;
        }
        else
        {
            if ((polygon->Attr & 0x001F0000) == 0)
                rp->RenderKey |= 0x2;
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16; // polygon ID
        }
    }
    else
    {
        rp->RenderKey |= 0x30000;
    }
}

u32* GLRenderer::SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32 texword, u32* vptr) const
{
    u32 z = poly->FinalZ[vid];
    u32 w = poly->FinalW[vid];

    u32 alpha = (poly->Attr >> 16) & 0x1F;

    // Z should always fit within 16 bits, so it's okay to do this
    u32 zshift = 0;
    while (z > 0xFFFF) { z >>= 1; zshift++; }

    u32 x, y;
    if (ScaleFactor > 1)
    {
        x = (vtx->HiresPosition[0] * ScaleFactor) >> 4;
        y = (vtx->HiresPosition[1] * ScaleFactor) >> 4;
    }
    else
    {
        x = vtx->FinalPosition[0];
        y = vtx->FinalPosition[1];
    }

    // correct nearly-vertical edges that would look vertical on the DS
    /*{
        int vtopid = vid - 1;
        if (vtopid < 0) vtopid = poly->NumVertices-1;
        Vertex* vtop = poly->Vertices[vtopid];
        if (vtop->FinalPosition[1] >= vtx->FinalPosition[1])
        {
            vtopid = vid + 1;
            if (vtopid >= poly->NumVertices) vtopid = 0;
            vtop = poly->Vertices[vtopid];
        }
        if ((vtop->FinalPosition[1] < vtx->FinalPosition[1]) &&
            (vtx->FinalPosition[0] == vtop->FinalPosition[0]-1))
        {
            if (ScaleFactor > 1)
                x = (vtop->HiresPosition[0] * ScaleFactor) >> 4;
            else
                x = vtop->FinalPosition[0];
        }
    }*/

    if (WideFrame)
        x = WideVertexX(vtx->HiresPosition[0]);
    if (WideFrameY)
        y = WideVertexY(vtx->HiresPosition[1]);

    *vptr++ = x | (y << 16);
    *vptr++ = z | (w << 16);

    *vptr++ =  (vtx->FinalColor[0] >> 1) |
              ((vtx->FinalColor[1] >> 1) << 8) |
              ((vtx->FinalColor[2] >> 1) << 16) |
              (alpha << 24);

    *vptr++ = (u16)vtx->TexCoords[0] | ((u16)vtx->TexCoords[1] << 16);

    // Split TexParam into 2 because some GPUs don't have 32 bit ints. TexPalette only uses 13 bits
    // (texword: TexParam & 0xFFFF, or the heap offset with GPU::GLTexCache, RendererPolygon::TexWord)
    *vptr++ = vtxattr | (zshift << 16);
    *vptr++ = texword;
    *vptr++ = (poly->TexParam >> 16 ) | (poly->TexPalette << 16);

    return vptr;
}

// wide views always go through the hi-res position, which GPU3D already placed on the wide target
u32 GLRenderer::WideVertexX(s32 hiresx) const
{
    return (std::max(hiresx + WideXAdjust, 0) * ScaleFactor) >> 4;
}

// and Y with a margin (camera stage 2), on the target's rows, which start TargetMargin rows above DS line 0
u32 GLRenderer::WideVertexY(s32 hiresy) const
{
    return (std::max(hiresy + WideYAdjust, 0) * ScaleFactor) >> 4;
}

// the target row a polygon line [line] (0..192, of the frame its polygons were built for, squeezed with
// a margin) starts at: the squeeze maps the 192 lines over the whole view, 192 + 2 x FrameMargin rows,
// exactly for a full-screen viewport, and the view sits TargetMargin - FrameMargin rows down the
// target. clamped to it
int GLRenderer::TargetRow(int line) const noexcept
{
    if (!WideFrameY)
        return line * ScaleFactor;
    // in the target's rows at the scale, as a line's edge lands between native rows when stretched
    int row = line * (192 + 2 * FrameMargin) * ScaleFactor / 192 + (TargetMargin - FrameMargin) * ScaleFactor;
    return std::clamp(row, 0, ScreenH);
}

void GLRenderer::BuildPolygons(GLRenderer::RendererPolygon* polygons, int npolys)
{
    u32* vptr = &VertexBuffer[0];
    u32 vidx = 0;

    u32 iidx = 0;
    NumEdges = 0;

    for (int i = 0; i < npolys; i++)
    {
        RendererPolygon* rp = &polygons[i];
        Polygon* poly = rp->PolyData;

        rp->IndicesOffset = iidx;
        rp->NumIndices = 0;

        u32 vidx_first = vidx;

        u32 polyattr = poly->Attr;

        u32 alpha = (polyattr >> 16) & 0x1F;

        u32 vtxattr = polyattr & 0x1F00C8F0;
        if (poly->FacingView) vtxattr |= (1<<8);
        if (poly->WBuffer)    vtxattr |= (1<<9);

        // assemble vertices
        if (poly->Type == 1) // line
        {
            rp->PrimType = GL_LINES;

            u32 lastx, lasty;
            int nout = 0;
            for (u32 j = 0; j < poly->NumVertices; j++)
            {
                Vertex* vtx = poly->Vertices[j];

                if (j > 0)
                {
                    if (lastx == vtx->FinalPosition[0] &&
                        lasty == vtx->FinalPosition[1]) continue;
                }

                lastx = vtx->FinalPosition[0];
                lasty = vtx->FinalPosition[1];

                vptr = SetupVertex(poly, j, vtx, vtxattr, rp->TexWord, vptr);

                IndexBuffer[iidx++] = vidx;
                rp->NumIndices++;

                vidx++;
                nout++;
                if (nout >= 2) break;
            }
        }
        else if (poly->NumVertices == 3) // regular triangle
        {
            rp->PrimType = GL_TRIANGLES;

            for (int j = 0; j < 3; j++)
            {
                Vertex* vtx = poly->Vertices[j];

                vptr = SetupVertex(poly, j, vtx, vtxattr, rp->TexWord, vptr);
                vidx++;
            }

            // build a triangle
            IndexBuffer[iidx++] = vidx_first;
            IndexBuffer[iidx++] = vidx - 2;
            IndexBuffer[iidx++] = vidx - 1;
            rp->NumIndices += 3;
        }
        else // quad, pentagon, etc
        {
            rp->PrimType = GL_TRIANGLES;

            if (!BetterPolygons)
            {
                // regular triangle-splitting

                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    vptr = SetupVertex(poly, j, vtx, vtxattr, rp->TexWord, vptr);

                    if (j >= 2)
                    {
                        // build a triangle
                        IndexBuffer[iidx++] = vidx_first;
                        IndexBuffer[iidx++] = vidx - 1;
                        IndexBuffer[iidx++] = vidx;
                        rp->NumIndices += 3;
                    }

                    vidx++;
                }
            }
            else
            {
                // attempt at 'better' splitting
                // this doesn't get rid of the error while splitting a bigger polygon into triangles
                // but we can attempt to reduce it

                u32 cX = 0, cY = 0;
                float cZ = 0;
                float cW = 0;

                float cR = 0, cG = 0, cB = 0;
                float cS = 0, cT = 0;

                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    cX += vtx->HiresPosition[0];
                    cY += vtx->HiresPosition[1];

                    float fw = (float)poly->FinalW[j] * poly->NumVertices;
                    cW += 1.0f / fw;

                    if (poly->WBuffer) cZ += poly->FinalZ[j] / fw;
                    else               cZ += poly->FinalZ[j];

                    cR += (vtx->FinalColor[0] >> 1) / fw;
                    cG += (vtx->FinalColor[1] >> 1) / fw;
                    cB += (vtx->FinalColor[2] >> 1) / fw;

                    cS += vtx->TexCoords[0] / fw;
                    cT += vtx->TexCoords[1] / fw;
                }

                cX /= poly->NumVertices;
                cY /= poly->NumVertices;

                cW = 1.0f / cW;

                if (poly->WBuffer) cZ *= cW;
                else               cZ /= poly->NumVertices;

                cR *= cW;
                cG *= cW;
                cB *= cW;

                cS *= cW;
                cT *= cW;

                if (WideFrame)
                    cX = WideVertexX((s32)cX);
                else
                    cX = (cX * ScaleFactor) >> 4;
                if (WideFrameY)
                    cY = WideVertexY((s32)cY);
                else
                    cY = (cY * ScaleFactor) >> 4;

                u32 w = (u32)cW;

                u32 z = (u32)cZ;
                u32 zshift = 0;
                while (z > 0xFFFF) { z >>= 1; zshift++; }

                // build center vertex
                *vptr++ = cX | (cY << 16);
                *vptr++ = z | (w << 16);

                *vptr++ =  (u32)cR |
                          ((u32)cG << 8) |
                          ((u32)cB << 16) |
                          (alpha << 24);

                *vptr++ = (u16)cS | ((u16)cT << 16);

                // Split TexParam into 2 because some GPUs don't have 32 bit ints. TexPalette only uses 13 bits
                *vptr++ = vtxattr | (zshift << 16);
                *vptr++ = rp->TexWord;
                *vptr++ = (poly->TexParam >> 16 ) | (poly->TexPalette << 16);

                vidx++;

                // build the final polygon
                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    vptr = SetupVertex(poly, j, vtx, vtxattr, rp->TexWord, vptr);

                    if (j >= 1)
                    {
                        // build a triangle
                        IndexBuffer[iidx++] = vidx_first;
                        IndexBuffer[iidx++] = vidx - 1;
                        IndexBuffer[iidx++] = vidx;
                        rp->NumIndices += 3;
                    }

                    vidx++;
                }

                IndexBuffer[iidx++] = vidx_first;
                IndexBuffer[iidx++] = vidx - 1;
                IndexBuffer[iidx++] = vidx_first + 1;
                rp->NumIndices += 3;
            }
        }

        if (EdgeMark != 0)
            BuildEdges(rp, vidx_first, vidx);
    }

    NumVertices = vidx;
    NumIndices = iidx;
}

// GPU::GLEdgeMark 1 (E1): the outline of the polygon whose vertices BuildPolygons just wrote
// (VertexBuffer[vidxFirst] up to vidxEnd), for kRenderFS's EdgeFlag. the rim's edges only, in the
// target pixels the vertices were placed at, so a fan's inner diagonals and BetterPolygons' centre
// vertex don't count. a fragment is flagged when one of its four neighbors one DS pixel away (the
// edge pass's offset, ScaleFactor target pixels) is outside an edge's half plane, which for a
// convex polygon is outside the polygon: the hi-res form of the software renderer's flags on the
// span ends and the top and bottom rows. only polygons the opaque draws use; lines and polygons
// without area flag all their fragments (count 0), and so does every polygon with GPU::GLEdgeMark 2
void GLRenderer::BuildEdges(const RendererPolygon* rp, u32 vidxFirst, u32 vidxEnd)
{
    const Polygon* poly = rp->PolyData;
    u32 info = 0;

    bool opaqueDraw = !poly->IsShadowMask
        && (!poly->Translucent || (poly->Attr & 0x001F0000) == 0x001F0000);
    // GPU::GLEdgeFast bit 0: where the edge pass can find flags, around the vertices of what's
    // drawn opaque (one pixel more, for lines)
    if (opaqueDraw && (EdgeFast & 1))
    {
        for (u32 v = vidxFirst; v < vidxEnd; v++)
        {
            u32 pos = VertexBuffer[v * 7];
            s32 x = pos & 0xFFFF, y = pos >> 16;
            OpaqueBox[0] = std::min(OpaqueBox[0], x - 1);
            OpaqueBox[1] = std::min(OpaqueBox[1], y - 1);
            OpaqueBox[2] = std::max(OpaqueBox[2], x + 1);
            OpaqueBox[3] = std::max(OpaqueBox[3], y + 1);
        }
    }
    // GPU::GLEdgeMark 2: every opaque fragment is flagged
    if (EdgeMark == 1 && opaqueDraw && poly->Type != 1 && NumEdges + poly->NumVertices <= MaxEdges)
    {
        u32 n = poly->NumVertices;
        // BetterPolygons' centre vertex comes first, before the rim
        u32 rim = vidxEnd - n;
        s32 x[10], y[10];
        for (u32 j = 0; j < n; j++)
        {
            u32 pos = VertexBuffer[(rim + j) * 7];
            x[j] = pos & 0xFFFF;
            y[j] = pos >> 16;
        }

        s64 area = 0;
        for (u32 j = 0; j < n; j++)
        {
            u32 k = (j + 1 == n) ? 0 : j + 1;
            area += (s64)x[j] * y[k] - (s64)x[k] * y[j];
        }

        if (area != 0)
        {
            double sign = area > 0 ? 1.0 : -1.0;
            u32 first = NumEdges;
            // GPU::GLEdgeFast bit 1: polygons with up to 4 edges keep them out of EdgeTex, and each
            // of their vertices carries them (EdgeLineBuffer). the same floats as EdgeTex would get
            bool lines = EdgeSet == EdgeLineShader;
            float line[12] {};
            u32 numLines = 0;
            for (u32 j = 0; j < n; j++)
            {
                u32 k = (j + 1 == n) ? 0 : j + 1;
                double dx = x[k] - x[j], dy = y[k] - y[j];
                double len = std::sqrt(dx * dx + dy * dy);
                if (len == 0.0) continue;

                // the inward normal: left of each edge when the area is positive
                double nx = -dy * sign / len, ny = dx * sign / len;
                double reach = ScaleFactor * std::max(std::abs(nx), std::abs(ny)) + 1.0 / 64;
                float e0 = (float)nx, e1 = (float)ny, e2 = (float)(-(nx * x[j] + ny * y[j]) - reach);
                if (lines && numLines < 4)
                {
                    line[numLines * 3 + 0] = e0;
                    line[numLines * 3 + 1] = e1;
                    line[numLines * 3 + 2] = e2;
                }
                numLines++;
                float* e = &EdgeData[NumEdges * 4];
                e[0] = e0;
                e[1] = e1;
                e[2] = e2;
                e[3] = 0.0f;
                NumEdges++;
            }
            if (lines && numLines > 0 && numLines <= 4)
            {
                // not in EdgeTex after all
                NumEdges = first;
                info = numLines << 16;
                for (u32 v = vidxFirst; v < vidxEnd; v++)
                    memcpy(&EdgeLineBuffer[v * 12], line, sizeof(line));
            }
            else if (NumEdges > first)
                info = first | ((NumEdges - first) << 16);
        }
    }

    for (u32 v = vidxFirst; v < vidxEnd; v++)
        EdgeInfoBuffer[v] = info;
}

int GLRenderer::RenderSinglePolygon(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];

    glDrawElements(rp->PrimType, rp->NumIndices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->IndicesOffset * 2));

    return 1;
}

int GLRenderer::RenderPolygonBatch(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];
    GLuint primtype = rp->PrimType;
    u32 key = rp->RenderKey;
    int numpolys = 0;
    u32 numindices = 0;

    for (int iend = i; iend < NumFinalPolys; iend++)
    {
        const RendererPolygon* cur_rp = &PolygonList[iend];
        if (cur_rp->PrimType != primtype) break;
        if (cur_rp->RenderKey != key) break;

        numpolys++;
        numindices += cur_rp->NumIndices;
    }

    glDrawElements(primtype, numindices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->IndicesOffset * 2));
    return numpolys;
}

// the lines the software renderer draws a polygon on (see SoftRenderer::RenderScanline)
static void GetPolygonLines(const Polygon* polygon, int& ytop, int& ybottom)
{
    ytop = std::max(polygon->YTop, 0);
    ybottom = std::min((polygon->YBottom == polygon->YTop) ? polygon->YTop + 1 : polygon->YBottom, 192);
}

void GLRenderer::ClearShadowMaskLines(const Polygon* polygon, const bool* lastWasMask) const
{
    // clear the stencil buffer (with the caller's stencil mask) on the polygon's lines where
    // the last polygon wasn't a mask. DS line y is GL row y here, the target isn't flipped.
    // scissor is off for the whole 3D frame (RenderFrame turns it off), so it's just turned
    // back off after, without querying it (glGet can stall threaded drivers)
    int ytop, ybottom;
    GetPolygonLines(polygon, ytop, ybottom);

    bool scissorOn = false;

    for (int y = ytop; y < ybottom; )
    {
        if (lastWasMask[y]) { y++; continue; }

        int yend = y + 1;
        while (yend < ybottom && !lastWasMask[yend]) yend++;

        if (!scissorOn)
        {
            glEnable(GL_SCISSOR_TEST);
            scissorOn = true;
        }

        // with a margin the lines are the squeezed frame's, stretched over the target (TargetRow)
        int row0 = TargetRow(y), row1 = TargetRow(yend);
        glScissor(0, row0, ScreenW, row1 - row0);
        glClear(GL_STENCIL_BUFFER_BIT);

        y = yend;
    }

    if (scissorOn) glDisable(GL_SCISSOR_TEST);
}

void GLRenderer::SetDepthBias(float bias)
{
    // for the program in use (UseRenderShader). the opaque, translucent and shadow mask passes
    // share it, so every draw sets its bias instead of relying on a resting value of 0
    if (DepthBias[CurShaderID] == bias) return;
    glUniform1f(DepthBiasLoc[CurShaderID], bias);
    DepthBias[CurShaderID] = bias;
}

void GLRenderer::SetPolygonDepthBias(u32 polyattr)
{
    // depth-equal polygons (attr bit 14) are drawn with GL_LEQUAL, but the DS test passes within a
    // margin (+-0x200 Z-buffering, +-0xFF W-buffering), so move them forward by it. that covers
    // the far side, the near side stays open. depth writes store the moved depth, slightly nearer
    // than the DS would. set right before every draw, for the shader in use
    float bias = 0.0f;
    if (EqualDepthMargin && (polyattr & (1<<14)))
        bias = ((CurShaderID % FullShader) & 1) ? -255.0f : -512.0f; // bit 0 of the index is W-buffering, twins too

    SetDepthBias(bias);
}

// GPU::GLTransSkip bit 1: the AlphaClass_* of each byte of an A3I5 ([0]) or A5I3 ([1]) texel, with
// the alpha the shader gets from it (TextureFetch_A3I5, TextureFetch_A5I3): 31 passes the opaque
// draw's discard, 1-30 the translucent draw's, 0 neither
struct TexelAlphaTables
{
    u8 Class[2][256];

    TexelAlphaTables()
    {
        for (int b = 0; b < 256; b++)
        {
            int a3i5 = ((b & 0xE0) >> 3) + ((b & 0xE0) >> 6);
            int a5i3 = (b & 0xF8) >> 3;
            int alpha[2] = { a3i5, a5i3 };
            for (int t = 0; t < 2; t++)
            {
                if (alpha[t] == 31)    Class[t][b] = 1; // AlphaClass_Full
                else if (alpha[t] > 0) Class[t][b] = 2; // AlphaClass_Partial
                else                   Class[t][b] = 0;
            }
        }
    }
};
static const TexelAlphaTables kTexelAlpha;

// the classes of a need-opaque polygon's texture: every texel of its width x height, which is every
// texel any wrap or flip mode can reach (TexcoordWrap keeps s and t inside), at the address the shader
// reads it from (TexParam's low 16 bits x 8 plus the texel index, one byte a texel) in the same copy
// TexMem is uploaded from. a texture reaching past TexMem's 512 KB has texelFetch read out of range,
// which GL leaves undefined, so it gets both classes. once a frame per texture (TexAlphaKey)
u8 GLRenderer::TextureAlphaClass(const u8* vram, u32 texparam)
{
    u32 key = texparam & 0x1FF0FFFF; // address, size, format: wrap, flip and color 0 don't change it
    u32 slot = (key * 2654435761u) >> 20; // TexAlphaSlots = 4096, more than polygons, so never full
    while (TexAlphaStamp[slot] == TexAlphaFrame)
    {
        if (TexAlphaKey[slot] == key)
            return TexAlphaValue[slot];
        slot = (slot + 1) & (TexAlphaSlots - 1);
    }

    u32 format = (texparam >> 26) & 0x7;
    u32 addr = (texparam & 0xFFFF) << 3;
    u32 texels = (8u << ((texparam >> 20) & 0x7)) * (8u << ((texparam >> 23) & 0x7));

    u8 value = AlphaClass_Full | AlphaClass_Partial;
    u32 scanned = 0; // bytes read, for GLTRANS
    if ((format == 1 || format == 6) && addr + texels <= sizeof(GPU::VRAMFlat_Texture))
    {
        const u8* table = kTexelAlpha.Class[format == 6];
        const u8* p = vram + addr;
        value = 0;
        // in blocks, so it can stop once both are found
        for (u32 i = 0; i < texels && value != (AlphaClass_Full | AlphaClass_Partial); i += 256)
        {
            u32 end = std::min(i + 256, texels);
            u8 acc = 0;
            for (u32 j = i; j < end; j++)
                acc |= table[p[j]];
            value |= acc;
            scanned = end;
        }
    }

#ifdef MELONADE_DEBUG_TOOLS
    if (TransStats)
    {
        TransStats->Sec.Textures += scanned > 0;
        TransStats->Sec.Texels += scanned;
    }
#else
    (void)scanned;
#endif

    TexAlphaStamp[slot] = TexAlphaFrame;
    TexAlphaKey[slot] = key;
    TexAlphaValue[slot] = value;
    return value;
}

// GPU::GLTransSkip bit 1, before the passes: TransAlphaClass for every polygon. a need-opaque polygon
// (translucent, polygon alpha 31, so an A3I5 or A5I3 texture) goes by FinalColor in the shader: with
// texturing off (DISP3DCNT bit 0) or decal (mode 1, and shadow, mode 3, which takes the decal path)
// the fragment's alpha is the vertex color's, 31, so only the opaque draw passes. modulate and
// toon/highlight (modes 0 and 2) multiply it by the texel's, so the texture's classes decide. the
// texture is read with texelFetch, nothing filters or interpolates texel alpha (TextureLookup_Linear
// isn't used), and there's no alpha test in these shaders
void GLRenderer::ClassifyTransAlpha(const GPU& gpu)
{
#ifdef MELONADE_DEBUG_TOOLS
    s64 start = TransStats ? GLStatsNowNs() : 0;
#endif
    if (++TexAlphaFrame == 0)
    {
        // the stamps wrapped: forget them all
        memset(TexAlphaStamp, 0, sizeof(TexAlphaStamp));
        TexAlphaFrame = 1;
    }

    bool texturing = (gpu.GPU3D.RenderDispCnt & (1<<0)) != 0;
    const u8* vram = gpu.VRAMFlat_Texture;

    for (int i = 0; i < NumFinalPolys; i++)
    {
        const Polygon* polygon = PolygonList[i].PolyData;
        u32 attr = polygon->Attr;
        if (polygon->IsShadowMask || !polygon->Translucent || (attr & 0x001F0000) != 0x001F0000)
        {
            TransAlphaClass[i] = AlphaClass_Full | AlphaClass_Partial;
            continue;
        }

        u32 format = (polygon->TexParam >> 26) & 0x7;
        u32 blendmode = (attr >> 4) & 0x3;
        u8 value;
        if (!texturing || format == 0 || (blendmode & 1))
            value = AlphaClass_Full;
        else
            value = TextureAlphaClass(vram, polygon->TexParam);
        TransAlphaClass[i] = value;

#ifdef MELONADE_DEBUG_TOOLS
        if (TransStats)
        {
            GLTransStats::Counters& sec = TransStats->Sec;
            sec.NeedOpaque++;
            if (value == AlphaClass_Full)          sec.Full++;
            else if (value == AlphaClass_Partial)  sec.Partial++;
            else if (value != 0)                   sec.Mixed++;
            else                                   sec.Neither++;
        }
#endif
    }

#ifdef MELONADE_DEBUG_TOOLS
    if (TransStats)
    {
        s64 ns = GLStatsNowNs() - start;
        TransStats->Sec.ScanNs += ns;
        TransStats->Sec.PeakScanNs = std::max(TransStats->Sec.PeakScanNs, ns);
    }
#endif
}

// GPU::GLNoDiscard bit 1 (R1 step 2): whether the opaque shader can return any texel of this
// texture with alpha under 31 (so its `discard` can fire), going by the texture fetches in
// kRenderFSCommon, for a modulate / toon / highlight polygon with alpha 31, whose fragment alpha
// is then the texel's (FinalColor). every texel of the width x height, as TextureAlphaClass reads
// them for T1 (TexcoordWrap keeps s and t inside), from the same copy TexMem is uploaded from.
// fills in the key's chunks for InvalidateOpaqueTex: all of them, even when it stops at the first
// transparent texel, so a texture that turns opaque later gets scanned again. only asked for
// NoDiscard_Scan formats (I2/I4/I8 with color 0 transparent, 4x4, direct)
u8 GLRenderer::ScanOpaqueTexture(const u8* vram, u32 texparam, OpaqueTexEntry& entry)
{
    u32 format = (texparam >> 26) & 0x7;
    u32 addr = (texparam & 0xFFFF) << 3;
    u32 texels = (8u << ((texparam >> 20) & 0x7)) * (8u << ((texparam >> 23) & 0x7));
    const u32 vramSize = sizeof(GPU::VRAMFlat_Texture);

    entry.First0 = entry.First1 = 1;
    entry.Last0 = entry.Last1 = 0;

    u32 bytes = 0;
    switch (format)
    {
    case 2: bytes = texels / 4; break; // I2
    case 3: bytes = texels / 2; break; // I4
    case 4: bytes = texels; break;     // I8
    case 5: bytes = texels / 4; break; // 4x4 compressed: 4 bytes a block, plus the slot 1 index data
    case 7: bytes = texels * 2; break; // direct
    default: return TexScan_OutOfRange;
    }

    // past TexMem's 512 KB the shader's texelFetch reads out of range, which GL leaves undefined.
    // a 4x4 texture spanning two 128 KB slots would take its palette words from two places (the
    // hardware only has slots 0 and 2 anyway); neither qualifies, which depends on nothing in VRAM
    if (addr + bytes > vramSize)
        return TexScan_OutOfRange;
    if (format == 5 && (addr >> 17) != ((addr + bytes - 1) >> 17))
        return TexScan_OutOfRange;

    entry.First0 = addr / VRAMDirtyGranularity;
    entry.Last0 = (addr + bytes - 1) / VRAMDirtyGranularity;

#ifdef MELONADE_DEBUG_TOOLS
    if (OpaqueStats)
    {
        OpaqueStats->Sec.Textures++;
        OpaqueStats->Sec.Bytes += bytes;
    }
#endif

    const u8* p = vram + addr;
    bool found = false;
    if (format == 2)
    {
        // TextureFetch_I2: index 0 is alpha0, which TexParam bit 29 makes 0
        for (u32 i = 0; i < bytes && !found; i++)
        {
            u8 b = p[i];
            found = !(b & 0x03) || !(b & 0x0C) || !(b & 0x30) || !(b & 0xC0);
        }
        return found ? TexScan_Index0 : TexScan_Opaque;
    }
    else if (format == 3)
    {
        for (u32 i = 0; i < bytes && !found; i++)
            found = !(p[i] & 0x0F) || !(p[i] & 0xF0);
        return found ? TexScan_Index0 : TexScan_Opaque;
    }
    else if (format == 4)
    {
        for (u32 i = 0; i < bytes && !found; i++)
            found = p[i] == 0;
        return found ? TexScan_Index0 : TexScan_Opaque;
    }
    else if (format == 7)
    {
        // TextureFetch_Direct: alpha is bit 15, the high byte's bit 7 (addr is even)
        for (u32 i = 1; i < bytes && !found; i += 2)
            found = !(p[i] & 0x80);
        return found ? TexScan_Direct0 : TexScan_Opaque;
    }

    // TextureFetch_Compressed: a block's 4 bytes hold 2-bit indices, and index 3 is transparent
    // (vec4(0)) when the block's palette word (slot 1, 0x20000 + half the block's offset in its
    // slot, +0x10000 from 0x40000 on) has mode 0 or 1 (bits 14-15). inside one 128 KB slot that
    // address grows with the block's, so the index data read is the first block's word to the
    // last one's
    auto slot1Of = [](u32 a)
    {
        u32 s = 0x20000 + ((a & 0x1FFFC) >> 1);
        if (a >= 0x40000) s += 0x10000;
        return s;
    };
    entry.First1 = slot1Of(addr) / VRAMDirtyGranularity;
    entry.Last1 = (slot1Of(addr + bytes - 4) + 1) / VRAMDirtyGranularity;

    for (u32 i = 0; i < bytes && !found; i += 4)
    {
        u32 slot1 = slot1Of(addr + i);
        u32 palinfo = vram[slot1] | (vram[slot1 + 1] << 8);
        if ((palinfo >> 14) >= 2) continue;
        for (u32 j = 0; j < 4 && !found; j++)
        {
            u8 b = p[i + j];
            found = (b & 0x03) == 0x03 || (b & 0x0C) == 0x0C || (b & 0x30) == 0x30 || (b & 0xC0) == 0xC0;
        }
    }
    return found ? TexScan_Comp0 : TexScan_Opaque;
}

// GPU::GLNoDiscard bit 1: ScanOpaqueTexture's class, from the cache when this texture was scanned
// and nothing it read has changed since. vram is gpu.VRAMFlat_Texture after this frame's
// MakeVRAMFlat_TextureCoherent and InvalidateOpaqueTex
u8 GLRenderer::OpaqueTexClass(const u8* vram, u32 texparam)
{
    u32 key = texparam & 0x1FF0FFFF;
    u32 hash = (key * 2654435761u) >> 20; // OpaqueTexSlots = 4096
    u32 slot = hash;
    for (; OpaqueTexIndex[slot] >= 0; slot = (slot + 1) & (OpaqueTexSlots - 1))
    {
        const OpaqueTexEntry& e = OpaqueTex[OpaqueTexIndex[slot]];
        if (e.Key == key)
            return e.Value;
    }

    if (NumOpaqueTex == OpaqueTexMax)
    {
        // full: start over (textures that are still drawn get scanned again)
        ClearOpaqueTex();
        slot = hash;
    }

    OpaqueTexEntry& e = OpaqueTex[NumOpaqueTex];
    e.Key = key;
    e.Value = ScanOpaqueTexture(vram, texparam, e);
    OpaqueTexIndex[slot] = (s16)NumOpaqueTex;
    NumOpaqueTex++;
    return e.Value;
}

void GLRenderer::ClearOpaqueTex()
{
    memset(OpaqueTexIndex, 0xFF, sizeof(OpaqueTexIndex));
    NumOpaqueTex = 0;
}

// GPU::GLNoDiscard bit 1: drops the cached scans that read a chunk this frame's texture upload
// changed (RenderFrame's textureDirty, the 512 byte chunks VRAMFlat_Texture and TexMem were just
// brought up to date in; all of them after Reset or a renderer switch, TexMemDirtyAll), and
// rebuilds the index from the ones left. dirty is textureDirty as a count of the changed chunks
// below each one (TexDirtyPrefix), built once a frame for this and the texel heap (GPU::GLTexCache)
static_assert(VRAMDirtyGranularity == 512, "TexDirtyPrefix counts 512 byte chunks");
void GLRenderer::InvalidateOpaqueTex(const TexDirtyPrefix& dirty)
{
    if (dirty.Count() == 0)
        return;

    int kept = 0;
    for (int i = 0; i < NumOpaqueTex; i++)
    {
        const OpaqueTexEntry& e = OpaqueTex[i];
        if (dirty.Changed(e.First0, e.Last0) || dirty.Changed(e.First1, e.Last1))
            continue;
        OpaqueTex[kept++] = e;
    }
    if (kept == NumOpaqueTex)
        return;

    memset(OpaqueTexIndex, 0xFF, sizeof(OpaqueTexIndex));
    NumOpaqueTex = kept;
    for (int i = 0; i < kept; i++)
    {
        u32 slot = (OpaqueTex[i].Key * 2654435761u) >> 20;
        while (OpaqueTexIndex[slot] >= 0)
            slot = (slot + 1) & (OpaqueTexSlots - 1);
        OpaqueTexIndex[slot] = (s16)i;
    }
}

// GPU::GLNoDiscard bit 1, after the frame's PolygonList is set up (and before anything reads the
// render keys): the opaque polygons whose texture decides (NoDiscard_Scan) get the no-discard key
// bit when the scan finds no texel under alpha 31. same frames as step 1 (NoDiscard: Z-buffer, the
// program built)
void GLRenderer::MarkScannedNoDiscard(const GPU& gpu, bool texturing)
{
#ifdef MELONADE_DEBUG_TOOLS
    s64 start = OpaqueStats ? GLStatsNowNs() : 0;
#endif
    const u8* vram = gpu.VRAMFlat_Texture;
    for (int i = 0; i < NumFinalPolys; i++)
    {
        RendererPolygon& rp = PolygonList[i];
        if (NoDiscardClass(rp.PolyData, texturing) != NoDiscard_Scan)
            continue;
        if (OpaqueTexClass(vram, rp.PolyData->TexParam) == TexScan_Opaque)
            rp.RenderKey |= RenderKey_NoDiscard;
    }
#ifdef MELONADE_DEBUG_TOOLS
    if (OpaqueStats)
        OpaqueStats->Sec.NdScanNs += GLStatsNowNs() - start;
#endif
}

namespace
{
// GPU::GLTexCache: the heap's decodes go into TexHeap, which RenderFrame bound on unit 2. the unit
// that was active before the first upload is made active again at the end (Restore), so later binds
// that don't pick a unit don't land on unit 2
struct TexHeapUploader : GLTexHeap::UploadSink
{
    bool Active = false;
    GLint PrevUnit = GL_TEXTURE0;
    GLenum Unit = 0;
    void Select(GLenum unit)
    {
        if (!Active)
        {
            glGetIntegerv(GL_ACTIVE_TEXTURE, &PrevUnit);
            Active = true;
        }
        if (Unit != unit)
        {
            glActiveTexture(unit);
            Unit = unit;
        }
    }
    void Upload(u32 x, u32 y, u32 w, u32 h, const u8* data) override
    {
        Select(GL_TEXTURE2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, data);
    }
    // GPU::GLTexCache 128: the color plane, which RenderFrame bound on unit 4, uploaded as
    // TexPalLinear is
    void UploadColor(u32 x, u32 y, u32 w, u32 h, const u16* data) override
    {
        Select(GL_TEXTURE4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, data);
    }
    void Restore()
    {
        if (Active)
            glActiveTexture((GLenum)PrevUnit);
        Active = false;
        Unit = 0;
    }
};
}

// GPU::GLTexCache (T2 step 1, docs/plan-t2-texcache.md), after the frame's PolygonList is set up and
// before BuildPolygons writes the vertices: every textured polygon (texturing on, a format, not a
// shadow mask, which never samples) whose texture the heap holds, or can decode within the frame's
// budget, reads it from there (TexWord = 0x40000000 | its offset). the rest keep TexParam's VRAM
// offset and the old path, which is always exact. render keys and batches don't change. vram is
// VRAMFlat_Texture, which TexMem was just brought up to date from
bool GLRenderer::AssignTexCache(const GPU& gpu, bool texturing)
{
    bool anyFull = false;
#ifdef MELONADE_DEBUG_TOOLS
    GLTexCacheStats* st = TexCacheStats.get();
    s64 start = st ? GLStatsNowNs() : 0;
    TexHeap->Timing = st != nullptr;
#endif
    TexHeap->BeginFrame();
    // GPU::GLTexCache 128: only with programs built for it and the plane there
    bool colorPlane = TexColorPlane && TexColorID != 0 && TexHeap->ColorPlane();

    if (texturing)
    {
        const u8* vram = gpu.VRAMFlat_Texture;
        TexHeapUploader uploader;
        // polygons in a row often share a texture: the last answer is reused for the same TexParam
        // and palette (the heap's state can't change in between, nothing else runs)
        u32 lastParam = 0, lastPal = 0, lastOffset = 0;
        int lastResult = -1;
        GLTexHeap::Plane lastPlane = GLTexHeap::PlaneNone;
        for (int i = 0; i < NumFinalPolys; i++)
        {
            RendererPolygon& rp = PolygonList[i];
            const Polygon* polygon = rp.PolyData;
            u32 format = (polygon->TexParam >> 26) & 0x7;
            if (polygon->IsShadowMask || format == 0)
                continue;

            int result;
            u32 offset = 0;
            GLTexHeap::Plane plane = GLTexHeap::PlaneNone;
            if (lastResult >= 0 && polygon->TexParam == lastParam && polygon->TexPalette == lastPal)
            {
                offset = lastOffset;
                result = lastResult < GLTexHeap::NumServed ? GLTexHeap::Hit : lastResult;
                plane = lastPlane;
            }
            else
            {
                result = TexHeap->Use(vram, polygon->TexParam, polygon->TexPalette, uploader, offset, plane);
                lastParam = polygon->TexParam;
                lastPal = polygon->TexPalette;
                lastOffset = offset;
                lastResult = result;
                lastPlane = plane;
            }
            bool color = false, colorFull = false;
            if (result < GLTexHeap::NumServed)
            {
                rp.TexWord = 0x40000000 | offset;
                // step 4a (128): the color plane's bits (TextureLookup_Cached): 22 has one, 23 takes
                // the alpha from the record (A3I5, A5I3). step 4b: only while the plane matches this
                // frame's palette (PlaneOk); otherwise the records, which read TexPalLinear
                if (colorPlane && plane == GLTexHeap::PlaneOk)
                {
                    color = true;
                    rp.TexWord |= TexWord_Color | ((format == 1 || format == 6) ? TexWord_RecAlpha : 0);
                }
                else if (TexColorOnly)
                {
                    // 256: the cache-only programs have no record path, so a served texture without a
                    // current color plane (4x4, direct, palette-volatile, a re-resolve over the budget)
                    // draws with the twins, whose heap lookup has it
                    colorFull = true;
                    rp.RenderKey |= RenderKey_TexFull;
                    anyFull = true;
                }
            }
            else if (TexCacheOnly)
            {
                // step 2: the old path is only in the twins. a key bit, so batches split here
                rp.RenderKey |= RenderKey_TexFull;
                anyFull = true;
            }

#ifdef MELONADE_DEBUG_TOOLS
            if (st)
            {
                // as CountOpaqueR1's: the shoelace area of the 1x vertices, in screens
                s64 sum = 0;
                u32 n = polygon->NumVertices;
                for (u32 j = 0; j < n; j++)
                {
                    const Vertex* va = polygon->Vertices[j];
                    const Vertex* vb = polygon->Vertices[(j + 1) < n ? (j + 1) : 0];
                    sum += (s64)va->FinalPosition[0] * vb->FinalPosition[1] - (s64)vb->FinalPosition[0] * va->FinalPosition[1];
                }
                double area = (sum < 0 ? -sum : sum) / (2.0 * 256 * 192);
                st->Sec.Polys[result]++;
                st->Sec.Area[result] += area;
                st->Sec.FormatArea[format] += area;
                if (result < GLTexHeap::NumServed)
                    st->Sec.FormatServed[format] += area;
                if (color)
                {
                    st->Sec.ColorPolys++;
                    st->Sec.ColorArea += area;
                }
                if (colorFull)
                {
                    st->Sec.ColorFullPolys++;
                    st->Sec.ColorFullArea += area;
                }
                // step 4b: served color-format polygons that read their records instead
                if (result < GLTexHeap::NumServed && plane == GLTexHeap::PlaneVolatile)
                {
                    st->Sec.ColorVolPolys++;
                    st->Sec.ColorVolArea += area;
                }
                if (result < GLTexHeap::NumServed && plane == GLTexHeap::PlaneBudget)
                {
                    st->Sec.ColorBudgetPolys++;
                    st->Sec.ColorBudgetArea += area;
                }
            }
#endif
        }
        uploader.Restore();
    }

#ifdef MELONADE_DEBUG_TOOLS
    if (st)
    {
        s64 ns = GLStatsNowNs() - start;
        st->Sec.Frames++;
        st->Sec.Entries += TexHeap->NumEntries();
        st->Sec.UsedTexels += TexHeap->UsedTexels();
        if (TexHeap->ColorPlane())
            st->Sec.PalVolEntries += TexHeap->NumPalVolatile();
        st->Sec.AssignNs += ns;
        st->Sec.AssignPeakNs = std::max(st->Sec.AssignPeakNs, ns);

        // step 2's batches: the opaque pass's polygons and the translucent ones, each in list order as
        // the passes walk them, with and without RenderKey_TexFull, and the program switches between
        // the two forms it adds (each pass starts on the cache-only one)
        for (int translucent = 0; translucent < 2; translucent++)
        {
            bool cur = false;
            for (int i = 0; i < NumFinalPolys; )
            {
                const RendererPolygon* rp = &PolygonList[i];
                if (rp->PolyData->IsShadowMask || rp->PolyData->Translucent != (translucent != 0)) { i++; continue; }
                bool full = PolyFull(rp);
                if (full != cur) { st->Sec.Switches++; cur = full; }
                st->Sec.Draws++;
                int end = i;
                while (end < NumFinalPolys && PolygonList[end].PrimType == rp->PrimType && PolygonList[end].RenderKey == rp->RenderKey)
                {
                    if (PolyFull(&PolygonList[end])) st->Sec.FullPolys++;
                    end++;
                }
                i = end;
            }
            if (cur) st->Sec.Switches++;

            for (int i = 0; i < NumFinalPolys; )
            {
                const RendererPolygon* rp = &PolygonList[i];
                if (rp->PolyData->IsShadowMask || rp->PolyData->Translucent != (translucent != 0)) { i++; continue; }
                u32 key = rp->RenderKey & ~RenderKey_TexFull;
                st->Sec.DrawsBase++;
                int end = i + 1;
                while (end < NumFinalPolys && PolygonList[end].PrimType == rp->PrimType
                       && (PolygonList[end].RenderKey & ~RenderKey_TexFull) == key)
                    end++;
                i = end;
            }
        }
    }
#endif
    return anyFull;
}

#ifdef MELONADE_DEBUG_TOOLS
// GPU::SceneStats, GLOPAQUE (R1 count): the class of a modulate / toon / highlight polygon's texture
// at alpha 31, by the same scan (and cache) GPU::GLNoDiscard bit 1 decides with, so texop is what
// that bit can draw without discard. A3I5 / A5I3 polygons are translucent and go by T1's classes
// instead, so they aren't asked here
u8 GLRenderer::OpaqueTextureClass(const u8* vram, u32 texparam)
{
    u32 format = (texparam >> 26) & 0x7;
    bool color0 = (texparam & (1u << 29)) != 0;
    // alpha0 is 1.0: every texel comes out opaque without looking
    if (format >= 2 && format <= 4 && !color0)
        return GLOpaqueStats::TexPal;

    switch (OpaqueTexClass(vram, texparam))
    {
    case TexScan_Opaque: return GLOpaqueStats::TexOpaque;
    case TexScan_Index0: return GLOpaqueStats::Color0;
    case TexScan_Direct0: return GLOpaqueStats::Direct0;
    case TexScan_Comp0: return GLOpaqueStats::Comp0;
    default: return GLOpaqueStats::OutOfRange;
    }
}

// GPU::SceneStats, GLOPAQUE (R1 count, see GLOpaqueStats): after ClassifyTransAlpha, which it reads
// for need-opaque polygons. goes by FinalColor and kRenderFS's opaque branch: the only discard is
// col.a < 30.5/31, there's no alpha test in these shaders, and col.a is the vertex alpha (the
// polygon's) times the texel's for modulate / toon / highlight, the vertex alpha alone for decal and
// shadow (mode 1 and 3) and with texturing off or no texture. counts only, draws nothing
void GLRenderer::CountOpaqueR1(const GPU& gpu)
{
    GLOpaqueStats& st = *OpaqueStats;
    s64 start = GLStatsNowNs();

    bool wbuffer = gpu.GPU3D.RenderPolygonRAM[0]->WBuffer;
    int b = wbuffer ? 1 : 0;
    st.Sec.Frames++;
    if (wbuffer) st.Sec.WFrames++;
    else         st.Sec.ZFrames++;

    bool texturing = (gpu.GPU3D.RenderDispCnt & (1<<0)) != 0;
    const u8* vram = gpu.VRAMFlat_Texture;

    for (int i = 0; i < NumFinalPolys; i++)
    {
        const Polygon* polygon = PolygonList[i].PolyData;
        if (polygon->IsShadowMask) continue;

        u32 attr = polygon->Attr;
        u32 alpha = (attr >> 16) & 0x1F;
        u32 format = (polygon->TexParam >> 26) & 0x7;
        u32 blendmode = (attr >> 4) & 0x3;

        int cls;
        if (polygon->Translucent)
        {
            if (alpha != 31) continue; // a plain translucent polygon, never in the opaque shader
            u8 v = TransAlphaClass[i];
            if (v == AlphaClass_Full)       cls = GLOpaqueStats::NeedFull;
            else if (v & AlphaClass_Full)   cls = GLOpaqueStats::NeedMixed;
            else                            cls = GLOpaqueStats::NeedNone;
        }
        else if (alpha == 0)
            cls = GLOpaqueStats::Alpha0;
        else if (!texturing || format == 0)
            cls = GLOpaqueStats::Untex;
        else if (blendmode & 1)
            cls = GLOpaqueStats::Decal;
        else
            cls = OpaqueTextureClass(vram, polygon->TexParam);

        s64 sum = 0;
        u32 n = polygon->NumVertices;
        for (u32 j = 0; j < n; j++)
        {
            const Vertex* va = polygon->Vertices[j];
            const Vertex* vb = polygon->Vertices[(j + 1) < n ? (j + 1) : 0];
            sum += (s64)va->FinalPosition[0] * vb->FinalPosition[1] - (s64)vb->FinalPosition[0] * va->FinalPosition[1];
        }
        double area = (sum < 0 ? -sum : sum) / (2.0 * 256 * 192);

        st.Sec.Polys[b][cls]++;
        st.Sec.Area[b][cls] += area;
        if (!wbuffer && (PolygonList[i].RenderKey & RenderKey_NoDiscard))
        {
            st.Sec.NoDiscard++;
            st.Sec.NoDiscardArea += area;
        }
    }

    // GPU::GLNoDiscard's batches: RenderSceneChunk's opaque loop over the same list, with and without
    // the key bit, and the program switches it makes (it starts on the normal program)
    if (!wbuffer)
    {
        bool cur = false;
        for (int i = 0; i < NumFinalPolys; )
        {
            const RendererPolygon* rp = &PolygonList[i];
            if (rp->PolyData->IsShadowMask || rp->PolyData->Translucent) { i++; continue; }

            bool nd = (rp->RenderKey & RenderKey_NoDiscard) != 0;
            if (nd != cur) { st.Sec.ProgSwitches++; cur = nd; }
            st.Sec.Draws++;
            int end = i + 1;
            // without GPU::GLTexCache step 2's bit, whose batches GLTEXCACHE counts
            while (end < NumFinalPolys && PolygonList[end].PrimType == rp->PrimType
                   && (PolygonList[end].RenderKey & ~RenderKey_TexFull) == (rp->RenderKey & ~RenderKey_TexFull))
                end++;
            i = end;
        }
        if (cur) st.Sec.ProgSwitches++;

        for (int i = 0; i < NumFinalPolys; )
        {
            const RendererPolygon* rp = &PolygonList[i];
            if (rp->PolyData->IsShadowMask || rp->PolyData->Translucent) { i++; continue; }

            u32 key = rp->RenderKey & ~(RenderKey_NoDiscard | RenderKey_TexFull);
            st.Sec.DrawsBase++;
            int end = i + 1;
            while (end < NumFinalPolys && PolygonList[end].PrimType == rp->PrimType
                   && (PolygonList[end].RenderKey & ~(RenderKey_NoDiscard | RenderKey_TexFull)) == key)
                end++;
            i = end;
        }
    }

    st.Sec.ScanNs += GLStatsNowNs() - start;
}
#endif

void GLRenderer::LogFrameStats(const GPU3D& gpu3d)
{
    // debug: what kinds of polygons the frame has, to find what a game draws its shadows with
    int numopaque = 0, numtrans = 0, nummasks = 0, numshadows = 0;
    int numequalopaque = 0, numequaltrans = 0, numequaltransdepth = 0;
    for (int i = 0; i < NumFinalPolys; i++)
    {
        const Polygon* polygon = PolygonList[i].PolyData;
        if (polygon->Translucent) numtrans++;
        else                      numopaque++;
        if (polygon->IsShadowMask) nummasks++;
        else if (polygon->IsShadow) numshadows++;

        if (!(polygon->Attr & (1<<14))) continue;
        if (!polygon->Translucent) numequalopaque++;
        else
        {
            numequaltrans++;
            if (polygon->Attr & (1<<11)) numequaltransdepth++;
        }
    }

    bool wbuffer = gpu3d.RenderNumPolygons && gpu3d.RenderPolygonRAM[0]->WBuffer;
    Platform::Log(Platform::LogLevel::Info,
                  "SHADOWSTATS f=%u %c polys=%d opaque=%d trans=%d masks=%d shadows=%d eq_opaque=%d eq_trans=%d eq_trans_dw=%d\n",
                  ShadowLogFrame, wbuffer ? 'W' : 'Z', NumFinalPolys, numopaque, numtrans, nummasks, numshadows,
                  numequalopaque, numequaltrans, numequaltransdepth);
}

int GLRenderer::BeginShadowLogQuery()
{
    // GLES only has 'any samples passed', which is enough to see a shadow that drew nothing
    if (!ShadowLogActive || NumShadowLogQueries >= 64) return -1;

    if (!ShadowLogQueries[0]) glGenQueries(64, ShadowLogQueries);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, ShadowLogQueries[NumShadowLogQueries]);
    return NumShadowLogQueries++;
}

void GLRenderer::LogShadowPolygon(char type, int i, int query)
{
    if (!ShadowLogActive || NumShadowLogEntries >= 64) return;

    const Polygon* polygon = PolygonList[i].PolyData;
    ShadowLogEntry& entry = ShadowLogEntries[NumShadowLogEntries++];
    entry.Type = type;
    entry.Index = i;
    entry.Attr = polygon->Attr;
    entry.WBuffer = polygon->WBuffer;
    entry.FacingView = polygon->FacingView;
    entry.YTop = polygon->YTop;
    entry.YBottom = polygon->YBottom;
    entry.ZMin = 0xFFFFFFFF;
    entry.ZMax = 0;
    for (u32 v = 0; v < polygon->NumVertices; v++)
    {
        entry.ZMin = std::min(entry.ZMin, (u32)polygon->FinalZ[v]);
        entry.ZMax = std::max(entry.ZMax, (u32)polygon->FinalZ[v]);
    }
    entry.Query = query;
}

void GLRenderer::FlushShadowLog()
{
    // one line per frame, masks (M) and shadows (S) in pass 3 order. a=polygon attr (bit 14 depth
    // equal, bit 11 depth write), W/Z buffer, F/B facing, z=depth range (W when W-buffering),
    // px=whether the shadow drew any pixels
    char line[3072];
    int len = snprintf(line, sizeof(line), "SHADOWLOG f=%u", ShadowLogFrame);

    for (int i = 0; i < NumShadowLogEntries && len < (int)sizeof(line) - 96; i++)
    {
        const ShadowLogEntry& entry = ShadowLogEntries[i];
        len += snprintf(line + len, sizeof(line) - len, " | %c%d id=%u a=%08X %c%c y=%d-%d z=%X-%X",
                        entry.Type, entry.Index, (entry.Attr >> 24) & 0x3F, entry.Attr,
                        entry.WBuffer ? 'W' : 'Z', entry.FacingView ? 'F' : 'B',
                        entry.YTop, entry.YBottom, entry.ZMin, entry.ZMax);

        if (entry.Query >= 0)
        {
            GLuint passed = 0;
            glGetQueryObjectuiv(ShadowLogQueries[entry.Query], GL_QUERY_RESULT, &passed);
            len += snprintf(line + len, sizeof(line) - len, " px=%u", passed);
        }
    }

    Platform::Log(Platform::LogLevel::Info, "%s\n", line);

    NumShadowLogEntries = 0;
    NumShadowLogQueries = 0;
}

void GLRenderer::RenderSceneChunk(const GPU3D& gpu3d, int y, int h)
{
    bool wbuffer = gpu3d.RenderPolygonRAM[0]->WBuffer;

    if (h != 192) glScissor(0, y*ScaleFactor, ScreenW, h*ScaleFactor);

    GLboolean fogenable = (gpu3d.RenderDispCnt & (1<<7)) ? GL_TRUE : GL_FALSE;

    // TODO: proper 'equal' depth test!
    // (has margin of +-0x200 in Z-buffer mode, +-0xFF in W-buffer mode)
    // for now we're using GL_LEQUAL to make it work to some extent

    // pass 1: opaque pixels

    GL_TIMER(Opaque);
    FRAG_PASS(Opaque);
    UseRenderShader(wbuffer, RenderMode_Opaque);
    glLineWidth(1.0);

    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glBindVertexArray(VertexArrayID);

    for (int i = 0; i < NumFinalPolys; )
    {
        RendererPolygon* rp = &PolygonList[i];

        if (rp->PolyData->IsShadowMask) { i++; continue; }
        if (rp->PolyData->Translucent) { i++; continue; }

        // GPU::GLNoDiscard: the batch's program (RenderKey_NoDiscard is part of the batch key). the
        // same program otherwise, so this only changes anything with the switch on
        UseRenderShader(wbuffer, RenderMode_Opaque, false, (rp->RenderKey & RenderKey_NoDiscard) != 0, PolyFull(rp));

        if (rp->PolyData->Attr & (1<<14))
            glDepthFunc(GL_LEQUAL);
        else
            glDepthFunc(GL_LESS);

        u32 polyattr = rp->PolyData->Attr;
        u32 polyid = (polyattr >> 24) & 0x3F;

        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glStencilMask(0xFF);

        SetPolygonDepthBias(polyattr);
        i += RenderPolygonBatch(i);
    }

    // edge flags (GPU::GLEdgeMark 1): the opaque draws above (and the need-opaque ones below) set
    // attr G with UseRenderShader's edge-marking programs, the translucent ones mask it off
    // (translucent pixels keep the flag and ID of the opaque pixel under them, like software)

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);

    if (gpu3d.RenderDispCnt & (1<<3))
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    else
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);

    glLineWidth(1.0);

    // GPU::GLTransSkip bit 1: which of a need-opaque polygon's two draws can pass a fragment (see
    // ClassifyTransAlpha). a draw that would discard all of them changes nothing, and every draw after
    // it sets all the state it uses. shadows keep their translucent draws, which do the stencil work
    bool alphaClasses = (GLTransSkip & 2) != 0;
    // redrawn: bit 0 already leaves the opaque draw out
    auto needsDraws = [&](int i, bool needopaque, bool redrawn, bool& drawopaque, bool& drawtrans)
    {
        drawopaque = needopaque && !redrawn;
        drawtrans = true;
        if (!needopaque || !alphaClasses) return;

        u8 value = TransAlphaClass[i];
        if (drawopaque && !(value & AlphaClass_Full))
        {
            drawopaque = false;
            TRANS_SKIPPED(Skip1Opaque);
        }
        if (!(value & AlphaClass_Partial) && !PolygonList[i].PolyData->IsShadow)
        {
            drawtrans = false;
            TRANS_SKIPPED(Skip1Trans);
        }
    };

    if (NumOpaqueFinalPolys > -1)
    {
        bool clear0 = (gpu3d.RenderClearAttr1 & 0x001F0000) == 0;

        // GPU::GLTransSkip bit 2 (FetchClear0, set in RenderFrame, which also cleared the stencil to
        // 0xBF for it): no pass 2. pass 3 draws what it did, in polygon order:
        // * non-shadow translucent draws use the framebuffer fetch program with blending off, which
        //   writes the source as it is where the destination alpha is 0 (the clear plane, so per
        //   pixel with a clear bitmap) and blends like the fixed-function state elsewhere. the
        //   attr target only gets the fog bit, 0 blended or not
        // * pass 3's stencil test (NOTEQUAL 0x40|id over 0x7F) already keeps a polygon from drawing
        //   twice where it drew, and a later one with the same ID from drawing over it, as pass 2's
        //   marks did. the clear plane's 0xBF keeps 0x7F from looking like translucent ID 63
        // * a need-opaque polygon's opaque part is drawn by pass 3, so bit 0 doesn't apply
        // * shadows keep their pass 3 draws with fixed-function blending. pass 2 drew the ones
        //   with the clear plane's ID over shadowed clear pixels blended as well, and pass 3 then
        //   drew them there again
        bool pass2 = clear0 && !FetchClear0;
#ifdef MELONADE_DEBUG_TOOLS
        if (TransStats && clear0)
        {
            TransStats->Sec.Clear0Frames++;
            TransStats->Sec.FetchFrames += FetchClear0;
        }
#endif

        // pass 2: if needed, render translucent pixels that are against background pixels
        // when background alpha is zero, those need to be rendered with blending disabled
        // (with a clear bitmap this still tests the clear attribute's alpha, not the bitmap's
        // per-pixel alpha; upstream's GL renderer has the same gap)

        if (pass2)
        {
            GL_TIMER(Clear0);
            FRAG_PASS(Clear0);
            glDisable(GL_BLEND);

            for (int i = 0; i < NumFinalPolys; )
            {
                RendererPolygon* rp = &PolygonList[i];

                if (rp->PolyData->IsShadowMask)
                {
                    // draw actual shadow mask

                    UseRenderShader(wbuffer, RenderMode_ShadowMask);

                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);

                    glDepthFunc(GL_LESS);
                    glStencilFunc(GL_EQUAL, 0xFF, 0xFF);
                    glStencilOp(GL_KEEP, GL_INVERT, GL_KEEP);
                    glStencilMask(0x01);

                    SetDepthBias(0.0f);
                    i += RenderPolygonBatch(i);
                }
                else if (rp->PolyData->Translucent)
                {
                    bool needopaque = ((rp->PolyData->Attr & 0x001F0000) == 0x001F0000);

                    u32 polyattr = rp->PolyData->Attr;
                    u32 polyid = (polyattr >> 24) & 0x3F;

                    if (polyattr & (1<<14))
                        glDepthFunc(GL_LEQUAL);
                    else
                        glDepthFunc(GL_LESS);

                    bool drawopaque, drawtrans;
                    needsDraws(i, needopaque, false, drawopaque, drawtrans);

                    if (drawopaque)
                    {
                        UseRenderShader(wbuffer, RenderMode_Opaque, false, false, PolyFull(rp));

                        glDisable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

                        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                        glStencilMask(0xFF);

                        glDepthMask(GL_TRUE);

                        SetPolygonDepthBias(polyattr);
                        RenderSinglePolygon(i);
                    }
                    else if (needopaque)
                    {
                        // the one piece of the left-out draw's state that's used after it: this
                        // pass's translucent draws (not shadows) keep the blend state they find, which
                        // a shadow before them can have turned on
                        glDisable(GL_BLEND);
                    }

                    // only a need-opaque polygon, which is drawn on its own
                    if (!drawtrans) { i++; continue; }

                    UseRenderShader(wbuffer, RenderMode_Translucent, false, false, PolyFull(rp));

                    GLboolean transfog;
                    if (!(polyattr & (1<<15))) transfog = fogenable;
                    else                       transfog = GL_FALSE;

                    if (rp->PolyData->IsShadow)
                    {
                        // shadow against clear-plane will only pass if its polyID matches that of the clear plane
                        u32 clrpolyid = (gpu3d.RenderClearAttr1 >> 24) & 0x3F;
                        if (polyid != clrpolyid) { i++; continue; }

                        glEnable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                        glStencilFunc(GL_EQUAL, 0xFE, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40|polyid)); // heheh

                        if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                        else                    glDepthMask(GL_FALSE);

                        SetPolygonDepthBias(polyattr);
                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    }
                    else
                    {
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                        glStencilFunc(GL_EQUAL, 0xFF, 0xFE);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40|polyid)); // heheh

                        if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                        else                    glDepthMask(GL_FALSE);

                        SetPolygonDepthBias(polyattr);
                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    }
                }
                else
                    i++;
            }

            glEnable(GL_BLEND);
            glStencilMask(0xFF);
        }

        // pass 3: translucent pixels

        GL_TIMER(Trans);
        FRAG_PASS(Trans);

        // with ShadowMaskLineClear, the shadow bits are cleared per line like the software renderer:
        // a mask clears its lines unless the last polygon on that line (in list order, drawn in this
        // pass or not) was a mask too. clearing the whole screen for every mask wipes the mask of an
        // earlier shadow whenever another polygon sorts between the two
        bool lastWasMask[192] {};
        int nextLinePoly = 0;
        bool maskCleared = false;

        // with fewer than two masks, the first one's full clear is all there is
        bool trackLines = false;
        if (ShadowMaskLineClear)
        {
            int nummasks = 0;
            for (int i = 0; i < NumFinalPolys && nummasks < 2; i++)
                if (PolygonList[i].PolyData->IsShadowMask) nummasks++;
            trackLines = (nummasks >= 2);
        }

        for (int i = 0; i < NumFinalPolys; )
        {
            RendererPolygon* rp = &PolygonList[i];

            if (trackLines)
            {
                for (; nextLinePoly < i; nextLinePoly++)
                {
                    const Polygon* polygon = PolygonList[nextLinePoly].PolyData;
                    int ytop, ybottom;
                    GetPolygonLines(polygon, ytop, ybottom);
                    for (int y = ytop; y < ybottom; y++)
                        lastWasMask[y] = polygon->IsShadowMask;
                }
            }

            if (rp->PolyData->IsShadowMask)
            {
                // clear shadow bits in stencil buffer
                // the first mask clears the whole screen, which also drops the bits left by the
                // clear plane and pass 2

                glStencilMask(0x80);
                if (!ShadowMaskLineClear || !maskCleared)
                {
                    glClear(GL_STENCIL_BUFFER_BIT);
                    maskCleared = true;
                }
                else
                    ClearShadowMaskLines(rp->PolyData, lastWasMask);

                // draw actual shadow mask

                UseRenderShader(wbuffer, RenderMode_ShadowMask);

                glDisable(GL_BLEND);
                glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);

                glStencilFunc(GL_ALWAYS, 0x80, 0x80);
                LogShadowPolygon('M', i, -1);

                // one at a time, so each mask's clear comes right before it
                if (ShadowMaskDepthTest && (rp->PolyData->Attr & (1<<14)))
                {
                    // depth-equal mask: the DS test passes within a margin (+-0x200 Z-buffering,
                    // +-0xFF W-buffering) and the mask marks where it fails, so mark where the
                    // polygon is more than that in front, then more than that behind
                    float margin = wbuffer ? 255.0f : 512.0f;

                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

                    glDepthFunc(GL_LESS);
                    SetDepthBias(margin);
                    if (ShadowMaskLineClear) RenderSinglePolygon(i);
                    else                     RenderPolygonBatch(i);

                    glDepthFunc(GL_GREATER);
                    SetDepthBias(-margin);
                    i += ShadowMaskLineClear ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                }
                else
                {
                    glDepthFunc(GL_LESS);
                    glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);

                    SetDepthBias(0.0f);
                    i += ShadowMaskLineClear ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                }
            }
            else if (rp->PolyData->Translucent)
            {
                bool needopaque = ((rp->PolyData->Attr & 0x001F0000) == 0x001F0000);

                u32 polyattr = rp->PolyData->Attr;
                u32 polyid = (polyattr >> 24) & 0x3F;

                if (polyattr & (1<<14))
                    glDepthFunc(GL_LEQUAL);
                else
                    glDepthFunc(GL_LESS);

                // GPU::GLTransSkip bit 0: pass 2 drew this opaque part already, with the same
                // program, vertices and bias (0 without bit 14), so a fragment that survives the
                // discard now did then, and stored its depth or failed against a nearer one. every
                // depth write since was under GL_LESS or GL_LEQUAL (a depth-equal one moved nearer,
                // never farther), and masks, the stencil clears and the shadows' stencil draws write
                // no depth, so the depth there is still at most this fragment's. GL_LESS fails, and
                // with the stencil test ALWAYS and KEEP on a depth fail, the draw writes nothing.
                // a depth-equal polygon's GL_LEQUAL can pass again, so it's still drawn. without
                // pass 2 (bit 2) this is the only opaque draw
                bool redrawn = needopaque && pass2 && (GLTransSkip & 1) && !(polyattr & (1<<14));
                if (redrawn)
                    TRANS_SKIPPED(Skip0);

                bool drawopaque, drawtrans;
                needsDraws(i, needopaque, redrawn, drawopaque, drawtrans);

                if (drawopaque)
                {
                    UseRenderShader(wbuffer, RenderMode_Opaque, false, false, PolyFull(rp));

                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

                    glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0xFF);

                    glDepthMask(GL_TRUE);

                    SetPolygonDepthBias(polyattr);
                    RenderSinglePolygon(i);
                }

                // only a need-opaque polygon that isn't a shadow, which is drawn on its own
                if (!drawtrans) { i++; continue; }

                // bit 2: the framebuffer fetch program, for all but shadows
                bool fetch = FetchClear0 && !rp->PolyData->IsShadow;
                UseRenderShader(wbuffer, RenderMode_Translucent, fetch, false, PolyFull(rp));

                GLboolean transfog;
                if (!(polyattr & (1<<15))) transfog = fogenable;
                else                       transfog = GL_FALSE;

                if (rp->PolyData->IsShadow)
                {
                    // depth-equal shadow: GL_LEQUAL also misses the part of the margin behind the
                    // pixel, so test the polygon moved that far forward. the near side of the
                    // margin can't be done in one draw, but GL_LEQUAL passed it already.
                    // with only maskdepth, not with depth write (bit 11): the moved depth would be
                    // stored, and a fixup draw can't tell this shadow's pixels from earlier ones with
                    // the same ID. equaldepth accepts that, like for every other depth-equal polygon
                    bool depthbias = (polyattr & (1<<14)) &&
                        (EqualDepthMargin || (ShadowMaskDepthTest && !(polyattr & (1<<11))));
                    float bias = 0.0f;
                    if (depthbias) bias = wbuffer ? -255.0f : -512.0f;
                    SetDepthBias(bias);

                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glStencilFunc(GL_EQUAL, polyid, 0x3F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                    glStencilMask(0x80);

                    RenderSinglePolygon(i);

                    glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                    glStencilFunc(GL_EQUAL, 0xC0|polyid, 0x80);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);

                    int query = BeginShadowLogQuery();
                    LogShadowPolygon('S', i, query);

                    i += RenderSinglePolygon(i);

                    if (query >= 0) glEndQuery(GL_ANY_SAMPLES_PASSED);
                }
                else
                {
                    // the fetch program blends itself
                    if (fetch) glDisable(GL_BLEND);
                    else       glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                    glStencilFunc(GL_NOTEQUAL, 0x40|polyid, 0x7F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);

                    SetPolygonDepthBias(polyattr);
                    i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                }
            }
            else
            {
                // opaque shadows were drawn in pass 1, without the mask
                if (rp->PolyData->IsShadow) LogShadowPolygon('O', i, -1);
                i++;
            }
        }
    }

#ifdef MELONADE_DEBUG_TOOLS
    // SCENEFRAGS' covered count with a clear alpha of 0 goes by the color's alpha, before fog changes
    // it (the other case counts by stencil after the frame, see RenderFrame)
    if (FragCounter && (gpu3d.RenderClearAttr1 & 0x001F0000) == 0)
        CountCoveredByAlpha();
#endif

    // GPU::GLEdgeSkip (E2): the edge draw can't change a pixel when nothing set the edge flag this
    // frame (GPU::GLEdgeMark 0, or nothing drawn opaque, see RenderFrame: attr G is then 0 from
    // every clear and opaque draw, and the translucent ones mask it off), since the edge shader
    // blends (0, 0, 0, 0) over every pixel without it
    bool edgepass = (gpu3d.RenderDispCnt & (1<<5)) && (EdgeMark != 0 || !EdgeSkip);
    bool fogpass = (gpu3d.RenderDispCnt & (1<<7)) != 0;

    if (edgepass || fogpass) // fog/edge enabled
    {
        // the depth buffer's detach goes with the first of them
        if (edgepass) GL_TIMER(Edge);
        else          GL_TIMER(Fog);

        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_FALSE);
        glStencilFunc(GL_ALWAYS, 0, 0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);
        // While depth and stencil writing operations are disabled by the commands above, the fact that the same texture is used as both input and output results in undefined
        // behaviour, which manifests as visual artifacts on some devices. Depth/stencil texture is attached again after fog/edge rendering
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, 0, 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, DepthBufferTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, AttrBufferTex);

        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);

        if (edgepass)
        {
            // edge marking: a pixel with the flag whose neighbor one DS pixel away has another
            // opaque ID and is further away (depth-equal polygons' bias undone). outside the
            // target, the neighbor is the clear plane's ID and depth, as the software renderer's
            // screen borders are
            glUseProgram(FinalPassEdgeShader);
            glUniform1i(EdgeULoc[0], ScaleFactor);
            glUniform1f(EdgeULoc[1], (wbuffer ? 255.0f : 512.0f) / 16777216.0f);
            glUniform1i(EdgeULoc[2], (gpu3d.RenderClearAttr1 >> 24) & 0x3F);
            glUniform1f(EdgeULoc[3], (float)(((gpu3d.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF) / 16777216.0f);
            glUniform1i(EdgeULoc[4], EdgeFast & 1);

            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

            // GPU::GLEdgeFast bit 0: only flagged pixels can change, and those are inside OpaqueBox
            // (BuildEdges). a stencil test isn't possible here: the depth/stencil texture is
            // detached for this pass, since the shader samples its depth
            bool scissor = (EdgeFast & 1) && EdgeMark != 0;
            if (scissor)
            {
                // an empty box (nothing drawn opaque, which turns EdgeMark off anyway) scissors everything away
                s32 x0 = 0, y0 = 0, w = 0, h = 0;
                if (OpaqueBox[0] <= OpaqueBox[2] && OpaqueBox[1] <= OpaqueBox[3])
                {
                    x0 = std::clamp(OpaqueBox[0], 0, ScreenW);
                    y0 = std::clamp(OpaqueBox[1], 0, ScreenH);
                    w = std::clamp(OpaqueBox[2], 0, ScreenW) - x0;
                    h = std::clamp(OpaqueBox[3], 0, ScreenH) - y0;
                }
                glEnable(GL_SCISSOR_TEST);
                glScissor(x0, y0, w, h);
            }

            glDrawArrays(GL_TRIANGLES, 0, 2*3);

            if (scissor)
                glDisable(GL_SCISSOR_TEST);
        }

        if (fogpass)
        {
            // fog

            if (edgepass) GL_TIMER(Fog);
            glUseProgram(FinalPassFogShader);

            if (gpu3d.RenderDispCnt & (1<<6))
                glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            else
                glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);

            {
                u32 c = gpu3d.RenderFogColor;
                u32 r = c & 0x1F;
                u32 g = (c >> 5) & 0x1F;
                u32 b = (c >> 10) & 0x1F;
                u32 a = (c >> 16) & 0x1F;

                glBlendColor((float)b/31.0, (float)g/31.0, (float)r/31.0, (float)a/31.0);
            }

            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }

        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, DepthBufferTex, 0);
    }
}


template <u32 Size>
static bool AnyDirty(const NonStupidBitField<Size>& dirty, u32 start, u32 count)
{
    for (u32 i = start; i < start + count; i++)
        if (dirty[i]) return true;
    return false;
}

void GLRenderer::RenderFrame(GPU& gpu)
{
#ifdef MELONADE_DEBUG_TOOLS
    if (gpu.GLTimers || Timers || gpu.SceneStats == 2 || FragCounter)
        UpdateDebugStats(gpu);

    // GLTRANS goes with SCENESTATS
    if (gpu.SceneStats && !TransStats)
        TransStats = std::make_unique<GLTransStats>();
    else if (!gpu.SceneStats && TransStats)
        TransStats.reset();
    if (TransStats)
        TransStats->FrameStart(GLStatsNowNs(), gpu.GLTransSkip);
    // GLOPAQUE (the R1 count) goes with it
    if (gpu.SceneStats && !OpaqueStats)
        OpaqueStats = std::make_unique<GLOpaqueStats>();
    else if (!gpu.SceneStats && OpaqueStats)
        OpaqueStats.reset();
    if (OpaqueStats)
        OpaqueStats->FrameStart(GLStatsNowNs());
    // GLTEXCACHE too, while the heap exists (GPU::GLTexCache)
    if (gpu.SceneStats && TexHeap && !TexCacheStats)
    {
        TexCacheStats = std::make_unique<GLTexCacheStats>();
        TexHeap->Sec = {};
    }
    else if ((!gpu.SceneStats || !TexHeap) && TexCacheStats)
        TexCacheStats.reset();
    if (TexCacheStats)
        TexCacheStats->FrameStart(GLStatsNowNs(), *TexHeap, TexCacheAsked);
#endif
    bool keep = KeepRequested;
    KeepRequested = false;
    // an earlier readback is of the frame this replaces (a skipped one too, to keep it simple)
    EarlyCapture = false;

    // same path as the software and compute renderers: DeriveState gathers the dirty 512 byte
    // chunks of the texture and palette mappings (clearing the bank dirty bits they came from,
    // so this runs exactly once per frame), the flat copies are made coherent, and only the
    // rows of TexMem / TexPalMem covering those chunks are uploaded below
    auto textureDirty = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
    auto texPalDirty = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);

    bool textureChanged = gpu.MakeVRAMFlat_TextureCoherent(textureDirty);
    bool texPalChanged = gpu.MakeVRAMFlat_TexPalCoherent(texPalDirty);

    if (TexMemDirtyAll)
    {
        textureDirty.SetRange(0, 512*1024 / VRAMDirtyGranularity);
        texPalDirty.SetRange(0, 96*1024 / VRAMDirtyGranularity);
        textureChanged = texPalChanged = true;
        TexMemDirtyAll = false;
    }

    // GPU::GLNoDiscard bit 1: the cached texture scans read VRAMFlat_Texture, which TexMem follows,
    // and both just changed in exactly these chunks. this runs every frame whatever the switch, so
    // nothing cached goes stale while it's off. palette RAM doesn't matter (alpha is the texel's)
    // GPU::GLTexCache: the heap's textures were decoded from VRAMFlat_Texture too, and those under a
    // changed chunk get hashed again at their next use. the chunks are counted once for both
    if (textureChanged && (NumOpaqueTex || TexHeap))
        TextureDirtyPrefix.Build(textureDirty);
    if (gpu.GLNoDiscard & 4)
        ClearOpaqueTex();
    else if (textureChanged && NumOpaqueTex)
        InvalidateOpaqueTex(TextureDirtyPrefix);
    if (textureChanged && TexHeap)
        TexHeap->Invalidate(TextureDirtyPrefix);
    // GPU::GLTexCache 128 (T2 step 4b): the color planes were resolved from TexPalConvBuffer, which
    // the palette upload below converts again in exactly these chunks (all of them after Reset, a
    // renderer switch or a state load: ResetVRAMCache makes every mapping mismatch, so DeriveState
    // returns every chunk; bank remaps the same way). the planes over a changed chunk get their
    // words hashed again at their next use, which runs after that upload, so it sees the palette
    // TexPalLinear and the record path see in this frame
    if (texPalChanged && TexHeap && TexHeap->ColorPlane())
    {
        TexPalettePrefix.Build(texPalDirty);
        TexHeap->InvalidatePalette(TexPalettePrefix);
    }

    // the targets match the polygons drawn here (see FrameTargetWidth). this runs after the
    // frame's Blit, so the compositor's width only changes between frames as well. with a Blit
    // after it (KeepFrameForBlit), only when the targets stay as they are
    int frameTargetWidth = FrameTargetWidth(gpu);
    if (ResizeTargets(BetterPolygons, ScaleFactor, frameTargetWidth, FrameTargetMargin(gpu, frameTargetWidth)))
        keep = false;

    // nothing changed since the last frame: MainFramebuffer still holds it, so Blit and the
    // capture readback keep working from it (upstream's GL renderer skips the same way)
    if (!textureChanged && !texPalChanged && !TargetsChanged && gpu.GPU3D.RenderFrameIdentical)
    {
#ifdef MELONADE_DEBUG_TOOLS
        if (Timers)
            Timers->Sec.Skipped++;
#endif
        return;
    }
    TargetsChanged = false;

#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->BeginSet(GLTimerStats::Set_Render);
#endif
    GL_TIMER(Upload);

    if (keep)
        KeepColorBuffer();

    // GPU::GLTexProbe (T2 measurement probe) and GPU::GLTexCache (T2 step 1): rebuilds the render
    // programs only when a switch changed, so with both at 0 this is two compares
    if (gpu.GLTexProbe != TexProbeAsked || gpu.GLTexCache != TexCacheAsked)
        UpdateRenderVariant(gpu);

    CurShaderID = -1;
    CurRenderMode = -1;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, MainFramebuffer);

    ShaderConfig.uScreenSize[0] = ScreenW;
    ShaderConfig.uScreenSize[1] = ScreenH;
    ShaderConfig.uDispCnt = gpu.GPU3D.RenderDispCnt;

    WideFrame = TargetWidth != WideMelon::NativeWidth || gpu.GPU3D.RenderViewWidth != WideMelon::NativeWidth;
    WideXAdjust = (TargetWidth - gpu.GPU3D.RenderViewWidth) << 3;
    WideFrameY = TargetMargin != 0 || gpu.GPU3D.RenderViewMargin != 0;
    WideYAdjust = (TargetMargin - gpu.GPU3D.RenderViewMargin) << 4;
    FrameMargin = gpu.GPU3D.RenderViewMargin;
    ShadowMaskLineClear = gpu.ShadowMaskLineClear;
    ShadowMaskDepthTest = gpu.ShadowMaskDepthTest;
    EqualDepthMargin = gpu.EqualDepthMargin;
    GLTransSkip = gpu.GLTransSkip;
    EdgeSkip = gpu.GLEdgeSkip != 0;
    // GPU::GLEdgeMark (E1): only for frames with edge marking on, and only once a polygon is drawn
    // opaque (below), as nothing else can set the flag
    EdgeMark = (gpu.GPU3D.RenderDispCnt & (1<<5)) ? std::clamp(gpu.GLEdgeMark, 0, 2) : 0;
    // GPU::GLEdgeFast: bit 1 only matters to mode 1's outlines
    EdgeFast = gpu.GLEdgeFast & 3;
    EdgeSet = (EdgeMark == 1 && (EdgeFast & 2) && !EdgeLinesFailed) ? EdgeLineShader : EdgeShader;
#ifdef MELONADE_DEBUG_TOOLS
    FailEdgeLines = (gpu.GLEdgeFast & 4) != 0;
#endif
    OpaqueBox[0] = OpaqueBox[1] = INT32_MAX;
    OpaqueBox[2] = OpaqueBox[3] = INT32_MIN;
    ClearFast = gpu.GLClearFast;
    // GPU::GLTransSkip bit 2: without the extension, or when its programs don't build, the frame
    // draws as without the bit
    FetchClear0 = (GLTransSkip & 4) && (gpu.GPU3D.RenderClearAttr1 & 0x001F0000) == 0 && PrepareFetchShaders();
    // GPU::GLNoDiscard: Z-buffer frames only (RenderSceneChunk picks the shader by the first polygon,
    // as here), and not while SCENEFRAGS has the counting builds in (there's no counting build of
    // the no-discard program)
    NoDiscard = gpu.GLNoDiscard && gpu.GPU3D.RenderNumPolygons && !gpu.GPU3D.RenderPolygonRAM[0]->WBuffer;
#ifdef MELONADE_DEBUG_TOOLS
    if (FragCounter) NoDiscard = false;
#endif
    if (NoDiscard) NoDiscard = PrepareNoDiscardShader();

    // debug: log the last frame's masks and shadows, its queries should be done by now
    if (NumShadowLogEntries) FlushShadowLog();
    ShadowLogActive = gpu.ShadowLogFrames > 0;
    if (ShadowLogActive) gpu.ShadowLogFrames--;
    ShadowLogFrame++;

    for (int i = 0; i < 32; i++)
    {
        u16 c = gpu.GPU3D.RenderToonTable[i];
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;

        ShaderConfig.uToonColors[i][0] = (float)r / 31.0;
        ShaderConfig.uToonColors[i][1] = (float)g / 31.0;
        ShaderConfig.uToonColors[i][2] = (float)b / 31.0;
    }

    for (int i = 0; i < 8; i++)
    {
        u16 c = gpu.GPU3D.RenderEdgeTable[i];
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;

        ShaderConfig.uEdgeColors[i][0] = (float)r / 31.0;
        ShaderConfig.uEdgeColors[i][1] = (float)g / 31.0;
        ShaderConfig.uEdgeColors[i][2] = (float)b / 31.0;
    }

    {
        u32 c = gpu.GPU3D.RenderFogColor;
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;
        u32 a = (c >> 16) & 0x1F;

        ShaderConfig.uFogColor[0] = (float)r / 31.0;
        ShaderConfig.uFogColor[1] = (float)g / 31.0;
        ShaderConfig.uFogColor[2] = (float)b / 31.0;
        ShaderConfig.uFogColor[3] = (float)a / 31.0;
    }

    for (int i = 0; i < 34; i++)
    {
        u8 d = gpu.GPU3D.RenderFogDensityTable[i];
        ShaderConfig.uFogDensity[i][0] = (float)d / 127.0;
    }

    ShaderConfig.uFogOffset = gpu.GPU3D.RenderFogOffset;
    ShaderConfig.uFogShift = gpu.GPU3D.RenderFogShift;

    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    void* unibuf = glMapBufferRange(GL_UNIFORM_BUFFER, 0, sizeof(ShaderConfig), GL_MAP_WRITE_BIT);
    if (unibuf) memcpy(unibuf, &ShaderConfig, sizeof(ShaderConfig));
    glUnmapBuffer(GL_UNIFORM_BUFFER);

    // the fog pass and the compositor rebind units 0 and 1 every frame,
    // so the render and clear-bitmap shaders need these bound again here
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, TexMemID);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, TexPalMemID);
    // GPU::GLTexCache: units 2 and 3, which the compositor rebinds too
    if (TexHeap)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, TexHeapID);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, TexPalLinearID);
        // GPU::GLTexCache 128: the color plane on unit 4, which the compositor rebinds too
        if (TexColorID)
        {
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, TexColorID);
        }
        glActiveTexture(GL_TEXTURE1);
    }

    if (textureChanged)
    {
        // TexMem is 1024 texels wide: one row is two dirty chunks of a texture slot
        constexpr u32 chunksPerRow = 1024 / VRAMDirtyGranularity;
        constexpr u32 numRows = sizeof(gpu.VRAMFlat_Texture) / 1024;

        glActiveTexture(GL_TEXTURE0);
        for (u32 row = 0; row < numRows; )
        {
            if (!AnyDirty(textureDirty, row*chunksPerRow, chunksPerRow)) { row++; continue; }

            u32 start = row;
            while (row < numRows && AnyDirty(textureDirty, row*chunksPerRow, chunksPerRow)) row++;

            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, start, 1024, row - start, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                &gpu.VRAMFlat_Texture[start * 1024]);
        }
    }

    if (texPalChanged)
    {
        // TexPalMem is 1024 16-bit texels wide: one row is four dirty chunks, 8 rows per 16K slot.
        // the 15-bit palette entries are swizzled into RGB5_A1 on the way
        constexpr u32 chunksPerRow = 2048 / VRAMDirtyGranularity;
        constexpr u32 numRows = 6 * 8;

        glActiveTexture(GL_TEXTURE1);
        for (u32 row = 0; row < numRows; )
        {
            if (!AnyDirty(texPalDirty, row*chunksPerRow, chunksPerRow)) { row++; continue; }

            u32 start = row;
            while (row < numRows && AnyDirty(texPalDirty, row*chunksPerRow, chunksPerRow)) row++;

            const u16* src = (const u16*)&gpu.VRAMFlat_TexPal[start * 2048];
            u16* dst = &TexPalConvBuffer[start * 1024];
            for (u32 j = 0; j < (row - start) * 1024; j++)
            {
                u16 value = src[j];

                u8 a = (value >> 15) & 0x1;
                u8 b = (value >> 10) & 0x1F;
                u8 g = (value >> 5) & 0x1F;
                u8 r = (value >> 0) & 0x1F;

                dst[j] = (r << 11) | (g << 6) | (b << 1) | a;
            }

            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, start, 1024, row - start, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dst);
            // GPU::GLTexCache: the same words into TexPalLinear, where a 1024 entry row is four
            // 256 entry rows
            if (TexHeap)
            {
                glActiveTexture(GL_TEXTURE3);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 4 * start, 256, 4 * (row - start), GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dst);
                glActiveTexture(GL_TEXTURE1);
            }
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);

    glViewport(0, 0, ScreenW, ScreenH);

    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);

    // clear buffers
    // TODO: check whether 'clear polygon ID' affects translucent polyID
    // (for example when alpha is 1..30)
    GL_TIMER(Clear);
    {
        u32 polyid = (gpu.GPU3D.RenderClearAttr1 >> 24) & 0x3F;

        // the clear plane's stencil is 0xFF. FetchClear0 (no pass 2) makes it 0xBF: pass 3's
        // translucent test (NOTEQUAL 0x40|id over 0x7F) would take 0x7F for translucent ID 63 and
        // keep those polygons off the clear plane, where pass 2 drew them. bit 6 is read by nothing
        // else in that frame (CountCoveredPixels runs with a clear alpha other than 0)
        GLint clearstencil = FetchClear0 ? 0xBF : 0xFF;
        glDepthFunc(GL_ALWAYS);
        glStencilFunc(GL_ALWAYS, clearstencil, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        bool cleardraw = true;
        if (gpu.GPU3D.RenderDispCnt & (1<<14))
        {
            // clear bitmap: the shader reads it from TexMem, which the dirty tracking above
            // keeps current (and a write to it defeats the identical-frame skip). the bitmap
            // is tiled at native resolution across the whole target, with native x=0 at the
            // centre 256 columns of a wide one, like the capture readback
            glUseProgram(ClearShaderBitmap);

            int xorigin = ((TargetWidth - WideMelon::NativeWidth) / 2);
            int xoff = ((int)((gpu.GPU3D.RenderClearAttr2 >> 16) & 0xFF) - xorigin) & 0xFF;
            // native y=0 at DS line 0, TargetMargin rows down a target with a margin
            int yoff = ((int)((gpu.GPU3D.RenderClearAttr2 >> 24) & 0xFF) - TargetMargin) & 0xFF;

            glUniform1i(ClearBitmapULoc[0], ScaleFactor);
            glUniform2i(ClearBitmapULoc[1], xoff, yoff);
            glUniform1ui(ClearBitmapULoc[2], polyid);
        }
        else if (ClearFast & 1)
        {
            // GPU::GLClearFast bit 0 (D1): what kClearVS and kClearFS write with the quad below.
            // every write mask is open (above) and the scissor test is off, so the clears cover what
            // the quad does, all of MainFramebuffer. same float values as the shader's outputs, which
            // the GL turns into the targets' formats the same way: color (RGBA8, the DS's BGR order
            // swizzled into RGB) and attr (RGB8: poly ID/63, edge flag 0, fog flag) round to the
            // same bytes (k*255/31 and k*255/63 are never near a half), depth is the window z the
            // quad's NDC z gives, z/2^24 (both exact in float), stencil the reference above
            u32 r = gpu.GPU3D.RenderClearAttr1 & 0x1F;
            u32 g = (gpu.GPU3D.RenderClearAttr1 >> 5) & 0x1F;
            u32 b = (gpu.GPU3D.RenderClearAttr1 >> 10) & 0x1F;
            u32 fog = (gpu.GPU3D.RenderClearAttr1 >> 15) & 0x1;
            u32 a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
            u32 z = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;

            const GLfloat color[4] = {(float)b / 31.0f, (float)g / 31.0f, (float)r / 31.0f, (float)a / 31.0f};
            const GLfloat attr[4] = {(float)polyid / 63.0f, 0.0f, (float)fog, 1.0f};
            glClearBufferfv(GL_COLOR, 0, color);
            glClearBufferfv(GL_COLOR, 1, attr);
            glClearBufferfi(GL_DEPTH_STENCIL, 0, (float)z / 16777216.0f, clearstencil);
            cleardraw = false;
        }
        else
        {
            glUseProgram(ClearShaderPlain);

            u32 r = gpu.GPU3D.RenderClearAttr1 & 0x1F;
            u32 g = (gpu.GPU3D.RenderClearAttr1 >> 5) & 0x1F;
            u32 b = (gpu.GPU3D.RenderClearAttr1 >> 10) & 0x1F;
            u32 fog = (gpu.GPU3D.RenderClearAttr1 >> 15) & 0x1;
            u32 a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
            u32 z = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;

            /*if (r) r = r*2 + 1;
            if (g) g = g*2 + 1;
            if (b) b = b*2 + 1;*/

            glUniform4ui(ClearUniformLoc[0], r, g, b, a);
            glUniform1ui(ClearUniformLoc[1], z);
            glUniform1ui(ClearUniformLoc[2], polyid);
            glUniform1ui(ClearUniformLoc[3], fog);
        }

        if (cleardraw)
        {
            glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
            glBindVertexArray(ClearVertexArrayID);
            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }
    }

    if (gpu.GPU3D.RenderNumPolygons)
    {
        GL_TIMER(Upload);
        // render shit here
        int npolys = 0;
        int firsttrans = -1;
        bool texturing = (gpu.GPU3D.RenderDispCnt & (1<<0)) != 0;
        bool anyOpaqueDraw = false;
        for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
        {
            const Polygon* polygon = gpu.GPU3D.RenderPolygonRAM[i];
            if (polygon->Degenerate) continue;

            SetupPolygon(&PolygonList[npolys], gpu.GPU3D.RenderPolygonRAM[i]);
            if (NoDiscard && NoDiscardClass(polygon, texturing) == NoDiscard_Yes)
                PolygonList[npolys].RenderKey |= RenderKey_NoDiscard;
            if (firsttrans < 0 && polygon->Translucent)
                firsttrans = npolys;
            // GPU::GLEdgeMark: what can set the edge flag, opaque and need-opaque polygons
            if (!polygon->IsShadowMask && (!polygon->Translucent || (polygon->Attr & 0x001F0000) == 0x001F0000))
                anyOpaqueDraw = true;

            npolys++;
        }
        NumFinalPolys = npolys;
        NumOpaqueFinalPolys = firsttrans;
        // GPU::GLNoDiscard bit 1 (R1 step 2): the textures that need a scan
        if (NoDiscard && (gpu.GLNoDiscard & 2))
            MarkScannedNoDiscard(gpu, texturing);
        // GPU::GLTexCache: the polygons whose texture the heap holds (or can decode now) read it
        if (TexCache && TexHeap)
        {
            // step 2: the twins with the old decode path, the first frame a polygon needs one
            if (AssignTexCache(gpu, texturing) && !PrepareFullShaders())
                TexCacheOff();
        }
        // GPU::GLEdgeMark 1: the edge-marking builds of this frame's opaque programs, which are
        // settled now (the twins and TexCacheOff above). without them the frame marks nothing
        if (!anyOpaqueDraw || (EdgeMark != 0 && !PrepareEdgeShaders()))
            EdgeMark = 0;

        BuildPolygons(&PolygonList[0], npolys);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices*7*4, VertexBuffer);
        if (EdgeMark != 0)
        {
            glBindBuffer(GL_ARRAY_BUFFER, EdgeInfoBufferID);
            glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices*4, EdgeInfoBuffer);
        }
        if (EdgeMark == 1 && EdgeSet == EdgeLineShader)
        {
            glBindBuffer(GL_ARRAY_BUFFER, EdgeLineBufferID);
            glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices*12*4, EdgeLineBuffer);
        }
        if (EdgeMark == 1)
        {
            // the outlines BuildPolygons made (BuildEdges), whole rows of EdgeTex
            glActiveTexture(GL_TEXTURE5);
            if (!EdgeTexID)
            {
                glGenTextures(1, &EdgeTexID);
                glBindTexture(GL_TEXTURE_2D, EdgeTexID);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, EdgeTexWidth, MaxEdges / EdgeTexWidth, 0, GL_RGBA, GL_FLOAT, nullptr);
            }
            else
                glBindTexture(GL_TEXTURE_2D, EdgeTexID);
            u32 rows = (NumEdges + EdgeTexWidth - 1) / EdgeTexWidth;
            if (rows)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, EdgeTexWidth, rows, GL_RGBA, GL_FLOAT, EdgeData);
            glActiveTexture(GL_TEXTURE0);
        }

        // bind to access the index buffer
        glBindVertexArray(VertexArrayID);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, NumIndices * 2, IndexBuffer);

        // VRAMFlat_Texture is what TexMem was just brought up to date from
        bool classify = (GLTransSkip & 2) != 0;
#ifdef MELONADE_DEBUG_TOOLS
        if (TransStats)
        {
            TransStats->Sec.Frames++;
            classify = true;
        }
#endif
        if (classify)
            ClassifyTransAlpha(gpu);
#ifdef MELONADE_DEBUG_TOOLS
        // after ClassifyTransAlpha, whose classes it reads (TransStats makes classify true)
        if (OpaqueStats)
            CountOpaqueR1(gpu);
#endif

#ifdef MELONADE_DEBUG_TOOLS
        if (FragCounter)
            FragCounter->Ring.BeginFrame(GLFragCounter::Binding, ScreenW * ScreenH);
#endif
        RenderSceneChunk(gpu.GPU3D, 0, 192);
#ifdef MELONADE_DEBUG_TOOLS
        if (FragCounter)
        {
            // with a clear alpha of 0 RenderSceneChunk counted them already
            if ((gpu.GPU3D.RenderClearAttr1 & 0x001F0000) != 0)
                CountCoveredPixels();
            FragCounter->Ring.EndFrame();
        }
#endif
    }
    else
        NumFinalPolys = 0;

    if (ClearFast & 2)
    {
        // GPU::GLClearFast bit 1 (D1): attr, depth and stencil are only read during the frame (the
        // passes, the fog and edge pass, CountCoveredPixels above), and the next frame's clear
        // writes all of them before anything reads them again. what's read after the frame is
        // the color target only: the capture readback, the hi-res capture, hash3d and the
        // compositor, and a frame skipped as identical keeps using that. so a tiled GPU needn't
        // store these two to memory
        const GLenum attachments[2] = {GL_COLOR_ATTACHMENT1, GL_DEPTH_STENCIL_ATTACHMENT};
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, attachments);
    }

#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->EndSet();
#endif
    if (ShadowLogActive) LogFrameStats(gpu.GPU3D);
}

void GLRenderer::Stop(const GPU& gpu)
{
    CurGLCompositor.Stop(gpu);
    EarlyCapture = false;
}

void GLRenderer::PrepareCaptureFrame()
{
    ReadBackCaptureFrame(PixelbufferID);
}

// the downscale target is only ever read back, so the early readback writing it changes nothing
// a capture sees
void GLRenderer::ReadBackCaptureFrame(GLuint pixelBuffer) noexcept
{
#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
    {
        Timers->BeginSet(GLTimerStats::Set_Capture);
        Timers->Begin(GLPass::Capture);
    }
#endif
    glBindFramebuffer(GL_READ_FRAMEBUFFER, MainFramebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DownscaleFramebuffer);
    GLenum bufferAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &bufferAttachment);
    // the game only ever sees the native 256 it asked for, so capture
    // (and GetLine) read back the centre of a wide target, and its middle 192 rows with a margin
    int captureX = ((TargetWidth - WideMelon::NativeWidth) / 2) * ScaleFactor;
    int captureY = TargetMargin * ScaleFactor;
    glBlitFramebuffer(captureX, captureY, captureX + WideMelon::NativeWidth * ScaleFactor, captureY + 192 * ScaleFactor,
                      0, 0, 256, 192, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixelBuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, DownscaleFramebuffer);
    glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->EndSet();
#endif
}

void GLRenderer::FlushCommands()
{
    glFlush();
}

void GLRenderer::DropPaceFences() noexcept
{
    for (int i = 0; i < PaceFenceCount; i++)
        glDeleteSync(PaceFences[i]);
    PaceFenceCount = 0;
}

// the oldest count fences are done with
void GLRenderer::PopPaceFences(int count) noexcept
{
    for (int i = 0; i < count; i++)
        glDeleteSync(PaceFences[i]);
    PaceFenceCount -= count;
    std::copy(PaceFences + count, PaceFences + count + PaceFenceCount, PaceFences);
    std::copy(PaceFenceNs + count, PaceFenceNs + count + PaceFenceCount, PaceFenceNs);
}

// a fence still unsignaled this long after it was made (by the clock: the emulator was paused, or
// the GPU is stuck) holds nothing
static constexpr s64 PaceStaleNs = 500000000;

// GPU::GLPace, at the start of every Blit: drops the fences of the frames the GPU has finished,
// which leaves one for each that's still unfinished, and logs once a second. the GPU runs them in
// order, so it stops at the first that isn't done. a poll that fails counts as done, so it can't
// get stuck. the polls don't flush: the frontend's frame-end flush has sent the fences (WaitForPace
// flushes for the times it doesn't run)
void GLRenderer::UpdatePace(const GPU& gpu) noexcept
{
    if (gpu.GLPace == 0)
    {
        DropPaceFences();
        return;
    }

    s64 now = PaceNowNs();
    int done = 0;
    while (done < PaceFenceCount)
    {
        bool signaled = glClientWaitSync(PaceFences[done], 0, 0) != GL_TIMEOUT_EXPIRED;
        if (!signaled && now - PaceFenceNs[done] < PaceStaleNs)
            break;
        PaceLog.Stale += !signaled;
        done++;
    }
    PopPaceFences(done);
    PaceLog.MaxPending = std::max(PaceLog.MaxPending, PaceFenceCount);

    // debug only (debug.melonade.glpace), once a second: frames got a fence, waits counts the Blits
    // that waited for an earlier frame, wait_ms and maxwait_ms are their time in all and the longest
    // one. timeouts gave up after 100 ms, stale fences were dropped unsignaled for their age, and
    // maxpending is the most frames left unfinished at a Blit
    if (!gpu.GLPaceLog)
        return;
    if (PaceLog.StartNs == 0)
        PaceLog.StartNs = now;
    if (now - PaceLog.StartNs >= 1000000000)
    {
        Platform::Log(Platform::LogLevel::Info, "GLPACE mode=%d frames=%u waits=%u wait_ms=%.1f maxwait_ms=%.1f "
            "timeouts=%u stale=%u maxpending=%d\n",
            gpu.GLPace, PaceLog.Frames, PaceLog.Waits, PaceLog.WaitNs / 1e6, PaceLog.MaxWaitNs / 1e6,
            PaceLog.Timeouts, PaceLog.Stale, PaceLog.MaxPending);
        PaceLog = {};
        PaceLog.StartNs = now;
    }
}

// GPU::GLPace: holds the emulator thread until at most one earlier frame is unfinished, so the
// driver's queue stays about a frame deep and a GPU that falls behind costs fps instead of piling
// up delay. 100 ms at most, and a fence that turns stale meanwhile stops holding, so one that never
// signals can't hang the thread. the first wait flushes, as the newest fences aren't sent yet when
// the frontend's frame-end flush didn't run (it doesn't while the DS sleeps)
void GLRenderer::WaitForPace() noexcept
{
    s64 start = PaceNowNs();
    s64 deadline = start + 100000000;
    GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
    PaceLog.Waits++;
    while (PaceFenceCount > 1)
    {
        s64 now = PaceNowNs();
        s64 staleAt = PaceFenceNs[0] + PaceStaleNs;
        if (now >= staleAt)
        {
            PopPaceFences(1);
            PaceLog.Stale++;
            continue;
        }
        if (now >= deadline)
        {
            PaceLog.Timeouts++;
            break;
        }

        GLenum result = glClientWaitSync(PaceFences[0], flags, (GLuint64)(std::min(deadline, staleAt) - now));
        flags = 0;
        if (result != GL_TIMEOUT_EXPIRED)
            PopPaceFences(1);
    }
    s64 waited = PaceNowNs() - start;
    PaceLog.WaitNs += waited;
    PaceLog.MaxWaitNs = std::max(PaceLog.MaxWaitNs, waited);
}

void GLRenderer::Blit(const GPU& gpu)
{
    // GPU::GLPace: before this frame's compositor goes in the queue, the frame before the last must
    // be done. the Blit's the spot as its fence covers the whole frame. a frame that mapped a
    // capture readback (at line 0, or at line 192 for held lines) already waited for the GPU to get
    // past that fence, so the two never both wait
    if (gpu.GLPace != 0 || PaceFenceCount != 0)
        UpdatePace(gpu);
    if (gpu.GLPace != 0 && PaceFenceCount >= 2)
        WaitForPace();

#ifdef MELONADE_DEBUG_TOOLS
    // the compositor's GPU timers, which it starts through BeginGPUTimer
    if (Timers)
        Timers->BeginSet(GLTimerStats::Set_Blit);
#endif
    CurGLCompositor.RenderFrame(gpu, *this);
#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->EndSet();
#endif

    // GPU::GLPace: signals when the GPU has finished this frame, the compositor and everything
    // before it, the 3D render it shows too. not flushed here, which would send the compositor off
    // earlier than it goes now: the frontend's frame-end flush (or the one after a late Blit) does
    if (gpu.GLPace != 0)
    {
        if (PaceFenceCount == MaxPaceFences)
            PopPaceFences(1);
        PaceFences[PaceFenceCount] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        PaceFenceNs[PaceFenceCount++] = PaceNowNs();
        PaceLog.Frames++;
    }

    // the compositor and its hi-res capture pass were the last to read the kept frame, and GL
    // keeps their reads ahead of the render that next draws into it
    if (KeptColorTex)
    {
        SpareColorTex = KeptColorTex;
        KeptColorTex = 0;
    }
}

// GPU::BlitAfterReadback: line 0's readback, made right after the render at line 215, into the
// early buffer, so GetLine's stays as the last line-0 readback left it until line 0 takes this.
// it reads MainFramebuffer's centre, and whatever could change that before line 0 clears
// EarlyCapture: a render (HalfCamera's redraw at line 0 too), a resize, a reset, a stop or a
// savestate load
void GLRenderer::PrepareEarlyCaptureFrame()
{
    ReadBackCaptureFrame(EarlyPixelbufferID);
    EarlyCapture = true;
}

// at line 0, with use where PrepareCaptureFrame would run: the early buffer holds what it would
// write into GetLine's, so the two swap instead. without use, or once it's stale, GetLine's buffer
// stays as it is, and the next early readback overwrites the other
bool GLRenderer::TakeEarlyCaptureFrame(bool use)
{
    bool early = use && EarlyCapture;
    EarlyCapture = false;
    if (early)
        std::swap(PixelbufferID, EarlyPixelbufferID);
    return early;
}

void GLRenderer::SetOutputTexture(int buffer, u32 texture)
{
    CurGLCompositor.SetOutputTexture(buffer, (GLuint) texture);
}

void GLRenderer::BindOutputTexture(int buffer)
{
    CurGLCompositor.BindOutputTexture(buffer);
}

void GLRenderer::SavestateLoaded()
{
    CurGLCompositor.ResetWingHold();
    CurGLCompositor.ResetHiresCapture();
    CurGLCompositor.ResetFilter2D();
    CurGLCompositor.ResetShadow2D();
    // line 0 reads back MainFramebuffer again, as it did before the early readback
    EarlyCapture = false;
    // GPU::GLTexCache: nothing to do. the load ends in GPU::ResetVRAMCache, so every texture and
    // palette chunk counts as changed at the next frame's DeriveState: the records are rehashed
    // against VRAM, and (128, step 4b) every color plane's palette words against the loaded palette
    // (step 4a had no palette tracking and cleared the heap here)
}

u32* GLRenderer::GetLine(int line)
{
    int stride = 256;

    if (line == 0)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
        u8* data = (u8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * stride * 192, GL_MAP_READ_BIT);
        if (data) memcpy(&Framebuffer[stride*0], data, 4*stride*192);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    u64* ptr = (u64*)&Framebuffer[stride * line];
    for (int i = 0; i < stride; i+=2)
    {
        // Data is in BGRA format but we need to convert it to RGBA
        u64 redBits = (*ptr & 0x000000FC000000FC) << 16;
        u64 blueBits = (*ptr & 0x00FC000000FC0000) >> 16;

        u64 rgb = (*ptr & 0x0000FC000000FC00) | redBits | blueBits;
        u64 a = *ptr & 0xF8000000F8000000;

        *ptr++ = (rgb >> 2) | (a >> 3);
    }

    return &Framebuffer[stride * line];
}

void GLRenderer::SetupAccelFrame()
{
    // the 3D frame of the Blit's own frame, kept while the next one is drawn (see KeepFrameForBlit)
    glBindTexture(GL_TEXTURE_2D, KeptColorTex ? KeptColorTex : ColorBufferTex);
}

// debug only (GPU::GLTimers): the compositor's parts (ComputePass::Comp and the ones after it), in
// the Blit's set
void GLRenderer::BeginGPUTimer(int pass)
{
#ifdef MELONADE_DEBUG_TOOLS
    static_assert(GLPass::Count - GLPass::Comp == ComputePass::Count - ComputePass::FirstBlit,
                  "GLPass has ComputePass's compositor parts, in the same order");
    if (Timers)
        Timers->Begin(pass >= ComputePass::FirstBlit && pass < ComputePass::Count
                      ? GLPass::Comp + (pass - ComputePass::FirstBlit) : GLPass::CompExtra);
#endif
}

void GLRenderer::EndGPUTimer()
{
#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->End();
#endif
}

// debug only (GPU::Hash3DLogging): the frame RenderFrame left in MainFramebuffer (drawn or kept as
// identical), downscaled into the capture's target the way the capture readback does it, and read back
// into the hash ring. the capture readbacks blit into that target again before they read it
void GLRenderer::QueueHash3D(GPU& gpu, u32 frame)
{
#ifdef MELONADE_DEBUG_TOOLS
    if (!Hash3D)
        Hash3D = std::make_unique<Hash3DReadback>(true);

    GLint readFB = 0, drawFB = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor)
        glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, MainFramebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DownscaleFramebuffer);
    GLenum bufferAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &bufferAttachment);
    int captureX = ((TargetWidth - WideMelon::NativeWidth) / 2) * ScaleFactor;
    int captureY = TargetMargin * ScaleFactor;
    glBlitFramebuffer(captureX, captureY, captureX + WideMelon::NativeWidth * ScaleFactor, captureY + 192 * ScaleFactor,
                      0, 0, 256, 192, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, DownscaleFramebuffer);
    Hash3D->Queue(frame);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFB);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
#endif
}

#ifdef MELONADE_DEBUG_TOOLS
// at the start of every RenderFrame while GPU::GLTimers or GPU::SceneStats 2 is on, or their stats
// still exist: makes the timers when the switch is on (again if it changed), drops them when it's
// off, sets up the fragment counter once, and lets them read finished results and log
void GLRenderer::UpdateDebugStats(const GPU& gpu)
{
    int mode = (gpu.GLTimers == 1 || gpu.GLTimers == 2) ? gpu.GLTimers : 0;
    if (!Timers || Timers->Mode != mode)
    {
        Timers.reset();
        if (mode != 0)
            Timers = std::make_unique<GLTimerStats>(mode);
    }

    if (gpu.SceneStats == 2 && !FragCountTried)
    {
        FragCountTried = true;
        SetupFragCount();
    }

    s64 now = GLStatsNowNs();
    if (Timers)
        Timers->FrameStart(now, ScaleFactor);
    if (FragCounter)
        FragCounter->FrameStart(now);
}

// GPU::SceneStats 2: the render programs are rebuilt as their counting builds, for the rest of the
// renderer's life. needs SSBOs in fragment shaders, which GLES 3.1 and 3.2 allow a driver to leave out
void GLRenderer::SetupFragCount()
{
    GLint fragmentBlocks = 0, bindings = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragmentBlocks);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
    if (fragmentBlocks < 1 || bindings < 1)
    {
        Platform::Log(Platform::LogLevel::Warn, "SCENEFRAGS: no SSBOs in fragment shaders here (GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS=%d, "
            "bindings=%d), scenestats 2 runs as 1\n", fragmentBlocks, bindings);
        return;
    }

    GLuint cover = 0, coverAlpha = 0;
    if (!OpenGL::CompileVertexFragmentProgram(cover, kCoverVS, kCoverFS, "CoverShader", {{"vPosition", 0}}, {{"oColor", 0}}))
    {
        Platform::Log(Platform::LogLevel::Warn, "SCENEFRAGS: the cover program didn't build, scenestats 2 runs as 1\n");
        return;
    }
    if (!OpenGL::CompileComputeProgram(coverAlpha, kCoverAlphaCS, "CoverAlphaShader"))
    {
        glDeleteProgram(cover);
        Platform::Log(Platform::LogLevel::Warn, "SCENEFRAGS: the alpha cover program didn't build, scenestats 2 runs as 1\n");
        return;
    }

    GLuint normal[2] = { RenderShader[0], RenderShader[1] };
    if (!BuildRenderShader(false, true))
    {
        glDeleteProgram(cover);
        glDeleteProgram(coverAlpha);
        Platform::Log(Platform::LogLevel::Warn, "SCENEFRAGS: the counting render programs didn't build, scenestats 2 runs as 1\n");
        return;
    }
    if (!BuildRenderShader(true, true))
    {
        // back to the normal Z-buffer one
        glDeleteProgram(RenderShader[0]);
        BuildRenderShader(false, false);
        glDeleteProgram(normal[0]);
        glDeleteProgram(cover);
        glDeleteProgram(coverAlpha);
        Platform::Log(Platform::LogLevel::Warn, "SCENEFRAGS: the counting render programs didn't build, scenestats 2 runs as 1\n");
        return;
    }
    glDeleteProgram(normal[0]);
    glDeleteProgram(normal[1]);
    // the framebuffer fetch builds (GPU::GLTransSkip bit 2) are made again as counting builds the
    // next time a frame wants them
    glDeleteProgram(RenderShader[2]);
    glDeleteProgram(RenderShader[3]);
    RenderShader[2] = RenderShader[3] = 0;
    FragPassLoc[2] = FragPassLoc[3] = -1;
    // and GPU::GLTexCache step 2's twins, the next frame a polygon needs them, and GPU::GLEdgeMark 1's
    // builds, the next edge-marking frame
    DropFullShaders();
    DropEdgeShaders();
    FetchShadersTried = false;
    CurShaderID = -1;
    CurRenderMode = -1;

    CoverShader = cover;
    CoverAlphaShader = coverAlpha;
    CoverAlphaSizeLoc = glGetUniformLocation(coverAlpha, "uSize");
    glUseProgram(coverAlpha);
    glUniform1i(glGetUniformLocation(coverAlpha, "uColor"), 3);
    FragCounter = std::make_unique<GLFragCounter>();
    Platform::Log(Platform::LogLevel::Info, "SCENEFRAGS: counting fragments with an atomic per fragment, which slows the renderer down\n");
}

// the counter the render programs add to from here on, set in both
void GLRenderer::SetFragPass(int pass)
{
    for (int i = 0; i < NumRenderShaders; i++)
    {
        if (RenderShader[i] && FragPassLoc[i] >= 0)
            glProgramUniform1i(RenderShader[i], FragPassLoc[i], pass);
    }
}

// after the frame's last draw: counts the pixels whose stencil a polygon wrote (see GLFragCounter),
// and leaves the state as it found it
void GLRenderer::CountCoveredPixels()
{
    GLboolean colorMask[2][4];
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, colorMask[0]);
    glGetBooleani_v(GL_COLOR_WRITEMASK, 1, colorMask[1]);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLint depthFunc = 0, stencilFunc = 0, stencilRef = 0, stencilValueMask = 0, stencilWriteMask = 0;
    GLint stencilFail = 0, stencilDepthFail = 0, stencilPass = 0, program = 0, vertexArray = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &stencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilValueMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask);
    glGetIntegerv(GL_STENCIL_FAIL, &stencilFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilDepthFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilPass);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean stencilTest = glIsEnabled(GL_STENCIL_TEST);

    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NOTEQUAL, 0x7F, 0x7F);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0);

    glUseProgram(CoverShader);
    glBindVertexArray(ClearVertexArrayID);
    glDrawArrays(GL_TRIANGLES, 0, 2*3);

    glColorMaski(0, colorMask[0][0], colorMask[0][1], colorMask[0][2], colorMask[0][3]);
    glColorMaski(1, colorMask[1][0], colorMask[1][1], colorMask[1][2], colorMask[1][3]);
    glDepthFunc(depthFunc);
    glDepthMask(depthMask);
    glStencilFunc(stencilFunc, stencilRef, stencilValueMask);
    glStencilOp(stencilFail, stencilDepthFail, stencilPass);
    glStencilMask(stencilWriteMask);
    if (!depthTest)
        glDisable(GL_DEPTH_TEST);
    if (!stencilTest)
        glDisable(GL_STENCIL_TEST);
    glUseProgram(program);
    glBindVertexArray(vertexArray);
}

// with a clear alpha of 0, before fog: the pixels of the color target with alpha (see kCoverAlphaCS).
// with the clear bitmap, its own pixels with alpha count too. leaves the state as it found it
void GLRenderer::CountCoveredByAlpha()
{
    GLint program = 0, activeTexture = 0, texture = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

    glBindTexture(GL_TEXTURE_2D, ColorBufferTex);
    glUseProgram(CoverAlphaShader);
    glUniform2i(CoverAlphaSizeLoc, ScreenW, ScreenH);
    glDispatchCompute((ScreenW + 15) / 16, (ScreenH + 15) / 16, 1);

    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    glActiveTexture((GLenum)activeTexture);
    glUseProgram(program);
    CurShaderID = -1;
    CurRenderMode = -1;
}
#endif

}
