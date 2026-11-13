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
#include <initializer_list>
#include <memory>
#include <vector>

#include "types.h"

#include "GPU3D.h"

#include "OpenGLSupport.h"
#include "GPU_OpenGL.h"

#include "GPU3D_TexcacheOpenGL.h"
#include "GPU3D_ComputeStats.h"
#include "GPU3D_ComputeRooms.h"
#include "GPU3D_GLStats.h"
#include "GPU3D_Hash3D.h"

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
    // the scale in use, which can be below the one asked for when that doesn't fit the GPU's
    // limits (see FitScale). the frontend sizes its textures by this
    [[nodiscard]] int GetScaleFactor() const noexcept { return ScaleFactor; }
    // dithers the polygons' interpolated vertex colors to 6 bits instead of dropping the low bits
    // (see the Rasterise shader). not something the DS does, so it's off by default
    void SetColorDither(bool dither) noexcept;

    // WideMelon: same rules as GLRenderer's (see GPU3D_OpenGL.h). the work buffers and shaders are
    // built for the width, so a change of GetOutputWidth recompiles every shader, at RenderFrame
    // (or with the next SetRenderSettings). the targets are rounded up to whole coarse tiles, and
    // only the first GetOutputWidth() x scale columns reach the compositor
    void SetViewWidth(int width) noexcept;
    [[nodiscard]] int GetViewWidth() const noexcept override { return ViewWidth; }
    [[nodiscard]] int GetOutputWidth() const noexcept override { return TargetWidth; }
    [[nodiscard]] bool RunsHiresCapture(int debug) const noexcept override { return CurGLCompositor.RunsHiresCapture(debug); }
    void SavestateLoaded() override;

    void VCount144(GPU& gpu) override;

    void RenderFrame(GPU& gpu) override;
    void RestartFrame(GPU& gpu) override;
    u32* GetLine(int line) override;

    void SetupAccelFrame() override;
    void PrepareCaptureFrame() override;
    void FlushCommands() override;
    bool FlushAfterCaptureBlit() const override { return true; }
    [[nodiscard]] bool CanDrawReplayedScene() const noexcept override { return true; }

    [[nodiscard]] bool SupportsBlitAfterRender() const noexcept override { return true; }
    bool KeepFrameForBlit(const GPU& gpu) override;
    void PrepareEarlyCaptureFrame() override;
    bool TakeEarlyCaptureFrame(bool use) override;

    void SetOutputTexture(int buffer, u32 texture) override;
    void BindOutputTexture(int buffer) override;
    bool TakeOutputLayout(int buffer, OutputLayout& layout) override { return CurGLCompositor.TakeOutputLayout(buffer, layout); }

    void Blit(const GPU& gpu) override;
    void Stop(const GPU& gpu) override;

#ifdef MELONADE_DEBUG_TOOLS
    void BeginGPUTimer(int pass) final { if (Stats) Stats->Begin(pass); }
    void EndGPUTimer() final { if (Stats) Stats->End(); }
    void QueueHash3D(GPU& gpu, u32 frame) final;
#endif
    [[nodiscard]] bool NeedsCaptureRedraw() const noexcept override { return PaceStale; }
    void RedrawSkippedFrame(GPU& gpu) override;

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
    // how many of them the shaders use, set by ResizeTargets from GPU::ComputeOpt: 2 with the
    // attributes packed into the color layer (ComputeOpt_PackAttr, the default), 1 when that layer
    // holds the depth as well (ComputeOpt_OneTileBuffer), or all three as it was before those.
    // TileLayersAlloc is how many have storage. it starts at what OptMask's first value gives
    int TileLayers = 2;
    int TileLayersAlloc = 0;
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
    // GPU::ComputeTileSize as of the last RenderFrame (0, 8, 16 or 32), the most threads a work
    // group can have (a tile's worth rasterises it), and whether the fallback was logged
    int ForcedTileSize = 0;
    int MaxGroupThreads = 0;
    bool TileFallbackLogged = false;
    int TileSizeFor(int width, int scale, int forced) const noexcept;
    // GPU::ComputeOpt as of the last RenderFrame, which the shaders are compiled for. it starts at
    // GPU's default (0xDF), so only a changed debug switch compiles them twice, and the first
    // ResizeTargets already gives the tile memory the default's two layers
    static constexpr int ComputeOpt_Small = 1 << 3;
    static constexpr int ComputeOpt_TexSkip = 1 << 4;
    // a swap that brought the last swap's scene again, with the same registers, counts as an
    // identical frame (GPU3D::RenderSceneRepeated)
    static constexpr int ComputeOpt_SwapSkip = 1 << 5;
    // pacing can leave a render out with HalfCamera on, while it has no in-between frames to make
    // (HalfCamera::CanSkipRender)
    static constexpr int ComputeOpt_HalfCamPace = 1 << 6;
    // Rasterise puts a pixel's attributes in the bits its color leaves free, so the tile memory
    // has no attribute layer (Tilebuffers in the shaders)
    static constexpr int ComputeOpt_PackAttr = 1 << 7;
    // with ComputeOpt_PackAttr: color and depth share one buffer, where that fits a storage block.
    // off by default, and OptMask never has it without ComputeOpt_PackAttr
    static constexpr int ComputeOpt_OneTileBuffer = 1 << 8;
    // the bits that are defines in the shaders as they are. bits 7 and 8 are as well, but by the
    // tile memory layout they give (TileLayersFor), which is what a change of them is told by
    static constexpr int ComputeOpt_Shaders = 0xF;
    static constexpr int ComputeOpt_All = 0x1FF;
    int OptMask = ComputeOpt_All & ~(ComputeOpt_SwapSkip | ComputeOpt_OneTileBuffer);
    // how many tile memory buffers those bits give at the current tile size and MaxWorkTiles
    int TileLayersFor(int optMask) const noexcept;
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
    static constexpr int UniformIdxColorDither = 2;

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

        s32 ClearBitmapOffset[2];
    };
    // std140: the ivec2 needs 8 byte alignment, which the 146 uints before it already give
    static_assert(offsetof(MetaUniform, ClearBitmapOffset) == 584 && sizeof(MetaUniform) == 592,
        "MetaUniform must match the std140 layout of the shader's MetaUniform block");
    GLuint MetaUniformMemory {};

    GLuint Samplers[9] {};

    // 3D clear bitmap (DISP3DCNT bit 14): texture slots 2 and 3 converted on the CPU into
    // R32UI textures, like upstream's compute renderer, read with texelFetch.
    // [0]: colour as the packed 6-bit RGB + 5-bit alpha the shaders use, [1]: depth24 | fog<<24.
    // ClearBitmapDirty accumulates the texcache's slot 2/3 bits until the mode is on
    GLuint ClearBitmapTex[2] = {};
    u32 ClearBitmap[2][256*256];
    u8 ClearBitmapDirty = 0x3;

    GLuint Framebuffer = 0;
    // GPU::BlitAfterReadback: the 3D image the render at line 215 replaced, which the Blit right
    // after it still reads (SetupAccelFrame binds it), and the image the render after that writes.
    // Framebuffer swaps with them, so with the switch on two images take turns. the spare is made
    // the first time a frame is kept and dropped by ResizeTargets
    GLuint KeptFramebuffer {}, SpareFramebuffer {};
    // set by KeepFrameForBlit for the next RenderFrame, which clears it
    bool KeepRequested = false;
    GLuint LowResFramebuffer {};
    // the buffer GetLine maps, and the early readback's (PrepareEarlyCaptureFrame), which swap
    // with their fences when line 0 takes it (TakeEarlyCaptureFrame)
    GLuint PixelBuffer {};
    GLuint EarlyPixelBuffer {};
    GLuint CaptureFrameBuffer {};
    // GPU::CaptureSpinUs (copied at RenderFrame): a fence after the capture readback, polled
    // before the map
    GLsync CaptureFence = nullptr;
    GLsync EarlyCaptureFence = nullptr;
    // the early readback is in EarlyPixelBuffer and is still what line 0 would read back. cleared
    // (with its fence) by anything that draws into the low res image or replaces the state
    bool EarlyCapture = false;
    int CaptureSpinUs = 0;
#ifdef MELONADE_DEBUG_TOOLS
    // when RenderFrame last started (ns), for the capture stats
    s64 RenderFrameStartNs = 0;
    // the COMPTIME and COMPPEAK logs (GPU::ComputeTimers and ComputePeaks). null while both are
    // off, the default, and then nothing here makes a GL call for them
    std::unique_ptr<ComputeStats> Stats;
    void UpdateStats(const GPU& gpu);
    // HASH3D (GPU::Hash3DLogging), made at the first frame it's on
    std::unique_ptr<Hash3DReadback> Hash3D;
    // COMPDEPTH (GPU::ComputeDepthStats): DepthBlend is built with its counters while DepthStatsOn,
    // set at RenderFrame from the switch (once it's off on a driver that can't, it stays off)
    bool DepthStatsOn = false;
    bool DepthStatsFailed = false;
    std::unique_ptr<ComputeDepthCounter> DepthCounter;
    void UpdateDepthStats(const GPU& gpu);

    // the COMPSLOW line: RenderFrame's CPU time by phase, logged for a frame over 30 ms, with how
    // many programs it was the first to dispatch since they were compiled (DispatchedPrograms)
    struct SlowPhases
    {
        s64 Pace, Resize, Warm, Texcache, ClearBitmap, Setup, TexGet, Grow, Upload, Bin, Rast, Final, End;
    };
    SlowPhases Slow {};
    s64 SlowLast = 0;
    int SlowFirsts = 0;
    std::vector<GLuint> DispatchedPrograms;
    void SlowLap(s64& phase) noexcept;
    void NoteProgram(GLuint program);
    void LogSlow(int numPolygons, u32 numVariants, u32 tileBound, int setupIndices) noexcept;
#endif

    // GPU::ComputeWarm, from Reset: at the first RenderFrame after the targets were set up, every
    // byte of the work buffers is written once on the GPU (Warm), and a grow writes the grown
    // ones, so the driver commits their pages there and not in the middle of a game's first heavy
    // render. debug builds wait for it and log COMPWARM and COMPGROW's touch
    bool WarmEnabled = true;
    bool WarmPending = false;
    // the program that writes the zeros, compiled with the batch (CompileShaderStep) or at Warm
    GLuint ShaderWarmZero = 0;
    bool WarmZeroFailed = false;
    bool WarmZeroReady(OpenGL::ShaderCacheStats* cache);
    u64 ZeroBuffer(GLuint buffer) noexcept;
    double TouchBuffers(std::initializer_list<GLuint> buffers, u64& bytes) noexcept;
    void Warm();
    // glUseProgram, which debug builds also count for COMPSLOW
    void UseProgram(GLuint program)
    {
        glUseProgram(program);
#ifdef MELONADE_DEBUG_TOOLS
        NoteProgram(program);
#endif
    }

    u32 FramebufferCPU[256*192];

    int ScreenWidth {}, ScreenHeight {};
    int TilesPerLine {}, TileLines {};
    int ScaleFactor = -1;
    int MaxWorkTiles {};
    // the room the work buffers have now, which TargetFits and the shaders don't know about: the
    // tile memory layers in work tiles (MaxWorkTiles at most), the X spans and their setup indices
    // in spans (YSpanIndices.size() at most). ResizeTargets starts them small and GrowWorkBuffers
    // makes them bigger before a frame that could need more (GPU::ComputeGrow). an allocation
    // that failed leaves 0, and a frame that needs more than there is isn't drawn
    int WorkTileRoom {};
    int XSpanRoom {};
    // GPU::ComputeGrow and GPU::ComputeRooms, and the cart's game code and header CRC (0 for none),
    // from Reset: what the rooms remembered for this game are found by (ComputeWorkRooms)
    bool UseRooms = false;
    u32 RoomGameCode = 0;
    u32 RoomHeaderCRC = 0;
    // Reset saw another cart while the rooms were set up for the last one's key, until ResizeTargets
    bool RoomsKeyChanged = false;
    ComputeWorkRooms::Key RoomKey() const noexcept;
    void ShrinkWorkBuffers() noexcept;
    // after a grow that failed, how many more grows are left out (GrowRetryIn), and how many
    // were left out last time, which doubles with every failure in a row
    int GrowRetryIn = 0;
    int GrowRetryWait = 0;
    bool HiresCoordinates {};
    // the next frame is drawn even if it's the same: ResizeTargets made a new, empty Framebuffer,
    // the hi-res coordinates setting changed how the polygons are set up, a reset emptied the
    // texture cache, or the last frame wasn't drawn for want of room (GrowWorkBuffers)
    bool ForceRender = true;
    // the texture cache was emptied (Reset) and nothing was drawn since, so what the image was
    // drawn with isn't cached any more. for ComputeOpt_TexSkip
    bool TexcacheEmptied = false;

    // the COMPWHY log (GPU::ComputeWhy), zeroed with every line: why RenderFrame drew. a render
    // can have several causes, and Sole counts the ones that had only that one
    enum
    {
        Why_Flush, Why_Regs, Why_HalfCam, Why_Other, Why_TexInval, Why_TexDirty, Why_ClearBitmap,
        Why_Force, Why_Stale, Why_Count
    };
    struct
    {
        s64 StartNs = 0;
        u32 Frames = 0, Extra = 0, Renders = 0, Same = 0, Paced = 0, TexSaved = 0, SwapSaved = 0;
        u32 FlushSame = 0, Captured = 0;
        u32 Cause[Why_Count] {}, Sole[Why_Count] {};
    } WhyLog;
    // GPU3D::MidScenesSame as of the last COMPWHY line
    u32 WhyMidSameSeen = 0;
    void LogWhy(const GPU& gpu) noexcept;

    bool ColorDither = false;

    // Mali diagnostics (docs/shelved/compute-on-mali.md): GPU::ComputeBin and ComputeSSBO as the
    // programs are compiled for (a change recompiles them at RenderFrame, like ComputeOpt), and
    // GPU::ComputeCheck's readback (RunCheck): its own program and scratch buffer, made at the
    // first check, and the renders with polygons counted to pick every Nth
    int BinMode = 0;
    bool SetupSSBO = false;
    int CalcMode = 0;
    // GPU::ComputeFlat as compiled: starts at GPU's default, so only the switch compiles twice
    int FlatMode = 1;
    GLint MaxTexBufferSize = 0;
    GLuint ShaderCheckSetup = 0;
    bool CheckSetupFailed = false;
    GLuint CheckScratch = 0;
    GLsizeiptr CheckScratchSize = 0;
    u32 CheckRenders = 0;
    void RunCheck(int numPolygons, u32 numVariants, int numSetupIndices, u32 totalTileBound);

    // GPU::ComputePace: a fence for each render the GPU hasn't finished, oldest first. past the
    // room for them the oldest is forgotten
    static constexpr int MaxPaceFences = 8;
    GLsync PaceFences[MaxPaceFences] {};
    int PaceFenceCount = 0;
    // the last render was left out for pacing (mode 2), so the image is older than the scene and
    // the next render isn't skipped as identical. PaceRedraw is set while RedrawSkippedFrame draws
    // it after all, and PaceRenderNs is when the last render was submitted
    bool PaceStale = false;
    bool PaceRedraw = false;
    s64 PaceRenderNs = 0;
    // line 0 read the 3D back for a display capture since the last render at line 215
    bool CapturedSinceRender = false;
    // the COMPPACE log, zeroed with every line
    struct
    {
        s64 StartNs = 0, WaitNs = 0;
        u32 Renders = 0, Skipped = 0, Kept = 0, Redraws = 0, Waits = 0, Timeouts = 0;
        int MaxPending = 0;
    } PaceLog;

    void ResetPace() noexcept;
    void DropPaceFences() noexcept;
    void PopPaceFences(int count) noexcept;
    void UpdatePace(const GPU& gpu) noexcept;
    void WaitForPace() noexcept;
    bool CanSkipForPace(const GPU& gpu, bool keep, bool captured) const noexcept;

    int RequestedViewWidth = 256;
    int ViewWidth = 256;
    int TargetWidth = 256;
    int MaxTargetWidth {};
    // as in GLRenderer: the compositor's (384+2) x scale rows have to fit (see FitScale)
    int MaxTargetHeight {};
    s64 MaxBufferSize {};

    // set per frame, as in GLRenderer: the polygons were built for a wide view, or are drawn into
    // one, and how far to move them when the two widths differ
    bool WideFrame {};
    s32 WideXAdjust {};

    GLCompositor CurGLCompositor;

    int ShaderStepIdx = 0;
    // GPU::ShaderCache, from Reset
    bool UseShaderCache = true;
    // what the program cache did for the batch of compiles in progress, for its save at the end
    // and the COMPSHADER line
    OpenGL::ShaderCacheStats ShaderBatch {};
    s64 ShaderBatchNs = 0;

    void CompileShaderStep(int step);
    void DeleteShaders();

    bool TargetFits(int width, int scale) const noexcept;
    bool BuffersFit(int width, int scale) const noexcept;
    bool ScaleFits(int scale) const noexcept;
    int FitScale(int scale) const noexcept;
    int ResolveViewWidth(int width, int scale) noexcept;
    void ResizeTargets(int scale, bool highResolutionCoordinates, int targetWidth);
    bool AllocTileMemory(int workTiles);
    bool AllocXSpans(int spans);
    void GrowWorkBuffers(u32 workTiles, u32 spans);
    int FrameTargetWidth(const GPU& gpu) noexcept;
    void KeepFramebuffer() noexcept;
    void ReadBackCaptureFrame(GLuint pixelBuffer, GLsync& fence) noexcept;
    void DropEarlyCapture() noexcept;
    s32 WideVertexX(s32 hiresx) const;

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2]);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2]);

    bool CompileShader(GLuint& shader, const std::string& source, const std::initializer_list<const char*>& defines);
};

}

#endif