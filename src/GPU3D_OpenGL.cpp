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

bool GLRenderer::BuildRenderShader(bool wbuffer, bool fragCount)
{
    char shadername[32];
    snprintf(shadername, sizeof(shadername), "RenderShader%c%s", wbuffer?'W':'Z', fragCount ? "Count" : "");

    // kShaderHeader has no newline of its own, the common sources start with one
    const char* wbufdef = wbuffer ? "\n#define WBuffer" : "";

    std::string vsbuf;
    vsbuf += kShaderHeader;
    vsbuf += wbufdef;
    vsbuf += kRenderVSCommon;
    vsbuf += kRenderVS;

    std::string fsbuf;
    fsbuf += kShaderHeader;
    fsbuf += wbufdef;
    fsbuf += kRenderFSCommon;
#ifdef MELONADE_DEBUG_TOOLS
    if (fragCount)
    {
        // debug only: the counter declarations, and the count first thing in main
        fsbuf += kRenderFSFragCount;
        std::string fs = kRenderFS;
        size_t mainPos = fs.find("void main()");
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
        {{"vPosition", 0}, {"vColor", 1}, {"vTexcoord", 2}, {"vPolygonAttr", 3}},
        {{"oColor", 0}, {"oAttr", 1}});

    if (!ret) return false;

    GLint uni_id = glGetUniformBlockIndex(prog, "uConfig");
    glUniformBlockBinding(prog, uni_id, 0);

    glUseProgram(prog);

    uni_id = glGetUniformLocation(prog, "TexMem");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(prog, "TexPalMem");
    glUniform1i(uni_id, 1);

    int id = (int)wbuffer;
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

void GLRenderer::UseRenderShader(bool wbuffer, int mode)
{
    int id = (int)wbuffer;
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

#ifdef MELONADE_DEBUG_TOOLS
    // for a later experiment (pass 2's rule for clear alpha 0 done in the shader, see
    // docs/plan-gpu-discovery.md T1): whether the shader could read the color target. one name at a
    // time with glGetStringi, which GLES 3 and a desktop core profile both have (glGetString's list
    // is GL_INVALID_ENUM on the latter), compared whole, since the EXT one is a prefix of its
    // non-coherent sibling
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
        Platform::Log(Platform::LogLevel::Info, "GLTRANS: framebuffer fetch GL_EXT_shader_framebuffer_fetch=%d "
            "GL_ARM_shader_framebuffer_fetch=%d GL_EXT_shader_framebuffer_fetch_non_coherent=%d\n",
            hasExtension("GL_EXT_shader_framebuffer_fetch") ? 1 : 0,
            hasExtension("GL_ARM_shader_framebuffer_fetch") ? 1 : 0,
            hasExtension("GL_EXT_shader_framebuffer_fetch_non_coherent") ? 1 : 0);
    }
#endif

    return result;
}

GLRenderer::~GLRenderer()
{
    assert(glDeleteTextures != nullptr);

    glDeleteTextures(1, &TexMemID);
    glDeleteTextures(1, &TexPalMemID);

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
    glDeleteVertexArrays(1, &ClearVertexArrayID);
    glDeleteBuffers(1, &ClearVertexBufferID);

    glDeleteBuffers(1, &ShaderConfigUBO);

    for (int i = 0; i < 2; i++)
    {
        if (!RenderShader[i]) continue;
        glDeleteProgram(RenderShader[i]);
    }
    glDeleteProgram(ClearShaderPlain);
    glDeleteProgram(ClearShaderBitmap);
    glDeleteProgram(FinalPassEdgeShader);
    glDeleteProgram(FinalPassFogShader);

    if (ShadowLogQueries[0]) glDeleteQueries(64, ShadowLogQueries);

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
    TargetsChanged = true;
    EarlyCapture = false;
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

void GLRenderer::SetViewWidth(int width) noexcept
{
    RequestedViewWidth = WideMelon::SanitizeWidth(width);

    // the scale decides whether it fits, the first SetRenderSettings resolves it otherwise.
    // the targets follow in RenderFrame
    if (ScaleFactor > 0)
        ViewWidth = ResolveViewWidth(RequestedViewWidth, ScaleFactor);
}

void GLRenderer::SetRenderSettings(bool betterpolygons, int scale) noexcept
{
    ViewWidth = ResolveViewWidth(RequestedViewWidth, scale);

    int targetWidth = TargetWidth;
    if (!TargetFits(targetWidth, scale))
        targetWidth = ViewWidth;

    ResizeTargets(betterpolygons, scale, targetWidth);
}

// true when anything changed
bool GLRenderer::ResizeTargets(bool betterpolygons, int scale, int targetWidth) noexcept
{
    if (betterpolygons == BetterPolygons && scale == ScaleFactor && targetWidth == TargetWidth)
        return false;

    CurGLCompositor.SetRenderSettings(scale, targetWidth);
    ScaleFactor = scale;
    TargetWidth = targetWidth;
    BetterPolygons = betterpolygons;
    TargetsChanged = true;
    // the pixel buffers are reallocated below
    EarlyCapture = false;

    ScreenW = targetWidth * scale;
    ScreenH = 192 * scale;

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

// GPU::BlitAfterReadback, at line 215: the compositor's size follows the targets, so a render that
// resizes them gets the Blit first. otherwise the render draws into the spare color buffer and the
// one it replaces stays for the Blit (see KeepColorBuffer). a render that's skipped keeps nothing,
// and the Blit reads the color buffer as it was
bool GLRenderer::KeepFrameForBlit(const GPU& gpu)
{
    if (FrameTargetWidth(gpu) != TargetWidth)
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

u32* GLRenderer::SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32* vptr) const
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

    *vptr++ = x | (y << 16);
    *vptr++ = z | (w << 16);

    *vptr++ =  (vtx->FinalColor[0] >> 1) |
              ((vtx->FinalColor[1] >> 1) << 8) |
              ((vtx->FinalColor[2] >> 1) << 16) |
              (alpha << 24);

    *vptr++ = (u16)vtx->TexCoords[0] | ((u16)vtx->TexCoords[1] << 16);

    // Split TexParam into 2 because some GPUs don't have 32 bit ints. TexPalette only uses 13 bits
    *vptr++ = vtxattr | (zshift << 16);
    *vptr++ = poly->TexParam & 0xFFFF;
    *vptr++ = (poly->TexParam >> 16 ) | (poly->TexPalette << 16);

    return vptr;
}

// wide views always go through the hi-res position, which GPU3D already placed on the wide target
u32 GLRenderer::WideVertexX(s32 hiresx) const
{
    return (std::max(hiresx + WideXAdjust, 0) * ScaleFactor) >> 4;
}

void GLRenderer::BuildPolygons(GLRenderer::RendererPolygon* polygons, int npolys)
{
    u32* vptr = &VertexBuffer[0];
    u32 vidx = 0;

    u32 iidx = 0;
    u32 eidx = EdgeIndicesOffset;

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

                vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

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

                vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);
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

                    vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

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
                *vptr++ = poly->TexParam & 0xFFFF;
                *vptr++ = (poly->TexParam >> 16 ) | (poly->TexPalette << 16);

                vidx++;

                // build the final polygon
                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

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

        rp->EdgeIndicesOffset = eidx;
        rp->NumEdgeIndices = 0;

        u32 vidx_cur = vidx_first;
        for (u32 j = 1; j < poly->NumVertices; j++)
        {
            IndexBuffer[eidx++] = vidx_cur;
            IndexBuffer[eidx++] = vidx_cur + 1;
            vidx_cur++;
            rp->NumEdgeIndices += 2;
        }
        IndexBuffer[eidx++] = vidx_cur;
        IndexBuffer[eidx++] = vidx_first;
        rp->NumEdgeIndices += 2;
    }

    NumVertices = vidx;
    NumIndices = iidx;
    NumEdgeIndices = eidx - EdgeIndicesOffset;
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

int GLRenderer::RenderPolygonEdgeBatch(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];
    u32 key = rp->RenderKey;
    int numpolys = 0;
    u32 numindices = 0;

    for (int iend = i; iend < NumFinalPolys; iend++)
    {
        const RendererPolygon* cur_rp = &PolygonList[iend];
        if (cur_rp->RenderKey != key) break;

        numpolys++;
        numindices += cur_rp->NumEdgeIndices;
    }

    glDrawElements(GL_LINES, numindices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->EdgeIndicesOffset * 2));
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

        glScissor(0, y*ScaleFactor, ScreenW, (yend-y)*ScaleFactor);
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
        bias = CurShaderID ? -255.0f : -512.0f;

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

    // if edge marking is enabled, mark all opaque edges
    // TODO BETTER EDGE MARKING!!! THIS SUCKS
    /*if (RenderDispCnt & (1<<5))
    {
        UseRenderShader(wbuffer, RenderMode_Edge); // no such mode exists any more, the edge pass was never built
        glLineWidth(1.5);

        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);

        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_FALSE);

        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);

        for (int i = 0; i < NumFinalPolys; )
        {
            RendererPolygon* rp = &PolygonList[i];

            if (rp->PolyData->IsShadowMask) { i++; continue; }

            i += RenderPolygonEdgeBatch(i);
        }

        glDepthMask(GL_TRUE);
    }*/

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

        // pass 2: if needed, render translucent pixels that are against background pixels
        // when background alpha is zero, those need to be rendered with blending disabled
        // (with a clear bitmap this still tests the clear attribute's alpha, not the bitmap's
        // per-pixel alpha; upstream's GL renderer has the same gap)

        if (clear0)
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
                        UseRenderShader(wbuffer, RenderMode_Opaque);

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

                    UseRenderShader(wbuffer, RenderMode_Translucent);

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
                // a depth-equal polygon's GL_LEQUAL can pass again, so it's still drawn
                bool redrawn = needopaque && clear0 && (GLTransSkip & 1) && !(polyattr & (1<<14));
                if (redrawn)
                    TRANS_SKIPPED(Skip0);

                bool drawopaque, drawtrans;
                needsDraws(i, needopaque, redrawn, drawopaque, drawtrans);

                if (drawopaque)
                {
                    UseRenderShader(wbuffer, RenderMode_Opaque);

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

                UseRenderShader(wbuffer, RenderMode_Translucent);

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
                    glEnable(GL_BLEND);
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

    if (gpu3d.RenderDispCnt & 0x00A0) // fog/edge enabled
    {
        // the depth buffer's detach goes with the first of them
        if (gpu3d.RenderDispCnt & (1<<5)) GL_TIMER(Edge);
        else                              GL_TIMER(Fog);

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

        if (gpu3d.RenderDispCnt & (1<<5))
        {
            // edge marking
            // TODO: depth/polyid values at screen edges

            glUseProgram(FinalPassEdgeShader);

            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }

        if (gpu3d.RenderDispCnt & (1<<7))
        {
            // fog

            if (gpu3d.RenderDispCnt & (1<<5)) GL_TIMER(Fog);
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

    // the targets match the polygons drawn here (see FrameTargetWidth). this runs after the
    // frame's Blit, so the compositor's width only changes between frames as well. with a Blit
    // after it (KeepFrameForBlit), only when the targets stay as they are
    if (ResizeTargets(BetterPolygons, ScaleFactor, FrameTargetWidth(gpu)))
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

    CurShaderID = -1;
    CurRenderMode = -1;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, MainFramebuffer);

    ShaderConfig.uScreenSize[0] = ScreenW;
    ShaderConfig.uScreenSize[1] = ScreenH;
    ShaderConfig.uDispCnt = gpu.GPU3D.RenderDispCnt;

    WideFrame = TargetWidth != WideMelon::NativeWidth || gpu.GPU3D.RenderViewWidth != WideMelon::NativeWidth;
    WideXAdjust = (TargetWidth - gpu.GPU3D.RenderViewWidth) << 3;
    ShadowMaskLineClear = gpu.ShadowMaskLineClear;
    ShadowMaskDepthTest = gpu.ShadowMaskDepthTest;
    EqualDepthMargin = gpu.EqualDepthMargin;
    GLTransSkip = gpu.GLTransSkip;

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

        glDepthFunc(GL_ALWAYS);
        glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        if (gpu.GPU3D.RenderDispCnt & (1<<14))
        {
            // clear bitmap: the shader reads it from TexMem, which the dirty tracking above
            // keeps current (and a write to it defeats the identical-frame skip). the bitmap
            // is tiled at native resolution across the whole target, with native x=0 at the
            // centre 256 columns of a wide one, like the capture readback
            glUseProgram(ClearShaderBitmap);

            int xorigin = ((TargetWidth - WideMelon::NativeWidth) / 2);
            int xoff = ((int)((gpu.GPU3D.RenderClearAttr2 >> 16) & 0xFF) - xorigin) & 0xFF;
            int yoff = (gpu.GPU3D.RenderClearAttr2 >> 24) & 0xFF;

            glUniform1i(ClearBitmapULoc[0], ScaleFactor);
            glUniform2i(ClearBitmapULoc[1], xoff, yoff);
            glUniform1ui(ClearBitmapULoc[2], polyid);
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

        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);
        glDrawArrays(GL_TRIANGLES, 0, 2*3);
    }

    if (gpu.GPU3D.RenderNumPolygons)
    {
        GL_TIMER(Upload);
        // render shit here
        int npolys = 0;
        int firsttrans = -1;
        for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
        {
            if (gpu.GPU3D.RenderPolygonRAM[i]->Degenerate) continue;

            SetupPolygon(&PolygonList[npolys], gpu.GPU3D.RenderPolygonRAM[i]);
            if (firsttrans < 0 && gpu.GPU3D.RenderPolygonRAM[i]->Translucent)
                firsttrans = npolys;

            npolys++;
        }
        NumFinalPolys = npolys;
        NumOpaqueFinalPolys = firsttrans;

        BuildPolygons(&PolygonList[0], npolys);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices*7*4, VertexBuffer);

        // bind to access the index buffer
        glBindVertexArray(VertexArrayID);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, NumIndices * 2, IndexBuffer);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, EdgeIndicesOffset * 2, NumEdgeIndices * 2, IndexBuffer + EdgeIndicesOffset);

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
    // (and GetLine) read back the centre of a wide target
    int captureX = ((TargetWidth - WideMelon::NativeWidth) / 2) * ScaleFactor;
    glBlitFramebuffer(captureX, 0, captureX + WideMelon::NativeWidth * ScaleFactor, ScreenH,
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

void GLRenderer::Blit(const GPU& gpu)
{
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

// debug only (GPU::GLTimers): the compositor's draw (ComputePass::Comp) and everything else it runs
// (ComputePass::CompExtra), in the Blit's set
void GLRenderer::BeginGPUTimer(int pass)
{
#ifdef MELONADE_DEBUG_TOOLS
    if (Timers)
        Timers->Begin(pass == ComputePass::Comp ? GLPass::Comp : GLPass::CompExtra);
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
    glBlitFramebuffer(captureX, 0, captureX + WideMelon::NativeWidth * ScaleFactor, ScreenH,
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
    for (int i = 0; i < 2; i++)
    {
        if (FragPassLoc[i] >= 0)
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
