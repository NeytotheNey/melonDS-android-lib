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

#pragma once

#ifdef OGLRENDERER_ENABLED
#include "GPU3D.h"
#include "GPU_OpenGL.h"
#include "OpenGLSupport.h"
#include "GPU3D_GLStats.h"
#include "GPU3D_Hash3D.h"

namespace melonDS
{
class GPU;

class GLRenderer : public Renderer3D
{
public:
    ~GLRenderer() override;
    void Reset(GPU& gpu) override;

    void SetRenderSettings(bool betterpolygons, int scale) noexcept;
    void SetBetterPolygons(bool betterpolygons) noexcept;
    void SetScaleFactor(int scale) noexcept;
    [[nodiscard]] bool GetBetterPolygons() const noexcept { return BetterPolygons; }
    [[nodiscard]] int GetScaleFactor() const noexcept { return ScaleFactor; }
    // GPU::OutputCap (experiment K4): how many times the native size the frontend shows the screens
    // at, at most (0 for unknown), which GLCompositor::OutputNeed turns into the need the
    // compositor's output is capped by (GLCompositor::CappedScale). call it before every frame, on
    // the GL (emulator) thread, after the frontend set what that reads (filtered 2D, hi-res capture,
    // the GPU 2D shadow): only a change reaches the compositor, and it never touches the 3D targets.
    // GetOutputScale is the output's scale, which the frontend sizes its textures by (with
    // GetOutputWidth); GetScaleFactor stays the 3D's
    void SetOutputNeed(const GPU& gpu, int frontendNeed) noexcept;
    [[nodiscard]] int GetOutputScale() const noexcept { return CurGLCompositor.GetScaleFactor(); }

    // WideMelon: width of the 3D view in native pixels (even, 256..768, anything else means 256).
    // * it's kept across SetRenderSettings calls and re-checked when the scale changes: it drops
    //   back to 256 when width x scale doesn't fit in a texture, or the wide compositor shader
    //   doesn't build. a new GLRenderer starts at 256, so the frontend has to set it again.
    // * SetViewWidth and SetRenderSettings can compile that shader and reallocate the targets, so
    //   call them on the GL (emulator) thread, between RunFrame calls.
    // * GetViewWidth is the width new geometry is built for. GetOutputWidth is the width of the
    //   targets and the compositor's output, which follows at RenderFrame (VCount 215) once the
    //   polygons being drawn match, so it can lag a frame or two, or longer on static 3D.
    //   the frontend's output texture must be GetOutputWidth() x scale wide, checked every frame
    void SetViewWidth(int width) noexcept;
    [[nodiscard]] int GetViewWidth() const noexcept override { return ViewWidth; }
    [[nodiscard]] int GetOutputWidth() const noexcept override { return TargetWidth; }
    [[nodiscard]] bool RunsHiresCapture(int debug) const noexcept override { return CurGLCompositor.RunsHiresCapture(debug); }

    void VCount144(GPU& gpu) override {};
    void RenderFrame(GPU& gpu) override;
    void Stop(const GPU& gpu) override;
    u32* GetLine(int line) override;

    void SetupAccelFrame() override;
    void PrepareCaptureFrame() override;
    void FlushCommands() override;
    void Blit(const GPU& gpu) override;

    [[nodiscard]] bool SupportsBlitAfterRender() const noexcept override { return true; }
    bool KeepFrameForBlit(const GPU& gpu) override;
    void PrepareEarlyCaptureFrame() override;
    bool TakeEarlyCaptureFrame(bool use) override;

    void SetOutputTexture(int buffer, u32 texture) override;
    void BindOutputTexture(int buffer) override;
    bool TakeOutputLayout(int buffer, OutputLayout& layout) override { return CurGLCompositor.TakeOutputLayout(buffer, layout); }
    void SavestateLoaded() override;

    // debug only: the compositor's passes for GPU::GLTimers, and GPU::Hash3DLogging (no-ops in
    // release builds)
    void BeginGPUTimer(int pass) override;
    void EndGPUTimer() override;
    void QueueHash3D(GPU& gpu, u32 frame) override;

    static std::unique_ptr<GLRenderer> New() noexcept;
private:
    // Used by New()
    GLRenderer(GLCompositor&& compositor) noexcept;

    // GL version requirements
    // * texelFetch: 3.0 (GLSL 1.30)     (3.2/1.50 for MS)
    // * UBO: 3.1

    struct RendererPolygon
    {
        Polygon* PolyData;

        u32 NumIndices;
        u32 IndicesOffset;
        GLuint PrimType;

        u32 NumEdgeIndices;
        u32 EdgeIndicesOffset;

        u32 RenderKey;
    };

    GLCompositor CurGLCompositor;
    RendererPolygon PolygonList[2048] {};

    // fragCount: the debug build that counts fragments (GPU::SceneStats 2, GLFragCounter). fetch: the
    // framebuffer fetch build (GPU::GLTransSkip bit 2), RenderShader[2 + wbuffer]. nodiscard: the
    // opaque-only build without discard (GPU::GLNoDiscard), RenderShader[4], Z-buffer only
    bool BuildRenderShader(bool wbuffer, bool fragCount = false, bool fetch = false, bool nodiscard = false);
    void UseRenderShader(bool wbuffer, int mode, bool fetch = false, bool nodiscard = false);
    bool PrepareFetchShaders();
    bool PrepareNoDiscardShader();
    void SetupPolygon(RendererPolygon* rp, Polygon* polygon) const;
    u32* SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32* vptr) const;
    u32 WideVertexX(s32 hiresx) const;
    bool TargetFits(int width, int scale) const noexcept;
    bool ScaleFits(int scale) const noexcept;
    int FitScale(int scale) const noexcept;
    int ResolveViewWidth(int width, int scale) noexcept;
    int FrameTargetWidth(const GPU& gpu) noexcept;
    bool ResizeTargets(bool betterpolygons, int scale, int targetWidth) noexcept;
    void KeepColorBuffer() noexcept;
    void ReadBackCaptureFrame(GLuint pixelBuffer) noexcept;
    void BuildPolygons(RendererPolygon* polygons, int npolys);
    int RenderSinglePolygon(int i) const;
    int RenderPolygonBatch(int i) const;
    int RenderPolygonEdgeBatch(int i) const;
    void ClearShadowMaskLines(const Polygon* polygon, const bool* lastWasMask) const;
    void SetDepthBias(float bias);
    void SetPolygonDepthBias(u32 polyattr);
    void ClassifyTransAlpha(const GPU& gpu);
    u8 TextureAlphaClass(const u8* vram, u32 texparam);
    void LogFrameStats(const GPU3D& gpu3d);
    int BeginShadowLogQuery();
    void LogShadowPolygon(char type, int i, int query);
    void FlushShadowLog();
    void RenderSceneChunk(const GPU3D& gpu3d, int y, int h);

    enum
    {
        RenderMode_Opaque = 0,
        RenderMode_Translucent,
        RenderMode_ShadowMask,
    };


    GLuint ClearShaderPlain {};
    // clear bitmap (DISP3DCNT bit 14), sampling slots 2 and 3 of texture VRAM from TexMem
    GLuint ClearShaderBitmap {};

    // [0]: Z-buffer, [1]: W-buffer; the pass is selected with uRenderMode. uDepthBias is
    // cached per program: GL keeps a program's uniforms across glUseProgram, so the cache
    // only has to follow what this code set. [2] and [3]: their framebuffer fetch builds
    // (GPU::GLTransSkip bit 2), only translucent draws use them, made at the first frame that
    // wants them (PrepareFetchShaders). bit 0 of an index is always W-buffering. [4]: the Z-buffer
    // opaque build without discard (GPU::GLNoDiscard), made at the first frame that wants it
    // (PrepareNoDiscardShader); it has no uRenderMode, it only draws opaque
    GLuint RenderShader[5] {};
    GLint RenderModeULoc[5] {};
    GLint DepthBiasLoc[5] {};
    float DepthBias[5] {};
    int CurShaderID = -1;
    int CurRenderMode = -1;

    // GL_EXT_shader_framebuffer_fetch is listed (checked at init). FetchShadersTried: a build of
    // RenderShader[2] and [3] was tried since they were last dropped
    bool HasFramebufferFetch = false;
    bool FetchShadersTried = false;
    // set per frame: GPU::GLTransSkip bit 2 with a clear alpha of 0, the extension and its programs.
    // the clear0 pass is left out and pass 3 does its work (RenderSceneChunk)
    bool FetchClear0 = false;
    // GPU::GLNoDiscard (R1 step 1): NoDiscardTried, a build of RenderShader[4] was tried since it was
    // last dropped. NoDiscard, set per frame: the switch, a Z-buffer frame and the program; then
    // SetupPolygon's caller marks the polygons drawn with it (RenderKey_NoDiscard)
    bool NoDiscardTried = false;
    bool NoDiscard = false;
    // an opaque polygon's RenderKey bit for the no-discard program: it splits batches at the
    // switches between programs, nothing else. no other key uses bit 15
    static constexpr u32 RenderKey_NoDiscard = 0x8000;

    GLuint FinalPassEdgeShader {};
    GLuint FinalPassFogShader {};

    // std140 compliant structure
    struct
    {
        float uScreenSize[2];       // vec2       0 / 2
        u32 uDispCnt;               // int        2 / 1
        u32 __pad0;
        float uToonColors[32][4];   // vec4[32]   4 / 128
        float uEdgeColors[8][4];    // vec4[8]    132 / 32
        float uFogColor[4];         // vec4       164 / 4
        float uFogDensity[34][4];   // float[34]  168 / 136
        u32 uFogOffset;             // int        304 / 1
        u32 uFogShift;              // int        305 / 1
        u32 _pad1[2];               // int        306 / 2
    } ShaderConfig {};

    GLuint ShaderConfigUBO {};
    int NumFinalPolys {}, NumOpaqueFinalPolys {};

    GLuint ClearVertexBufferID = 0, ClearVertexArrayID {};
    GLint ClearUniformLoc[4] {};
    GLint ClearBitmapULoc[3] {}; // uScale, uOffset, uOpaquePolyID

    // vertex buffer
    // * XYZW: 4x16bit
    // * RGBA: 4x8bit
    // * ST: 2x16bit
    // * polygon data: 3x32bit (polygon attrib, texture VRAM offset, (texture attrib (low 16 bit), texture palette (high 16 bit))
    //
    // polygon attributes:
    // * bit4-7, 11, 14-15, 24-29: POLYGON_ATTR
    // * bit16-20: Z shift
    // * bit8: front-facing (?)
    // * bit9: W-buffering (?)

    GLuint VertexBufferID {};
    u32 VertexBuffer[10240 * 7] {};
    u32 NumVertices {};

    GLuint VertexArrayID {};
    GLuint IndexBufferID {};
    u16 IndexBuffer[2048 * 40] {};
    u32 NumIndices {}, NumEdgeIndices {};

    const u32 EdgeIndicesOffset = 2048 * 30;

    GLuint TexMemID {};
    GLuint TexPalMemID {};

    // texture VRAM upload tracking. the dirty texture / palette chunks come from the shared
    // VRAMDirty_Texture / VRAMDirty_TexPal sets (consumed once per RenderFrame, like the other
    // renderers do). TexMemDirtyAll forces a full upload after construction or Reset, since a
    // previous renderer may have consumed the dirty bits before us
    bool TexMemDirtyAll = true;
    // the colour target was reallocated (scale, width): the frame can't be skipped
    bool TargetsChanged = true;
    u16 TexPalConvBuffer[1024 * 48] {};

    int ScaleFactor {};
    // GPU::OutputCap: the need SetOutputNeed last worked out, which ResizeTargets caps the output by
    int OutputNeed = 0;
    bool BetterPolygons {};
    int ScreenW {}, ScreenH {};

    int RequestedViewWidth = 256;
    int ViewWidth = 256;
    int TargetWidth = 256;
    int MaxTargetWidth {};
    // the tallest texture and viewport there can be, which the compositor's (384+2) x scale rows
    // have to fit in (see FitScale)
    int MaxTargetHeight {};

    // set per frame: the polygons were built for a wide view, or are drawn into one. when the
    // two widths differ (right after a change), WideXAdjust keeps them centred
    bool WideFrame {};
    s32 WideXAdjust {};

    // set per frame from GPU::ShadowMaskLineClear, GPU::ShadowMaskDepthTest and GPU::EqualDepthMargin
    bool ShadowMaskLineClear = true;
    bool ShadowMaskDepthTest = true;
    bool EqualDepthMargin = true;
    // set per frame from GPU::GLTransSkip
    int GLTransSkip = 0;
    // set per frame from GPU::GLEdgeSkip and GPU::GLClearFast
    bool EdgeSkip = false;
    int ClearFast = 0;

    // GPU::GLTransSkip bit 1: what a need-opaque polygon's fragments can come out as (AlphaClass_*),
    // per PolygonList entry, filled by ClassifyTransAlpha before the passes. both bits for every
    // other polygon, and for a texture the scan couldn't vouch for
    enum
    {
        AlphaClass_Full = 1,    // some fragment can have alpha 31: the opaque draw can pass it
        AlphaClass_Partial = 2, // some can have alpha 1-30: the translucent draw can pass it
    };
    u8 TransAlphaClass[2048] {};
    // the frame's scanned textures (by TexParam's address, size and format), so polygons sharing one
    // scan it once. a slot is in use when its stamp is this frame's
    static constexpr int TexAlphaSlots = 4096;
    u32 TexAlphaKey[TexAlphaSlots] {};
    u32 TexAlphaStamp[TexAlphaSlots] {};
    u8 TexAlphaValue[TexAlphaSlots] {};
    u32 TexAlphaFrame = 0;

    // debug only (GPU::ShadowLogFrames): the masks and shadows of a frame, logged at the start of
    // the next one so their occlusion queries are done by then
    struct ShadowLogEntry
    {
        char Type;
        int Index;
        u32 Attr;
        bool WBuffer, FacingView;
        s32 YTop, YBottom;
        u32 ZMin, ZMax;
        int Query;
    };
    bool ShadowLogActive = false;
    u32 ShadowLogFrame = 0;
    int NumShadowLogEntries = 0;
    int NumShadowLogQueries = 0;
    ShadowLogEntry ShadowLogEntries[64] {};
    GLuint ShadowLogQueries[64] {};

    GLuint ColorBufferTex {}, DepthBufferTex {}, AttrBufferTex {};
    // GPU::BlitAfterReadback: the 3D frame the render at line 215 replaced, which the Blit right
    // after it still reads (SetupAccelFrame binds it), and the color buffer the render after that
    // draws into. ColorBufferTex swaps with them, so with the switch on two color buffers take
    // turns. the spare is made the first time a frame is kept and dropped on a resize
    GLuint KeptColorTex {}, SpareColorTex {};
    // set by KeepFrameForBlit for the next RenderFrame, which clears it
    bool KeepRequested = false;
    // PrepareEarlyCaptureFrame's readback is in EarlyPixelbufferID and is still what line 0 would
    // read back. cleared by anything that draws into or resizes the 3D frame, or replaces the state
    bool EarlyCapture = false;
    GLuint DownScaleBufferTex {};
    // the buffer GetLine maps, and the early readback's, which swap when line 0 takes it
    // (TakeEarlyCaptureFrame). both are sized by ResizeTargets
    GLuint PixelbufferID {};
    GLuint EarlyPixelbufferID {};

    GLuint MainFramebuffer {}, DownscaleFramebuffer {};
    u32 Framebuffer[256*192] {};

#ifdef MELONADE_DEBUG_TOOLS
    // GPU discovery (docs/plan-gpu-discovery.md), all null while their switches are off, and then
    // nothing here makes a GL call for them. GLTIME (GPU::GLTimers)
    std::unique_ptr<GLTimerStats> Timers;
    // SCENEFRAGS (GPU::SceneStats 2): set up once, at the first frame with the switch on. while it
    // exists, RenderShader holds the counting builds, FragPassLoc their uFragPass
    std::unique_ptr<GLFragCounter> FragCounter;
    bool FragCountTried = false;
    GLuint CoverShader = 0;
    // the covered count when the clear alpha is 0 (kCoverAlphaCS), and its uSize
    GLuint CoverAlphaShader = 0;
    GLint CoverAlphaSizeLoc = -1;
    GLint FragPassLoc[5] {-1, -1, -1, -1, -1};
    // HASH3D (GPU::Hash3DLogging)
    std::unique_ptr<Hash3DReadback> Hash3D;
    // GLTRANS (GPU::SceneStats, with GPU::GLTransSkip)
    std::unique_ptr<GLTransStats> TransStats;
    // GLOPAQUE (GPU::SceneStats): the R1 count, after ClassifyTransAlpha
    std::unique_ptr<GLOpaqueStats> OpaqueStats;

    void CountOpaqueR1(const GPU& gpu);
    u8 OpaqueTextureClass(const u8* vram, u32 texparam);
    void UpdateDebugStats(const GPU& gpu);
    void SetupFragCount();
    void SetFragPass(int pass);
    void CountCoveredPixels();
    void CountCoveredByAlpha();
#endif
};
}
#endif