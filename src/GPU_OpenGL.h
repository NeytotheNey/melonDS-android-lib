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
private:
    GLCompositor(GLuint CompShader) noexcept;
    int Scale = 0;
    int ViewWidth = 256;
    int ScreenH = 0, ScreenW = 0;

    GLuint CompShader {};
    GLuint CompScaleLoc = 0;

    GLuint CompShaderWide {};
    bool CompShaderWideFailed = false;
    GLuint CompWideScaleLoc = 0;
    GLuint CompWideRatioLoc = 0;
    GLuint CompWideOffsetLoc = 0;
    GLuint CompWideHoldLoc = 0;

    // WideMelon wing hold. games with 3D on both screens move unit A (and the 3D) to the other
    // screen every frame, so each half only gets 3D wings every other frame. the frames in
    // between reuse the half's wings from the last frame that had them, kept in HeldWingsTex
    // (both wings side by side, full output height). see PrepareWingHold
    static constexpr int WingHoldNever = 1000;
    struct WingHoldHalf
    {
        int SinceLive = WingHoldNever;  // frames since this half last had unit A's 3D wings
        int SinceCopy = WingHoldNever;  // frames since they were copied to HeldWingsTex
        int HoldRun = 0;                // frames held in a row
        std::array<u32, 6> CopyLines {};// the copied frame's lines that had 3D wings
    };
    std::array<WingHoldHalf, 2> WingHold {};
    std::array<bool, 2> WingCopy {};
    std::array<u32, 12> WingHoldLines {};   // uvec4 uWingHold[3] in the shader
    GLuint HeldWingsTex = 0;
    GLuint HeldWingsFB = 0;
    int HeldWingsW = 0, HeldWingsH = 0;

    void PrepareWingHold(const GPU& gpu, int backbuf) noexcept;
    void CopyHeldWings(int backbuf) noexcept;

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
