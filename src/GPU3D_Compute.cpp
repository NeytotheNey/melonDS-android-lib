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
#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>
#include <utility>

#include "Utils.h"
#include "Platform.h"
#include "WideMelon.h"

#include "OpenGLSupport.h"

#include "GPU3D_Compute_shaders.h"

namespace melonDS
{

static s64 SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ComputeRenderer::ComputeRenderer(GLCompositor&& compositor)
    : Renderer3D(true), Texcache(TexcacheOpenGLLoader()), CurGLCompositor(std::move(compositor))
{}

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
    shaderSource += "#define ScreenWidth ";
    shaderSource += std::to_string(ScreenWidth);
    shaderSource += "\n#define ScreenHeight ";
    shaderSource += std::to_string(ScreenHeight);
    shaderSource += "\n#define MaxWorkTiles ";
    shaderSource += std::to_string(MaxWorkTiles);
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
    shaderSource += source;

    std::string filePath = "/data/data/me.magnum.melonds.dev/files/" + shaderFileName + ".txt";
    //FILE* f = fopen(filePath.c_str(), "w");
    //if (f)
    //{
    //    fwrite(shaderSource.c_str(), shaderSource.size(), 1, f);
    //    fclose(f);
    //}

    return OpenGL::CompileComputeProgram(shader, shaderSource.c_str(), shaderName.c_str());
}

void ComputeRenderer::ShaderCompileStep(int& current, int& count)
{
    current = ShaderStepIdx;
    ShaderStepIdx++;
    count = 33;
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
    // and the work buffers grow with the width
    GLint64 maxBufferSize = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBufferSize);
    result->MaxBufferSize = maxBufferSize;
    // for a forced tile size (TileSizeFor)
    GLint maxGroupThreads = 0;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxGroupThreads);
    result->MaxGroupThreads = maxGroupThreads;

    //glDebugMessageCallback(blah, NULL);
    //glEnable(GL_DEBUG_OUTPUT);
    glGenBuffers(1, &result->YSpanSetupMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result->YSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupY)*MaxYSpanSetups, nullptr, GL_DYNAMIC_DRAW);
    
    glGenBuffers(1, &result->RenderPolygonMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result->RenderPolygonMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(RenderPolygon)*2048, nullptr, GL_DYNAMIC_DRAW);

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
}

void ComputeRenderer::Reset(GPU& gpu)
{
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
    return maxWorkTiles <= 65535 * 32 ? forced : tileSize;
}

bool ComputeRenderer::TargetFits(int width, int scale) const noexcept
{
    if (width == WideMelon::NativeWidth)
        return true;
    if (MaxTargetWidth > 0 && width * scale > MaxTargetWidth)
        return false;

    // the biggest work buffer is a tile memory layer: 4*TileSize^2 bytes for each of the
    // MaxWorkTiles (16 per tile), so 64 bytes per pixel of the rounded up target
    int tileSize = TileSizeFor(width, scale, ForcedTileSize);
    s64 screenW = (s64)InternalWidthFor(width, scale, tileSize) * scale;
    s64 largestBuffer = 64 * screenW * (192 * scale);

    // the WorkDescs keep a work tile's index in 21 bits, and SortWork dispatches a group per 32
    // of them along X, which only goes to 65535
    s64 maxWorkTiles = (screenW / tileSize) * (192 * scale / tileSize) * 16;
    if (maxWorkTiles > 65535 * 32)
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
    CurGLCompositor.SetRenderSettings(scale, targetWidth);

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

    MaxWorkTiles = TilesPerLine*TileLines*16;

    for (int i = 0; i < tilememoryLayer_Num; i++)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, TileMemory[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4*TileSize*TileSize*MaxWorkTiles, nullptr, GL_DYNAMIC_DRAW);
    }

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

    // eh those are pretty bad guesses
    // though real hw shouldn't be eable to render all 2048 polygons on every line either
    int maxYSpanIndices = 64*2048 * ScaleFactor;
    YSpanIndices.resize(maxYSpanIndices);

    glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
    glBufferData(GL_TEXTURE_BUFFER, maxYSpanIndices*2*4, nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, XSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupX)*maxYSpanIndices, nullptr, GL_DYNAMIC_DRAW);

    glBindTexture(GL_TEXTURE_BUFFER, YSpanIndicesTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA16UI, YSpanIndicesTextureMemory);
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

    Stats->FrameStart(RenderFrameStartNs, ScaleFactor, TileSize, (u32)MaxWorkTiles, (u32)YSpanIndices.size(),
        (u32)MaxYSpanSetups, (u32)MaxVariants);
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

void ComputeRenderer::RenderFrame(GPU& gpu)
{
#ifdef MELONADE_DEBUG_TOOLS
    RenderFrameStartNs = SteadyNowNs();
    if (gpu.ComputeTimers || gpu.ComputePeaks || gpu.ComputeSync || Stats)
        UpdateStats(gpu);
#endif
    CaptureSpinUs = gpu.CaptureSpinUs;
    // the COMPWHY log tells a swap with the same scene from one with a new scene
    gpu.GPU3D.SceneHashLogging = gpu.ComputeWhy;

    // GPU::ComputePace: leaves a fence for each earlier render the GPU hasn't finished
    if (gpu.ComputePace != 0 || PaceFenceCount != 0)
        UpdatePace(gpu);
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
    // changes it, so it's the same compile as for a new target
    int optMask = gpu.ComputeOpt & ComputeOpt_All;
    bool optChanged = ((optMask ^ OptMask) & ComputeOpt_Shaders) != 0;
    OptMask = optMask;
    // ComputeOpt_SwapSkip: has GPU3D compare each swap's scene with the one before, from the next
    // VBlank on. a swap that changed nothing a render reads then counts like a frame without one
    gpu.GPU3D.SceneRepeatCheck = (OptMask & ComputeOpt_SwapSkip) != 0;
    bool identical = gpu.GPU3D.RenderFrameIdentical
        || ((OptMask & ComputeOpt_SwapSkip) && gpu.GPU3D.RenderSceneRepeated);
    int targetWidth = FrameTargetWidth(gpu);
    if (targetWidth != TargetWidth || TileSizeFor(targetWidth, ScaleFactor, ForcedTileSize) != TileSize || optChanged)
    {
        keep = false;
        ResizeTargets(ScaleFactor, HiresCoordinates, targetWidth);
        int current, count;
        while (NeedsShaderCompile())
            ShaderCompileStep(current, count);
    }

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

    int numYSpans = 0;
    int numSetupIndices = 0;
    // the polygons that fit in the span buffers, the rest of the frame is dropped
    int numPolygons = gpu.GPU3D.RenderNumPolygons;
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
    // their bounding boxes. binning normally makes no more work than this, and if it does the shaders
    // loop over the rest, so it only decides how many work groups are launched
    u32 variantTileBound[MaxVariants];
    u32 totalTileBound = 0;

    bool enableTextureMaps = gpu.GPU3D.RenderDispCnt & (1<<0);

    for (int i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
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
                Texcache.GetTexture(gpu, polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer, textureLastVariant);
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
        if (numYSpans + 2 * (int)nverts > MaxYSpanSetups
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

    if (numYSpans > 0)
    {
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
    glUseProgram(ShaderClearCoarseBinMask);
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

        glUseProgram(ShaderClearIndirectWorkCount);
        glDispatchCompute((numVariants+31)/32, 1, 1);

        // calculate x-spans
        BeginGPUTimer(ComputePass::Interp);
        glBindImageTexture(0, YSpanIndicesTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glUseProgram(ShaderInterpXSpans[wbuffer]);
        glDispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // bin polygons
        BeginGPUTimer(ComputePass::Bin);
        glUseProgram(ShaderBinCombined);
        glDispatchCompute(((numPolygons + 31) / 32), ScreenWidth/CoarseTileW, ScreenHeight/CoarseTileH);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // calculate list offsets
        BeginGPUTimer(ComputePass::Offs);
        glUseProgram(ShaderCalculateWorkListOffset);
        glDispatchCompute((numVariants + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | commandBarrier);

        // sort shader work
        BeginGPUTimer(ComputePass::Sort);
        glUseProgram(ShaderSortWork);
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

        // one timer over every variant's dispatch
        BeginGPUTimer(ComputePass::Rast);
        glActiveTexture(GL_TEXTURE0);

        for (int i = 0; i < tilememoryLayer_Num; i++)
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
                    glUseProgram(shader);
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

    BeginGPUTimer(ComputePass::Depth);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
    glActiveTexture(GL_TEXTURE0);

    // compose final image
    glUseProgram(ShaderDepthBlend[wbuffer]);
    glDispatchCompute(ScreenWidth/TileSize, ScreenHeight/TileSize, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

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
    
    glUseProgram(ShaderFinalPass[finalPassShader]);
    glDispatchCompute(ScreenWidth/32, ScreenHeight, 1);
    // the compositor reads the 3D image with texelFetch, and capture reads the low res one with glReadPixels
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

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
            Stats->NoteWorkTiles(wanted, (u32)MaxWorkTiles);
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
    int targetWidth = FrameTargetWidth(gpu);
    if (targetWidth != TargetWidth || TileSizeFor(targetWidth, ScaleFactor, ForcedTileSizeOf(gpu)) != TileSize)
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
