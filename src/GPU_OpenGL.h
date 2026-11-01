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

namespace melonDS
{
class GPU;
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
    // drops the held WideMelon wings (see WingHold), for a reset or a savestate load.
    // CPU state only, so it needs no GL context
    void ResetWingHold() noexcept;
    // drops the hi-res capture copies (see HiresCap), for a reset or a savestate load. CPU state
    // only too: the textures stay for the next capture
    void ResetHiresCapture() noexcept;
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
private:
    GLCompositor(GLuint CompShader) noexcept;
    int Scale = 0;
    int ViewWidth = 256;
    int ScreenH = 0, ScreenW = 0;

    GLuint CompShader {};
    GLuint CompScaleLoc = 0;
    GLuint CompHiresCapLoc = 0;

    GLuint CompShaderWide {};
    bool CompShaderWideFailed = false;
    GLuint CompWideScaleLoc = 0;
    GLuint CompWideRatioLoc = 0;
    GLuint CompWideOffsetLoc = 0;
    GLuint CompWideHiresCapLoc = 0;

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
    // bits 5-7 are the 2D renderer's hi-res capture flag (bit 7) and bank (5-6), 8-12 the wings'
    // fade EVY, and 13 was free
    static constexpr u32 WingHeldBit = 1u << 13;
    static constexpr u32 HiresDisplayFlag = 1u << 7;
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
    // made the first time a pass needs them and freed when the scale changes
    struct HiresBankState
    {
        bool Valid = false;
        int RowStart = 0;
        int Rows = 0;

        bool Covers(int row) const noexcept { return Valid && row >= RowStart && row < RowStart + Rows; }
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
        GLuint PassFB = 0;
        GLuint PlanesTex = 0;   // GL_RGBA8UI 770x192: the record's planes, then 2 control texels per line
        GLuint SrcBTex = 0;     // GL_R16UI 256x192: the record's native source B words

        GLuint Shader = 0;
        bool ShaderFailed = false;
        GLuint ScaleLoc = 0, WideOffsetLoc = 0, DstRowLoc = 0;

        // the bank the frame's VRAM display lines show from (bound on unit 3), or -1
        int ShowBank = -1;

        // the HIRESCAP log, GPU::HiresCaptureDebug only
        s64 LogStartNs = 0;
        u32 Passes = 0, Captures = 0, LinesShown = 0, LinesCandidate = 0, LinesHiresB = 0, LinesNativeB = 0;
        std::array<u32, HiresCapSkipCount> Skips {};
        u32 CheckCount = 0;
    };
    HiresCaptureState HiresCap {};
    std::array<u32, 192*2> HiresCapCtrl {};

    bool PrepareHiresDisplay(const GPU& gpu, int backbuf, bool ctrlCopied) noexcept;
    void RunHiresCapture(const GPU& gpu, int backbuf) noexcept;
    bool PrepareHiresCaptureShader() noexcept;
    static GLuint MakeHiresTexture(GLenum format, int width, int height) noexcept;
    void FreeHiresBanks() noexcept;
    void FreeHiresCapture() noexcept;
    void LogHiresCapture() noexcept;
#ifdef MELONADE_DEBUG_TOOLS
    void CheckHiresCapture(const GPU& gpu, int bank, int rowStart, int rows) noexcept;
#endif

    GLuint CompVertexBufferID = 0;
    GLuint CompVertexArrayID = 0;

    struct CompVertex
    {
        std::array<float, 2> Position {};
        std::array<float, 2> Texcoord {};
    };
    std::array<CompVertex, 2*3*2> CompVertices {};

    GLuint CompScreenInputTex = 0;
    std::array<GLuint, 2> CompScreenOutputTex {};
    std::array<GLuint, 2> CompScreenOutputFB {};
};

}
