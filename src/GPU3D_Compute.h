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

#ifndef GPU3D_COMPUTE
#define GPU3D_COMPUTE

#include <cstddef>
#include <memory>

#include "types.h"

#include "GPU3D.h"

#include "OpenGLSupport.h"
#include "GPU_OpenGL.h"

#include "GPU3D_TexcacheOpenGL.h"

#include "NonStupidBitfield.h"

namespace melonDS
{

class ComputeRenderer : public Renderer3D
{
public:
    static std::unique_ptr<ComputeRenderer> New();
    ~ComputeRenderer() override;

    void Reset(GPU& gpu) override;

    void SetRenderSettings(int scale, bool highResolutionCoordinates);

    // WideMelon: same rules as GLRenderer's (see GPU3D_OpenGL.h). the work buffers and shaders are
    // built for the width, so a change of GetOutputWidth recompiles every shader, at RenderFrame
    // (or with the next SetRenderSettings). the targets are rounded up to whole coarse tiles, and
    // only the first GetOutputWidth() x scale columns reach the compositor
    void SetViewWidth(int width) noexcept;
    [[nodiscard]] int GetViewWidth() const noexcept override { return ViewWidth; }
    [[nodiscard]] int GetOutputWidth() const noexcept override { return TargetWidth; }
    void SavestateLoaded() override;

    void VCount144(GPU& gpu) override;

    void RenderFrame(GPU& gpu) override;
    void RestartFrame(GPU& gpu) override;
    u32* GetLine(int line) override;

    void SetupAccelFrame() override;
    void PrepareCaptureFrame() override;
    void FlushCommands() override;
    bool FlushAfterCaptureBlit() const override { return true; }

    void SetOutputTexture(int buffer, u32 texture) override;
    void BindOutputTexture(int buffer) override;

    void Blit(const GPU& gpu) override;
    void Stop(const GPU& gpu) override;

    bool NeedsShaderCompile() override { return ShaderStepIdx != 33; }
    void ShaderCompileStep(int& current, int& count) override;
private:
    ComputeRenderer(GLCompositor&& compositor);

    GLuint ShaderInterpXSpans[2] {};
    GLuint ShaderBinCombined {};
    GLuint ShaderDepthBlend[2] {};
    GLuint ShaderRasteriseNoTexture[2] {};
    GLuint ShaderRasteriseNoTextureToon[2] {};
    GLuint ShaderRasteriseNoTextureHighlight[2] {};
    GLuint ShaderRasteriseUseTextureDecal[2] {};
    GLuint ShaderRasteriseUseTextureModulate[2] {};
    GLuint ShaderRasteriseUseTextureToon[2] {};
    GLuint ShaderRasteriseUseTextureHighlight[2] {};
    GLuint ShaderRasteriseShadowMask[2] {};
    GLuint ShaderClearCoarseBinMask {};
    GLuint ShaderClearIndirectWorkCount {};
    GLuint ShaderCalculateWorkListOffset {};
    GLuint ShaderSortWork {};
    GLuint ShaderFinalPass[8] {};

    GLuint YSpanIndicesTextureMemory {};
    GLuint YSpanIndicesTexture {};
    GLuint YSpanSetupMemory {};
    GLuint XSpanSetupMemory {};
    GLuint BinResultMemory {};
    GLuint RenderPolygonMemory {};
    GLuint WorkDescMemory {};

    enum
    {
        tilememoryLayer_Color,
        tilememoryLayer_Depth,
        tilememoryLayer_Attr,
        tilememoryLayer_Num,
    };

    GLuint TileMemory[tilememoryLayer_Num] {};
    GLuint FinalTileMemory {};

    u32 DummyLine[256] = {};

    struct SpanSetupY
    {
        // Attributes
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        // Interpolator
        s32 I0, I1;
        s32 Linear;
        s32 IRecip;
        s32 W0n, W0d, W1d;

        // Slope
        s32 Increment;

        s32 X0, X1, Y0, Y1;
        s32 XMin, XMax;
        s32 DxInitial;

        s32 XCovIncr;
        u32 IsDummy;
    };
    struct SpanSetupX
    {
        s32 X0, X1;

        s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;

        s32 XRecip;

        u32 Flags;

        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;

        s32 CovLInitial, CovRInitial;
    };
    struct SetupIndices
    {
        u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
    };
    struct RenderPolygon
    {
        u32 FirstXSpan;
        s32 YTop, YBot;

        s32 XMin, XMax;
        s32 XMinY, XMaxY;

        u32 Variant;
        u32 Attr;

        float TextureLayer;
    };

    int TileSize {};
    static constexpr int CoarseTileCountX = 8;
    int CoarseTileCountY {};
    int CoarseTileArea {};
    int CoarseTileW {};
    int CoarseTileH {};
    int ClearCoarseBinMaskLocalSize {};

    static constexpr int BinStride = 2048/32;
    static constexpr int CoarseBinStride = BinStride/32;

    static constexpr int MaxVariants = 256;

    static constexpr int UniformIdxCurVariant = 0;
    static constexpr int UniformIdxTextureSize = 1;

    static constexpr int MaxFullscreenLayers = 16;

    struct BinResultHeader
    {
        u32 VariantWorkCount[MaxVariants*4];
        u32 SortedWorkOffset[MaxVariants];
        // the real work count of each variant, VariantWorkCount's .y and .z hold the rasterise
        // dispatch split in two so neither goes past the 65535 work groups GL allows
        u32 VariantWorkRealCount[MaxVariants];

        u32 SortWorkWorkCount[4];
    };

    static const int MaxYSpanSetups = 6144*2;
    std::vector<SetupIndices> YSpanIndices;
    SpanSetupY YSpanSetups[MaxYSpanSetups];
    RenderPolygon RenderPolygons[2048];

    TexcacheOpenGL Texcache;

    struct MetaUniform
    {
        u32 NumPolygons;
        u32 NumVariants;

        u32 AlphaRef;
        u32 DispCnt;

        u32 ToonTable[4*34];

        u32 ClearColor, ClearDepth, ClearAttr;

        u32 FogOffset, FogShift, FogColor;

        float ClearBitmapOffset[2];
    };
    // std140: the vec2 needs 8 byte alignment, which the 146 uints before it already give
    static_assert(offsetof(MetaUniform, ClearBitmapOffset) == 584 && sizeof(MetaUniform) == 592,
        "MetaUniform must match the std140 layout of the shader's MetaUniform block");
    GLuint MetaUniformMemory {};

    GLuint Samplers[9] {};

    // 3D clear bitmap (DISP3DCNT bit 14): texture slots 2 and 3 converted on the CPU into
    // R32UI textures with REPEAT wrapping, like upstream's compute renderer.
    // [0]: colour as the packed 6-bit RGB + 5-bit alpha the shaders use, [1]: depth24 | fog<<24.
    // ClearBitmapDirty accumulates the texcache's slot 2/3 bits until the mode is on
    GLuint ClearBitmapTex[2] = {};
    u32 ClearBitmap[2][256*256];
    u8 ClearBitmapDirty = 0x3;

    GLuint Framebuffer = 0;
    GLuint LowResFramebuffer {};
    GLuint PixelBuffer {};
    GLuint CaptureFrameBuffer {};
    // GPU::CaptureSpinUs (copied at RenderFrame): a fence after the capture readback, polled
    // before the map
    GLsync CaptureFence = nullptr;
    int CaptureSpinUs = 0;
#ifdef MELONADE_DEBUG_TOOLS
    // when RenderFrame last started (ns), for the capture stats
    s64 RenderFrameStartNs = 0;
#endif

    u32 FramebufferCPU[256*192];

    int ScreenWidth {}, ScreenHeight {};
    int TilesPerLine {}, TileLines {};
    int ScaleFactor = -1;
    int MaxWorkTiles {};
    bool HiresCoordinates {};
    // the next frame is drawn even if it's the same: ResizeTargets made a new, empty Framebuffer,
    // or the hi-res coordinates setting changed how the polygons are set up
    bool ForceRender = true;

    int RequestedViewWidth = 256;
    int ViewWidth = 256;
    int TargetWidth = 256;
    int MaxTargetWidth {};
    s64 MaxBufferSize {};

    // set per frame, as in GLRenderer: the polygons were built for a wide view, or are drawn into
    // one, and how far to move them when the two widths differ
    bool WideFrame {};
    s32 WideXAdjust {};

    GLCompositor CurGLCompositor;

    int ShaderStepIdx = 0;

    void DeleteShaders();

    bool TargetFits(int width, int scale) const noexcept;
    int ResolveViewWidth(int width, int scale) noexcept;
    void ResizeTargets(int scale, bool highResolutionCoordinates, int targetWidth);
    s32 WideVertexX(s32 hiresx) const;

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2]);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2]);

    bool CompileShader(GLuint& shader, const std::string& source, const std::initializer_list<const char*>& defines);
};

}

#endif