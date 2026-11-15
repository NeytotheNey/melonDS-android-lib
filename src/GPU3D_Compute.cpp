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

#include "GPU3D_Compute.h"

#include <assert.h>
#include <cstdarg>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <thread>
#include <utility>

#include "Utils.h"
#include "Platform.h"
#include "WideMelon.h"

#include "OpenGLSupport.h"
#include "NDS.h"
#include "NDSCart.h"

#include "GPU3D_Compute_shaders.h"

namespace melonDS
{

static s64 SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// debug builds: adds the time since the last lap to one of RenderFrame's COMPSLOW phases
#ifdef MELONADE_DEBUG_TOOLS
#define SLOW_LAP(phase) SlowLap(Slow.phase)
#else
#define SLOW_LAP(phase) ((void)0)
#endif

ComputeRenderer::ComputeRenderer(GLCompositor&& compositor)
    : Renderer3D(true), Texcache(TexcacheOpenGLLoader()), CurGLCompositor(std::move(compositor))
{}

#ifdef MELONADE_DEBUG_TOOLS
// debug only (GPU::ComputeDepthStats, ComputeDepthCounter): DepthBlend with its counters, made from
// the real source by inserting them at its lines, so the source without them stays as it is. false
// when a line it looks for isn't there (DepthBlend changed), and then nothing is counted
static bool AddDepthStats(std::string& src)
{
    size_t start = src.find("void ProcessCoarseMask(");
    if (start == std::string::npos)
        return false;
    auto insertAt = [&](const char* anchor, const char* text, bool after) -> bool
    {
        size_t pos = src.find(anchor, start);
        if (pos == std::string::npos)
            return false;
        src.insert(after ? pos + strlen(anchor) : pos, text);
        return true;
    };

    // walking a polygon's work tile, finding a colored pixel there, failing the first depth test (a
    // shadow mask's sets the stencil)
    if (!insertAt("workIdx++;", " DepthStatsVisited++;", true)) return false;
    {
        size_t pos = src.find("if (tileColor != 0U)", start);
        size_t brace = pos == std::string::npos ? std::string::npos : src.find('{', pos);
        if (brace == std::string::npos) return false;
        src.insert(brace + 1, " DepthStatsColored++;");
    }
    if (!insertAt("if ((dstattr & 0x3U) == 0U || writeSecondLayer)", "DepthStatsFailed++; ", false)) return false;
    {
        const char* anchor = "stencil = 0x1U;";
        size_t pos = src.find(anchor, start);
        if (pos == std::string::npos) return false;
        src.replace(pos, strlen(anchor), "{ stencil = 0x1U; DepthStatsFailed++; }");
    }
    {
        const char* anchor = "void main()";
        size_t pos = src.find(anchor, start);
        if (pos == std::string::npos) return false;
        src.replace(pos, strlen(anchor), "void DepthBlendMain()");
    }

    // the counters, declared before the functions that count, and a main that sums them per work group
    size_t decl = src.find("void PlotTranslucent(");
    if (decl == std::string::npos || decl > start)
        return false;
    src.insert(decl,
        "layout (std430, binding = 8) buffer DepthStatsBuffer { uint DepthStatsCount[]; };\n"
        "shared uint DepthStatsShared[3];\n"
        "uint DepthStatsVisited = 0U, DepthStatsColored = 0U, DepthStatsFailed = 0U;\n\n");
    src +=
        "\nvoid main()\n"
        "{\n"
        "    if (gl_LocalInvocationIndex == 0U)\n"
        "    {\n"
        "        DepthStatsShared[0] = 0U; DepthStatsShared[1] = 0U; DepthStatsShared[2] = 0U;\n"
        "    }\n"
        "    memoryBarrierShared();\n"
        "    barrier();\n"
        "    DepthBlendMain();\n"
        "    atomicAdd(DepthStatsShared[0], DepthStatsVisited);\n"
        "    atomicAdd(DepthStatsShared[1], DepthStatsColored);\n"
        "    atomicAdd(DepthStatsShared[2], DepthStatsFailed);\n"
        "    memoryBarrierShared();\n"
        "    barrier();\n"
        "    if (gl_LocalInvocationIndex == 0U)\n"
        "    {\n"
        "        atomicAdd(DepthStatsCount[0], DepthStatsShared[0]);\n"
        "        atomicAdd(DepthStatsCount[1], DepthStatsShared[1]);\n"
        "        atomicAdd(DepthStatsCount[2], DepthStatsShared[2]);\n"
        "    }\n"
        "}\n";
    return true;
}
#endif

bool ComputeRenderer::CompileShader(GLuint& shader, const std::string& source, const std::initializer_list<const char*>& defines)
{
    std::string shaderFileName = "shader_";
    int i = 0;

    std::string shaderName;
    std::string shaderSource;
    shaderSource += "#version 320 es\n";
    shaderSource += "precision highp float;\n";
    shaderSource += "precision highp int;\n";
    shaderSource += "precision highp usampler2DArray;\n";
    shaderSource += "precision highp usampler2D;\n";
    shaderSource += "precision highp image2D;\n";
    shaderSource += "precision highp uimage2D;\n";
    for (const char* define : defines)
    {
        if (i++ > 0) shaderFileName += '_';
        shaderFileName += define;

        shaderSource += "#define ";
        shaderSource += define;
        shaderSource += '\n';
        shaderName += define;
        shaderName += ',';
    }
    // GPU::ComputeBin and ComputeSSBO, Mali diagnostics: defines in the two programs they change
    if (&source == &ComputeRendererShaders::BinCombined && BinMode != 0)
    {
        shaderSource += BinMode == 1 ? "#define BinFirstPass32\n" : "#define BinNoShared\n";
        if (BinMode == 3)
            shaderSource += "#define BinPlainPack\n";
    }
    if (&source == &ComputeRendererShaders::InterpSpans && SetupSSBO)
        shaderSource += "#define SetupIndicesSSBO\n";
    // GPU::ComputeCalc 1: CalcOffsets with the old atomic offsets
    if (&source == &ComputeRendererShaders::CalcOffsets && CalcMode != 0)
        shaderSource += "#define CalcAtomic\n";
    // GPU::ComputeFlat 0: VariantWorkCount as uvec4s again, in every program with the binning buffer
    if (FlatMode == 0 && source.find("BinResultBuffer") != std::string::npos)
        shaderSource += "#define VariantCountVec4\n";
    // GPU::ComputeOpt: a define for each bit. with none of them the source is what it was
    static const char* const optDefines[] = {"OptLinearNoDivide", "OptFieldLoads", "OptSecondLayer", "OptSmall"};
    for (int bit = 0; bit < 4; bit++)
    {
        if (OptMask & (1 << bit))
        {
            shaderSource += "#define ";
            shaderSource += optDefines[bit];
            shaderSource += '\n';
        }
    }
    // bits 7 and 8 go by the tile memory's layout, as one buffer can fall back to two
    // (TileLayersFor). with all three layers the source is what it was
    if (TileLayers < tilememoryLayer_Num)
        shaderSource += "#define OptPackAttr\n";
    if (TileLayers == 1)
        shaderSource += "#define OptOneTileBuffer\n";
    shaderSource += "#define ScreenWidth ";
    shaderSource += std::to_string(ScreenWidth);
    shaderSource += "\n#define ScreenHeight ";
    shaderSource += std::to_string(ScreenHeight);
    shaderSource += "\n#define MaxWorkTiles ";
    shaderSource += std::to_string(MaxWorkTiles);
    shaderSource += "\n#define MaxPolygons ";
    shaderSource += std::to_string(MaxPolygons);
    shaderSource += "\n#define PolyIdxBits ";
    shaderSource += std::to_string(PolyIdxBits());
    shaderSource += "\n#define TileSize ";
    shaderSource += std::to_string(TileSize);
    shaderSource += "\nconst int CoarseTileCountY = ";
    shaderSource += std::to_string(CoarseTileCountY) + ";";
    shaderSource += "\n#define CoarseTileArea ";
    shaderSource += std::to_string(CoarseTileArea);
    shaderSource += "\n#define ClearCoarseBinMaskLocalSize ";
    shaderSource += std::to_string(ClearCoarseBinMaskLocalSize);
    if (TargetWidth != WideMelon::NativeWidth)
    {
        // WideMelon: the target's real width in pixels (ScreenWidth is rounded up to whole tiles),
        // and where its centre 256 starts in native pixels, for capture
        shaderSource += "\n#define WideOutputWidth ";
        shaderSource += std::to_string(TargetWidth * ScaleFactor);
        shaderSource += "\n#define WideLowResOffset ";
        shaderSource += std::to_string((TargetWidth - WideMelon::NativeWidth) / 2);
    }

    shaderSource += ComputeRendererShaders::Common;
#ifdef MELONADE_DEBUG_TOOLS
    // debug only: DepthBlend with the COMPDEPTH counters (GPU::ComputeDepthStats)
    if (DepthStatsOn && &source == &ComputeRendererShaders::DepthBlend)
    {
        std::string counted = source;
        if (AddDepthStats(counted))
        {
            shaderSource += counted;
            shaderName += "DepthStats,";
        }
        else
        {
            Platform::Log(Platform::LogLevel::Warn, "COMPDEPTH: DepthBlend's source isn't what the counters expect, staying off\n");
            DepthStatsOn = false;
            DepthStatsFailed = true;
            shaderSource += source;
        }
    }
    else
#endif
    shaderSource += source;

    std::string filePath = "/data/data/me.magnum.melonds.dev/files/" + shaderFileName + ".txt";
    //FILE* f = fopen(filePath.c_str(), "w");
    //if (f)
    //{
    //    fwrite(shaderSource.c_str(), shaderSource.size(), 1, f);
    //    fclose(f);
    //}

    return OpenGL::CompileComputeProgram(shader, shaderSource.c_str(), shaderName.c_str(), UseShaderCache ? &ShaderBatch : nullptr);
}

void ComputeRenderer::ShaderCompileStep(int& current, int& count)
{
    current = ShaderStepIdx;
    ShaderStepIdx++;
    count = 33;

    // a batch is the 33 programs of one configuration. the Android frontend and RenderFrame compile
    // them all at once, the Qt one a step per frame
    if (current == 0)
    {
        ShaderBatch = {};
        ShaderBatchNs = 0;
    }
    s64 start = SteadyNowNs();
    CompileShaderStep(current);
    // the zero program for Warm, which doesn't depend on the targets, goes through the program
    // cache with the batch while its file is read anyway
    if (current == count - 1 && WarmEnabled)
        WarmZeroReady(UseShaderCache ? &ShaderBatch : nullptr);
    // one write for the batch, with the programs it added or the stamps it moved. then the binaries
    // leave memory until the next batch reads the file again, rather than sit there all game
    if (current == count - 1 && UseShaderCache)
    {
        OpenGL::SaveShaderCache(ShaderBatch);
        OpenGL::ReleaseShaderCache();
    }
    ShaderBatchNs += SteadyNowNs() - start;

#ifdef MELONADE_DEBUG_TOOLS
    if (current == count - 1)
    {
        Platform::Log(Platform::LogLevel::Info, "COMPSHADER programs=%d hits=%u misses=%u rejected=%u uncached=%u ms=%.1f loaded=%llu saved=%llu cache=%d scale=%d width=%d tile=%d opt=%d\n",
            count, ShaderBatch.Hits, ShaderBatch.Misses, ShaderBatch.Rejected, ShaderBatch.Uncached,
            ShaderBatchNs / 1e6, (unsigned long long)ShaderBatch.BytesLoaded, (unsigned long long)ShaderBatch.BytesSaved,
            UseShaderCache ? 1 : 0, ScaleFactor, TargetWidth, TileSize, OptMask);
    }
#endif
}

void ComputeRenderer::CompileShaderStep(int current)
{
    switch (current)
    {
    case 0:
        CompileShader(ShaderInterpXSpans[0], ComputeRendererShaders::InterpSpans, {"InterpSpans", "ZBuffer"});
        return;
    case 1:
        CompileShader(ShaderInterpXSpans[1], ComputeRendererShaders::InterpSpans, {"InterpSpans", "WBuffer"});
        return;
    case 2:
        CompileShader(ShaderBinCombined, ComputeRendererShaders::BinCombined, {"BinCombined"});
        return;
    case 3:
        CompileShader(ShaderDepthBlend[0], ComputeRendererShaders::DepthBlend, {"DepthBlend", "ZBuffer"});
        return;
    case 4:
        CompileShader(ShaderDepthBlend[1], ComputeRendererShaders::DepthBlend, {"DepthBlend", "WBuffer"});
        return;
    case 5:
        CompileShader(ShaderRasteriseNoTexture[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "NoTexture"});
        return;
    case 6:
        CompileShader(ShaderRasteriseNoTexture[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "NoTexture"});
        return;
    case 7:
        CompileShader(ShaderRasteriseNoTextureToon[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Toon"});
        return;
    case 8:
        CompileShader(ShaderRasteriseNoTextureToon[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Toon"});
        return;
    case 9:
        CompileShader(ShaderRasteriseNoTextureHighlight[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Highlight"});
        return;
    case 10:
        CompileShader(ShaderRasteriseNoTextureHighlight[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Highlight"});
        return;
    case 11:
        CompileShader(ShaderRasteriseUseTextureDecal[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Decal"});
        return;
    case 12:
        CompileShader(ShaderRasteriseUseTextureDecal[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Decal"});
        return;
    case 13:
        CompileShader(ShaderRasteriseUseTextureModulate[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Modulate"});
        return;
    case 14:
        CompileShader(ShaderRasteriseUseTextureModulate[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Modulate"});
        return;
    case 15:
        CompileShader(ShaderRasteriseUseTextureToon[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Toon"});
        return;
    case 16:
        CompileShader(ShaderRasteriseUseTextureToon[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Toon"});
        return;
    case 17:
        CompileShader(ShaderRasteriseUseTextureHighlight[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Highlight"});
        return;
    case 18:
        CompileShader(ShaderRasteriseUseTextureHighlight[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Highlight"});
        return;
    case 19:
        CompileShader(ShaderRasteriseShadowMask[0], ComputeRendererShaders::Rasterise, {"Rasterise", "ZBuffer", "ShadowMask"});
        return;
    case 20:
        CompileShader(ShaderRasteriseShadowMask[1], ComputeRendererShaders::Rasterise, {"Rasterise", "WBuffer", "ShadowMask"});
        return;
    case 21:
        CompileShader(ShaderClearCoarseBinMask, ComputeRendererShaders::ClearCoarseBinMask, {"ClearCoarseBinMask"});
        return;
    case 22:
        CompileShader(ShaderClearIndirectWorkCount, ComputeRendererShaders::ClearIndirectWorkCount, {"ClearIndirectWorkCount"});
        return;
    case 23:
        CompileShader(ShaderCalculateWorkListOffset, ComputeRendererShaders::CalcOffsets, {"CalculateWorkOffsets"});
        return;
    case 24:
        CompileShader(ShaderSortWork, ComputeRendererShaders::SortWork, {"SortWork"});
        return;
    case 25:
        CompileShader(ShaderFinalPass[0], ComputeRendererShaders::FinalPass, {"FinalPass"});
        return;
    case 26:
        CompileShader(ShaderFinalPass[1], ComputeRendererShaders::FinalPass, {"FinalPass", "EdgeMarking"});
        return;
    case 27:
        CompileShader(ShaderFinalPass[2], ComputeRendererShaders::FinalPass, {"FinalPass", "Fog"});
        return;
    case 28:
        CompileShader(ShaderFinalPass[3], ComputeRendererShaders::FinalPass, {"FinalPass", "EdgeMarking", "Fog"});
        return;
    case 29:
        CompileShader(ShaderFinalPass[4], ComputeRendererShaders::FinalPass, {"FinalPass", "AntiAliasing"});
        return;
    case 30:
        CompileShader(ShaderFinalPass[5], ComputeRendererShaders::FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking"});
        return;
    case 31:
        CompileShader(ShaderFinalPass[6], ComputeRendererShaders::FinalPass, {"FinalPass", "AntiAliasing", "Fog"});
        return;
    case 32:
        CompileShader(ShaderFinalPass[7], ComputeRendererShaders::FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking", "Fog"});
        return;
    default:
        __builtin_unreachable();
        return;
    }
}

void blah(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
    printf("%s\n", message);
}

std::unique_ptr<ComputeRenderer> ComputeRenderer::New()
{
    std::optional<GLCompositor> compositor =  GLCompositor::New();
    if (!compositor)
        return nullptr;

    std::unique_ptr<ComputeRenderer> result = std::unique_ptr<ComputeRenderer>(new ComputeRenderer(std::move(*compositor)));

    // the 3D image is a texture, and the compositor draws a viewport as wide
    GLint maxTextureSize = 0;
    GLint maxViewportDims[2] = {};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);
    result->MaxTargetWidth = std::min(maxTextureSize, maxViewportDims[0]);
    result->MaxTargetHeight = std::min(maxTextureSize, maxViewportDims[1]);
    // and the work buffers grow with the width
    GLint64 maxBufferSize = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBufferSize);
    result->MaxBufferSize = maxBufferSize;
    // for a forced tile size (TileSizeFor)
    GLint maxGroupThreads = 0;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxGroupThreads);
    result->MaxGroupThreads = maxGroupThreads;
    // which driver this is, once, and the texture buffer limit the setup indices are read under
    // (docs/shelved/compute-on-mali.md)
    GLint maxTexBuffer = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexBuffer);
    result->MaxTexBufferSize = maxTexBuffer;
    {
        const char* vendor = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version = (const char*)glGetString(GL_VERSION);
        const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        Platform::Log(Platform::LogLevel::Info, "COMPGL vendor=\"%s\" renderer=\"%s\" version=\"%s\" glsl=\"%s\" maxtexbuffer=%d\n",
            vendor ? vendor : "?", renderer ? renderer : "?", version ? version : "?", glsl ? glsl : "?", maxTexBuffer);
    }

    //glDebugMessageCallback(blah, NULL);
    //glEnable(GL_DEBUG_OUTPUT);
    glGenBuffers(1, &result->YSpanSetupMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result->YSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupY)*result->YSpanSetupCapacity, nullptr, GL_DYNAMIC_DRAW);
    
    glGenBuffers(1, &result->RenderPolygonMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result->RenderPolygonMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(RenderPolygon)*result->RenderPolygonCapacity, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &result->XSpanSetupMemory);
    glGenBuffers(1, &result->BinResultMemory);
    glGenBuffers(1, &result->FinalTileMemory);
    glGenBuffers(1, &result->YSpanIndicesTextureMemory);
    glGenBuffers(tilememoryLayer_Num, result->TileMemory);
    glGenBuffers(1, &result->WorkDescMemory);

    glGenTextures(1, &result->YSpanIndicesTexture);
    glGenTextures(1, &result->LowResFramebuffer);
    glBindTexture(GL_TEXTURE_2D, result->LowResFramebuffer);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 256, 192);

    glGenBuffers(1, &result->MetaUniformMemory);
    glBindBuffer(GL_UNIFORM_BUFFER, result->MetaUniformMemory);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(MetaUniform), nullptr, GL_DYNAMIC_DRAW);

    glGenSamplers(9, result->Samplers);
    for (u32 j = 0; j < 3; j++)
    {
        for (u32 i = 0; i < 3; i++)
        {
            const GLenum translateWrapMode[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
            glSamplerParameteri(result->Samplers[i+j*3], GL_TEXTURE_WRAP_S, translateWrapMode[i]);
            glSamplerParameteri(result->Samplers[i+j*3], GL_TEXTURE_WRAP_T, translateWrapMode[j]);
            glSamplerParameteri(result->Samplers[i+j*3], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(result->Samplers[i+j*3], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }

    // init textures for the clear bitmap
    glGenTextures(2, result->ClearBitmapTex);
    for (int i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, result->ClearBitmapTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 256, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    }

    glGenBuffers(1, &result->PixelBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, result->PixelBuffer);
    glBufferData(GL_PIXEL_PACK_BUFFER, 256*192*4, NULL, GL_DYNAMIC_READ);
    glGenBuffers(1, &result->EarlyPixelBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, result->EarlyPixelBuffer);
    glBufferData(GL_PIXEL_PACK_BUFFER, 256*192*4, NULL, GL_DYNAMIC_READ);

    glGenFramebuffers(1, &result->CaptureFrameBuffer);

    return result;
}

ComputeRenderer::~ComputeRenderer()
{
#ifdef MELONADE_DEBUG_TOOLS
    // its queries, while the context is still current
    Stats.reset();
    Hash3D.reset();
    DepthCounter.reset();
#endif
    DropPaceFences();
    Texcache.Reset();

    // On Adreno, deleting a buffer doesn't give its memory back right away: the driver keeps the
    // storage of deleted buffers until the next game allocates, so leaving a 4x compute game kept
    // ~250 MB of graphics memory on the ROM list. Storage that glBufferData replaces is freed, so
    // shrink the buffers before deleting them, which leaves ~130 MB at 4x. The order matters on the
    // phone: shrinking the small buffers first kept the big ones anyway, biggest first doesn't.
    // Which buffer is biggest depends on the scale and width, so they're sorted by their real size
    std::pair<GLint64, GLuint> buffers[] = {
        { 0, TileMemory[0] }, { 0, TileMemory[1] }, { 0, TileMemory[2] }, { 0, XSpanSetupMemory },
        { 0, FinalTileMemory }, { 0, WorkDescMemory }, { 0, BinResultMemory },
        { 0, YSpanIndicesTextureMemory }, { 0, YSpanSetupMemory }, { 0, RenderPolygonMemory },
        { 0, PixelBuffer }, { 0, EarlyPixelBuffer }, { 0, MetaUniformMemory } };
    for (auto& [size, buffer] : buffers)
    {
        if (buffer == 0)
            continue;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &size);
    }
    std::stable_sort(std::begin(buffers), std::end(buffers),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [size, buffer] : buffers)
    {
        if (buffer == 0)
            continue;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4, nullptr, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glFinish();

    glDeleteBuffers(1, &YSpanSetupMemory);
    glDeleteBuffers(1, &RenderPolygonMemory);
    glDeleteBuffers(1, &XSpanSetupMemory);
    glDeleteBuffers(1, &BinResultMemory);
    glDeleteBuffers(tilememoryLayer_Num, TileMemory);
    glDeleteBuffers(1, &WorkDescMemory);
    glDeleteBuffers(1, &FinalTileMemory);
    glDeleteBuffers(1, &YSpanIndicesTextureMemory);
    glDeleteTextures(1, &YSpanIndicesTexture);
    // GPU::ComputeCheck's, if a check ran
    if (CheckScratch)
        glDeleteBuffers(1, &CheckScratch);
    if (ShaderCheckSetup)
        glDeleteProgram(ShaderCheckSetup);
    glDeleteTextures(1, &Framebuffer);
    glDeleteTextures(1, &KeptFramebuffer);
    glDeleteTextures(1, &SpareFramebuffer);
    glDeleteTextures(1, &LowResFramebuffer);
    glDeleteBuffers(1, &MetaUniformMemory);

    glDeleteSamplers(9, Samplers);
    glDeleteTextures(2, ClearBitmapTex);
    glDeleteBuffers(1, &PixelBuffer);
    glDeleteBuffers(1, &EarlyPixelBuffer);
    glDeleteFramebuffers(1, &CaptureFrameBuffer);
    if (CaptureFence)
        glDeleteSync(CaptureFence);
    if (EarlyCaptureFence)
        glDeleteSync(EarlyCaptureFence);

    DeleteShaders();
    glDeleteProgram(ShaderWarmZero);
    // the binaries in memory are only for compiles, and the file has every one a batch added
    OpenGL::ReleaseShaderCache();
}

void ComputeRenderer::DeleteShaders()
{
    std::initializer_list<GLuint*> allPrograms =
    {
        &ShaderInterpXSpans[0],
        &ShaderInterpXSpans[1],
        &ShaderBinCombined,
        &ShaderDepthBlend[0],
        &ShaderDepthBlend[1],
        &ShaderRasteriseNoTexture[0],
        &ShaderRasteriseNoTexture[1],
        &ShaderRasteriseNoTextureToon[0],
        &ShaderRasteriseNoTextureToon[1],
        &ShaderRasteriseNoTextureHighlight[0],
        &ShaderRasteriseNoTextureHighlight[1],
        &ShaderRasteriseUseTextureDecal[0],
        &ShaderRasteriseUseTextureDecal[1],
        &ShaderRasteriseUseTextureModulate[0],
        &ShaderRasteriseUseTextureModulate[1],
        &ShaderRasteriseUseTextureToon[0],
        &ShaderRasteriseUseTextureToon[1],
        &ShaderRasteriseUseTextureHighlight[0],
        &ShaderRasteriseUseTextureHighlight[1],
        &ShaderRasteriseShadowMask[0],
        &ShaderRasteriseShadowMask[1],
        &ShaderClearCoarseBinMask,
        &ShaderClearIndirectWorkCount,
        &ShaderCalculateWorkListOffset,
        &ShaderSortWork,
        &ShaderFinalPass[0],
        &ShaderFinalPass[1],
        &ShaderFinalPass[2],
        &ShaderFinalPass[3],
        &ShaderFinalPass[4],
        &ShaderFinalPass[5],
        &ShaderFinalPass[6],
        &ShaderFinalPass[7],
    };
    // Program names are shared with the frontend's contexts, so clear them after deleting. Deleting a stale name again (in the destructor, or
    // when the settings change before the shaders are compiled again) could free a program that reused it
    for (GLuint* program : allPrograms)
    {
        glDeleteProgram(*program);
        *program = 0;
    }
#ifdef MELONADE_DEBUG_TOOLS
    // the next programs' first dispatches count again (COMPSLOW)
    DispatchedPrograms.clear();
#endif
}

void ComputeRenderer::Reset(GPU& gpu)
{
    // set before the first compile, which comes before the first RenderFrame
    UseShaderCache = gpu.ShaderCache;
    // and before the targets' first setup (SetRenderSettings), which starts the rooms. without
    // ComputeGrow the rooms are the most at the first frame, so there's nothing to remember.
    // a cart without a game code (homebrew) or no cart has no rooms. DSiWare is loaded as a cart
    // here (MelonInstance::loadRom), so it has them
    UseRooms = gpu.ComputeGrow && gpu.ComputeRooms;
    WarmEnabled = gpu.ComputeWarm;
    u32 gameCode = 0, headerCRC = 0;
    if (const NDSCart::CartCommon* cart = gpu.NDS.GetNDSCart())
    {
        const NDSHeader& header = cart->GetHeader();
        if (header.GameCodeAsU32() != 0 && memcmp(header.GameCode, "####", 4) != 0)
        {
            gameCode = header.GameCodeAsU32();
            headerCRC = header.HeaderCRC16;
        }
    }
    // another cart with the renderer kept (the Qt frontend can): the rooms in use are the last
    // game's, so they aren't remembered for this one until ResizeTargets starts its own
    if ((gameCode != RoomGameCode || headerCRC != RoomHeaderCRC) && ScaleFactor != -1)
        RoomsKeyChanged = true;
    RoomGameCode = gameCode;
    RoomHeaderCRC = headerCRC;
    Texcache.Reset();
    TexcacheEmptied = true;
    // what the image was drawn with is gone, so the next frame is drawn whatever GPU3D says of it.
    // it used to take the caller dirtying all of VRAM after this to get there (TexcacheEmptied)
    ForceRender = true;
    // held WideMelon wings and hi-res capture copies belong to the old scene, and so does the
    // filtered 2D map
    CurGLCompositor.ResetWingHold();
    CurGLCompositor.ResetHiresCapture();
    CurGLCompositor.ResetFilter2D();
    CurGLCompositor.ResetShadow2D();
    ClearBitmapDirty = 0x3;
    DropEarlyCapture();
    DropPaceFences();
    ResetPace();
}

void ComputeRenderer::SavestateLoaded()
{
    CurGLCompositor.ResetWingHold();
    CurGLCompositor.ResetHiresCapture();
    CurGLCompositor.ResetFilter2D();
    CurGLCompositor.ResetShadow2D();
    // line 0 reads the low res image back again, as it did before the early readback
    DropEarlyCapture();
    ResetPace();
}

// the tile size for a scale: starting at 4.5x it doubles every time the scale doubles
static int TileSizeForScale(int scale)
{
    u8 TileScale = 2 * scale / 9;
    TileScale = GetMSBit(TileScale);
    TileScale <<= 1;
    TileScale += TileScale == 0;
    return std::min(8 * TileScale, 32);
}

// the binning dispatch covers whole coarse tiles and the bin mask clear whole groups of tiles, so
// a wide target is rounded up until both divide evenly (256 always does). the columns past the
// real width are rendered, but the final pass doesn't store them
static int InternalWidthFor(int width, int scale, int tileSize)
{
    int coarseTileW = 8 * tileSize; // CoarseTileCountX tiles
    int clearLocalSize = tileSize < 32 ? 64 : 48; // ClearCoarseBinMaskLocalSize
    int tileLines = 192 * scale / tileSize;
    while ((width * scale) % coarseTileW != 0 ||
           ((width * scale / tileSize) * tileLines) % clearLocalSize != 0)
        width++;
    return width;
}

// debug.melonade.computetile: the size it forces, 0 for none
static int ForcedTileSizeOf(const GPU& gpu)
{
    int size = gpu.ComputeTileSize;
    return (size == 8 || size == 16 || size == 32) ? size : 0;
}

// the tile size of a target: the one the scale picks, or the forced one where that can work. a
// work group rasterises a tile with a thread per pixel, and the work tiles have to fit SortWork's
// dispatch and the WorkDescs (see TargetFits). the rest follows from the size: the coarse tiles
// divide 192 lines at any scale for all three sizes, and InternalWidthFor rounds the width up
int ComputeRenderer::TileSizeFor(int width, int scale, int forced) const noexcept
{
    int tileSize = TileSizeForScale(scale);
    if (forced == 0 || forced == tileSize)
        return tileSize;
    if (MaxGroupThreads > 0 && forced * forced > MaxGroupThreads)
        return tileSize;

    s64 screenW = (s64)InternalWidthFor(width, scale, forced) * scale;
    s64 maxWorkTiles = (screenW / forced) * (192 * scale / forced) * 16;
    return maxWorkTiles <= WorkTileLimitFor(MaxPolygonsFor(width, WideList)) ? forced : tileSize;
}

// a native target always fits here: the scale was already fitted to it (FitScale)
bool ComputeRenderer::TargetFits(int width, int scale) const noexcept
{
    return width == WideMelon::NativeWidth || BuffersFit(width, scale);
}

// the scale's native target fits the texture, viewport and work group limits and its work buffers
// fit BuffersFit's. below 9x it always does on the GPUs Compute runs on, past it the tiles are 32
// px, 1024 threads a group, and the biggest buffer, 64 bytes a pixel at the most work tiles, is
// 453 MB at 12x and 805 MB at 16x, past the OnePlus 7 Pro's 512 MB storage block at 16x
bool ComputeRenderer::ScaleFits(int scale) const noexcept
{
    if (MaxTargetHeight > 0 && (384 + 2) * scale > MaxTargetHeight)
        return false;
    int tileSize = TileSizeFor(WideMelon::NativeWidth, scale, ForcedTileSize);
    if (MaxGroupThreads > 0 && tileSize * tileSize > MaxGroupThreads)
        return false;
    return BuffersFit(WideMelon::NativeWidth, scale);
}

// as GLRenderer::FitScale
int ComputeRenderer::FitScale(int scale) const noexcept
{
    int fit = std::max(scale, 1);
    while (fit > 1 && !ScaleFits(fit))
        fit--;
    if (fit < scale)
        Platform::Log(Platform::LogLevel::Warn, "Compute renderer: %dx doesn't fit the GPU's limits (%dx%d textures, %d threads a group, %lld byte buffers), using %dx\n",
            scale, MaxTargetWidth, MaxTargetHeight, MaxGroupThreads, (long long)MaxBufferSize, fit);
    return fit;
}

bool ComputeRenderer::BuffersFit(int width, int scale) const noexcept
{
    if (MaxTargetWidth > 0 && width * scale > MaxTargetWidth)
        return false;

    // the biggest work buffer is a tile memory layer: 4*TileSize^2 bytes for each of the
    // MaxWorkTiles (16 per tile), so 64 bytes per pixel of the rounded up target. the one buffer
    // of ComputeOpt_OneTileBuffer is twice that, and only used where it fits (TileLayersFor)
    int tileSize = TileSizeFor(width, scale, ForcedTileSize);
    s64 screenW = (s64)InternalWidthFor(width, scale, tileSize) * scale;
    s64 largestBuffer = 64 * screenW * (192 * scale);

    // the WorkDescs keep a work tile's index in 21 bits (20 with WideMelon's wide list), and SortWork
    // dispatches a group per 32 of them along X, which only goes to 65535
    s64 maxWorkTiles = (screenW / tileSize) * (192 * scale / tileSize) * 16;
    if (maxWorkTiles > WorkTileLimitFor(MaxPolygonsFor(width, WideList)))
        return false;
    // the sizes are passed to glBufferData as int
    if (largestBuffer > INT32_MAX)
        return false;
    return MaxBufferSize <= 0 || largestBuffer <= MaxBufferSize;
}

int ComputeRenderer::ResolveViewWidth(int width, int scale) noexcept
{
    if (width == WideMelon::NativeWidth)
        return width;

    if (!TargetFits(width, scale))
    {
        Platform::Log(Platform::LogLevel::Warn, "WideMelon: %dx%d does not fit in %d or its buffers in %lld bytes, using native width\n",
            width, scale, MaxTargetWidth, (long long)MaxBufferSize);
        return WideMelon::NativeWidth;
    }

    if (!CurGLCompositor.PrepareWide())
        return WideMelon::NativeWidth;

    return width;
}

void ComputeRenderer::SetViewWidth(int width) noexcept
{
    RequestedViewWidth = WideMelon::SanitizeWidth(width);

    // the scale decides whether it fits, the first SetRenderSettings resolves it otherwise.
    // the targets follow in RenderFrame
    if (ScaleFactor > 0)
        ViewWidth = ResolveViewWidth(RequestedViewWidth, ScaleFactor);
}

void ComputeRenderer::SetRenderSettings(int scale, bool highResolutionCoordinates)
{
    scale = FitScale(scale);
    ViewWidth = ResolveViewWidth(RequestedViewWidth, scale);

    // a new renderer has no polygons drawn yet, so it starts at the view width and its shaders
    // are only compiled once
    int targetWidth = ScaleFactor == -1 ? ViewWidth : TargetWidth;
    if (!TargetFits(targetWidth, scale))
        targetWidth = ViewWidth;

    // the frontend calls this on every settings change, but the shaders and work buffers only
    // depend on the scale and the target width. hi-res coordinates are only used here on the CPU
    if (scale == ScaleFactor && targetWidth == TargetWidth)
    {
        if (highResolutionCoordinates != HiresCoordinates)
        {
            HiresCoordinates = highResolutionCoordinates;
            ForceRender = true;
        }
        return;
    }

    ResizeTargets(scale, highResolutionCoordinates, targetWidth);
}

// GPU::OutputCap: the targets stay as they are, only the compositor's output follows the need
void ComputeRenderer::SetOutputNeed(const GPU& gpu, int frontendNeed) noexcept
{
    // first, so a cap that starts here builds GPU::OutputCap3D's programs (and the capture pass's) along with the exact one
    CurGLCompositor.SetOutputCap3D(gpu);

    int need = GLCompositor::OutputNeed(gpu, frontendNeed);
    bool anyRatio = GLCompositor::OutputAnyRatio(gpu);
    if (need != OutputNeed || anyRatio != OutputAnyRatio)
    {
        OutputNeed = need;
        OutputAnyRatio = anyRatio;
        // before the targets' first setup, that one caps it
        if (ScaleFactor > 0)
            CurGLCompositor.SetRenderSettings(GLCompositor::CappedScale(ScaleFactor, need, anyRatio), ScaleFactor, TargetWidth);
    }

    // debug: a line when the cap's outcome or its reason changes
    CurGLCompositor.LogOutputCap(gpu, frontendNeed);
}

void ComputeRenderer::SetColorDither(bool dither) noexcept
{
    if (dither != ColorDither)
    {
        ColorDither = dither;
        ForceRender = true;
    }
}

void ComputeRenderer::ResizeTargets(int scale, bool highResolutionCoordinates, int targetWidth)
{
    CurGLCompositor.SetRenderSettings(GLCompositor::CappedScale(scale, OutputNeed, OutputAnyRatio), scale, targetWidth);

    if (ScaleFactor != -1)
    {
        DeleteShaders();
    }

    ShaderStepIdx = 0;

    ScaleFactor = scale;
    TargetWidth = targetWidth;
    ScreenHeight = 192 * ScaleFactor;

    TileSize = TileSizeFor(TargetWidth, ScaleFactor, ForcedTileSize);
    if (ForcedTileSize != 0 && TileSize != ForcedTileSize && !TileFallbackLogged)
    {
        TileFallbackLogged = true;
        Platform::Log(Platform::LogLevel::Warn, "Compute: %d px tiles can't work at %dx%d (%d threads a group at most), using %d px\n",
            ForcedTileSize, TargetWidth, ScaleFactor, MaxGroupThreads, TileSize);
    }

    std::printf("Scale: %d\n", ScaleFactor);
    std::printf("TileSize: %d\n", TileSize);

    CoarseTileCountY = TileSize < 32 ? 4 : 6;
    ClearCoarseBinMaskLocalSize = TileSize < 32 ? 64 : 48;
    CoarseTileArea = CoarseTileCountX * CoarseTileCountY;
    CoarseTileW = CoarseTileCountX * TileSize;
    CoarseTileH = CoarseTileCountY * TileSize;

    ScreenWidth = InternalWidthFor(TargetWidth, ScaleFactor, TileSize) * ScaleFactor;

    TilesPerLine = ScreenWidth/TileSize;
    TileLines = ScreenHeight/TileSize;

    HiresCoordinates = highResolutionCoordinates;

    // WideMelon's wide list (GPU::WidePolys) on a wide target: twice the polygons, so twice the bin
    // masks a tile and the Y spans, and a bit more of each WorkDesc for the polygon index, which leaves
    // the work tile index 20 bits (TileSizeFor and BuffersFit keep MaxWorkTiles under WorkTileLimit)
    MaxPolygons = MaxPolygonsFor(TargetWidth, WideList);
    BinStride = MaxPolygons/32;
    CoarseBinStride = BinStride/32;
    YSpanSetupLimit = MaxPolygons == (int)PolygonRAMSize ? MaxYSpanSetups : (int)NativeVertexRAMSize*2;
    if (YSpanSetupLimit != YSpanSetupCapacity)
    {
        YSpanSetupCapacity = YSpanSetupLimit;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, YSpanSetupMemory);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupY)*YSpanSetupCapacity, nullptr, GL_DYNAMIC_DRAW);
    }
    if (MaxPolygons != RenderPolygonCapacity)
    {
        RenderPolygonCapacity = MaxPolygons;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, RenderPolygonMemory);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(RenderPolygon)*RenderPolygonCapacity, nullptr, GL_DYNAMIC_DRAW);
    }

    MaxWorkTiles = (int)std::min<s64>((s64)TilesPerLine*TileLines*16, WorkTileLimit());

    // the tile memory's layout, which the shaders compiled after this are built for
    TileLayers = TileLayersFor(OptMask);
#ifdef MELONADE_DEBUG_TOOLS
    // the only way to tell that one buffer fell back to two
    Platform::Log(Platform::LogLevel::Info, "COMPTILES buffers=%d opt=%d scale=%d tile=%d maxblock=%lld\n",
        TileLayers, OptMask, ScaleFactor, TileSize, (long long)MaxBufferSize);
#endif

    // the tile memory starts at 6 of the 16 layers a tile has room for and grows from there
    // (GrowWorkBuffers). White 2's overworld used 29k and NSMB's map 40k work tiles at 4x, and the
    // CPU's bound ran 1.8 times what binning used on White 2's title, so 6 (74k at 4x) should hold
    // both without growing. the title grows once or twice. if it fails the room is 0, and the
    // first frame with polygons grows it right away, whatever the grows before these targets did.
    // a game that grew past where they start before, at the same scale, width and layout, starts
    // at the rooms it grew to (ComputeWorkRooms), so that grow is part of the first frame's setup
    // and not a hitch mid-game, as on White 2's title. they have the grow's quarter of headroom in
    // them already, so nothing is added. a room that doesn't fit now is the usual start
    GrowRetryIn = GrowRetryWait = 0;
    // eh those are pretty bad guesses
    // though real hw shouldn't be eable to render all 2048 polygons on every line either
    int maxYSpanIndices = 64*MaxPolygons * ScaleFactor;
    YSpanIndices.resize(maxYSpanIndices);
    int startTiles = TilesPerLine*TileLines*6;
    int startSpans = maxYSpanIndices / 16;
    int roomTiles = 0, roomSpans = 0;
    bool roomsFound = UseRooms && ComputeWorkRooms::Find(RoomKey(), roomTiles, roomSpans);
    // the rooms are allocated last, as they're the only checked allocations here and the biggest:
    // a remembered room must not take the memory the unchecked ones below need. the old rooms'
    // storage is let go first, as the destructor does, so it isn't held through all of them
    ShrinkWorkBuffers();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, FinalTileMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4*3*2*ScreenWidth*ScreenHeight, nullptr, GL_DYNAMIC_DRAW);

    int binResultSize = sizeof(BinResultHeader)
        + TilesPerLine*TileLines*CoarseBinStride*4 // BinnedMaskCoarse
        + TilesPerLine*TileLines*BinStride*4 // BinnedMask
        + TilesPerLine*TileLines*BinStride*4; // WorkOffsets
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, BinResultMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, binResultSize, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, WorkDescMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MaxWorkTiles*2*4*2, nullptr, GL_DYNAMIC_DRAW);

    // the other images are at the old size. nothing is kept here: a kept frame only lives from
    // the render at line 215 to the Blit right after it, and KeepFrameForBlit refuses a render
    // that resizes. the low res image keeps its size, but a resize is rare enough to read it
    // back at line 0 again
    glDeleteTextures(1, &KeptFramebuffer);
    glDeleteTextures(1, &SpareFramebuffer);
    KeptFramebuffer = SpareFramebuffer = 0;
    DropEarlyCapture();
    DropPaceFences();

    if (Framebuffer != 0)
        glDeleteTextures(1, &Framebuffer);
    glGenTextures(1, &Framebuffer);
    glBindTexture(GL_TEXTURE_2D, Framebuffer);
    // only the real width, the wide compositor clips its 3D reads to the texture's size
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TargetWidth * ScaleFactor, ScreenHeight);
    // it's empty, so the next frame has to fill it even if it's the same
    ForceRender = true;

    s64 allocStart = SteadyNowNs();
    if (!(roomsFound && roomTiles > startTiles && AllocTileMemory(std::min(roomTiles, MaxWorkTiles))))
        AllocTileMemory(startTiles);
    s64 tileAllocNs = SteadyNowNs() - allocStart;

    // the GPU's copies of the span indices start at a 16th, 33k spans at 4x, where the overworld and
    // the map used 18k and 20k, or at the remembered room. the polygon setup still stops at
    // maxYSpanIndices, as it always has
    allocStart = SteadyNowNs();
    if (!(roomsFound && roomSpans > startSpans && AllocXSpans(std::min(roomSpans, maxYSpanIndices))))
        AllocXSpans(startSpans);
    s64 spanAllocNs = SteadyNowNs() - allocStart;
    // the rooms are this key's from here on (see Reset)
    RoomsKeyChanged = false;
    // the programs for these targets are compiled after this, so the buffers are written at the
    // next RenderFrame (Warm)
    WarmPending = true;

#ifdef MELONADE_DEBUG_TOOLS
    char code[5] = {};
    memcpy(code, &RoomGameCode, 4);
    Platform::Log(Platform::LogLevel::Info, "COMPROOMS game=%s crc=%04x scale=%d width=%d tile=%d layers=%d use=%d found=%d remembered=%d/%d start worktiles=%d xspans=%d of %d/%d ms=%.1f/%.1f\n",
        RoomGameCode ? code : "none", RoomHeaderCRC, ScaleFactor, TargetWidth, TileSize, TileLayers, UseRooms ? 1 : 0, roomsFound ? 1 : 0,
        roomTiles, roomSpans, WorkTileRoom, XSpanRoom, MaxWorkTiles, maxYSpanIndices, tileAllocNs / 1e6, spanAllocNs / 1e6);
#else
    (void)tileAllocNs;
    (void)spanAllocNs;
#endif
}

ComputeWorkRooms::Key ComputeRenderer::RoomKey() const noexcept
{
    return { RoomGameCode, RoomHeaderCRC, (u32)ScaleFactor, (u32)TileSize, (u32)TileLayers, (u32)TargetWidth,
        (u32)MaxWorkTiles, (u32)YSpanIndices.size() };
}

// the tile memory's layout for GPU::ComputeOpt's bits 7 and 8: a color, a depth and an attribute
// layer (12 bytes a pixel), color and depth with the attributes packed into the color
// (ComputeOpt_PackAttr, 8 bytes), or those two in one buffer (ComputeOpt_OneTileBuffer). that one
// is twice a layer's size, so where it's too big at MaxWorkTiles for a storage block, or for
// glBufferData's int, it stays two
int ComputeRenderer::TileLayersFor(int optMask) const noexcept
{
    if (!(optMask & ComputeOpt_PackAttr))
        return tilememoryLayer_Num;

    s64 oneBuffer = (s64)MaxWorkTiles * TileSize * TileSize * 8;
    bool fits = oneBuffer <= INT32_MAX && (MaxBufferSize <= 0 || oneBuffer <= MaxBufferSize);
    return ((optMask & ComputeOpt_OneTileBuffer) && fits) ? 1 : 2;
}

// glBufferData for the work buffers, which grow mid-game (GrowWorkBuffers), and whether the buffer
// really got the storage. the error flags are drained first, so an older error isn't taken for
// this one. nothing else in the core reads them, but debug builds log what was drained so it isn't
// lost. GL_BUFFER_SIZE is a second look, as the GL's state is undefined after GL_OUT_OF_MEMORY
static bool WorkBufferData(GLenum target, GLsizeiptr size, const char* what)
{
    // a lost context can keep returning errors, so this stops somewhere
    for (int i = 0; i < 16; i++)
    {
        GLenum old = glGetError();
        if (old == GL_NO_ERROR)
            break;
#ifdef MELONADE_DEBUG_TOOLS
        Platform::Log(Platform::LogLevel::Warn, "COMPGLERR 0x%04x before the %s allocation\n", old, what);
#endif
    }

    glBufferData(target, size, nullptr, GL_DYNAMIC_DRAW);
    GLenum err = glGetError();
    GLint64 real = -1;
    glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &real);
    if (err == GL_NO_ERROR && real == (GLint64)size)
        return true;

#ifdef MELONADE_DEBUG_TOOLS
    Platform::Log(Platform::LogLevel::Warn, "COMPGROW %s failed: %lld bytes, buffer has %lld, error 0x%04x\n",
        what, (long long)size, (long long)real, err);
#else
    // the 1st, 2nd, 4th, 8th... failure, which the backoff in GrowWorkBuffers spaces out anyway
    static u32 failures = 0;
    failures++;
    if ((failures & (failures - 1)) == 0)
        Platform::Log(Platform::LogLevel::Warn, "Compute renderer: no GPU memory for the %s (%lld bytes), 3D frames that need it are skipped (%u failures)\n",
            what, (long long)size, failures);
#endif
    return false;
}

// gives the tile memory layers and the X span buffers 4 bytes, which is what frees their storage
// on Adreno (see the destructor), with no room in them until AllocTileMemory and AllocXSpans
void ComputeRenderer::ShrinkWorkBuffers() noexcept
{
    for (int i = 0; i < TileLayersAlloc; i++)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, TileMemory[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4, nullptr, GL_DYNAMIC_DRAW);
    }
    TileLayersAlloc = 0;
    WorkTileRoom = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, XSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
    glBufferData(GL_TEXTURE_BUFFER, 4, nullptr, GL_DYNAMIC_DRAW);
    XSpanRoom = 0;
}

// the tile memory layers, with room for this many work tiles. glBufferData orphans the old
// storage, so a render still in flight keeps what it was bound to. nothing is kept from one render
// to the next in them: Rasterise writes the work tiles that DepthBlend then reads.
// only the first TileLayers of them are used: a word per pixel in each, or two in the one buffer
// that holds color and depth. a layer that isn't used never gets storage, and one that an
// earlier layout used is shrunk like the destructor does, which is what frees it on Adreno.
// that comes first, so a change of layout doesn't hold the old layers while the new ones are made.
// when a layer doesn't get its storage the room is 0, as the layers may be left with none
bool ComputeRenderer::AllocTileMemory(int workTiles)
{
    WorkTileRoom = workTiles;
    for (int i = TileLayers; i < TileLayersAlloc; i++)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, TileMemory[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4, nullptr, GL_DYNAMIC_DRAW);
    }
    TileLayersAlloc = TileLayers;

    GLsizeiptr size = (GLsizeiptr)(TileLayers == 1 ? 8 : 4)*TileSize*TileSize*workTiles;
    for (int i = 0; i < TileLayers; i++)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, TileMemory[i]);
        if (!WorkBufferData(GL_SHADER_STORAGE_BUFFER, size, "tile memory"))
        {
            WorkTileRoom = 0;
            return false;
        }
    }
    return true;
}

// the X spans and the setup indices they're made from, one each per span. the texture buffer
// is pointed at the setup indices' new storage again, in case a driver keeps the old size.
// the room is 0 when either doesn't get its storage
bool ComputeRenderer::AllocXSpans(int spans)
{
    XSpanRoom = spans;
    glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
    bool ok = WorkBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)spans*2*4, "setup indices");

    if (ok)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, XSpanSetupMemory);
        ok = WorkBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sizeof(SpanSetupX)*spans, "x spans");
    }

    glBindTexture(GL_TEXTURE_BUFFER, YSpanIndicesTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA16UI, YSpanIndicesTextureMemory);
    if (!ok)
        XSpanRoom = 0;
    return ok;
}

// GPU::ComputeGrow, before a frame's dispatches: makes room for the work tiles and spans it could
// need, from what the CPU set up, so nothing a frame draws depends on the room. binning makes at
// most workTiles (RenderFrame's totalTileBound), as a polygon is only binned to a tile its x-spans
// reach and InterpXSpans clamps those to their y-span's XMin and XMax, which lie inside the
// polygon's XMin and XMax that the bound's tiles cover. so Rasterise never writes past the room, and the MaxWorkTiles clamp in the shaders stays
// what it was. InterpXSpans runs whole groups of 32, which the room's step keeps inside. a buffer
// that grows gets a quarter more than asked, rounded up to the step, so a scene settles after a
// grow or two, and it never shrinks until the targets change.
// a grow that fails is tried again at what's needed, without the quarter, and then the room
// there was is allocated again, as the failed glBufferData may have left the buffer with nothing
// (0 if even that fails). RenderFrame then leaves out a frame that needs more than the room. the
// next grows are left out for a while, twice as long with every failure in a row, so a scene that
// keeps not fitting doesn't stall each frame on allocations that fail, but it's tried again
void ComputeRenderer::GrowWorkBuffers(u32 workTiles, u32 spans)
{
    auto roomFor = [](u32 need, int step, int max, bool headroom)
    {
        s64 room = (s64)need + (headroom ? need / 4 : 0);
        room = (room + step - 1) / step * step;
        return (int)std::min(room, (s64)max);
    };
    auto grow = [this](bool (ComputeRenderer::*alloc)(int), int oldRoom, int room, int exact)
    {
        if ((this->*alloc)(room))
            return true;
        if (exact < room && (this->*alloc)(exact))
            return true;
        if (oldRoom > 0)
            (this->*alloc)(oldRoom);
        return false;
    };

    // binning stops at MaxWorkTiles, the most there is room for
    u32 workNeed = std::min(workTiles, (u32)MaxWorkTiles);
    int maxSpans = (int)YSpanIndices.size();
    spans = std::min(spans, (u32)maxSpans);
    if (workNeed <= (u32)WorkTileRoom && spans <= (u32)XSpanRoom)
        return;
    if (GrowRetryIn > 0)
    {
        GrowRetryIn--;
        return;
    }

    bool failed = false;
    if (workNeed > (u32)WorkTileRoom)
    {
        // a step is a layer: one work tile for every tile
        int step = TilesPerLine*TileLines;
        int room = roomFor(workNeed, step, MaxWorkTiles, true);
        int oldRoom = WorkTileRoom;
        [[maybe_unused]] s64 start = SteadyNowNs();
        failed |= !grow(&ComputeRenderer::AllocTileMemory, oldRoom, room, roomFor(workNeed, step, MaxWorkTiles, false));
        // the calls' wall-clock time, to tell the allocation from the first dispatch that touches
        // it. then the new storage is written once (GPU::ComputeWarm), which debug builds time
        // (touch, -1 without)
        [[maybe_unused]] double allocMs = (SteadyNowNs() - start) / 1e6;
        [[maybe_unused]] double touchMs = -1;
        u64 touchBytes = 0;
        if (WarmEnabled && WorkTileRoom > oldRoom)
            touchMs = TouchBuffers({ TileMemory[0], TileMemory[1], TileMemory[2] }, touchBytes);
#ifdef MELONADE_DEBUG_TOOLS
        Platform::Log(Platform::LogLevel::Info, "COMPGROW worktiles %d->%d of %d bound=%u scale=%d tile=%d ms=%.1f touch=%.1f bytes=%llu\n",
            oldRoom, room, MaxWorkTiles, workTiles, ScaleFactor, TileSize, allocMs, touchMs, (unsigned long long)touchBytes);
        if (WorkTileRoom != room)
            Platform::Log(Platform::LogLevel::Warn, "COMPGROW worktiles wanted %d, has %d (was %d)\n",
                room, WorkTileRoom, oldRoom);
#endif
    }

    if (spans > (u32)XSpanRoom)
    {
        // a 64th of the most, a multiple of 32
        int step = maxSpans / 64;
        int room = roomFor(spans, step, maxSpans, true);
        int oldRoom = XSpanRoom;
        [[maybe_unused]] s64 start = SteadyNowNs();
        failed |= !grow(&ComputeRenderer::AllocXSpans, oldRoom, room, roomFor(spans, step, maxSpans, false));
        [[maybe_unused]] double allocMs = (SteadyNowNs() - start) / 1e6;
        [[maybe_unused]] double touchMs = -1;
        u64 touchBytes = 0;
        if (WarmEnabled && XSpanRoom > oldRoom)
            touchMs = TouchBuffers({ XSpanSetupMemory, YSpanIndicesTextureMemory }, touchBytes);
#ifdef MELONADE_DEBUG_TOOLS
        Platform::Log(Platform::LogLevel::Info, "COMPGROW xspans %d->%d of %d need=%u scale=%d ms=%.1f touch=%.1f bytes=%llu\n",
            oldRoom, room, maxSpans, spans, ScaleFactor, allocMs, touchMs, (unsigned long long)touchBytes);
        if (XSpanRoom != room)
            Platform::Log(Platform::LogLevel::Warn, "COMPGROW xspans wanted %d, has %d (was %d)\n",
                room, XSpanRoom, oldRoom);
#endif
    }

    // from 16 grows (a quarter second while every frame tries) up to 512
    GrowRetryWait = failed ? std::clamp(GrowRetryWait * 2, 16, 512) : 0;
    GrowRetryIn = GrowRetryWait;

    // the rooms there are now, which were all allocated (a grow that failed went back to the old
    // room or 0), so the next start of this game begins there. this frame hitched already, and
    // it's the only place the file is written apart from a stale stamp at the targets' setup
    if (UseRooms && !RoomsKeyChanged)
        ComputeWorkRooms::Remember(RoomKey(), WorkTileRoom, XSpanRoom);
}

#ifdef MELONADE_DEBUG_TOOLS
void ComputeRenderer::SlowLap(s64& phase) noexcept
{
    s64 now = SteadyNowNs();
    phase += now - SlowLast;
    SlowLast = now;
}

void ComputeRenderer::NoteProgram(GLuint program)
{
    if (std::find(DispatchedPrograms.begin(), DispatchedPrograms.end(), program) != DispatchedPrograms.end())
        return;
    DispatchedPrograms.push_back(program);
    SlowFirsts++;
}

// one line for a RenderFrame over 30 ms, 20 at most a run. setup is the polygon setup without
// texget, the texture cache's lookups (where a new texture is decoded and uploaded)
void ComputeRenderer::LogSlow(int numPolygons, u32 numVariants, u32 tileBound, int setupIndices) noexcept
{
    static int logged = 0;
    s64 total = SteadyNowNs() - RenderFrameStartNs;
    if (total <= 30000000 || logged >= 20)
        return;
    logged++;
    auto ms = [](s64 ns) { return ns / 1e6; };
    Platform::Log(Platform::LogLevel::Info, "COMPSLOW ms=%.1f pace=%.1f resize=%.1f warm=%.1f texcache=%.1f clearbmp=%.1f setup=%.1f texget=%.1f grow=%.1f upload=%.1f bin=%.1f rast=%.1f final=%.1f end=%.1f polys=%d variants=%u firsts=%d worktiles=%u/%d spans=%d/%d scale=%d tile=%d computewarm=%d\n",
        ms(total), ms(Slow.Pace), ms(Slow.Resize), ms(Slow.Warm), ms(Slow.Texcache), ms(Slow.ClearBitmap), ms(Slow.Setup - Slow.TexGet), ms(Slow.TexGet),
        ms(Slow.Grow), ms(Slow.Upload), ms(Slow.Bin), ms(Slow.Rast), ms(Slow.Final), ms(Slow.End), numPolygons, numVariants, SlowFirsts,
        tileBound, WorkTileRoom, setupIndices, XSpanRoom, ScaleFactor, TileSize, WarmEnabled ? 1 : 0);
}

#endif

// a program that writes 0 to every word of the buffer at binding 0. not one of the 33, as it
// doesn't depend on the targets: compiled once a renderer, at the end of the first batch so it goes
// through the program cache with it, or here without the cache if that didn't happen. one that
// doesn't compile only leaves the buffers to be written by the first renders, as before
bool ComputeRenderer::WarmZeroReady(OpenGL::ShaderCacheStats* cache)
{
    if (ShaderWarmZero == 0 && !WarmZeroFailed)
    {
        static const char* const source = R"(#version 320 es
layout (local_size_x = 64) in;
layout (std430, binding = 0) writeonly buffer ZeroBuffer
{
    uint Words[];
};
layout (location = 0) uniform uint WordCount;
void main()
{
    uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    if (i < WordCount)
        Words[i] = 0u;
}
)";
        WarmZeroFailed = !OpenGL::CompileComputeProgram(ShaderWarmZero, source, "WarmZero", cache);
        static bool warned = false;
        if (WarmZeroFailed && !warned)
        {
            warned = true;
            Platform::Log(Platform::LogLevel::Warn, "Compute renderer: the zero program didn't compile, so the work buffers aren't written ahead of the first renders\n");
        }
    }
    return ShaderWarmZero != 0;
}

// queues the zero program over all of a buffer's storage and returns its size, or 0 when it has
// none or is bigger than a storage block may be (the renderer binds none that big)
u64 ComputeRenderer::ZeroBuffer(GLuint buffer) noexcept
{
    if (buffer == 0)
        return 0;
    GLint64 size = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &size);
    if (size < 4 || (MaxBufferSize > 0 && size > MaxBufferSize))
        return 0;
    u64 groups = ((u64)size / 4 + 63) / 64;
    u64 x = std::min<u64>(groups, 65535);
    u64 y = (groups + x - 1) / x;
    if (y > 65535)
        return 0;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    // the last row of groups goes past the end, so the program stops at the buffer's words
    glUniform1ui(0, (GLuint)((u64)size / 4));
    glDispatchCompute((GLuint)x, (GLuint)y, 1);
    return (u64)size;
}

// writes every byte of these buffers once on the GPU, queued ahead of the renders that use them,
// so committing their pages happens here and not in the middle of the first render that touches
// them. none of them holds anything a render reads before writing it in the same render (see
// Warm), so the zeros change nothing. debug builds wait for it to time it, with what was queued
// before waited for first, outside the time; release builds don't, and return 0. -1 when there's
// no zero program
double ComputeRenderer::TouchBuffers(std::initializer_list<GLuint> buffers, u64& bytes) noexcept
{
    bytes = 0;
    if (!WarmZeroReady(nullptr))
        return -1;
#ifdef MELONADE_DEBUG_TOOLS
    glFinish();
    s64 start = SteadyNowNs();
#endif
    glUseProgram(ShaderWarmZero);
    for (GLuint buffer : buffers)
        bytes += ZeroBuffer(buffer);
    // the renders after read and write them as storage, read the setup indices as a texture
    // buffer and upload them with glBufferSubData
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    // RenderFrame binds its own before every dispatch, this is what it leaves there
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, RenderPolygonMemory);
#ifdef MELONADE_DEBUG_TOOLS
    glFinish();
    return (SteadyNowNs() - start) / 1e6;
#else
    return 0;
#endif
}

// GPU::ComputeWarm, at the first RenderFrame after ResizeTargets, once the programs are compiled:
// every work buffer written once (TouchBuffers). none of them holds anything a render reads before
// writing it in the same render: the tile memory, the X spans and the final tile memory are
// written by the passes before the ones that read them, the setup indices are uploaded, the work
// descriptions are written by binning and sorting, and of the bin results the coarse masks are
// cleared at every render, the masks and offsets are only read where this render's coarse bits
// are set, and the variant counts are cleared or written by this render's passes
void ComputeRenderer::Warm()
{
    u64 bytes = 0;
    // a room of 0 is an allocation that failed, which may have left its buffers in any state, so
    // they're left alone (0 is skipped), and so are the tile layers this layout doesn't use
    auto layer = [this](int i) { return WorkTileRoom > 0 && i < TileLayers ? TileMemory[i] : 0; };
    GLuint xspans = XSpanRoom > 0 ? XSpanSetupMemory : 0;
    GLuint setupIndices = XSpanRoom > 0 ? YSpanIndicesTextureMemory : 0;
    [[maybe_unused]] double memoryMs = TouchBuffers({ layer(0), layer(1), layer(2), xspans, setupIndices,
        FinalTileMemory, BinResultMemory, WorkDescMemory }, bytes);

#ifdef MELONADE_DEBUG_TOOLS
    Platform::Log(Platform::LogLevel::Info, "COMPWARM memory=%.1f bytes=%llu scale=%d width=%d tile=%d worktiles=%d xspans=%d\n",
        memoryMs, (unsigned long long)bytes, ScaleFactor, TargetWidth, TileSize, WorkTileRoom, XSpanRoom);
#endif
}

void ComputeRenderer::VCount144(GPU& gpu)
{

}

void ComputeRenderer::SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void ComputeRenderer::SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2])
{
    s32 x0 = positions[vertex][0];
    if (side)
    {
        span->DxInitial = -0x40000;
        x0--;
    }
    else
    {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;

    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;

    span->XCovIncr = 0;

    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void ComputeRenderer::SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2])
{
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    SetupAttrs(span, poly, from, to);

    s32 minXY, maxXY;
    bool negative = false;
    if (span->X1 > span->X0)
    {
        span->XMin = span->X0;
        span->XMax = span->X1-1;

        minXY = span->Y0;
        maxXY = span->Y1;
    }
    else if (span->X1 < span->X0)
    {
        span->XMin = span->X1;
        span->XMax = span->X0-1;
        negative = true;

        minXY = span->Y1;
        maxXY = span->Y0;
    }
    else
    {
        span->XMin = span->X0;
        if (side) span->XMin--;
        span->XMax = span->XMin;

        // doesn't matter for completely vertical slope
        minXY = span->Y0;
        maxXY = span->Y0;
    }

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = minXY;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = maxXY;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax+1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    // TODO: this is still not perfect (see for example x=169 y=33)
    if (ylen == 0)
    {
        span->Increment = 0;
    }
    else if (ylen == xlen)
    {
        span->Increment = 0x40000;
    }
    else
    {
        s32 yrecip = (1<<18) / ylen;
        span->Increment = (span->X1-span->X0) * yrecip;
        if (span->Increment < 0) span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side)
    {
        // right

        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    }
    else
    {
        // left

        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor)
    {
        if (side)
        {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        }
        else
        {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }

        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    }
    else
    {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    if (span->I0 != span->I1)
        span->IRecip = (1<<30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1))
    {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    }
    else
    {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

struct Variant
{
    GLuint Texture, Sampler;
    u16 Width, Height;
    u8 BlendMode;

    bool operator==(const Variant& other)
    {
        return Texture == other.Texture && Sampler == other.Sampler && BlendMode == other.BlendMode;
    }
};

/*
    Antialiasing
    W-Buffer
    With Texture
    0
    1, 3
    2
    without Texture
    2
    0, 1, 3

    => 20 Shader + 1x Shadow Mask
*/

// wide views always go through the hi-res position, which GPU3D already placed on the wide target
s32 ComputeRenderer::WideVertexX(s32 hiresx) const
{
    s32 x = std::max(hiresx + WideXAdjust, 0);
    return HiresCoordinates ? (x * ScaleFactor) >> 4 : (x >> 4) * ScaleFactor;
}

#ifdef MELONADE_DEBUG_TOOLS
// debug.melonade.computetimers, computepeaks and computesync, at the start of every RenderFrame:
// makes the stats when a switch is on (again if one changed), drops them when all are off, and lets
// them read the finished queries and log. computesync turns the other two off
void ComputeRenderer::UpdateStats(const GPU& gpu)
{
    bool sync = gpu.ComputeSync;
    bool timers = gpu.ComputeTimers && !sync;
    bool peaks = gpu.ComputePeaks && !sync;
    if (!Stats || Stats->WantTimers != timers || Stats->WantPeaks != peaks || Stats->Sync != sync)
    {
        Stats.reset();
        if (!timers && !peaks && !sync)
            return;
        Stats = std::make_unique<ComputeStats>(timers, peaks, sync);
    }

    Stats->FrameStart(RenderFrameStartNs, ScaleFactor, TileSize, (u32)MaxWorkTiles, (u32)WorkTileRoom,
        (u32)YSpanIndices.size(), (u32)XSpanRoom, (u32)YSpanSetupLimit, (u32)MaxVariants);
}

// debug.melonade.computedepth, at every RenderFrame while it's on or its counter exists: rebuilds the
// two DepthBlend programs with the counters when it goes on (and without when it goes off), outside
// of the program cache, and lets the counter read finished frames and log
void ComputeRenderer::UpdateDepthStats(const GPU& gpu)
{
    bool want = gpu.ComputeDepthStats && !DepthStatsFailed;
    if (want != DepthStatsOn)
    {
        if (want)
        {
            // the counters take the SSBO binding after the ones the passes use (0 to 7)
            GLint bindings = 0, computeBlocks = 0;
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
            glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &computeBlocks);
            if (bindings <= (GLint)ComputeDepthCounter::Binding)
            {
                Platform::Log(Platform::LogLevel::Warn, "COMPDEPTH: no free SSBO binding (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS=%d), staying off\n", bindings);
                DepthStatsFailed = true;
                return;
            }
            Platform::Log(Platform::LogLevel::Info, "COMPDEPTH: DepthBlend counts its work (bindings=%d computeblocks=%d)\n", bindings, computeBlocks);
        }

        // both builds first, and only both replace the ones there are
        DepthStatsOn = want;
        bool cache = UseShaderCache;
        UseShaderCache = false;
        GLuint programs[2] {};
        bool built = CompileShader(programs[0], ComputeRendererShaders::DepthBlend, {"DepthBlend", "ZBuffer"})
            && CompileShader(programs[1], ComputeRendererShaders::DepthBlend, {"DepthBlend", "WBuffer"});
        UseShaderCache = cache;
        // CompileShader turns DepthStatsOn off when the source didn't take the counters
        if (built && DepthStatsOn == want)
        {
            for (int i = 0; i < 2; i++)
            {
                glDeleteProgram(ShaderDepthBlend[i]);
                ShaderDepthBlend[i] = programs[i];
            }
        }
        else
        {
            // a failed build leaves 0, which glDeleteProgram ignores
            glDeleteProgram(programs[0]);
            glDeleteProgram(programs[1]);
            if (want)
            {
                if (!built)
                    Platform::Log(Platform::LogLevel::Warn, "COMPDEPTH: DepthBlend with the counters didn't build, staying off\n");
                DepthStatsFailed = true;
            }
            // the programs there are stay, built the way DepthStatsOn was before
            DepthStatsOn = !want;
        }
    }

    if (DepthStatsOn && !DepthCounter)
        DepthCounter = std::make_unique<ComputeDepthCounter>();
    else if (!DepthStatsOn)
        DepthCounter.reset();
    if (DepthCounter)
        DepthCounter->FrameStart(SteadyNowNs(), ScaleFactor);
}

// GPU::Hash3DLogging: the low res image the final pass wrote, the one display capture reads back,
// which a render left out (identical or paced) leaves as it was
void ComputeRenderer::QueueHash3D(GPU& gpu, u32 frame)
{
    if (!Hash3D)
        Hash3D = std::make_unique<Hash3DReadback>(false);

    GLint readFB = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, CaptureFrameBuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, LowResFramebuffer, 0);
    Hash3D->Queue(frame);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFB);
}
#endif

void ComputeRenderer::DropPaceFences() noexcept
{
    for (int i = 0; i < PaceFenceCount; i++)
        glDeleteSync(PaceFences[i]);
    PaceFenceCount = 0;
}

// the oldest count fences are done with
void ComputeRenderer::PopPaceFences(int count) noexcept
{
    for (int i = 0; i < count; i++)
        glDeleteSync(PaceFences[i]);
    PaceFenceCount -= count;
    std::copy(PaceFences + count, PaceFences + count + PaceFenceCount, PaceFences);
}

// GPU::ComputePace, at the start of every RenderFrame: drops the fences of the renders the GPU has
// finished, which leaves one for each that's still unfinished, and logs once a second. the GPU
// runs them in order, so it stops at the first that isn't done. the polls don't flush: every fence
// was flushed when it was made (see RenderFrame). a poll that fails counts as done, so it can't
// get stuck
void ComputeRenderer::UpdatePace(const GPU& gpu) noexcept
{
    if (gpu.ComputePace == 0)
    {
        DropPaceFences();
        return;
    }

    int done = 0;
    while (done < PaceFenceCount && glClientWaitSync(PaceFences[done], 0, 0) != GL_TIMEOUT_EXPIRED)
        done++;
    PopPaceFences(done);
    PaceLog.MaxPending = std::max(PaceLog.MaxPending, PaceFenceCount);

    // debug only (debug.melonade.computepace), once a second: renders were submitted, skipped were
    // left out for pacing, kept had two or more unfinished renders ahead and couldn't be left out
    // (mode 2, they wait instead), redraws are skipped renders drawn after all (RedrawSkippedFrame).
    // waits and timeouts count the waits and the ones that gave up
    if (!gpu.ComputePaceLog)
        return;
    s64 now = SteadyNowNs();
    if (PaceLog.StartNs == 0)
        PaceLog.StartNs = now;
    if (now - PaceLog.StartNs >= 1000000000)
    {
        Platform::Log(Platform::LogLevel::Info, "COMPPACE mode=%d renders=%u skipped=%u kept=%u redraws=%u waits=%u "
            "wait_ms=%.1f timeouts=%u maxpending=%d\n",
            gpu.ComputePace, PaceLog.Renders, PaceLog.Skipped, PaceLog.Kept, PaceLog.Redraws, PaceLog.Waits,
            PaceLog.WaitNs / 1e6, PaceLog.Timeouts, PaceLog.MaxPending);
        PaceLog = {};
        PaceLog.StartNs = now;
    }
}

// GPU::ComputePace: holds the emulator thread until at most one earlier render is unfinished. like
// GetLine's wait for the capture readback, it polls and yields for up to GPU::CaptureSpinUs first, so
// the thread's clock doesn't drop over a short wait, and only sleeps in the driver for what's left
// of a longer one. 100 ms at most in all, so a fence that never signals can't hang the thread. the
// fences were flushed when they were made, so the polls don't flush
void ComputeRenderer::WaitForPace() noexcept
{
    s64 start = SteadyNowNs();
    s64 spinEnd = start + (s64)CaptureSpinUs * 1000;
    s64 deadline = start + 100000000;
    PaceLog.Waits++;
    while (PaceFenceCount > 1)
    {
        s64 now = SteadyNowNs();
        if (now >= deadline)
        {
            PaceLog.Timeouts++;
            break;
        }

        bool spin = now < spinEnd;
        if (glClientWaitSync(PaceFences[0], 0, spin ? 0 : (GLuint64)(deadline - now)) != GL_TIMEOUT_EXPIRED)
            PopPaceFences(1);
        else if (spin)
            std::this_thread::yield();
    }
    PaceLog.WaitNs += SteadyNowNs() - start;
}

// debug only (debug.melonade.computewhy), once a second, from RenderFrame:
//   frames extra   RenderFrame calls at line 215, and the others (redraws for a capture, HalfCamera)
//   renders        calls that drew
//   same           calls left out as identical to the image
//   paced          calls left out for pacing (GPU::ComputePace 2)
//   texsaved       of same, the ones where texture or palette VRAM was written (ComputeOpt_TexSkip)
//   swapsaved      of same, the ones with a swap, whose scene and registers were the last swap's
//                  again (ComputeOpt_SwapSkip). with HalfCamera that's also the swaps whose
//                  in-between scene came out as the real one
//   midsame        HalfCamera's in-between scenes that came out as the real scene, polygon for
//                  polygon, and so weren't shown (ComputeOpt_SwapSkip). its own HALFCAM line has
//                  them under still, but the Smooth 30 FPS setting doesn't log that one
// then a/b for each thing that kept a render from being left out as identical: a the renders
// it applied to, b the ones where nothing else did
//   flush          a SWAP_BUFFERS brought a new polygon list. flushsame is how many of those had
//                  the same scene (GPU3D::HashRenderScene) and registers as the swap before. with
//                  ComputeOpt_SwapSkip those are left out (swapsaved) and don't count here, unless
//                  HalfCamera drew an in-between scene for this swap or the frame before, or the
//                  swap before came with other registers and so wasn't copied
//   regs           no swap, but a register the render reads changed (GPU3D::RenderRegsSame)
//   halfcam        the frame after one of HalfCamera's in-between scenes
//   other          not identical for anything else: HalfCamera outside of VBlank, or the first
//                  frame after a loaded state, a reset or the rendering engine going off
//   texinval       a cached texture's VRAM changed (or the cache was emptied by a reset)
//   texdirty       texture or palette VRAM was written, but no cached texture changed. only a
//                  cause with ComputeOpt_TexSkip off
//   clrbmp         the clear bitmap's VRAM was written while the bitmap is on
//   force          new targets, a changed setting or the first frame (ForceRender)
//   stale          the render before was left out for pacing, redraws included (PaceStale)
// captured is the renders in frames whose line 0 read the 3D back for a display capture. nothing
// in RenderFrame draws because of one, so it's no cause
void ComputeRenderer::LogWhy(const GPU& gpu) noexcept
{
    s64 now = SteadyNowNs();
    if (WhyLog.StartNs == 0)
        WhyLog.StartNs = now;
    if (now - WhyLog.StartNs < 1000000000)
        return;

    static const char* const names[Why_Count] = {"flush", "regs", "halfcam", "other", "texinval", "texdirty", "clrbmp", "force", "stale"};
    char causes[256];
    int length = 0;
    for (int i = 0; i < Why_Count; i++)
    {
        length += snprintf(causes + length, sizeof(causes) - length, " %s=%u/%u", names[i], WhyLog.Cause[i], WhyLog.Sole[i]);
        if (i == Why_Flush)
            length += snprintf(causes + length, sizeof(causes) - length, " flushsame=%u", WhyLog.FlushSame);
    }
    Platform::Log(Platform::LogLevel::Info, "COMPWHY frames=%u extra=%u renders=%u same=%u paced=%u texsaved=%u swapsaved=%u midsame=%u%s captured=%u opt=%d\n",
        WhyLog.Frames, WhyLog.Extra, WhyLog.Renders, WhyLog.Same, WhyLog.Paced, WhyLog.TexSaved, WhyLog.SwapSaved,
        gpu.GPU3D.MidScenesSame - WhyMidSameSeen, causes, WhyLog.Captured, OptMask);
    WhyMidSameSeen = gpu.GPU3D.MidScenesSame;
    WhyLog = {};
    WhyLog.StartNs = now;
}

// GPU::ComputePace 2: whether this render can be left out, which it can't when a display capture
// may read its 3D back: a render outside of line 215 is a redraw for one, a frame that captured
// most likely does again (as GPU::CaptureHeldThisFrame has it), and the game may have armed the next
// one already. a capture armed later gets a redraw at line 0 (RedrawSkippedFrame). nor into new
// targets or with settings that changed (ForceRender), nor with HalfCamera while it counts on its
// scenes being drawn (HalfCamera::CanSkipRender; without ComputeOpt_HalfCamPace, whenever there is
// one), nor when the last render is half a second old by the clock on the wall,
// whatever the fences say (the clock, as fast-forward runs many frames in that time)
bool ComputeRenderer::CanSkipForPace(const GPU& gpu, bool keep, bool captured) const noexcept
{
    u32 captureCnt = gpu.GPU2D_A.CaptureCnt;
    bool captureArmed = (captureCnt & (1u << 31)) && ((captureCnt >> 29) & 0x3) != 1;
    bool halfCamIdle = gpu.GPU3D.GetHalfCamera() == 0
        || ((OptMask & ComputeOpt_HalfCamPace) && gpu.GPU3D.CanHalfCameraSkipRender());
    return gpu.VCount == 215 && !PaceRedraw && !ForceRender && !keep && !captured && !captureArmed
        && halfCamIdle && SteadyNowNs() - PaceRenderNs < 500000000;
}

// a render that was left out for pacing, drawn after all: RenderFrame then draws what line 215
// would have, from the 3D state latched at VBlank and the textures as line 215 left them
void ComputeRenderer::RedrawSkippedFrame(GPU& gpu)
{
    if (!PaceStale)
        return;

    PaceRedraw = true;
    RenderFrame(gpu);
    PaceRedraw = false;
}

// a reset or a savestate load: the scene a render was left out of is gone, so there's nothing to
// redraw, and the next render is drawn whatever it looks like
void ComputeRenderer::ResetPace() noexcept
{
    if (PaceStale)
        ForceRender = true;
    PaceStale = false;
    CapturedSinceRender = false;
}

// GPU::ComputeCheck (docs/shelved/compute-on-mali.md): one count of the COMPCHECK line, with the
// first few cases it counted for the lines after it
namespace
{
struct CheckCount
{
    const char* Name;
    u32 Count = 0;
    std::vector<std::string> Examples;

    explicit CheckCount(const char* name) : Name(name) {}

    __attribute__((format(printf, 2, 3))) void Fail(const char* fmt, ...)
    {
        Count++;
        if (Examples.size() >= 3)
            return;
        char text[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        Examples.emplace_back(text);
    }
};

// copies [offset, offset + size) of a buffer out through glMapBufferRange, as GLES has no
// glGetBufferSubData
bool CheckRead(GLuint buffer, size_t offset, size_t size, void* out)
{
    if (size == 0)
        return true;
    glBindBuffer(GL_COPY_READ_BUFFER, buffer);
    const void* mapped = glMapBufferRange(GL_COPY_READ_BUFFER, (GLintptr)offset, (GLsizeiptr)size, GL_MAP_READ_BIT);
    if (mapped)
    {
        memcpy(out, mapped, size);
        glUnmapBuffer(GL_COPY_READ_BUFFER);
    }
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    return mapped != nullptr;
}
}

// GPU::ComputeCheck: after a render's dispatches, waits for the GPU, reads back what binning and
// InterpXSpans wrote and logs a COMPCHECK line with what doesn't add up, then a line for each of
// the first 3 cases of each count that isn't 0. the buffers' layouts are the shaders' std430 ones:
// BinResultBuffer is BinResultHeader and then the coarse masks (CoarseBinStride words a tile), the
// fine masks and the work offsets (BinStride words a tile, one per polygon group of 32), a tile
// being x + y * TilesPerLine; WorkDescs are uvec2s, the unsorted ones from 0 and the sorted ones
// from MaxWorkTiles, x = tile x | tile y << 16 in pixels, y = polygon | in-variant offset << 11
// (sorted: | unsorted index << 11), 12 for PolyIdxBits with MaxPolygons 4096; the X spans are
// SpanSetupX, one per setup index
void ComputeRenderer::RunCheck(int numPolygons, u32 numVariants, int numSetupIndices, u32 totalTileBound)
{
    static_assert(sizeof(BinResultHeader) == 6160, "BinResultHeader must match the std430 BinResultBuffer header");
    static_assert(sizeof(SpanSetupX) == 96, "SpanSetupX must match the std430 XSpanSetup");
    static_assert(sizeof(SetupIndices) == 8, "SetupIndices must be 4 u16s, like the RGBA16UI texels");

    s64 start = SteadyNowNs();
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    const int tiles = TilesPerLine * TileLines;
    const int groups = (numPolygons + 31) / 32;
    const int polyBits = PolyIdxBits();
    const u32 polyMask = (1u << polyBits) - 1;

    std::vector<u32> bin(sizeof(BinResultHeader) / 4 + (size_t)tiles * (CoarseBinStride + 2 * BinStride));
    bool binOk = CheckRead(BinResultMemory, 0, bin.size() * 4, bin.data());
    BinResultHeader header;
    memcpy(&header, bin.data(), sizeof(header));
    const u32* coarseMask = bin.data() + sizeof(BinResultHeader) / 4;
    const u32* fineMask = coarseMask + (size_t)tiles * CoarseBinStride;
    const u32* workOffset = fineMask + (size_t)tiles * BinStride;

    u32 total = binOk ? header.VariantWorkCount[0*4+3] : 0;
    u32 written = std::min(total, (u32)MaxWorkTiles);
    std::vector<u32> unsorted((size_t)written * 2), sorted((size_t)written * 2);
    bool workOk = binOk && CheckRead(WorkDescMemory, 0, unsorted.size() * 4, unsorted.data())
        && CheckRead(WorkDescMemory, (size_t)MaxWorkTiles * 8, sorted.size() * 4, sorted.data());
    std::vector<SpanSetupX> xspans(numSetupIndices);
    bool xspanOk = CheckRead(XSpanSetupMemory, 0, xspans.size() * sizeof(SpanSetupX), xspans.data());

    // a. the header: the total binning counted against the CPU's bound and the fine masks' bits,
    // the variants' real counts and the sorted offsets CalcOffsets handed out from them
    u32 maskBits = 0, realSum = 0;
    u32 offsetSum = binOk ? header.VariantWorkCount[1*4+3] : 0;
    CheckCount coarseC("coarsemask"), offsetsC("offsetgaps"), countC("countmismatch");
    if (binOk)
    {
        for (int t = 0; t < tiles; t++)
        {
            for (int g = 0; g < BinStride; g++)
            {
                u32 mask = g < groups ? fineMask[(size_t)t * BinStride + g] : 0;
                maskBits += (u32)__builtin_popcount(mask);
                bool coarse = (coarseMask[(size_t)t * CoarseBinStride + (g >> 5)] >> (g & 31)) & 1;
                if (coarse != (mask != 0))
                    coarseC.Fail("tile=%d,%d group=%d coarsebit=%d finemask=%08x",
                        t % TilesPerLine, t / TilesPerLine, g, coarse ? 1 : 0, mask);
            }
        }

        std::vector<std::pair<u32, u32>> spans;
        for (u32 v = 0; v < numVariants; v++)
        {
            u32 real = header.VariantWorkRealCount[v];
            realSum += real;
            if (real != 0)
                spans.emplace_back(header.SortedWorkOffset[v], v);
        }
        std::sort(spans.begin(), spans.end());
        u32 next = 0;
        for (const auto& [offset, v] : spans)
        {
            if (offset != next)
                offsetsC.Fail("variant=%u offset=%u count=%u expected offset=%u", v, offset, header.VariantWorkRealCount[v], next);
            next = offset + header.VariantWorkRealCount[v];
        }

        // the totals that should all be the same: one count for each of the three that isn't
        if (maskBits != written)
            countC.Fail("maskbits=%u written=%u", maskBits, written);
        if (realSum != written)
            countC.Fail("realsum=%u written=%u", realSum, written);
        if (offsetSum != realSum)
            countC.Fail("offsetsum=%u realsum=%u", offsetSum, realSum);
    }

    // b. every unsorted WorkDesc that was written
    CheckCount badPolyC("badpoly"), offscreenC("offscreen"), outsideBoxC("outsidebox"), varOffsetC("badvaroffset"), dupesC("dupes");
    std::vector<u64> keys;
    keys.reserve(written);
    for (u32 i = 0; workOk && i < written; i++)
    {
        u32 pos = unsorted[i*2], desc = unsorted[i*2+1];
        int px = (int)(pos & 0xFFFF), py = (int)(pos >> 16);
        int poly = (int)(desc & polyMask);
        u32 inVariant = desc >> polyBits;
        bool onScreen = px < ScreenWidth && py < ScreenHeight && px % TileSize == 0 && py % TileSize == 0;
        if (!onScreen)
            offscreenC.Fail("work=%u x=%d y=%d poly=%d screen=%dx%d", i, px, py, poly, ScreenWidth, ScreenHeight);
        if (poly >= numPolygons)
        {
            badPolyC.Fail("work=%u x=%d y=%d poly=%d polys=%d", i, px, py, poly, numPolygons);
            continue;
        }
        // the CPU bound's box: a pixel wider on each side, bottom exclusive
        const RenderPolygon& rp = RenderPolygons[poly];
        if (px > rp.XMax + 1 || px + TileSize - 1 < rp.XMin - 1 || py > rp.YBot - 1 || py + TileSize - 1 < rp.YTop)
            outsideBoxC.Fail("work=%u tile=%d,%d poly=%d box x=%d..%d y=%d..%d", i, px, py, poly, rp.XMin, rp.XMax, rp.YTop, rp.YBot - 1);
        if (rp.Variant >= numVariants || inVariant >= header.VariantWorkRealCount[rp.Variant])
            varOffsetC.Fail("work=%u poly=%d variant=%u offset=%u count=%u", i, poly, rp.Variant, inVariant,
                rp.Variant < numVariants ? header.VariantWorkRealCount[rp.Variant] : 0);
        if (onScreen)
            keys.push_back(((u64)((py / TileSize) * TilesPerLine + px / TileSize) << polyBits) | (u64)poly);
    }
    std::sort(keys.begin(), keys.end());
    for (size_t i = 1; i < keys.size(); i++)
    {
        if (keys[i] == keys[i-1])
            dupesC.Fail("tile=%d,%d poly=%d", (int)((keys[i] >> polyBits) % TilesPerLine), (int)((keys[i] >> polyBits) / TilesPerLine), (int)(keys[i] & polyMask));
    }

    // c. each tile and group's fine mask against the WorkDescs at its work offset
    CheckCount maskC("maskmismatch");
    for (int t = 0; binOk && workOk && t < tiles; t++)
    {
        u32 expectPos = (u32)((t % TilesPerLine) * TileSize) | ((u32)((t / TilesPerLine) * TileSize) << 16);
        for (int g = 0; g < groups; g++)
        {
            u32 mask = fineMask[(size_t)t * BinStride + g];
            u32 offset = workOffset[(size_t)t * BinStride + g];
            for (u32 k = 0; mask != 0; k++)
            {
                int bit = __builtin_ctz(mask);
                mask &= mask - 1;
                u32 idx = offset + k;
                if (idx >= written)
                {
                    maskC.Fail("tile=%d,%d group=%d bit=%d work=%u past written=%u", t % TilesPerLine, t / TilesPerLine, g, bit, idx, written);
                    continue;
                }
                u32 pos = unsorted[idx*2], poly = unsorted[idx*2+1] & polyMask;
                if (pos != expectPos || poly != (u32)(g * 32 + bit))
                    maskC.Fail("tile=%d,%d group=%d bit=%d work=%u has tile=%u,%u poly=%u", t % TilesPerLine, t / TilesPerLine, g, bit,
                        idx, pos & 0xFFFF, pos >> 16, poly);
            }
        }
    }

    // the sorted list: each unsorted index once, with its own WorkDesc, inside its variant's range
    CheckCount sortC("sortbad"), sortDupesC("sortdupes");
    std::vector<u8> seen(written);
    for (u32 j = 0; workOk && j < written; j++)
    {
        u32 pos = sorted[j*2], desc = sorted[j*2+1];
        u32 poly = desc & polyMask, idx = desc >> polyBits;
        if (idx >= written)
        {
            sortC.Fail("sorted=%u unsorted=%u past written=%u", j, idx, written);
            continue;
        }
        if (seen[idx]++)
            sortDupesC.Fail("sorted=%u unsorted=%u", j, idx);
        u32 variant = poly < (u32)numPolygons ? RenderPolygons[poly].Variant : MaxVariants;
        bool inRange = variant < numVariants && j >= header.SortedWorkOffset[variant]
            && j - header.SortedWorkOffset[variant] < header.VariantWorkRealCount[variant];
        if (unsorted[idx*2] != pos || (unsorted[idx*2+1] & polyMask) != poly || !inRange)
            sortC.Fail("sorted=%u unsorted=%u tile=%u,%u poly=%u, unsorted has tile=%u,%u poly=%u, in variant range=%d", j, idx,
                pos & 0xFFFF, pos >> 16, poly, unsorted[idx*2] & 0xFFFF, unsorted[idx*2] >> 16, unsorted[idx*2+1] & polyMask, inRange ? 1 : 0);
    }

    // d. each polygon's X spans, one a row, inside its box (X1 is exclusive)
    CheckCount xspanC("xspanbad");
    for (int p = 0; xspanOk && p < numPolygons; p++)
    {
        const RenderPolygon& rp = RenderPolygons[p];
        for (int r = 0; r < rp.YBot - rp.YTop; r++)
        {
            u32 idx = rp.FirstXSpan + (u32)r;
            if (idx >= (u32)numSetupIndices)
            {
                xspanC.Fail("poly=%d row=%d span=%u past setups=%d", p, r, idx, numSetupIndices);
                break;
            }
            const SpanSetupX& xs = xspans[idx];
            if (xs.X0 < rp.XMin || xs.X1 > rp.XMax + 1 || xs.X0 >= xs.X1)
                xspanC.Fail("poly=%d y=%d span=%u X0=%d X1=%d box x=%d..%d", p, rp.YTop + r, idx, xs.X0, xs.X1, rp.XMin, rp.XMax);
        }
    }

    // e. the setup indices read back through the texture buffer, both ways, against the CPU's
    CheckCount imgC("imgmismatch"), texC("texmismatch");
    u32 wraps = 0;
    bool texChecked = false;
    if (!ShaderCheckSetup && !CheckSetupFailed)
    {
        if (!OpenGL::CompileComputeProgram(ShaderCheckSetup, ComputeRendererShaders::CheckSetupIndices, "CheckSetupIndices", nullptr))
        {
            CheckSetupFailed = true;
            ShaderCheckSetup = 0;
            Platform::Log(Platform::LogLevel::Warn, "COMPCHECK: the setup index readback program didn't compile, check e is off\n");
        }
    }
    if (ShaderCheckSetup && numSetupIndices > 0)
    {
        GLsizeiptr need = (GLsizeiptr)numSetupIndices * 2 * 16;
        if (!CheckScratch)
            glGenBuffers(1, &CheckScratch);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, CheckScratch);
        if (need > CheckScratchSize)
        {
            glBufferData(GL_SHADER_STORAGE_BUFFER, need, nullptr, GL_DYNAMIC_READ);
            CheckScratchSize = need;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, CheckScratch);
        glBindImageTexture(0, YSpanIndicesTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_BUFFER, YSpanIndicesTexture);
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(ShaderCheckSetup);
        glUniform1ui(0, (GLuint)numSetupIndices);
        glDispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glFinish();

        std::vector<u32> fetched((size_t)numSetupIndices * 8);
        texChecked = CheckRead(CheckScratch, 0, fetched.size() * 4, fetched.data());
        for (int i = 0; texChecked && i < numSetupIndices; i++)
        {
            const SetupIndices& cpu = YSpanIndices[i];
            u32 want[4] = { cpu.PolyIdx, cpu.SpanIdxL, cpu.SpanIdxR, cpu.Y };
            for (int way = 0; way < 2; way++)
            {
                const u32* got = &fetched[(size_t)i * 8 + way * 4];
                if (memcmp(got, want, sizeof(want)) == 0)
                    continue;
                // the wrap at 65536 texels older Mali drivers are said to have
                if (way == 0 && i >= 65536)
                {
                    const SetupIndices& w = YSpanIndices[i % 65536];
                    if (got[0] == w.PolyIdx && got[1] == w.SpanIdxL && got[2] == w.SpanIdxR && got[3] == w.Y)
                        wraps++;
                }
                (way == 0 ? imgC : texC).Fail("index=%d cpu=%u,%u,%u,%u gpu=%u,%u,%u,%u", i, want[0], want[1], want[2], want[3],
                    got[0], got[1], got[2], got[3]);
            }
        }

        // what the rest of the frame had bound there
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, RenderPolygonMemory);
        glBindImageTexture(0, Framebuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    }

    std::string imgCount = texChecked ? std::to_string(imgC.Count) : "na";
    std::string texCount = texChecked ? std::to_string(texC.Count) : "na";
    Platform::Log(Platform::LogLevel::Info, "COMPCHECK render=%u polys=%d groups=%d variants=%u setups=%d screen=%dx%d tile=%d maxwork=%d "
        "xroom=%d texbufmax=%d read=%s total=%u bound=%u pastbound=%d written=%u maskbits=%u realsum=%u offsetsum=%u "
        "countmismatch=%u coarsemask=%u offsetgaps=%u badpoly=%u offscreen=%u outsidebox=%u badvaroffset=%u dupes=%u maskmismatch=%u "
        "sortbad=%u sortdupes=%u xspanbad=%u imgmismatch=%s texmismatch=%s wrap65536=%u binmode=%d ssbo=%d calc=%d flat=%d ms=%.1f\n",
        CheckRenders - 1, numPolygons, groups, numVariants, numSetupIndices, ScreenWidth, ScreenHeight, TileSize, MaxWorkTiles,
        XSpanRoom, MaxTexBufferSize, binOk && workOk && xspanOk ? "ok" : "mapfailed", total, totalTileBound, total > totalTileBound ? 1 : 0,
        written, maskBits, realSum, offsetSum, countC.Count, coarseC.Count, offsetsC.Count, badPolyC.Count, offscreenC.Count, outsideBoxC.Count,
        varOffsetC.Count, dupesC.Count, maskC.Count, sortC.Count, sortDupesC.Count, xspanC.Count,
        imgCount.c_str(), texCount.c_str(), wraps, BinMode, SetupSSBO ? 1 : 0, CalcMode, FlatMode, (SteadyNowNs() - start) / 1e6);
    const CheckCount* checks[] = { &countC, &coarseC, &offsetsC, &badPolyC, &offscreenC, &outsideBoxC, &varOffsetC, &dupesC, &maskC,
        &sortC, &sortDupesC, &xspanC, &imgC, &texC };
    for (const CheckCount* check : checks)
    {
        for (const std::string& example : check->Examples)
            Platform::Log(Platform::LogLevel::Info, "COMPCHECK %s %s\n", check->Name, example.c_str());
    }
}

void ComputeRenderer::RenderFrame(GPU& gpu)
{
#ifdef MELONADE_DEBUG_TOOLS
    RenderFrameStartNs = SteadyNowNs();
    Slow = {};
    SlowLast = RenderFrameStartNs;
    SlowFirsts = 0;
    if (gpu.ComputeTimers || gpu.ComputePeaks || gpu.ComputeSync || Stats)
        UpdateStats(gpu);
#endif
    CaptureSpinUs = gpu.CaptureSpinUs;
    // the COMPWHY log tells a swap with the same scene from one with a new scene
    gpu.GPU3D.SceneHashLogging = gpu.ComputeWhy;

    // GPU::ComputePace: leaves a fence for each earlier render the GPU hasn't finished
    if (gpu.ComputePace != 0 || PaceFenceCount != 0)
        UpdatePace(gpu);
    SLOW_LAP(Pace);
    // whether line 0 of this frame read the 3D back, for CanSkipForPace
    bool captured = CapturedSinceRender;
    if (gpu.VCount == 215)
        CapturedSinceRender = false;

    bool keep = KeepRequested;
    KeepRequested = false;
    // an earlier readback is of the frame this replaces (a skipped one too, to keep it simple)
    DropEarlyCapture();

    // the targets match the polygons drawn here (see FrameTargetWidth). this is inside RunFrame,
    // so the shaders for the new width are compiled right here. with a Blit after it
    // (KeepFrameForBlit), only when the targets stay as they are
    // GPU::ComputeTileSize is only known here, and a change of tile size takes new targets too
    ForcedTileSize = ForcedTileSizeOf(gpu);
    // the same for GPU::ComputeOpt, whose bits are defines in the shaders. only a debug switch
    // changes it, so it's the same compile as for a new target. that also gives the tile memory
    // the layout of its bits 7 and 8, which only count when they change it: bit 8 does nothing
    // without bit 7, or where one buffer doesn't fit
    int optMask = gpu.ComputeOpt & ComputeOpt_All;
    if (!(optMask & ComputeOpt_PackAttr))
        optMask &= ~ComputeOpt_OneTileBuffer;
    bool optChanged = ((optMask ^ OptMask) & ComputeOpt_Shaders) != 0 || TileLayersFor(optMask) != TileLayers;
    OptMask = optMask;
    // GPU::ComputeBin, ComputeSSBO, ComputeCalc and ComputeFlat (Mali diagnostics and fixes) are
    // shader defines too, and recompile the same way
    int binMode = std::clamp(gpu.ComputeBin, 0, 3);
    int calcMode = std::clamp(gpu.ComputeCalc, 0, 1);
    int flatMode = std::clamp(gpu.ComputeFlat, 0, 1);
    if (binMode != BinMode || gpu.ComputeSSBO != SetupSSBO || calcMode != CalcMode || flatMode != FlatMode)
    {
        BinMode = binMode;
        SetupSSBO = gpu.ComputeSSBO;
        CalcMode = calcMode;
        FlatMode = flatMode;
        optChanged = true;
    }
    // ComputeOpt_SwapSkip: has GPU3D compare each swap's scene with the one before, from the next
    // VBlank on. a swap that changed nothing a render reads then counts like a frame without one
    gpu.GPU3D.SceneRepeatCheck = (OptMask & ComputeOpt_SwapSkip) != 0;
    bool identical = gpu.GPU3D.RenderFrameIdentical
        || ((OptMask & ComputeOpt_SwapSkip) && gpu.GPU3D.RenderSceneRepeated);
    // the polygon list's size goes by the target and the latched scene's wide list (MaxPolygonsFor),
    // not GPU::WidePolys itself, which only reaches a scene at its swap
    WideList = gpu.GPU3D.RenderWideList;
    int targetWidth = FrameTargetWidth(gpu);
    if (targetWidth != TargetWidth || TileSizeFor(targetWidth, ScaleFactor, ForcedTileSize) != TileSize || optChanged
        || MaxPolygonsFor(targetWidth, WideList) != MaxPolygons)
    {
        keep = false;
        ResizeTargets(ScaleFactor, HiresCoordinates, targetWidth);
        int current, count;
        while (NeedsShaderCompile())
            ShaderCompileStep(current, count);
    }
    SLOW_LAP(Resize);
#ifdef MELONADE_DEBUG_TOOLS
    // after the compile above, which builds DepthBlend with the counters too while they're on
    if (gpu.ComputeDepthStats || DepthStatsOn || DepthCounter)
        UpdateDepthStats(gpu);
#endif
    // after the targets' setup and the compile (above, or the frontend's before RunFrame), before
    // anything of this frame. that's the game's first frame, under a black screen
    if (WarmPending)
    {
        WarmPending = false;
        if (WarmEnabled)
            Warm();
    }
    SLOW_LAP(Warm);

    WideFrame = TargetWidth != WideMelon::NativeWidth || gpu.GPU3D.RenderViewWidth != WideMelon::NativeWidth;
    WideXAdjust = (TargetWidth - gpu.GPU3D.RenderViewWidth) << 3;

    assert(!NeedsShaderCompile());
    // new targets are empty (here or after SetRenderSettings), so they're drawn even if the frame is the same
    // nor is a frame the same as the image when the render before it was left out for pacing
    // a redraw of a render left out for pacing (RedrawSkippedFrame) has to draw what line 215 would
    // have. the texture cache was updated there and nothing else changes the flat VRAM copies, so
    // it's left alone: what was written since waits for the next line 215, clear bitmap bits too
    bool redraw = PaceRedraw && PaceStale;
    u8 clrBitmapDirty = 0;
    bool texInvalidated = false;
    bool vramDirty = !redraw && Texcache.Update(gpu, clrBitmapDirty, texInvalidated);
    SLOW_LAP(Texcache);
    // ComputeOpt_TexSkip: a write to texture or palette VRAM only counts when it can change what
    // the same polygons look like. a render only reads VRAM through the texture cache and the
    // clear bitmap. every texture the last render drew with was cached by it, and an entry only
    // leaves the cache in Update (texInvalidated) or Reset (TexcacheEmptied), so without those a
    // render now would find the same entries with the same texels, however stale. the clear
    // bitmap is only read, and only uploaded, while the mode is on, and its bits are kept below
    bool clearBitmapChanged = (gpu.GPU3D.RenderDispCnt & (1<<14)) && (ClearBitmapDirty | clrBitmapDirty) != 0;
    bool texChanged = vramDirty;
    if (OptMask & ComputeOpt_TexSkip)
        texChanged = vramDirty && (texInvalidated || TexcacheEmptied || clearBitmapChanged);

    // debug only: what keeps this frame from being left out as identical, a bit per Why_ cause
    u32 why = 0;
    if (gpu.ComputeWhy)
    {
        LogWhy(gpu);
        if (gpu.VCount == 215 && !PaceRedraw)
            WhyLog.Frames++;
        else
            WhyLog.Extra++;

        u8 change = gpu.GPU3D.RenderFrameChange;
        if (!identical)
        {
            if (change & GPU3D::FrameChange_Flush) why |= 1 << Why_Flush;
            if (change & GPU3D::FrameChange_Regs) why |= 1 << Why_Regs;
            if (change & GPU3D::FrameChange_HalfCam) why |= 1 << Why_HalfCam;
            // HalfCamera outside of VBlank, or the latched state was replaced (a loaded state,
            // a reset, the rendering engine going off)
            if (change == 0) why |= 1 << Why_Other;
        }
        if (texChanged)
        {
            if (texInvalidated || TexcacheEmptied) why |= 1 << Why_TexInval;
            else if (clearBitmapChanged) why |= 1 << Why_ClearBitmap;
            else why |= 1 << Why_TexDirty;
        }
        if (ForceRender) why |= 1 << Why_Force;
        if (PaceStale) why |= 1 << Why_Stale;
    }

    // identical: what GPU3D latched at VBlank is what it latched the VBlank before, registers and
    // scene. without a swap (RenderFrameIdentical), or with one that brought the same polygons
    // with the same vertices in the same order (RenderSceneRepeated). either way a render would
    // read the same latched state as the last one, the same cached textures and clear bitmap
    // (texChanged) and the same settings and targets (ForceRender), and write the same pixels into
    // the image and the low res one a display capture reads back, so both are left as they are
    if (!redraw && !texChanged && identical && !ForceRender && !PaceStale)
    {
        // with ComputeOpt_TexSkip the clear bitmap's VRAM can have been written while the mode is
        // off. as in a render, the bits wait for the mode
        ClearBitmapDirty |= clrBitmapDirty;
        if (gpu.ComputeWhy)
        {
            WhyLog.Same++;
            WhyLog.TexSaved += vramDirty;
            WhyLog.SwapSaved += !gpu.GPU3D.RenderFrameIdentical;
        }
#ifdef MELONADE_DEBUG_TOOLS
        if (Stats)
            Stats->Sec.Skipped++;
#endif
        return;
    }

    if (gpu.ComputePace != 0 && PaceFenceCount >= 2)
    {
        if (gpu.ComputePace == 2 && CanSkipForPace(gpu, keep, captured))
        {
            // left out like an identical frame: the image, the low res one and the kept one stay
            // as they are. unlike there the scene did change, so the texture cache's clear bitmap
            // bits are kept for the next render, and PaceStale makes sure there is one even if the
            // frames after this are identical to the one that wasn't drawn
            ClearBitmapDirty |= clrBitmapDirty;
            PaceStale = true;
            PaceLog.Skipped++;
            WhyLog.Paced += gpu.ComputeWhy;
            return;
        }

        // it has to be drawn, so the queue is kept short by waiting
        if (gpu.ComputePace == 2)
            PaceLog.Kept++;
        WaitForPace();
    }
    SLOW_LAP(Pace);

    if (gpu.ComputeWhy)
    {
        WhyLog.Renders++;
        WhyLog.Captured += captured;
        WhyLog.FlushSame += (why & (1 << Why_Flush)) && (gpu.GPU3D.RenderFrameChange & GPU3D::FrameChange_FlushSame);
        for (int i = 0; i < Why_Count; i++)
        {
            WhyLog.Cause[i] += (why >> i) & 1;
            WhyLog.Sole[i] += why == (1u << i);
        }
    }

    if (redraw)
        PaceLog.Redraws++;
    PaceStale = false;
    ForceRender = false;
    TexcacheEmptied = false;

    // the final pass writes every texel of the image, so the spare one needs nothing else
    if (keep)
        KeepFramebuffer();

    // if we're using a clear bitmap, set that up
    ClearBitmapDirty |= clrBitmapDirty;
    if (gpu.GPU3D.RenderDispCnt & (1<<14))
    {
        if (ClearBitmapDirty & (1<<0))
        {
            u16* vram = (u16*)&gpu.VRAMFlat_Texture[0x40000];
            for (int i = 0; i < 256*256; i++)
            {
                u16 color = vram[i];
                u32 r = (color << 1) & 0x3E; if (r) r++;
                u32 g = (color >> 4) & 0x3E; if (g) g++;
                u32 b = (color >> 9) & 0x3E; if (b) b++;
                u32 a = (color & 0x8000) ? 31 : 0;

                ClearBitmap[0][i] = r | (g << 8) | (b << 16) | (a << 24);
            }

            glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, ClearBitmap[0]);
        }

        if (ClearBitmapDirty & (1<<1))
        {
            u16* vram = (u16*)&gpu.VRAMFlat_Texture[0x60000];
            for (int i = 0; i < 256*256; i++)
            {
                u16 val = vram[i];
                u32 depth = ((val & 0x7FFF) * 0x200) + 0x1FF;
                u32 fog = (val & 0x8000) << 9;

                ClearBitmap[1][i] = depth | fog;
            }

            glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, ClearBitmap[1]);
        }

        ClearBitmapDirty = 0;
    }
    SLOW_LAP(ClearBitmap);

    int numYSpans = 0;
    int numSetupIndices = 0;
    // the polygons that fit in the span buffers, the rest of the frame is dropped. at most MaxPolygons:
    // WideMelon's wide list can have more on the frame its target goes back to native
    int numPolygons = std::min((int)gpu.GPU3D.RenderNumPolygons, MaxPolygons);
    const int maxSetupIndices = (int)YSpanIndices.size();

    /*
        Some games really like to spam small textures, often
        to store the data like PPU tiles. E.g. Shantae
        or some Mega Man game. Fortunately they are usually kind
        enough to not vary the texture size all too often (usually
        they just use 8x8 or 16x for everything).

        This is the reason we have this whole mess where textures of
        the same size are put into array textures. This allows
        to increase the batch size.
        Less variance between each Variant hah!
    */
    u32 numVariants = 0, prevVariant, prevTexLayer;
    Variant variants[MaxVariants];
    // for the direct dispatches: how many tiles each variant's polygons could be binned to, from
    // their bounding boxes. binning makes no more work than this (see GrowWorkBuffers), which also
    // sizes the tile memory by it. the shaders would still loop over any more, so for the
    // dispatches it only decides how many work groups are launched
    u32 variantTileBound[MaxVariants];
    u32 totalTileBound = 0;

    bool enableTextureMaps = gpu.GPU3D.RenderDispCnt & (1<<0);

    for (int i = 0; i < numPolygons; i++)
    {
        Polygon* polygon = gpu.GPU3D.RenderPolygonRAM[i];

        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;

        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;

        RenderPolygons[i].FirstXSpan = numSetupIndices;
        RenderPolygons[i].Attr = polygon->Attr;

        bool foundVariant = false;
        if (i > 0)
        {
            // if the whole texture attribute matches
            // the texture layer will also match
            Polygon* prevPolygon = gpu.GPU3D.RenderPolygonRAM[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam
                && prevPolygon->TexPalette == polygon->TexPalette
                && (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30)
                && prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }

        if (!foundVariant)
        {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            u32* textureLastVariant = nullptr;
            // we always need to look up the texture to get the layer of the array texture
            if (enableTextureMaps && (polygon->TexParam >> 26) & 0x7)
            {
#ifdef MELONADE_DEBUG_TOOLS
                // a texture the cache doesn't have yet is decoded and uploaded here
                s64 texStart = SteadyNowNs();
#endif
                Texcache.GetTexture(gpu, polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer, textureLastVariant);
#ifdef MELONADE_DEBUG_TOOLS
                Slow.TexGet += SteadyNowNs() - texStart;
#endif
                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = Samplers[(wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3];

                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant)
                {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }

            if (!foundVariant)
            {
                for (int j = numVariants - 1; j >= 0; j--)
                {
                    if (variants[j] == variant)
                    {
                        foundVariant = true;
                        prevVariant = j;
                        goto foundVariant;
                    }
                }

                if (numVariants == MaxVariants)
                {
                    // out of variants, drop this polygon and the rest of the frame
                    numPolygons = i;
                    break;
                }
                prevVariant = numVariants;
                variantTileBound[numVariants] = 0;
                variants[numVariants] = variant;
                variants[numVariants].Width = TextureWidth(polygon->TexParam);
                variants[numVariants].Height = TextureHeight(polygon->TexParam);
                numVariants++;
            foundVariant:;

                if (textureLastVariant)
                    *textureLastVariant = prevVariant;
            }
        }
        RenderPolygons[i].Variant = prevVariant;
        RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView)
        {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        }
        else
        {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }

        s32 scaledPositions[10][2];
        s32 ytop = ScreenHeight, ybot = 0;
        for (int i = 0; i < polygon->NumVertices; i++)
        {
            if (HiresCoordinates)
            {
                scaledPositions[i][0] = (polygon->Vertices[i]->HiresPosition[0] * ScaleFactor) >> 4;
                scaledPositions[i][1] = (polygon->Vertices[i]->HiresPosition[1] * ScaleFactor) >> 4;
            }
            else
            {
                scaledPositions[i][0] = polygon->Vertices[i]->FinalPosition[0] * ScaleFactor;
                scaledPositions[i][1] = polygon->Vertices[i]->FinalPosition[1] * ScaleFactor;
            }
            if (WideFrame)
                scaledPositions[i][0] = WideVertexX(polygon->Vertices[i]->HiresPosition[0]);
            ytop = std::min(scaledPositions[i][1], ytop);
            ybot = std::max(scaledPositions[i][1], ybot);
        }

        // the span buffers are sized by guesses, so stop before they overflow. each side makes at
        // most one y-span per vertex, and each line one setup index (one if it's flat)
        if (numYSpans + 2 * (int)nverts > YSpanSetupLimit
            || numSetupIndices + std::max(ybot - ytop, 1) > maxSetupIndices)
        {
            numPolygons = i;
            break;
        }

        RenderPolygons[i].YTop = ytop;
        RenderPolygons[i].YBot = ybot;
        RenderPolygons[i].XMin = ScreenWidth;
        RenderPolygons[i].XMax = 0;

        if (ybot == ytop)
        {
            vtop = 0; vbot = 0;

            RenderPolygons[i].YBot++;

            int j = 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            j = nverts - 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanL = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vtop, 0, scaledPositions);
            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanR = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vbot, 1, scaledPositions);

            YSpanIndices[numSetupIndices].PolyIdx = i;
            YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
            YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
            YSpanIndices[numSetupIndices].Y = ytop;
            numSetupIndices++;
        }
        else
        {
            u32 curSpanL = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);

            for (u32 y = ytop; y < ybot; y++)
            {
                if (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                    {
                        curVL = nextVL;
                        if (polygon->FacingView)
                        {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts)
                                nextVL = 0;
                        }
                        else
                        {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0)
                                nextVL = nverts - 1;
                        }
                    }


                    assert(numYSpans < MaxYSpanSetups);
                    curSpanL = numYSpans;
                    SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
                }
                if (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                    {
                        curVR = nextVR;
                        if (polygon->FacingView)
                        {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0)
                                nextVR = nverts - 1;
                        }
                        else
                        {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts)
                                nextVR = 0;
                        }
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanR = numYSpans;
                    SetupYSpan(&RenderPolygons[i] ,&YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);
                }

                YSpanIndices[numSetupIndices].PolyIdx = i;
                YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                YSpanIndices[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }

        {
            // the tiles the bounding box touches, a pixel wider on each side for the edge rounding
            // (binning goes by the x-spans). bottom Y is exclusive
            const RenderPolygon& rp = RenderPolygons[i];
            s32 tileX0 = std::max(rp.XMin - 1, 0) / TileSize;
            s32 tileX1 = std::min(rp.XMax + 1, ScreenWidth - 1) / TileSize;
            s32 tileY0 = std::max(rp.YTop, 0) / TileSize;
            s32 tileY1 = (std::min(rp.YBot, ScreenHeight) - 1) / TileSize;
            if (tileX1 >= tileX0 && tileY1 >= tileY0 && rp.YBot > 0)
            {
                u32 tiles = (u32)((tileX1 - tileX0 + 1) * (tileY1 - tileY0 + 1));
                variantTileBound[prevVariant] += tiles;
                totalTileBound += tiles;
            }
        }

        //printf("polygon min max %d %d | %d %d\n", RenderPolygons[i].XMin, RenderPolygons[i].XMinY, RenderPolygons[i].XMax, RenderPolygons[i].XMaxY);
    }

    /*for (u32 i = 0; i < RenderNumPolygons; i++)
    {
        if (RenderPolygons[i].Variant >= numVariants)
        {
            printf("blarb2 %d %d %d\n", RenderPolygons[i].Variant, i, RenderNumPolygons);
        }
        //assert(RenderPolygons[i].Variant < numVariants);
    }*/

    SLOW_LAP(Setup);
    if (numYSpans > 0)
    {
        // before anything is uploaded or bound: the tile memory and the X spans get room for what
        // this frame could need. with GPU::ComputeGrow off they take the most at the first frame
        // with polygons, as before
        if (gpu.ComputeGrow)
            GrowWorkBuffers(totalTileBound, (u32)numSetupIndices);
        else
            GrowWorkBuffers((u32)MaxWorkTiles, (u32)maxSetupIndices);
        SLOW_LAP(Grow);

        // a grow that failed can leave less room than binning and InterpXSpans could use, and the
        // shaders only stop at MaxWorkTiles, so the frame isn't drawn: the image stays as the last
        // render left it (a kept one goes back to being the image), and the next frame is drawn
        // even if it's the same. nothing was dispatched yet, and the clear bitmap and the texture
        // cache it updated are what the next render reads anyway
        if (std::min(totalTileBound, (u32)MaxWorkTiles) > (u32)WorkTileRoom || numSetupIndices > XSpanRoom)
        {
            if (keep)
            {
                SpareFramebuffer = Framebuffer;
                Framebuffer = KeptFramebuffer;
                KeptFramebuffer = 0;
            }
            ForceRender = true;
#ifdef MELONADE_DEBUG_TOOLS
            LogSlow(numPolygons, numVariants, totalTileBound, numSetupIndices);
#endif
            return;
        }

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, YSpanSetupMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(SpanSetupY)*numYSpans, YSpanSetups);

        glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
        glBufferSubData(GL_TEXTURE_BUFFER, 0, numSetupIndices*4*2, YSpanIndices.data());

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, RenderPolygonMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, numPolygons*sizeof(RenderPolygon), RenderPolygons);
        // we haven't accessed image data yet, so we don't need to invalidate anything
    }

    //printf("found via %d %d %d of %d\n", foundviatexcache, foundviaprev, numslow, RenderNumPolygons);

    // bind everything
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, RenderPolygonMemory);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, XSpanSetupMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, YSpanSetupMemory);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, FinalTileMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, BinResultMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, WorkDescMemory);

    MetaUniform meta;
    meta.DispCnt = gpu.GPU3D.RenderDispCnt;
    meta.NumPolygons = numPolygons;
    meta.NumVariants = numVariants;
    meta.AlphaRef = gpu.GPU3D.RenderAlphaRef;
    {
        u32 r = (gpu.GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (gpu.GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (gpu.GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = gpu.GPU3D.RenderClearAttr1 & 0x3F008000;

        // in native pixels, wrapping at 256 across the whole target, with native x=0 at the
        // centre 256 columns of a wide one (like the capture readback and GLRenderer)
        int xorigin = (TargetWidth - WideMelon::NativeWidth) / 2;
        meta.ClearBitmapOffset[0] = ((int)((gpu.GPU3D.RenderClearAttr2 >> 16) & 0xFF) - xorigin) & 0xFF;
        meta.ClearBitmapOffset[1] = (gpu.GPU3D.RenderClearAttr2 >> 24) & 0xFF;
    }
    for (u32 i = 0; i < 32; i++)
    {
        u32 color = gpu.GPU3D.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++)
    {
        meta.ToonTable[i*4+1] = gpu.GPU3D.RenderFogDensityTable[i];
    }
    for (u32 i = 0; i < 8; i++)
    {
        u32 color = gpu.GPU3D.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = gpu.GPU3D.RenderFogOffset;
    meta.FogShift = gpu.GPU3D.RenderFogShift;
    {
        u32 fogR = (gpu.GPU3D.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (gpu.GPU3D.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (gpu.GPU3D.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (gpu.GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }

    glBindBuffer(GL_UNIFORM_BUFFER, MetaUniformMemory);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(MetaUniform), &meta);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, MetaUniformMemory);
    SLOW_LAP(Upload);

#ifdef MELONADE_DEBUG_TOOLS
    if (Stats)
    {
        Stats->NoteFrame((u32)numPolygons, numVariants, numYSpans > 0 ? numVariants : 0, (u32)numSetupIndices, (u32)numYSpans, totalTileBound,
            numPolygons < (int)gpu.GPU3D.RenderNumPolygons);
        Stats->BeginSet(false);
    }
#endif

    // the GPU timers (GPU::ComputeTimers): each pass runs up to the next BeginGPUTimer
    BeginGPUTimer(ComputePass::Clear);
    UseProgram(ShaderClearCoarseBinMask);
    glDispatchCompute(TilesPerLine*TileLines/ClearCoarseBinMaskLocalSize, 1, 1);

    bool wbuffer = false;
    if (numYSpans > 0)
    {
        wbuffer = gpu.GPU3D.RenderPolygonRAM[0]->WBuffer;

        // Adreno's driver waits on the CPU for the GPU to finish (gsl_command_waittimestamp, 5-6.5 ms
        // a frame at 4x) at the first indirect dispatch whose arguments the GPU wrote in this frame, so
        // CPU and GPU took turns. it's the indirect dispatch itself, dropping the command barriers
        // alone still waited. by default SortWork and Rasterise are dispatched with sizes from the
        // CPU's bounding box estimate instead, and the shaders loop if binning made more work than
        // that, so nothing here waits for the GPU and the command barriers aren't needed
        const bool directDispatch = gpu.ComputeDirectDispatch;
        const GLbitfield commandBarrier = directDispatch ? 0 : GL_COMMAND_BARRIER_BIT;

        UseProgram(ShaderClearIndirectWorkCount);
        glDispatchCompute((numVariants+31)/32, 1, 1);

        // calculate x-spans
        BeginGPUTimer(ComputePass::Interp);
        glBindImageTexture(0, YSpanIndicesTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        // GPU::ComputeSSBO: the same buffer as a storage buffer. binding 3 is the tile memory's
        // second layer later, which is bound again before Rasterise
        if (SetupSSBO)
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, YSpanIndicesTextureMemory);
        UseProgram(ShaderInterpXSpans[wbuffer]);
        glDispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // bin polygons
        BeginGPUTimer(ComputePass::Bin);
        UseProgram(ShaderBinCombined);
        glDispatchCompute(((numPolygons + 31) / 32), ScreenWidth/CoarseTileW, ScreenHeight/CoarseTileH);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // calculate list offsets
        BeginGPUTimer(ComputePass::Offs);
        UseProgram(ShaderCalculateWorkListOffset);
        glDispatchCompute((numVariants + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // sort shader work
        BeginGPUTimer(ComputePass::Sort);
        UseProgram(ShaderSortWork);
        if (directDispatch)
        {
            // a thread per work tile, binning stops at MaxWorkTiles (TargetFits keeps that to 65535
            // groups)
            u32 sortCount = std::min(totalTileBound, (u32)MaxWorkTiles);
            glDispatchCompute(std::max((sortCount + 31) / 32, 1u), 1, 1);
        }
        else
        {
            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, BinResultMemory);
            glDispatchComputeIndirect(offsetof(BinResultHeader, SortWorkWorkCount));
        }
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        SLOW_LAP(Bin);
        // one timer over every variant's dispatch
        BeginGPUTimer(ComputePass::Rast);
        glActiveTexture(GL_TEXTURE0);

        for (int i = 0; i < TileLayers; i++)
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2+i, TileMemory[i]);

        // rasterise
        {
            bool highLightMode = gpu.GPU3D.RenderDispCnt & (1<<1);

            GLuint shadersNoTexture[] =
            {
                ShaderRasteriseNoTexture[wbuffer],
                ShaderRasteriseNoTexture[wbuffer],
                highLightMode
                    ? ShaderRasteriseNoTextureHighlight[wbuffer]
                    : ShaderRasteriseNoTextureToon[wbuffer],
                ShaderRasteriseNoTexture[wbuffer],
                ShaderRasteriseShadowMask[wbuffer]
            };
            GLuint shadersUseTexture[] =
            {
                ShaderRasteriseUseTextureModulate[wbuffer],
                ShaderRasteriseUseTextureDecal[wbuffer],
                highLightMode
                    ? ShaderRasteriseUseTextureHighlight[wbuffer]
                    : ShaderRasteriseUseTextureToon[wbuffer],
                ShaderRasteriseUseTextureDecal[wbuffer],
                ShaderRasteriseShadowMask[wbuffer]
            };

            GLuint prevShader = 0;
            s32 prevTexture = 0, prevSampler = 0;
            for (int i = 0; i < numVariants; i++)
            {
                GLuint shader = 0;
                if (variants[i].Texture == 0)
                {
                    shader = shadersNoTexture[variants[i].BlendMode];
                }
                else
                {
                    shader = shadersUseTexture[variants[i].BlendMode];
                    if (variants[i].Texture != prevTexture)
                    {
                        glBindTexture(GL_TEXTURE_2D_ARRAY, variants[i].Texture);
                        prevTexture = variants[i].Texture;
                    }
                    if (variants[i].Sampler != prevSampler)
                    {
                        glBindSampler(0, variants[i].Sampler);
                        prevSampler = variants[i].Sampler;
                    }
                }
                assert(shader != 0);
                if (shader != prevShader)
                {
                    UseProgram(shader);
                    prevShader = shader;
                }

                glUniform1ui(UniformIdxCurVariant, i);
                glUniform1ui(UniformIdxColorDither, ColorDither ? 1 : 0);
                // OptSmall: the texcoords' 4 fraction bits come off here instead of in the shader.
                // the sizes are powers of two, so nothing is rounded
                float texcoordScale = (OptMask & ComputeOpt_Small) ? 1.f / 16.f : 1.f;
                glUniform2f(UniformIdxTextureSize, texcoordScale / variants[i].Width, texcoordScale / variants[i].Height);
                if (directDispatch)
                {
                    // a group per work tile, split across Y and Z like CalcOffsets does. at least
                    // one, so the loop in the shader covers the variant even if the estimate is off
                    u32 groups = std::clamp(variantTileBound[i], 1u, (u32)MaxWorkTiles);
                    u32 chunks = (groups + 65534) / 65535;
                    glDispatchCompute(1, chunks, (groups + chunks - 1) / chunks);
                }
                else
                {
                    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, BinResultMemory);
                    glDispatchComputeIndirect(offsetof(BinResultHeader, VariantWorkCount) + i*4*4);
                }
            }
        }
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    SLOW_LAP(Rast);

    BeginGPUTimer(ComputePass::Depth);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
    glActiveTexture(GL_TEXTURE0);

    // compose final image
#ifdef MELONADE_DEBUG_TOOLS
    if (DepthCounter)
        DepthCounter->Ring.BeginFrame(ComputeDepthCounter::Binding, ScreenWidth * ScreenHeight);
#endif
    UseProgram(ShaderDepthBlend[wbuffer]);
    glDispatchCompute(ScreenWidth/TileSize, ScreenHeight/TileSize, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
#ifdef MELONADE_DEBUG_TOOLS
    if (DepthCounter)
        DepthCounter->Ring.EndFrame();
#endif

    BeginGPUTimer(ComputePass::Final);
    glBindImageTexture(0, Framebuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, LowResFramebuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
    u32 finalPassShader = 0;
    if (gpu.GPU3D.RenderDispCnt & (1<<4))
        finalPassShader |= 0x4;
    if (gpu.GPU3D.RenderDispCnt & (1<<7))
        finalPassShader |= 0x2;
    if (gpu.GPU3D.RenderDispCnt & (1<<5))
        finalPassShader |= 0x1;
    
    UseProgram(ShaderFinalPass[finalPassShader]);
    glDispatchCompute(ScreenWidth/32, ScreenHeight, 1);
    // the compositor reads the 3D image with texelFetch, and capture reads the low res one with glReadPixels
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    SLOW_LAP(Final);

#ifdef MELONADE_DEBUG_TOOLS
    if (Stats)
    {
        Stats->EndSet();
        if (Stats->WantPeaks)
        {
            // what binning asked for is VariantWorkCount[0].w, which only the GPU knows. mapping
            // it waits for the GPU. without polygons nothing was binned and the count is an old one
            u32 wanted = 0;
            if (numYSpans > 0)
            {
                glBindBuffer(GL_COPY_READ_BUFFER, BinResultMemory);
                const u32* counts = (const u32*)glMapBufferRange(GL_COPY_READ_BUFFER,
                    offsetof(BinResultHeader, VariantWorkCount), 4 * sizeof(u32), GL_MAP_READ_BIT);
                if (counts)
                {
                    wanted = counts[3];
                    glUnmapBuffer(GL_COPY_READ_BUFFER);
                }
                glBindBuffer(GL_COPY_READ_BUFFER, 0);
            }
            Stats->NoteWorkTiles(wanted, (u32)MaxWorkTiles, (u32)WorkTileRoom, totalTileBound);
        }
    }
#endif

    // GPU::ComputePace: signals when the GPU has finished this render. flushed right away, so a
    // poll never finds it unflushed: the frontend's frame-end flush doesn't run while the DS sleeps
    if (gpu.ComputePace != 0)
    {
        if (PaceFenceCount == MaxPaceFences)
            PopPaceFences(1);
        PaceFences[PaceFenceCount++] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        PaceRenderNs = SteadyNowNs();
        PaceLog.Renders++;
    }

    glBindSampler(0, 0);
#ifdef MELONADE_DEBUG_TOOLS
    SLOW_LAP(End);
    LogSlow(numPolygons, numVariants, totalTileBound, numSetupIndices);
#endif

    // GPU::ComputeCheck: every Nth render with polygons, after all of its dispatches (and
    // after COMPSLOW's lap, which it would slow down)
    if (gpu.ComputeCheck > 0 && numYSpans > 0)
    {
        if (CheckRenders++ % (u32)gpu.ComputeCheck == 0)
            RunCheck(numPolygons, numVariants, numSetupIndices, totalTileBound);
    }

    /*u64 starttime = armGetSystemTick();
    EmuQueue.waitIdle();
    printf("total time %f\n", armTicksToNs(armGetSystemTick()-starttime)*0.000001f);*/

    /*for (u32 i = 0; i < RenderNumPolygons; i++)
    {
        if (RenderPolygons[i].Variant >= numVariants)
        {
            printf("blarb %d %d %d\n", RenderPolygons[i].Variant, i, RenderNumPolygons);
        }
        //assert(RenderPolygons[i].Variant < numVariants);
    }*/

    /*for (int i = 0; i < binresult->SortWorkWorkCount[0]*32; i++)
    {
        printf("sorted %x %x\n", binresult->SortedWork[i*2+0], binresult->SortedWork[i*2+1]);
    }*/
/*    if (polygonvisible != -1)
    {
        SpanSetupX* xspans = Gfx::DataHeap->CpuAddr<SpanSetupX>(XSpanSetupMemory);
        printf("span result\n");
        Polygon* poly = RenderPolygonRAM[polygonvisible];
        u32 xspanoffset = RenderPolygons[polygonvisible].FirstXSpan;
        for (u32 i = 0; i < (poly->YBottom - poly->YTop); i++)
        {
            printf("%d: %d - %d | %d %d | %d %d\n", i + poly->YTop, xspans[xspanoffset + i].X0, xspans[xspanoffset + i].X1, xspans[xspanoffset + i].__pad0, xspans[xspanoffset + i].__pad1, RenderPolygons[polygonvisible].YTop, RenderPolygons[polygonvisible].YBot);
        }
    }*/
/*
    printf("xspans: %d\n", numSetupIndices);
    SpanSetupX* xspans = Gfx::DataHeap->CpuAddr<SpanSetupX>(XSpanSetupMemory[curSlice]);
    for (int i = 0; i < numSetupIndices; i++)
    {
        printf("poly %d %d %d | line %d | %d to %d\n", YSpanIndices[i].PolyIdx, YSpanIndices[i].SpanIdxL, YSpanIndices[i].SpanIdxR, YSpanIndices[i].Y, xspans[i].X0, xspans[i].X1);
    }
    printf("bin result\n");
    BinResult* binresult = Gfx::DataHeap->CpuAddr<BinResult>(BinResultMemory);
    for (u32 y = 0; y < 192/8; y++)
    {
        for (u32 x = 0; x < 256/8; x++)
        {
            printf("%08x ", binresult->BinnedMaskCoarse[(x + y * (256/8)) * 2]);
        }
        printf("\n");
    }*/
}

void ComputeRenderer::RestartFrame(GPU& gpu)
{

}

u32* ComputeRenderer::GetLine(int line)
{
    int stride = 256;

    if (line == 0)
    {
#ifdef MELONADE_DEBUG_TOOLS
        s64 start = SteadyNowNs();
#endif
        if (CaptureFence)
        {
            // wait for the readback on the CPU, polling, instead of sleeping in the map, so the
            // emulator thread's clock doesn't drop meanwhile. the first poll also flushes, in case
            // nothing flushed the fence yet (the early flushes are off)
            s64 deadline = SteadyNowNs() + (s64)CaptureSpinUs * 1000;
            GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
            while (glClientWaitSync(CaptureFence, flags, 0) == GL_TIMEOUT_EXPIRED && SteadyNowNs() < deadline)
            {
                flags = 0;
                std::this_thread::yield();
            }
            glDeleteSync(CaptureFence);
            CaptureFence = nullptr;
        }
#ifdef MELONADE_DEBUG_TOOLS
        s64 mapStart = SteadyNowNs();
#endif

        glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelBuffer);
        u8* data = (u8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * stride * 192, GL_MAP_READ_BIT);
        if (data) memcpy(&FramebufferCPU[0], data, 4*stride*192);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

#ifdef MELONADE_DEBUG_TOOLS
        s64 end = SteadyNowNs();
        CaptureMapCount++;
        CaptureSpinNs += mapStart - start;
        CaptureMapWaitNs += end - mapStart;
        if (RenderFrameStartNs != 0 && end - RenderFrameStartNs < 100000000)
            CaptureSpanNs += end - RenderFrameStartNs;
#endif
    }

    return &FramebufferCPU[stride * line];
}

void ComputeRenderer::SetupAccelFrame()
{
    // the 3D image of the Blit's own frame, kept while the next one is drawn (see KeepFrameForBlit)
    glBindTexture(GL_TEXTURE_2D, KeptFramebuffer ? KeptFramebuffer : Framebuffer);
}

void ComputeRenderer::PrepareCaptureFrame()
{
    ReadBackCaptureFrame(PixelBuffer, CaptureFence);
}

void ComputeRenderer::ReadBackCaptureFrame(GLuint pixelBuffer, GLsync& fence) noexcept
{
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixelBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, CaptureFrameBuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, LowResFramebuffer, 0);
    glReadPixels(0, 0, 256, 192, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // GetLine(0) polls this before it maps the readback (see GPU::CaptureSpinUs)
    if (fence)
    {
        glDeleteSync(fence);
        fence = nullptr;
    }
    if (CaptureSpinUs > 0)
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

// the width RenderFrame sizes the targets for: the polygons match them, so after a width change
// they only shrink once the game has built geometry at the new width (see GLRenderer)
int ComputeRenderer::FrameTargetWidth(const GPU& gpu) noexcept
{
    int targetWidth = std::max(ViewWidth, gpu.GPU3D.RenderViewWidth);
    if (!TargetFits(targetWidth, ScaleFactor) ||
        (targetWidth != WideMelon::NativeWidth && !CurGLCompositor.PrepareWide()))
        targetWidth = ViewWidth;
    return targetWidth;
}

// GPU::BlitAfterReadback, at line 215: the compositor's size follows the targets, so a render that
// resizes them gets the Blit first. otherwise the render writes the spare 3D image and the one it
// replaces stays for the Blit (see KeepFramebuffer). a render that's skipped keeps nothing, and the
// Blit reads the image as it was
bool ComputeRenderer::KeepFrameForBlit(const GPU& gpu)
{
    // as RenderFrame will see it, which TileSizeFor goes by too
    WideList = gpu.GPU3D.RenderWideList;
    int targetWidth = FrameTargetWidth(gpu);
    if (targetWidth != TargetWidth || TileSizeFor(targetWidth, ScaleFactor, ForcedTileSizeOf(gpu)) != TileSize
        || MaxPolygonsFor(targetWidth, WideList) != MaxPolygons)
        return false;

    KeepRequested = true;
    return true;
}

// the render about to start writes the spare 3D image (the final pass binds Framebuffer), and the
// one it replaces is kept for the Blit. made like ResizeTargets makes Framebuffer
void ComputeRenderer::KeepFramebuffer() noexcept
{
    if (!SpareFramebuffer)
    {
        glGenTextures(1, &SpareFramebuffer);
        glBindTexture(GL_TEXTURE_2D, SpareFramebuffer);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TargetWidth * ScaleFactor, ScreenHeight);
    }

    KeptFramebuffer = Framebuffer;
    Framebuffer = SpareFramebuffer;
    SpareFramebuffer = 0;
}

// GPU::BlitAfterReadback: line 0's readback, made right after the render at line 215, into the
// early buffer with its own fence, so GetLine's buffer and fence stay as the last line-0 readback
// left them until line 0 takes this. it reads the low res image, and whatever could change that
// before line 0 drops it: a render (HalfCamera's redraw at line 0 too), a resize, a reset, a stop
// or a savestate load
void ComputeRenderer::PrepareEarlyCaptureFrame()
{
    ReadBackCaptureFrame(EarlyPixelBuffer, EarlyCaptureFence);
    EarlyCapture = true;
}

// at line 0, with use where PrepareCaptureFrame would run: the early buffer and fence are what it
// would make, so they swap in for GetLine's, and its old fence goes as PrepareCaptureFrame would
// have deleted it. without use, or once it's stale, GetLine's stay as they are
bool ComputeRenderer::TakeEarlyCaptureFrame(bool use)
{
    // this frame reads the 3D back, here or with the PrepareCaptureFrame that follows
    CapturedSinceRender |= use;

    if (!use || !EarlyCapture)
    {
        DropEarlyCapture();
        return false;
    }

    std::swap(PixelBuffer, EarlyPixelBuffer);
    if (CaptureFence)
        glDeleteSync(CaptureFence);
    CaptureFence = EarlyCaptureFence;
    EarlyCaptureFence = nullptr;
    EarlyCapture = false;
    return true;
}

void ComputeRenderer::DropEarlyCapture() noexcept
{
    EarlyCapture = false;
    if (EarlyCaptureFence)
    {
        glDeleteSync(EarlyCaptureFence);
        EarlyCaptureFence = nullptr;
    }
}

void ComputeRenderer::FlushCommands()
{
    glFlush();
}

void ComputeRenderer::SetOutputTexture(int buffer, u32 texture)
{
    CurGLCompositor.SetOutputTexture(buffer, (GLuint) texture);
}

void ComputeRenderer::BindOutputTexture(int buffer)
{
    CurGLCompositor.BindOutputTexture(buffer);
}

void ComputeRenderer::Blit(const GPU &gpu)
{
#ifdef MELONADE_DEBUG_TOOLS
    // the compositor's GPU timers, which it starts through BeginGPUTimer
    if (Stats)
        Stats->BeginSet(true);
#endif
    CurGLCompositor.RenderFrame(gpu, *this);
#ifdef MELONADE_DEBUG_TOOLS
    if (Stats)
        Stats->EndSet();
#endif

    // the compositor and its hi-res capture pass were the last to read the kept image. the final
    // pass that next writes it comes later in command order, the same as it always followed the
    // compositor's reads of the one image there was
    if (KeptFramebuffer)
    {
        SpareFramebuffer = KeptFramebuffer;
        KeptFramebuffer = 0;
    }
}

void ComputeRenderer::Stop(const GPU &gpu)
{
    CurGLCompositor.Stop(gpu);
    DropEarlyCapture();
}

}
