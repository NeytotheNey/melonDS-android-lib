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
#include "NonStupidBitfield.h"
#include "GPU_OpenGL.h"
#include "OpenGLSupport.h"
#include "GPU3D_GLStats.h"
#include "GPU3D_Hash3D.h"
#include "GPU3D_OpenGLTexHeap.h"

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
    // WideMelon camera stage 2: the 3D rows above and below the top screen's 192 (0..128, see
    // WideMelon::SanitizeMargin: 0 while the width is 256). same rules as the width: kept across
    // SetRenderSettings and width changes, dropped to 0 when (384 + 2 + 2 x margin) x scale rows don't
    // fit, latched by GPU3D with the width (GPU::WideY off treats it as 0), and the targets and the
    // compositor's output follow at RenderFrame. GetOutputMargin is the output's margin, which lags
    // like GetOutputWidth: the frontend's output texture must be (384 + 2 + 2 x GetOutputMargin()) x
    // scale rows tall, the top half's (192 + 2 x margin) x scale first (see OutputLayout::TopMargin)
    void SetViewMargin(int margin) noexcept;
    [[nodiscard]] int GetViewMargin() const noexcept override { return ViewMargin; }
    [[nodiscard]] int GetOutputMargin() const noexcept override { return TargetMargin; }
    [[nodiscard]] bool RunsHiresCapture(int debug) const noexcept override { return CurGLCompositor.RunsHiresCapture(debug); }

    void VCount144(GPU& gpu) override {};
    void RenderFrame(GPU& gpu) override;
    void Stop(const GPU& gpu) override;
    u32* GetLine(int line) override;

    void SetupAccelFrame() override;
    void PrepareCaptureFrame() override;
    void FlushCommands() override;
    void Blit(const GPU& gpu) override;
    // HalfCamera: RenderFrame copies what it draws from the polygons (SetupPolygon, BuildPolygons) and
    // hands it to GL before it returns, and nothing reads them after that (TexCacheOff runs inside it)
    [[nodiscard]] bool CanDrawReplayedScene() const noexcept override { return true; }

    [[nodiscard]] bool SupportsBlitAfterRender() const noexcept override { return true; }
    bool KeepFrameForBlit(const GPU& gpu) override;
    void PrepareEarlyCaptureFrame() override;
    bool TakeEarlyCaptureFrame(bool use) override;

    void SetOutputTexture(int buffer, u32 texture) override;
    void BindOutputTexture(int buffer) override;
    void SetOutputDepthTexture(int buffer, u32 texture) override;
    bool SetupAccelDepth(DepthCamera& camera) override;
    void SetSunShadows(const SunShadows& sun) override { Sun = sun; }
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
        // GPU::GLSeams bit 1: the polygon's skirt (BuildSkirt), drawn after the opaque pass
        u32 NumSkirtIndices;
        u32 SkirtIndicesOffset;
        GLuint PrimType;

        u32 RenderKey;
        // what the vertices' second polygon word (fPolygonAttr.y) is: TexParam's VRAM offset, or
        // with GPU::GLTexCache 0x40000000 | the texture's heap offset (below 2^22, AssignTexCache),
        // plus step 4a's TexWord_Color and TexWord_RecAlpha (bits 22 and 23; 24 to 29 are free)
        u32 TexWord;
    };

    GLCompositor CurGLCompositor;
    // PolygonRAMSize: WideMelon's wide list can hold that many
    RendererPolygon PolygonList[PolygonRAMSize] {};

    // fragCount: the debug build that counts fragments (GPU::SceneStats 2, GLFragCounter). fetch: the
    // framebuffer fetch build (GPU::GLTransSkip bit 2), RenderShader[2 + wbuffer]. nodiscard: the
    // opaque-only build without discard (GPU::GLNoDiscard), RenderShader[4], Z-buffer only
    // full: the twin with the old decode path (GPU::GLTexCache step 2), RenderShader[FullShader + id]
    // edge: the build whose opaque fragments write the edge flag (GPU::GLEdgeMark 1, E1),
    // RenderShader[EdgeShader + id]
    // edge 2: with the outlines of polygons with up to 4 edges as flat varyings (GPU::GLEdgeFast bit 1), RenderShader[EdgeLineShader + id]
    bool BuildRenderShader(bool wbuffer, bool fragCount = false, bool fetch = false, bool nodiscard = false, bool full = false, int edge = 0);
    void UseRenderShader(bool wbuffer, int mode, bool fetch = false, bool nodiscard = false, bool full = false);
    bool PrepareFullShaders();
    void DropFullShaders();
    bool PrepareEdgeShaders();
    void DropEdgeShaders();
    void BuildEdges(const RendererPolygon* rp, u32 vidxFirst, u32 vidxEnd);
    static bool PolyFull(const RendererPolygon* rp) { return (rp->RenderKey & RenderKey_TexFull) != 0; }
    bool PrepareFetchShaders();
    bool PrepareNoDiscardShader();
    void SetRenderVariant(int probe, bool cache, bool only, bool wrapvs, bool color, bool colorOnly);
    void UpdateRenderVariant(const GPU& gpu);
    void SetupPolygon(RendererPolygon* rp, Polygon* polygon) const;
    u32* SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32 texword, u32* vptr) const;
    u32 HiresToPos(s32 scaled) const;
    u32 WideVertexX(s32 hiresx) const;
    u32 WideVertexY(s32 hiresy) const;
    int TargetRow(int line) const noexcept;
    bool TargetFits(int width, int scale) const noexcept;
    bool MarginFits(int margin, int scale) const noexcept;
    int ResolveViewMargin(int margin, int width, int scale) noexcept;
    int FrameTargetMargin(const GPU& gpu, int targetWidth) const noexcept;
    bool ScaleFits(int scale) const noexcept;
    int FitScale(int scale) const noexcept;
    int ResolveViewWidth(int width, int scale) noexcept;
    int FrameTargetWidth(const GPU& gpu) noexcept;
    bool ResizeTargets(bool betterpolygons, int scale, int targetWidth, int targetMargin) noexcept;
    void KeepColorBuffer() noexcept;
    void ResolveDepth(const GPU3D& gpu3d) noexcept;
    void FreeDepthOut() noexcept;
    void ReadBackCaptureFrame(GLuint pixelBuffer) noexcept;
    void BuildPolygons(RendererPolygon* polygons, int npolys);
    struct SunFrame;
    struct SunClipVertex;
    bool PrepareSunFrame(const GPU3D& gpu3d, SunFrame& frame) const;
    static int ClipSunShadow(const SunFrame& frame, const SunClipVertex* in, int n, SunClipVertex* out);
    static int SunShadowPolygon(const SunFrame& frame, const GPU& gpu, const Polygon* poly, const std::array<double, 16>& inverse,
                                SunClipVertex* out);
    u32* SetupSunVertices(const SunFrame& frame, const RendererPolygon* rp, const SunClipVertex* shadow, int n, u32* vptr) const;
    void BuildSunShadows(const GPU& gpu, int npolys);
    void DrawSunShadows();
    static bool GrowsSeams(const Polygon* poly);
    void BuildSkirt(RendererPolygon* rp, u32 rim, u32& vidx, u32& sidx);
    int RenderSkirtBatch(int i) const;
    int RenderSinglePolygon(int i) const;
    int RenderPolygonBatch(int i) const;
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
    // (PrepareNoDiscardShader); it has no uRenderMode, it only draws opaque. [5] to [9]: GPU::GLTexCache
    // step 2's twins of [0] to [4] with the old decode path, for the polygons the heap doesn't serve
    // (RenderKey_TexFull), made the first frame that has such a polygon (PrepareFullShaders); while
    // step 2 runs, [0] to [4] are built without that path (TexCacheOnly)
    // [10] to [19]: GPU::GLEdgeMark 1's builds of [0] to [9] (EdgeShader + id) whose opaque fragments
    // write the edge flag, only [10], [11], [14] and the twins [15], [16], [19] (the opaque draws'
    // programs), made the first edge-marking frame (PrepareEdgeShaders) and used for the opaque
    // draws of those frames only (UseRenderShader)
    static constexpr int FullShader = 5;
    static constexpr int EdgeShader = 10;
    // [20] to [29]: the same with GPU::GLEdgeFast bit 1's flat varyings (EdgeLines), for those frames
    static constexpr int EdgeLineShader = 20;
    static constexpr int NumRenderShaders = 30;
    GLuint RenderShader[NumRenderShaders] {};
    GLint RenderModeULoc[NumRenderShaders] {};
    GLint DepthBiasLoc[NumRenderShaders] {};
    float DepthBias[NumRenderShaders] {};
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
    // SetupPolygon's caller marks the polygons drawn with it (RenderKey_NoDiscard), and with bit 1
    // MarkScannedNoDiscard the ones whose texture was scanned
    bool NoDiscardTried = false;
    bool NoDiscard = false;
    // an opaque polygon's RenderKey bit for the no-discard program: it splits batches at the
    // switches between programs, nothing else. no other key uses bit 15
    static constexpr u32 RenderKey_NoDiscard = 0x8000;
    // GPU::GLTexProbe (T2 measurement probe, renders wrong) and GPU::GLTexCache (T2 step 1): TexProbe
    // and TexCache, what the render programs were built with (TexCache: with the heap's lookup, and
    // the heap textures exist); the Asked ones, the switches' values at the last UpdateRenderVariant,
    // which RenderFrame calls when a switch differs from them (so a build that fails is tried once)
    int TexProbe = 0;
    int TexProbeAsked = 0;
    bool TexCache = false;
    int TexCacheAsked = 0;
    // GPU::GLTexCache step 2 (on unless bit 4, value 16, asks for step 1): RenderShader[0] to [4] were
    // built without the old decode path, and the polygons the heap doesn't serve carry
    // RenderKey_TexFull and draw with the twins. CacheFailed: a build of the twins failed, and the
    // cache is off until GPU::GLTexCache changes (TexCacheOff). FailTwins: debug only, GPU::GLTexCache
    // 32 makes the twins fail to build, to test that
    bool TexCacheOnly = false;
    // GPU::GLTexCache step 3 (bit 6, value 64): the programs with the heap lookup (step 2's, step 1's
    // and the twins) were built with its per-polygon wrap constants made in the vertex shader
    bool TexWrapVS = false;
    // GPU::GLTexCache step 4a (bit 7, value 128, measurement only): the programs with the heap lookup
    // read the color plane for polygons with TexWord_Color. TexColorOnly (bit 8, 256, debug): the
    // cache-only programs were built without the record path (with step 2 and 128 only)
    bool TexColorPlane = false;
    bool TexColorOnly = false;
    static constexpr u32 TexWord_Color = 0x400000;
    static constexpr u32 TexWord_RecAlpha = 0x800000;
    bool CacheFailed = false;
    bool FailTwins = false;
    static constexpr u32 RenderKey_TexFull = 0x40000;

    GLuint FinalPassEdgeShader {};
    GLuint FinalPassFogShader {};
    // kFinalPassEdgeFS's uEdgeScale, uDepthUnbias, uClearPolyID, uClearDepth, uEdgeFast
    GLint EdgeULoc[5] {};

    // GPU::GLEdgeMark (E1), set per frame: 1 and 2, the frame's opaque draws use
    // RenderShader[EdgeShader + id], whose fragments write attr G: the edge flag (1: near their
    // polygon's outline, EdgeTex; 2: always) and whether their depth was biased; 0 neither (the
    // switch, DISP3DCNT bit 5, no polygon drawn opaque, or the programs didn't build).
    // EdgeShadersFailed: a build failed, the frames draw as 0 until the render programs are made
    // again (DropEdgeShaders)
    int EdgeMark = 0;
    bool EdgeShadersFailed = false;
    // GPU::GLEdgeFast, set per frame: bit 0 the edge pass discards unflagged pixels early and is
    // scissored to OpaqueBox, the target pixels the opaque-drawn polygons can cover; bit 1 (with
    // EdgeMark 1) polygons with up to 4 edges carry them as flat varyings (EdgeLineBuffer, the
    // RenderShader[EdgeLineShader + id] builds). EdgeSet: the program set of the frame's opaque
    // draws, EdgeShader or EdgeLineShader
    int EdgeFast = 0;
    int EdgeSet = EdgeShader;
    // the EdgeLines builds failed once: bit 1 draws from the texture for the renderer's life.
    // FailEdgeLines: debug only, GPU::GLEdgeFast 4 makes them fail, to test that
    bool EdgeLinesFailed = false;
    bool FailEdgeLines = false;
    s32 OpaqueBox[4] {};
    // GPU::GLEdgeMark 1: each polygon's outline, one RGBA32F texel an edge (kRenderFS's EdgeFlag),
    // 1024 wide, unit 5, made the first frame that needs it; EdgeInfoBuffer, the per-vertex attribute
    // (location 4, its own buffer EdgeInfoBufferID) saying where a vertex's polygon's edges are
    static constexpr u32 EdgeTexWidth = 1024;
    static constexpr u32 MaxEdges = PolygonRAMSize * 10;
    // the first edge is 16 bits of the vertex's edge info
    static_assert(MaxEdges <= 65536);
    GLuint EdgeTexID {};
    GLuint EdgeInfoBufferID {};
    u32 NumEdges = 0;
    float EdgeData[MaxEdges * 4] {};

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
        float uPosScale = 1.0f;     // float      306 / 1
        u32 _pad1;                  // int        307 / 1
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
    // a bank's vertices and three more a polygon (upstream's 10240 could already overflow), for WideMelon's
    // wide list, and room for GPU::GLSeams bit 1's skirts, four outer corners a polygon (BuildSkirt
    // leaves a polygon without one when they don't fit). the indices stay below 65536
    static constexpr u32 MaxGLVertices = VertexRAMSize + 3 * PolygonRAMSize + 4 * PolygonRAMSize;
    u32 VertexBuffer[MaxGLVertices * 7] {};
    u32 NumVertices {};

    GLuint VertexArrayID {};
    GLuint IndexBufferID {};
    // the most a polygon takes is BetterPolygons' 10 triangles, then the skirts' (GPU::GLSeams bit 1)
    static constexpr u32 MaxSkirtIndices = PolygonRAMSize * 24;
    u16 IndexBuffer[PolygonRAMSize * 30 + MaxSkirtIndices] {};
    u32 NumIndices {};
    // GPU::GLSeams bit 1: the skirts' triangles while BuildPolygons builds, then copied after the polygons'
    u16 SkirtIndexBuffer[MaxSkirtIndices] {};
    // GPU::GLEdgeMark 1: a word per vertex of VertexBuffer, see EdgeTexID
    u32 EdgeInfoBuffer[MaxGLVertices] {};
    // GPU::GLEdgeFast bit 1: 12 floats a vertex (locations 5 to 7), up to 4 edges of its polygon
    float EdgeLineBuffer[MaxGLVertices * 12] {};
    GLuint EdgeLineBufferID {};

    GLuint TexMemID {};
    GLuint TexPalMemID {};

    // GPU::GLTexCache (T2 step 1, docs/plan-t2-texcache.md), all made when the switch turns on and
    // dropped when it turns off: TexHeapID, the decoded texel records (RGBA8UI, 1024 x TexHeapRows,
    // unit 2); TexPalLinearID, the palette as TexPalMem has it but 256 entries a row (RGB5_A1,
    // 256 x 192, unit 3), uploaded with it; TexHeap, what's in the heap
    GLuint TexHeapID {};
    GLuint TexPalLinearID {};
    // GPU::GLTexCache 128 (T2 step 4a): the color plane, RGB5_A1 1024 x TexHeapRows, unit 4, made
    // with the heap when the value has 128
    GLuint TexColorID {};
    u32 TexHeapRows = 0;
    std::unique_ptr<GLTexHeap> TexHeap;
    bool CreateTexCache(int mode);
    void DeleteTexCache();
    bool AssignTexCache(const GPU& gpu, bool texturing);
    void TexCacheOff();
    // the frame's changed texture chunks, for InvalidateOpaqueTex and the heap
    TexDirtyPrefix TextureDirtyPrefix;
    // and the changed palette chunks, for the heap's color planes (GPU::GLTexCache 128, step 4b)
    TexPalDirtyPrefix TexPalettePrefix;

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
    // and whether it may cap at a ratio that isn't whole (GPU::OutputCap bit 2, GLCompositor::OutputAnyRatio)
    bool OutputAnyRatio = false;
    bool BetterPolygons {};
    int ScreenW {}, ScreenH {};

    int RequestedViewWidth = 256;
    int ViewWidth = 256;
    int TargetWidth = 256;
    int MaxTargetWidth {};
    // the tallest texture and viewport there can be, which the compositor's (384+2) x scale rows
    // (plus twice the margin) have to fit in (see FitScale and MarginFits)
    int MaxTargetHeight {};
    // the margin asked for (0..128), the one new geometry gets (0 with the width at 256) and the
    // targets' (TargetWidth's), whose height is 192 + 2 x TargetMargin rows at the scale
    int RequestedViewMargin = 0;
    int ViewMargin = 0;
    int TargetMargin = 0;

    // set per frame: the polygons were built for a wide view, or are drawn into one. when the
    // two widths differ (right after a change), WideXAdjust keeps them centred. the same for the
    // margin (WideFrameY and WideYAdjust, in hi-res rows)
    bool WideFrame {};
    s32 WideXAdjust {};
    bool WideFrameY {};
    // GPU::GLSeams, set per frame (RenderFrame): vertex positions are in 1/(1 << PosFrac) target
    // pixels (0 without the switch; kRenderVS scales them back by uPosScale), PosSub keeps the hi-res
    // fraction (bit 0), PosGrow gives the opaque polygons skirts (bit 1, BuildSkirt)
    int PosFrac = 0;
    bool PosSub = false;
    bool PosGrow = false;
    // GPU::GLSeams as the last frame drawn had it, so a change draws an identical frame again
    int SeamsDrawn = 0;
    s32 WideYAdjust {};
    // the margin the frame's polygons were built for (GPU3D::RenderViewMargin), for TargetRow
    int FrameMargin = 0;

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
    u8 TransAlphaClass[PolygonRAMSize] {};
    // the frame's scanned textures (by TexParam's address, size and format), so polygons sharing one
    // scan it once. a slot is in use when its stamp is this frame's
    static constexpr int TexAlphaSlots = 4096;
    u32 TexAlphaKey[TexAlphaSlots] {};
    u32 TexAlphaStamp[TexAlphaSlots] {};
    u8 TexAlphaValue[TexAlphaSlots] {};
    u32 TexAlphaFrame = 0;

    // GPU::GLNoDiscard bit 1 (R1 step 2): what the opaque shader can return for an opaque polygon's
    // texture that needs a scan (direct, 4x4, a palette with color 0 transparent), from
    // ScanOpaqueTexture. TexScan_Opaque: no texel comes out under alpha 31, so it can drop the
    // discard. the others say why not: an index-0 palette texel, a direct texel with alpha bit 0, a
    // transparent 4x4 texel (index 3 in block modes 0 and 1), or a texture reaching past TexMem
    // (or a 4x4 one spanning two 128 KB slots)
    enum
    {
        TexScan_Opaque,
        TexScan_Index0,
        TexScan_Direct0,
        TexScan_Comp0,
        TexScan_OutOfRange,
    };
    // one scanned texture: its key (TexParam's address, size and format: wrap, flip and color 0
    // don't change it, the scan only runs for palettes with color 0 on) and class, and the 512 byte
    // VRAMDirty chunks it read, first to last (inclusive, First > Last for none): the texels, and
    // for 4x4 the slot 1 index data its blocks' palette words come from
    struct OpaqueTexEntry
    {
        u32 Key;
        u16 First0, Last0, First1, Last1;
        u8 Value;
    };
    // the scans kept across frames. OpaqueTex holds them in the order they were made, OpaqueTexIndex
    // finds them by key (open addressing, -1 is empty, a quarter full at most). an entry stays
    // until texture VRAM changes in a chunk it read (InvalidateOpaqueTex, from the same dirty set
    // the TexMem upload uses), the cache fills up, or Reset; GPU::GLNoDiscard bit 2 drops them all
    // every frame instead, as a check
    static constexpr int OpaqueTexMax = 1024;
    static constexpr int OpaqueTexSlots = 4096;
    OpaqueTexEntry OpaqueTex[OpaqueTexMax] {};
    s16 OpaqueTexIndex[OpaqueTexSlots];
    int NumOpaqueTex = 0;
    u8 ScanOpaqueTexture(const u8* vram, u32 texparam, OpaqueTexEntry& entry);
    u8 OpaqueTexClass(const u8* vram, u32 texparam);
    void ClearOpaqueTex();
    void InvalidateOpaqueTex(const TexDirtyPrefix& dirty);
    void MarkScannedNoDiscard(const GPU& gpu, bool texturing);

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
    // the frontend's depth of field (SetOutputDepthTexture): each render's depth resolved into an
    // RGBA8 texture at the end of the frame (kDepthResolveFS), since DepthBufferTex is shared by the
    // kept and the new color buffer and GPU::GLClearFast drops it after the frame. DepthOutTex goes
    // with ColorBufferTex and swaps like it, with a kept and a spare one; each is 0 until a render
    // resolves into it, and DepthOutValid says that one holds ColorBufferTex's frame. all of them
    // are freed once no depth is wanted, and on a resize
    bool DepthWanted = false;
    GLuint DepthOutTex {}, KeptDepthOutTex {}, SpareDepthOutTex {};
    bool DepthOutValid = false, KeptDepthOutValid = false;
    // the camera each of those frames was drawn with (GPU3D::RenderCamera as SetupAccelDepth hands it over), swapped with them
    DepthCamera DepthOutCamera {}, KeptDepthOutCamera {};
    // while depth is wanted and the render's camera is valid, its index (GPU3D::SceneCamera::Index): BuildPolygons tags the
    // polygons built under another one (VtxAttr_OtherCamera), whose pixels get flag 8 in the depth (attr A, kDepthResolveFS).
    // NoSceneCamera tags none. the bit is one of the vertex attribute word's that kRenderVS and kRenderFS read nothing else from
    u8 TagCamera = GPU3D::NoSceneCamera;
    static constexpr u32 VtxAttr_OtherCamera = 1u << 30;
    // the frontend's sun shadows (SetSunShadows), and what the last frame drawn had of them, so a change draws a frame RenderFrame
    // would skip as identical again
    SunShadows Sun {};
    SunShadows SunDrawn {};
    // per frame, while depth is wanted, the camera is valid (TagCamera) and Sun is on: the sun shadows of the polygons TagCamera tags
    // that write depth, which BuildSunShadows puts after BuildPolygons' vertices and indices and DrawSunShadows draws after the scene,
    // into attr A only (kRenderFS's DepthTag, from VtxAttr_SunShadow, one of the attribute word's bits the render shaders read
    // nothing else from). each is drawn in the passes its polygon writes depth in: opaque for opaque and need-opaque polygons,
    // translucent for translucent ones with depth write (attr bit 11). Full is PolyFull's program
    struct SunShadow
    {
        u32 IndicesOffset;
        u32 NumIndices;
        bool Full;
        bool Opaque;
        bool Trans;
    };
    SunShadow SunShadowList[PolygonRAMSize] {};
    int NumSunShadows = 0;
    static constexpr u32 VtxAttr_SunShadow = 1u << 13;
    // how much nearer than the scene a shadow is tested, in 24-bit depth (uDepthBias): three of the DS's 0x200 steps, as a floor's
    // depth can round up to one step nearer than the shadow's own and the feet can sit a little off it, while a character drawn
    // under its own projection (Pokemon White's) is several steps nearer than its true place, so it still hides its own shadow
    static constexpr float SunShadowDepthBias = 3.0f * 512.0f;
    // DepthResolveFB's attachment, which follows DepthOutTex as it swaps, and whether it was complete
    GLuint DepthResolveFB {}, DepthResolveAttached {};
    bool DepthResolveComplete = false;
    // built the first time depth is wanted; a failed build leaves the frontend without depth
    GLuint DepthResolveShader {};
    GLint DepthResolveWBufferLoc = -1;
    bool DepthResolveFailed = false;
    static DepthCamera ToDepthCamera(const GPU3D::SceneCamera& camera) noexcept;
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

    // GPU::GLPace: a fence for each Blit the GPU hasn't finished, oldest first, and when each was
    // made. past the room for them the oldest is forgotten
    static constexpr int MaxPaceFences = 8;
    GLsync PaceFences[MaxPaceFences] {};
    s64 PaceFenceNs[MaxPaceFences] {};
    int PaceFenceCount = 0;
    // the GLPACE log, zeroed with every line
    struct
    {
        s64 StartNs = 0, WaitNs = 0, MaxWaitNs = 0;
        u32 Frames = 0, Waits = 0, Timeouts = 0, Stale = 0;
        int MaxPending = 0;
    } PaceLog;

    void DropPaceFences() noexcept;
    void PopPaceFences(int count) noexcept;
    void UpdatePace(const GPU& gpu) noexcept;
    void WaitForPace() noexcept;

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
    GLint FragPassLoc[NumRenderShaders] {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                         -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                         -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // HASH3D (GPU::Hash3DLogging)
    std::unique_ptr<Hash3DReadback> Hash3D;
    // GLTRANS (GPU::SceneStats, with GPU::GLTransSkip)
    std::unique_ptr<GLTransStats> TransStats;
    // GLOPAQUE (GPU::SceneStats): the R1 count, after ClassifyTransAlpha
    std::unique_ptr<GLOpaqueStats> OpaqueStats;
    // GLTEXCACHE (GPU::SceneStats, with GPU::GLTexCache on)
    std::unique_ptr<GLTexCacheStats> TexCacheStats;

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