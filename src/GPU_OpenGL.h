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

#include "OpenGLSupport.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace melonDS
{
class GPU;
struct OutputLayout;
struct Shadow2DRecorder;
struct RenderSettings;
class GLRenderer;
class Renderer3D;
class GLCompositor
{
public:
    static std::optional<GLCompositor> New() noexcept;
    GLCompositor(const GLCompositor&) = delete;
    GLCompositor& operator=(const GLCompositor&) = delete;
    GLCompositor(GLCompositor&&) noexcept;
    GLCompositor& operator=(GLCompositor&&) noexcept;
    ~GLCompositor();

    void SetScaleFactor(int scale) noexcept;
    [[nodiscard]] int GetScaleFactor() const noexcept { return Scale; }

    // WideMelon: the output is viewWidth x scale wide, and the wide shader is only built when
    // PrepareWide() is first called. it returns false if that failed, then only 256 works
    void SetRenderSettings(int scale, int viewWidth) noexcept;
    [[nodiscard]] int GetViewWidth() const noexcept { return ViewWidth; }
    bool PrepareWide() noexcept;

    void Stop(const GPU& gpu) noexcept;
    void RenderFrame(const GPU& gpu, Renderer3D& renderer) noexcept;
    void SetOutputTexture(int buf, GLuint texture);
    void BindOutputTexture(int buf);
    // GPU::Comp1x: see Renderer3D::TakeOutputLayout
    bool TakeOutputLayout(int buf, OutputLayout& layout) noexcept;
    // drops the held WideMelon wings (see WingHold), for a reset or a savestate load.
    // CPU state only, so it needs no GL context
    void ResetWingHold() noexcept;
    // drops the hi-res capture copies (see HiresCap), for a reset or a savestate load. CPU state
    // only too: the textures stay for the next capture
    void ResetHiresCapture() noexcept;
    // makes the next frame redo the filtered 2D map (see Filter2DState::MapPasses), for a reset or
    // a savestate load. CPU state only too
    void ResetFilter2D() noexcept;
    // makes the next frame that draws GPU 2D shadow lines upload all of their memory again, for a
    // reset or a savestate load. CPU state only too
    void ResetShadow2D() noexcept;
    // whether the capture pass would run now (see RunHiresCapture): above 1x, or at 1x in the
    // check mode (GPU::HiresCaptureDebug 2), and while the capture program hasn't failed. GPU
    // only records capture lines and flags VRAM display lines while it does (GPU::Start2DJob)
    [[nodiscard]] bool RunsHiresCapture(int debug) const noexcept
    {
        return (Scale > 1 || debug >= 2) && !HiresCap.ShaderFailed;
    }

    // the fragment shader sources, as assembled around kCompositePixelFS
    static std::string CompositorFS();
    static std::string HiresCaptureFS();
    // the GPU 2D shadow programs' fragment shaders: 0 the middle-only composite, 1 the wide one,
    // 2 the check one (planes at 1x), and the OBJ pass
    static std::string Shadow2DFS(int variant);
    static std::string Shadow2DObjFS();

    // GPU::Filter2DRows: rows First to Last of a filtered 2D target (see RunFilter2D)
    struct Filter2DRowRun { int First, Last; };
private:
    GLCompositor(GLuint CompShader) noexcept;

    // debug only: the renderer whose GPU timers time the compositor's parts (ComputePass, from
    // Comp on), only during a RenderFrame with them on, null otherwise. a part starts its pass right
    // before its GL work, once it knows it runs, so a part that's left out has no query
    Renderer3D* GPUTimers = nullptr;
    void BeginGPUTimer(int pass) noexcept;

    // GPU::CompClear: right before the main draw, drops the output's old contents (see RenderFrame)
    void ClearOutput(int mode) noexcept;

    // GPU::Comp1x (experiment K2c, see RenderFrame). why a frame has no half at 1x, for the COMP1X log
    enum Comp1xOff
    {
        Comp1xOn,           // it could have: the halves that were plain are
        Comp1xOffFront,     // the frontend can't show a half at 1x (GPU::Comp1xFrontend)
        Comp1xOffScale,     // the output is at 1x anyway
        Comp1xOffView,      // a 256 view in mode 1
        Comp1xOffLines,     // the frame has no lines, so the screen texture is an older frame's
        Comp1xOffFilter,    // filtered 2D the map's scale can't do (K3): the output's scale isn't a multiple
                            // of the map's above it, or Filter2D 3's 256 view
        Comp1xOffShadow,    // the GPU 2D shadow draws over the output
        Comp1xOff_Count
    };
    struct Comp1xState
    {
        // what the last draw into each buffer left, and whether TakeOutputLayout has had it yet
        std::array<int, 2> Halves {};
        std::array<int, 2> Scale {};
        std::array<int, 2> HalfScale {};
        std::array<bool, 2> Drawn {};

        // the check mode's target, made the first time a check runs and freed with the switch off:
        // GL_RGBA8 CheckW x 384 at CheckScale, half h at it from row h * 192 * CheckScale
        GLuint CheckTex = 0, CheckFB = 0;
        int CheckW = 0, CheckScale = 0;
        u32 CheckFrames = 0;
        bool CheckFailed = false;

        // the COMP1X log, with GPU::SceneStats, GPU::GLTimers or in the check mode
        s64 LogStartNs = 0;
        u32 LogFrames = 0;
        std::array<u32, 3> LogHalves {};    // frames with only the top half at 1x, only the bottom one, both
        std::array<u32, Comp1xOff_Count> LogOff {};
        u32 LogFiltered = 0;                // of those, frames whose halves were filtered, at the map's scale
        u32 Checks = 0, CheckPixels = 0, Mismatches = 0, MismatchNative = 0;
    };
    Comp1xState OneX {};

    // how a frame's halves at 1x are drawn: with the main draw's program, at DrawScale (1, or the
    // filter map's with filtered 2D), whose u3DScale is at ScaleLoc and, for the filter's program,
    // uFilterRowOffset at RowOffsetLoc (-1 for the others)
    struct SmallHalfDraw
    {
        int DrawScale = 1;
        GLuint ScaleLoc = 0;
        GLint RowOffsetLoc = -1;
    };
    int PlainHalves(const GPU& gpu, int backbuf, bool wingHold, bool hiresDisplay, bool ctrlCopied) const noexcept;
    void DrawHalfAt1x(int half, int y, const SmallHalfDraw& draw) noexcept;
    void EndHalvesAt1x(const SmallHalfDraw& draw) noexcept;
    void CheckComp1x(int backbuf, int halves, const SmallHalfDraw& draw) noexcept;
    void LogComp1x(const GPU& gpu) noexcept;
    void FreeComp1xCheck() noexcept;

    int Scale = 0;
    int ViewWidth = 256;
    int ScreenH = 0, ScreenW = 0;

    GLuint CompShader {};
    GLuint CompScaleLoc = 0;
    GLuint CompHiresCapLoc = 0;

    // the texture units of the images hi-res lines show (see PrepareHiresDisplay)
    static constexpr int HiresBankUnit = 3;
    static constexpr int HiresStreamUnit = 8;

    GLuint CompShaderWide {};
    bool CompShaderWideFailed = false;
    GLuint CompWideScaleLoc = 0;
    GLuint CompWideRatioLoc = 0;
    GLuint CompWideOffsetLoc = 0;
    GLuint CompWideHiresCapLoc = 0;

    // the wide programs' wing planes (see GPU2D::AccelWingPlanesStart), GL_RGBA8UI, 6 texels per
    // line, both screens' 192 lines like the screen texture. made with the wide program and
    // uploaded with the frame's lines, so it's always the composited frame's
    static constexpr int WingPlanesUnit = 10;
    GLuint WingPlanesTex = 0;

    // WideMelon wing hold. games with 3D on both screens move unit A (and the 3D) to the other
    // screen every frame, so each half only gets 3D wings every other frame. the frames in
    // between reuse the half's wings from the last frame that had them, kept in HeldWingsTex
    // (both wings side by side, full output height). see PrepareWingHold
    static constexpr int WingHoldNever = 1000;
    struct WingHoldHalf
    {
        int SinceLive = WingHoldNever;  // frames since this half last had unit A's 3D (bit 22)
        int SinceCopy = WingHoldNever;  // frames since they were copied to HeldWingsTex
        int HoldRun = 0;                // frames held in a row
        std::array<u32, 6> CopyLines {};// the copied frame's lines that had 3D wings
    };
    std::array<WingHoldHalf, 2> WingHold {};
    std::array<bool, 2> WingCopy {};
    std::array<u32, 12> WingHoldLines {};   // one bit per output line (0-383): held
    // the control words (column 256*3 of the screen texture) of both halves as uploaded over the
    // frame's, when the wing hold or the hi-res capture display rewrites them: WingHeldBit set on
    // the held lines and cleared on the others, and HiresDisplayBits cleared on the lines that
    // don't show a hi-res copy. the master brightness field only uses bits 0-4 and 14-15 there:
    // bits 5-7 are the 2D renderer's hi-res capture flag (bit 7) and bank (5-6), 8-12 are kept
    // clear in a wide view, and 13 was free. once PrepareHiresDisplay rewrote them, bit 7 means
    // the line shows a hi-res image, and bit 5 that it's the stream's (HiresStreamTex) instead of
    // the VRAM display bank's (HiresBankTex)
    static constexpr u32 WingHeldBit = 1u << 13;
    static constexpr u32 HiresDisplayFlag = 1u << 7;
    static constexpr u32 HiresStreamBit = 1u << 5;
    static constexpr u32 HiresDisplayBits = 0xE0;
    std::array<u32, 384> CtrlWords {};
    GLuint HeldWingsTex = 0;
    GLuint HeldWingsFB = 0;
    int HeldWingsW = 0, HeldWingsH = 0;

    void PrepareWingHold(const GPU& gpu, int backbuf) noexcept;
    void CopyHeldWings(int backbuf) noexcept;

    // hi-res display capture (see GPU::HiresCapture). one copy per capture bank (A-D, the only
    // capture targets), GL_R16UI, 256 x 256 bank rows at Scale, holding the bank's words. a copy
    // only counts over the rows its last capture pass wrote (anything else is native, with no
    // rows kept from older passes), and only while nothing else wrote the bank. the textures are
    // made the first time a pass needs them and freed when the scale changes.
    // each texture, the scratch one too, is half of a pair with the native words of the capture it
    // holds (see HiresNativeRows), which regular display lines are compared with
    struct HiresBankState
    {
        bool Valid = false;
        int RowStart = 0;
        int Rows = 0;

        bool Covers(int row) const noexcept { return Valid && row >= RowStart && row < RowStart + Rows; }
    };
    // the native words of the capture a texture holds, as the pass found them in VRAM (256 per bank
    // row, for the rows below 192), and the rows that capture covered. nothing but the pass that
    // redoes the texture changes them: unlike the bank state, which says whether VRAM display can
    // still show the copy, a later write to the bank doesn't matter here, as a line that equals
    // these words proves by its pixels that it's this image
    struct HiresNativeRows
    {
        HiresBankState State;
        std::vector<u16> Words;
    };
    // HiresCapSkip_Count in GPU.h (checked in GPU_OpenGL.cpp)
    static constexpr int HiresCapSkipCount = 10;
    struct HiresCaptureState
    {
        std::array<HiresBankState, 4> Banks {};
        std::array<GLuint, 4> BankTex {};
        // the pass draws here, then swaps it with the destination's texture, so a capture that
        // reads its own destination as source B reads the old copy with no copy made
        GLuint ScratchTex = 0;
        std::array<HiresNativeRows, 4> BankNative {};
        HiresNativeRows ScratchNative {};
        GLuint PassFB = 0;
        GLuint PlanesTex = 0;   // GL_RGBA8UI 770x192: the record's planes, then 2 control texels per line
        GLuint SrcBTex = 0;     // GL_R16UI 256x192: the record's native source B words

        GLuint Shader = 0;
        bool ShaderFailed = false;
        GLuint ScaleLoc = 0, WideOffsetLoc = 0, DstRowLoc = 0;

        // the bank the frame's VRAM display lines show from (bound on unit 3), or -1
        int ShowBank = -1;
        // the image the frame's regular display lines show from (bound on unit 8): a bank's, the
        // scratch one's (HiresStreamScratch), or -1
        int StreamSource = -1;
        // GPU::ScreenSwap of the last frame, or -1 after a reset
        int LastScreenSwap = -1;
        // the last frame with regular display lines to tell by had no image that most of them match
        bool StreamMissed = false;

        // the HIRESCAP log, GPU::HiresCaptureDebug only
        s64 LogStartNs = 0;
        u32 Passes = 0, Captures = 0, LinesShown = 0, LinesCandidate = 0, LinesHiresB = 0, LinesNativeB = 0;
        u32 LinesStream = 0, LinesSwapNative = 0;
        std::array<u32, HiresCapSkipCount> Skips {};
        u32 CheckCount = 0;
    };
    static constexpr int HiresStreamScratch = 4;
    HiresCaptureState HiresCap {};
    std::array<u32, 192*2> HiresCapCtrl {};

    bool PrepareHiresDisplay(const GPU& gpu, int backbuf, bool ctrlCopied) noexcept;
    void PrepareHiresStream(const GPU& gpu, int backbuf) noexcept;
    void RunHiresCapture(const GPU& gpu, int backbuf) noexcept;
    bool PrepareHiresCaptureShader() noexcept;
    static GLuint MakeHiresTexture(GLenum format, int width, int height) noexcept;
    void FreeHiresBanks() noexcept;
    void FreeHiresCapture() noexcept;
    void LogHiresCapture() noexcept;
#ifdef MELONADE_DEBUG_TOOLS
    void CheckHiresCapture(const u8* bankNative, int bank, int rowStart, int rows) noexcept;
#endif

    // filtered 2D (see GPU::Filter2D and RunFilter2D). everything here is made the first time a
    // frame needs it, and the textures go at a scale change or when the filter goes off. the
    // compositor programs are the usual ones with FILTER2D defined, one for each of the view's
    // widths, so a frame without the filter runs exactly what it would without this. a frame whose
    // pre-pass would get the same keys as the one that made the map reuses it (see UpdateFilter2DKeys)
    static constexpr int Filter2DMapUnit = 9;
    struct Filter2DProgram
    {
        GLuint Program = 0;
        GLuint ScaleLoc = 0, RatioLoc = 0, OffsetLoc = 0, HiresCapLoc = 0, MapScaleLoc = 0, IdentityLoc = 0;
        GLint RowOffsetLoc = -1;
    };
    struct Filter2DState
    {
        GLuint KeyTex = 0, KeyFB = 0;       // GL_RG32UI 256x384: each native pixel's texel
        GLuint Map2xTex = 0, Map2xFB = 0;   // GL_RG32UI 512x768: the first pass's, when a second one reads it
        GLuint MapTex = 0, MapFB = 0;       // GL_R16UI 256x384 at MapScale: what the compositor reads
        int MapScale = 0;
        // what's in MapTex: the passes that made it (0 for nothing, as at the start, after a reset or
        // a savestate load and once the textures went), whether they wrote identity coordinates,
        // and the keys they read, 256 per line in UpdateFilter2DKeys's form
        int MapPasses = 0;
        bool MapIdentity = false;
        std::vector<u32> Keys;
        // GPU::Filter2DRows: the lines (half * 192 + line) whose keys UpdateFilter2DKeys last found
        // changed, and the row runs each pass redraws (see RunFilter2D), kept for their capacity
        std::array<bool, 384> LineChanged {};
        std::array<std::vector<Filter2DRowRun>, 3> Runs {};
        u32 RowCheckRuns = 0;

        bool Failed = false;                // a program didn't build or a target isn't complete
        GLuint KeyShader = 0;
        GLuint KeyHiresCapLoc = 0;
        GLuint MmpxShader = 0;
        GLuint MmpxSrcWLoc = 0, MmpxScreenHLoc = 0, MmpxIdentityLoc = 0;
        std::array<Filter2DProgram, 2> Comp {}; // middle only, wide

        // the FILTER2D log, GPU::Filter2DLogging only
        s64 LogStartNs = 0;
        u32 LogFrames = 0;
        u32 LogRan = 0, LogSkipped = 0, LogChangedLines = 0;
        s64 LogCheckNs = 0, LogCheckMaxNs = 0;
        // of the frames that ran, how many redrew only some rows, and the rows each pass drew (key,
        // first MMPX pass, second) over all of them; with GPU::Filter2DRows 2, the checks and the
        // texels that differed
        u32 LogPartial = 0;
        std::array<u64, 3> LogRows {};
        u32 LogRowChecks = 0, LogRowMismatches = 0;
    };
    Filter2DState Filter2D {};

    int PrepareFilter2D(const GPU& gpu, bool hasFramebuffers) noexcept;
    bool PrepareFilter2DShaders(bool wide) noexcept;
    bool MakeFilter2DTarget(GLuint& texture, GLuint& fb, GLenum format, int width, int height) noexcept;
    int UpdateFilter2DKeys(const GPU& gpu, int backbuf, bool hiresDisplay, bool ctrlCopied) noexcept;
    void RunFilter2D(const GPU& gpu, int backbuf, bool hiresDisplay, bool ctrlCopied, int passes) noexcept;
    void DrawFilter2DPasses(bool hiresDisplay, bool identity, int passes, bool full) noexcept;
    void CheckFilter2DRows(bool hiresDisplay, bool identity, int passes) noexcept;
    void FreeFilter2DTextures() noexcept;
    void FreeFilter2D() noexcept;
    void LogFilter2D(const GPU& gpu, int passes) noexcept;

    // the GPU 2D shadow renderer (GPU_Shadow2D.h, docs/plan-gpu-2d-shadow.md), while GPU::Shadow2D is on.
    // the recorder decides what a line can be drawn from, PrepareShadow2D which lines are drawn this
    // frame (the copy of their blocks with Shadow2D::F_Drawn), uploads what they read and runs the OBJ
    // pass, and RunShadow2D draws them over the compositor's output. everything is made the first time
    // a frame draws a line and freed once the recorder is off. units 11-13 are its textures (14 is
    // for the page log of increment 1b, 15 stays spare)
    static constexpr int Shadow2DMemUnit = 11;
    static constexpr int Shadow2DStateUnit = 12;
    static constexpr int Shadow2DObjUnit = 13;
    struct Shadow2DProgram
    {
        GLuint Program = 0;
        GLint ScaleLoc = -1, WideOffsetLoc = -1, ObjOffsetLoc = -1;
    };
    struct Shadow2DState
    {
        GLuint MemTex = 0;                  // GL_R32UI 256 x Shadow2D::MemRows: flats, extended palettes, palette
        u32 MemGeneration = 0;              // the recorder's Generation MemTex holds, 0 for none
        GLuint StateTex = 0;                // GL_RGBA32UI 8x384: the frame's state blocks, F_Drawn set
        GLuint ObjTex = 0, ObjDepth = 0, ObjFB = 0;  // GL_RG32UI ViewWidth x 384, and its 16-bit depth
        int ObjW = 0;
        GLuint InstanceBuffer = 0, InstanceVAO = 0;
        GLuint QuadBuffer = 0, QuadVAO = 0;
        GLuint CheckTex = 0, CheckFB = 0;   // GL_RGBA32UI 256x384, check mode only
        GLuint ObjProgram = 0;
        GLint ObjWLoc = -1, ObjWideOffsetLoc = -1;
        std::array<Shadow2DProgram, 3> Comp {};  // middle only, wide, check (SHADOW2D_PLANES)
        bool Failed = false;

        std::vector<u32> State;             // the blocks as uploaded, 384 lines
        std::vector<float> Quads;           // the row runs' triangles
        int DrawnLines = 0;

        // the GPU2D log, Shadow2DRecorder::Logging only
        s64 LogStartNs = 0;
        u32 LogFrames = 0;
        std::array<u32, 32> LogReasons {};
        u64 LogUploadBytes = 0;
        u64 LogInstances = 0;
        u32 CheckFrames = 0, Checks = 0, CheckLines = 0, Mismatches = 0;
    };
    Shadow2DState Shadow {};

    int PrepareShadow2D(const GPU& gpu, int backbuf, bool hasFramebuffers, bool hiresDisplay, bool ctrlCopied) noexcept;
    bool PrepareShadow2DResources() noexcept;
    bool PrepareShadow2DProgram(int variant) noexcept;
    void UploadShadow2D(const GPU& gpu, Shadow2DRecorder& rec) noexcept;
    void RunShadow2DObj(const Shadow2DRecorder& rec) noexcept;
    void BuildShadow2DQuads(bool check) noexcept;
    void RunShadow2D(int backbuf) noexcept;
    void CheckShadow2D(const GPU& gpu, int backbuf) noexcept;
    void LogShadow2D(const GPU& gpu) noexcept;
    void FreeShadow2DTextures() noexcept;
    void FreeShadow2D() noexcept;

    GLuint CompVertexBufferID = 0;
    GLuint CompVertexArrayID = 0;

    struct CompVertex
    {
        std::array<float, 2> Position {};
        std::array<float, 2> Texcoord {};
    };
    // 0-5 half 1's quad (output rows from 194 * Scale, the bottom screen: the top of clip space, as
    // the frontend reads the output upside down) and 6-11 half 0's. GPU::Comp1x: 12-17 half 0 over a
    // whole viewport, 18-23 half 1 (see DrawHalfAt1x)
    std::array<CompVertex, 2*3*2*2> CompVertices {};

    GLuint CompScreenInputTex = 0;
    // the compositor's own output textures only exist while a buffer is drawn into without a
    // frontend texture (see PrepareOutput): at 4x they're 6 MiB each, and a frontend that passes
    // its own through SetOutputTexture every frame never draws into them. OutputTexW/H is the
    // size of the one a buffer has, 0 without one
    std::array<GLuint, 2> CompScreenOutputTex {};
    std::array<int, 2> OutputTexW {}, OutputTexH {};
    // each framebuffer draws into the frontend's texture while OutputExternal is set, and into
    // CompScreenOutputTex (nothing while that's 0) otherwise
    std::array<bool, 2> OutputExternal {};
    std::array<GLuint, 2> CompScreenOutputFB {};

    void PrepareOutput(int buf) noexcept;
};

}
