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

    void VCount144(GPU& gpu) override {};
    void RenderFrame(GPU& gpu) override;
    void Stop(const GPU& gpu) override;
    u32* GetLine(int line) override;

    void SetupAccelFrame() override;
    void PrepareCaptureFrame() override;
    void FlushCommands() override;
    void Blit(const GPU& gpu) override;

    void SetOutputTexture(int buffer, u32 texture) override;
    void BindOutputTexture(int buffer) override;
    void SavestateLoaded() override;

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

    bool BuildRenderShader(u32 flags, const std::string& vs, const std::string& fs);
    void UseRenderShader(u32 flags);
    void SetupPolygon(RendererPolygon* rp, Polygon* polygon) const;
    u32* SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32* vptr) const;
    u32 WideVertexX(s32 hiresx) const;
    bool TargetFits(int width, int scale) const noexcept;
    int ResolveViewWidth(int width, int scale) noexcept;
    void ResizeTargets(bool betterpolygons, int scale, int targetWidth) noexcept;
    void BuildPolygons(RendererPolygon* polygons, int npolys);
    int RenderSinglePolygon(int i) const;
    int RenderPolygonBatch(int i) const;
    int RenderPolygonEdgeBatch(int i) const;
    void RenderSceneChunk(const GPU3D& gpu3d, int y, int h);

    enum
    {
        RenderFlag_WBuffer     = 0x01,
        RenderFlag_Trans       = 0x02,
        RenderFlag_ShadowMask  = 0x04,
        RenderFlag_Edge        = 0x08,
    };


    GLuint ClearShaderPlain {};

    GLuint RenderShader[16] {};
    GLuint CurShaderID = -1;

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

    int ScaleFactor {};
    bool BetterPolygons {};
    int ScreenW {}, ScreenH {};

    int RequestedViewWidth = 256;
    int ViewWidth = 256;
    int TargetWidth = 256;
    int MaxTargetWidth {};

    // set per frame: the polygons were built for a wide view, or are drawn into one. when the
    // two widths differ (right after a change), WideXAdjust keeps them centred
    bool WideFrame {};
    s32 WideXAdjust {};

    GLuint ColorBufferTex {}, DepthBufferTex {}, AttrBufferTex {};
    GLuint DownScaleBufferTex {};
    GLuint PixelbufferID {};

    GLuint MainFramebuffer {}, DownscaleFramebuffer {};
    u32 Framebuffer[256*192] {};
};
}
#endif