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

#include "GPU_OpenGL.h"

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "NDS.h"
#include "GPU.h"
#include "GPU3D_OpenGL.h"
#include "GPU3D_ComputeStats.h"
#include "GPU_CaptureReads.h"
#include "GPU_HiresStream.h"
#include "GPU_Shadow2D.h"
#include "OpenGLSupport.h"
#include "Platform.h"
#include "GPU_OpenGL_shaders.h"

namespace melonDS
{

using namespace OpenGL;
using Platform::Log;
using Platform::LogLevel;

std::string GLCompositor::CompositorFS()
{
    return std::string(kCompositorFS_Nearest_Head) + kCompositePixelFS + kCompositorFS_Nearest_Main + kCompositorFS_Nearest_Entry;
}

// GPU::OutputCap: the program with the DOWN3D entry point (see kCompositorFS_Down3D_Entry), in
// GPU::OutputCap3D mode [mode], for the 256 view or, with WIDE, a wide one, and with FILTER2D for
// filtered 2D
std::string GLCompositor::Down3DFS(int mode, bool wide, bool filter)
{
    std::string fs = std::string(kCompositorFS_Nearest_Head) + kCompositePixelFS + kCompositorFS_Nearest_Main
                   + kDown3DTapsFS + kCompositorFS_Down3D_Entry;
    // after the #version line, as in PrepareWide
    fs.insert(fs.find('\n') + 1, std::string(wide ? "#define WIDE\n" : "") + (filter ? "#define FILTER2D\n" : "")
              + "#define DOWN3D\n#define DOWN3D_MODE " + std::to_string(mode) + "\n");
    return fs;
}

std::string GLCompositor::Shadow2DFS(int variant)
{
    std::string fs = std::string(kCompositorFS_Nearest_Head) + kCompositePixelFS + kCompositorFS_Nearest_Main
                   + kShadow2DMemFS + kShadow2DFS_Main;
    static const char* const defines[Shadow2DPrograms] = {
        "#define SHADOW2D\n",
        "#define WIDE\n#define SHADOW2D\n",
        "#define SHADOW2D\n#define SHADOW2D_PLANES\n",
        "#define WIDE\n#define SHADOW2D\n#define SHADOW2D_PLANES\n",
    };
    // after the #version line, as in PrepareWide
    fs.insert(fs.find('\n') + 1, defines[variant]);
    return fs;
}

std::string GLCompositor::Shadow2DObjFS()
{
    return std::string(kShadow2DObjFS_Head) + kShadow2DMemFS + kShadow2DObjFS_Main;
}

std::string GLCompositor::HiresCaptureFS()
{
    return std::string(kHiresCaptureFS_Head) + kCompositePixelFS + kHiresCaptureFS_Main;
}

// GPU::OutputCap: the capture program with DOWN3D defined (see kHiresCaptureFS_Main), in
// GPU::OutputCap3D mode [mode]
std::string GLCompositor::HiresCaptureDown3DFS(int mode)
{
    std::string fs = std::string(kHiresCaptureFS_Head) + kCompositePixelFS + kDown3DTapsFS + kHiresCaptureFS_Main;
    // after the #version line, as in PrepareWide
    fs.insert(fs.find('\n') + 1, "#define DOWN3D\n#define DOWN3D_MODE " + std::to_string(mode) + "\n");
    return fs;
}

std::optional<GLCompositor> GLCompositor::New() noexcept
{
    assert(glBindAttribLocation != nullptr);
    GLuint CompShader {};

    if (!OpenGL::CompileVertexFragmentProgram(CompShader,
            kCompositorVS, CompositorFS(),
            "CompositorShader",
            {{"vPosition", 0}, {"vTexcoord", 1}},
            {{"oColor", 0}}))
        return std::nullopt;

    return { GLCompositor(CompShader) };
}

// CompShader's uniform locations and texture units, which leaves it bound
void GLCompositor::SetupCompShader() noexcept
{
    CompScaleLoc = glGetUniformLocation(CompShader, "u3DScale");
    CompHiresCapLoc = glGetUniformLocation(CompShader, "uHiresCap");

    glUseProgram(CompShader);
    GLuint screenTextureUniform = glGetUniformLocation(CompShader, "ScreenTex");
    glUniform1i(screenTextureUniform, 0);
    GLuint _3dTextureUniform = glGetUniformLocation(CompShader, "_3DTex");
    glUniform1i(_3dTextureUniform, 1);
    glUniform1i(glGetUniformLocation(CompShader, "HiresBankTex"), HiresBankUnit);
    glUniform1i(glGetUniformLocation(CompShader, "HiresStreamTex"), HiresStreamUnit);
}

// CompShader again, with highp floats for a scale above 8 or the mediump one New makes below it
// (see CompShaderForHighScale). when the build fails the program in use stays, and it isn't tried
// again until the scale goes back across 8. the bound program is put back
void GLCompositor::BuildCompShader(bool highScale) noexcept
{
    CompShaderForHighScale = highScale;

    std::string fs = CompositorFS();
    // after the #version line, as in PrepareWide
    if (highScale)
        fs.insert(fs.find('\n') + 1, "#define HIGHP\n");

    GLuint shader {};
    if (!OpenGL::CompileVertexFragmentProgram(shader,
            kCompositorVS, fs,
            highScale ? "CompositorShaderHighp" : "CompositorShader",
            {{"vPosition", 0}, {"vTexcoord", 1}},
            {{"oColor", 0}}))
    {
        Log(LogLevel::Warn, "compositor: the %s program didn't build, keeping the one in use\n", highScale ? "highp" : "mediump");
        return;
    }

    GLint bound = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    bool wasBound = (GLuint)bound == CompShader;
    glDeleteProgram(CompShader);
    CompShader = shader;
    SetupCompShader();
    glUseProgram(wasBound ? CompShader : (GLuint)bound);
}

// the main draw's two quads (CompVertices 0-11) for an output with [margin] (see SetRenderSettings)
void GLCompositor::SetupCompVertices(int margin) noexcept
{
    if (margin == 0)
    {
        // all this mess is to prevent bleeding
#define SETVERTEX(i, x, y, offset) \
    CompVertices[i].Position[0] = x; \
    CompVertices[i].Position[1] = y + offset; \
    CompVertices[i].Texcoord[0] = (x + 1.f) * (256.f / 2.f); \
    CompVertices[i].Texcoord[1] = (y + 1.f) * (384.f / 2.f)

        const float padOffset = 1.f/(192*2.f+2.f)*2.f;
        // top screen
        SETVERTEX(0, -1, 1, 0);
        SETVERTEX(1, 1, 0, padOffset);
        SETVERTEX(2, 1, 1, 0);
        SETVERTEX(3, -1, 1, 0);
        SETVERTEX(4, -1, 0, padOffset);
        SETVERTEX(5, 1, 0, padOffset);

        // bottom screen
        SETVERTEX(6, -1, 0, -padOffset);
        SETVERTEX(7, 1, -1, 0);
        SETVERTEX(8, 1, 0, -padOffset);
        SETVERTEX(9, -1, 0, -padOffset);
        SETVERTEX(10, -1, -1, 0);
        SETVERTEX(11, 1, -1, 0);

#undef SETVERTEX
        return;
    }

    // half 1 (texture rows 192-384) over the output's last 192 rows, half 0 (-margin to 192 + margin)
    // over its first 192 + 2 x margin, in clip space of the whole (384 + 2 + 2 x margin) rows
    const float rows = 384.f + 2.f + 2.f * margin;
    const float half1Bottom = 1.f - 2.f * 192.f / rows;
    const float half0Top = -1.f + 2.f * (192.f + 2.f * margin) / rows;
    const float top0 = 192.f + margin, bottom0 = -(float)margin;
    auto set = [this](int i, float x, float y, float v)
    {
        CompVertices[i].Position = {x, y};
        CompVertices[i].Texcoord = {(x + 1.f) * (256.f / 2.f), v};
    };
    set(0, -1, 1, 384.f);
    set(1, 1, half1Bottom, 192.f);
    set(2, 1, 1, 384.f);
    set(3, -1, 1, 384.f);
    set(4, -1, half1Bottom, 192.f);
    set(5, 1, half1Bottom, 192.f);

    set(6, -1, half0Top, top0);
    set(7, 1, -1, bottom0);
    set(8, 1, half0Top, top0);
    set(9, -1, half0Top, top0);
    set(10, -1, -1, bottom0);
    set(11, 1, -1, bottom0);
}

GLCompositor::GLCompositor(GLuint compShader) noexcept : CompShader(compShader)
{
    SetupCompShader();

    SetupCompVertices(0);

    // GPU::Comp1x: each half over a whole viewport, in the same order (see DrawHalfAt1x)
    for (int half = 0; half < 2; half++)
    {
        const float xs[6] = {-1, 1, 1, -1, -1, 1};
        const float ys[6] = {1, -1, 1, 1, -1, -1};
        for (int i = 0; i < 6; i++)
        {
            CompVertex& vertex = CompVertices[12 + half * 6 + i];
            vertex.Position = {xs[i], ys[i]};
            vertex.Texcoord = {(xs[i] + 1.f) * (256.f / 2.f), half * 192.f + (ys[i] + 1.f) * (192.f / 2.f)};
        }
    }

    glGenBuffers(1, &CompVertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(CompVertices), &CompVertices[0], GL_STATIC_DRAW);

    glGenVertexArrays(1, &CompVertexArrayID);
    glBindVertexArray(CompVertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CompVertex), (void*)(offsetof(CompVertex, Position)));
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(CompVertex), (void*)(offsetof(CompVertex, Texcoord)));

    glGenFramebuffers(CompScreenOutputFB.size(), &CompScreenOutputFB[0]);

    glGenTextures(1, &CompScreenInputTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CompScreenInputTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 256*3 + 1, 192*2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, NULL);

    // the output textures are made by PrepareOutput, for the buffers that need one

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLCompositor::~GLCompositor()
{
    assert(glDeleteFramebuffers != nullptr);

    glDeleteFramebuffers(CompScreenOutputFB.size(), &CompScreenOutputFB[0]);
    glDeleteTextures(1, &CompScreenInputTex);
    glDeleteTextures(CompScreenOutputTex.size(), &CompScreenOutputTex[0]);
    glDeleteFramebuffers(1, &HeldWingsFB);
    glDeleteTextures(1, &HeldWingsTex);
    glDeleteTextures(1, &WingPlanesTex);
    FreeHiresCapture();
    FreeFilter2D();
    FreeShadow2D();
    FreeComp1xCheck();

    glDeleteVertexArrays(1, &CompVertexArrayID);
    glDeleteBuffers(1, &CompVertexBufferID);

    glDeleteProgram(CompShader);
    glDeleteProgram(CompShaderWide);
    for (std::array<Down3DProgram, 4>& view : Down3D)
    {
        for (Down3DProgram& program : view)
            glDeleteProgram(program.Program);
    }
    glDeleteSamplers(1, &Down3DSampler);
}


GLCompositor::GLCompositor(GLCompositor&& other) noexcept :
    Scale(other.Scale),
    Scale3D(other.Scale3D),
    ViewWidth(other.ViewWidth),
    Margin(other.Margin),
    ScreenH(other.ScreenH),
    ScreenW(other.ScreenW),
    CompScaleLoc(other.CompScaleLoc),
    CompHiresCapLoc(other.CompHiresCapLoc),
    CompShaderForHighScale(other.CompShaderForHighScale),
    Down3D(other.Down3D),
    Down3DMode(other.Down3DMode),
    Down3DHiresCapture(other.Down3DHiresCapture),
    Down3DFilter2D(other.Down3DFilter2D),
    AskedScale(other.AskedScale),
    Down3DSampler(other.Down3DSampler),
    OutputCapLogged(other.OutputCapLogged),
    CompShaderWide(other.CompShaderWide),
    CompShaderWideFailed(other.CompShaderWideFailed),
    CompWideScaleLoc(other.CompWideScaleLoc),
    CompWideRatioLoc(other.CompWideRatioLoc),
    CompWideOffsetLoc(other.CompWideOffsetLoc),
    CompWideHiresCapLoc(other.CompWideHiresCapLoc),
    CompWideMarginLoc(other.CompWideMarginLoc),
    WingPlanesTex(other.WingPlanesTex),
    WingHold(other.WingHold),
    WingCopy(other.WingCopy),
    WingHoldLines(other.WingHoldLines),
    HeldWingsTex(other.HeldWingsTex),
    HeldWingsFB(other.HeldWingsFB),
    HeldWingsW(other.HeldWingsW),
    HeldWingsH(other.HeldWingsH),
    HiresCap(other.HiresCap),
    Filter2D(std::move(other.Filter2D)),
    Shadow(std::move(other.Shadow)),
    OneX(other.OneX),
    CompVertices(other.CompVertices),
    CompShader(other.CompShader),
    CompVertexBufferID(other.CompVertexBufferID),
    CompVertexArrayID(other.CompVertexArrayID),
    CompScreenInputTex(other.CompScreenInputTex),
    CompScreenOutputTex(other.CompScreenOutputTex),
    OutputTexW(other.OutputTexW),
    OutputTexH(other.OutputTexH),
    OutputExternal(other.OutputExternal),
    CompScreenOutputFB(other.CompScreenOutputFB)
{
    other.HeldWingsTex = {};
    other.HeldWingsFB = {};
    other.WingPlanesTex = {};
    other.HiresCap = {};
    other.Filter2D = {};
    other.Shadow = {};
    other.OneX = {};
    other.CompScreenOutputFB = {};
    other.CompScreenInputTex = {};
    other.CompScreenOutputTex = {};
    other.CompVertexArrayID = {};
    other.CompVertexBufferID = {};
    other.CompShader = {};
    other.CompShaderWide = {};
    for (std::array<Down3DProgram, 4>& view : other.Down3D)
    {
        for (Down3DProgram& program : view)
            program.Program = {};
    }
    other.Down3DSampler = {};
}

GLCompositor& GLCompositor::operator=(GLCompositor&& other) noexcept
{
    if (this != &other)
    {
        Scale = other.Scale;
        Scale3D = other.Scale3D;
        ViewWidth = other.ViewWidth;
        Margin = other.Margin;
        ScreenH = other.ScreenH;
        ScreenW = other.ScreenW;
        CompScaleLoc = other.CompScaleLoc;
        CompHiresCapLoc = other.CompHiresCapLoc;
        CompShaderForHighScale = other.CompShaderForHighScale;
        Down3DMode = other.Down3DMode;
        Down3DHiresCapture = other.Down3DHiresCapture;
        Down3DFilter2D = other.Down3DFilter2D;
        AskedScale = other.AskedScale;
        OutputCapLogged = other.OutputCapLogged;
        CompShaderWideFailed = other.CompShaderWideFailed;
        CompWideScaleLoc = other.CompWideScaleLoc;
        CompWideRatioLoc = other.CompWideRatioLoc;
        CompWideOffsetLoc = other.CompWideOffsetLoc;
        CompWideHiresCapLoc = other.CompWideHiresCapLoc;
        CompWideMarginLoc = other.CompWideMarginLoc;
        WingHold = other.WingHold;
        WingCopy = other.WingCopy;
        WingHoldLines = other.WingHoldLines;
        HeldWingsW = other.HeldWingsW;
        HeldWingsH = other.HeldWingsH;
        CompVertices = other.CompVertices;

        // Clean up these resources before overwriting them
        glDeleteProgram(CompShader);
        CompShader = other.CompShader;

        glDeleteProgram(CompShaderWide);
        CompShaderWide = other.CompShaderWide;

        for (std::array<Down3DProgram, 4>& view : Down3D)
        {
            for (Down3DProgram& program : view)
                glDeleteProgram(program.Program);
        }
        Down3D = other.Down3D;

        glDeleteSamplers(1, &Down3DSampler);
        Down3DSampler = other.Down3DSampler;

        glDeleteBuffers(1, &CompVertexBufferID);
        CompVertexBufferID = other.CompVertexBufferID;

        glDeleteVertexArrays(1, &CompVertexArrayID);
        CompVertexArrayID = other.CompVertexArrayID;

        glDeleteTextures(1, &CompScreenInputTex);
        CompScreenInputTex = other.CompScreenInputTex;

        glDeleteTextures(CompScreenOutputTex.size(), &CompScreenOutputTex[0]);
        CompScreenOutputTex = other.CompScreenOutputTex;
        OutputTexW = other.OutputTexW;
        OutputTexH = other.OutputTexH;
        OutputExternal = other.OutputExternal;

        glDeleteFramebuffers(CompScreenOutputFB.size(), &CompScreenOutputFB[0]);
        CompScreenOutputFB = other.CompScreenOutputFB;

        glDeleteTextures(1, &HeldWingsTex);
        HeldWingsTex = other.HeldWingsTex;

        glDeleteFramebuffers(1, &HeldWingsFB);
        HeldWingsFB = other.HeldWingsFB;

        glDeleteTextures(1, &WingPlanesTex);
        WingPlanesTex = other.WingPlanesTex;

        FreeHiresCapture();
        HiresCap = other.HiresCap;

        FreeFilter2D();
        Filter2D = std::move(other.Filter2D);

        FreeShadow2D();
        Shadow = std::move(other.Shadow);

        FreeComp1xCheck();
        OneX = other.OneX;

        other.HeldWingsTex = {};
        other.HeldWingsFB = {};
        other.WingPlanesTex = {};
        other.HiresCap = {};
        other.Filter2D = {};
        other.Shadow = {};
        other.OneX = {};
        other.CompScreenOutputFB = {};
        other.CompScreenInputTex = {};
        other.CompScreenOutputTex = {};
        other.CompVertexArrayID = {};
        other.CompVertexBufferID = {};
        other.CompShader = {};
        other.CompShaderWide = {};
        for (std::array<Down3DProgram, 4>& view : other.Down3D)
        {
            for (Down3DProgram& program : view)
                program.Program = {};
        }
        other.Down3DSampler = {};
    }

    return *this;
}

bool GLCompositor::PrepareWide() noexcept
{
    if (CompShaderWide || CompShaderWideFailed)
        return CompShaderWide != 0;

    // the same shaders with WIDE defined, which has to come after the #version line
    std::string vs = kCompositorVS;
    std::string fs = CompositorFS();
    vs.insert(vs.find('\n') + 1, "#define WIDE\n");
    fs.insert(fs.find('\n') + 1, "#define WIDE\n");

    GLuint shader {};
    if (!OpenGL::CompileVertexFragmentProgram(shader,
            vs, fs,
            "CompositorShaderWide",
            {{"vPosition", 0}, {"vTexcoord", 1}},
            {{"oColor", 0}}))
    {
        CompShaderWideFailed = true;
        return false;
    }

    CompShaderWide = shader;
    CompWideScaleLoc = glGetUniformLocation(CompShaderWide, "u3DScale");
    CompWideRatioLoc = glGetUniformLocation(CompShaderWide, "uWideRatio");
    CompWideOffsetLoc = glGetUniformLocation(CompShaderWide, "uWideOffset");
    CompWideHiresCapLoc = glGetUniformLocation(CompShaderWide, "uHiresCap");
    CompWideMarginLoc = glGetUniformLocation(CompShaderWide, "uWideMargin");

    glUseProgram(CompShaderWide);
    glUniform1i(glGetUniformLocation(CompShaderWide, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(CompShaderWide, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HeldWingsTex"), 2);
    glUniform1i(glGetUniformLocation(CompShaderWide, "WingPlanesTex"), WingPlanesUnit);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HiresBankTex"), HiresBankUnit);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HiresStreamTex"), HiresStreamUnit);

    glGenTextures(1, &WingPlanesTex);
    glActiveTexture(GL_TEXTURE0 + WingPlanesUnit);
    glBindTexture(GL_TEXTURE_2D, WingPlanesTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, GPU2D::WingPlanesWords, 192*2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, NULL);
    glActiveTexture(GL_TEXTURE0);

    return true;
}

bool GLCompositor::PrepareDown3D(bool wide, int mode, bool filter) noexcept
{
    Down3DProgram& program = filter ? Filter2D.Down3D[wide][mode] : Down3D[wide][mode];
    if (program.Program || program.Failed)
        return program.Program != 0;

    // a wide view's has the wide vertex shader too, as in PrepareWide
    std::string vs = kCompositorVS;
    if (wide)
        vs.insert(vs.find('\n') + 1, "#define WIDE\n");

    static const char* const names[2][2] = {
        {"CompositorShaderDown3D", "CompositorShaderWideDown3D"},
        {"CompositorShaderFilter2DDown3D", "CompositorShaderWideFilter2DDown3D"},
    };
    GLuint shader {};
    if (!OpenGL::CompileVertexFragmentProgram(shader,
            vs, Down3DFS(mode, wide, filter),
            names[filter][wide],
            {{"vPosition", 0}, {"vTexcoord", 1}},
            {{"oColor", 0}}))
    {
        const char* what = filter ? "filtered DOWN3D" : "DOWN3D";
        if (mode == 0)
            Log(LogLevel::Warn, "compositor: the %s %s program didn't build, the output stays at the 3D's scale there\n",
                wide ? "wide" : "256", what);
        else
            Log(LogLevel::Warn, "compositor: the %s %s mode %d program didn't build, the capped output uses the exact one\n",
                wide ? "wide" : "256", what, mode);
        program.Failed = true;
        return false;
    }

    program.Program = shader;
    program.ScaleLoc = glGetUniformLocation(shader, "u3DScale");
    program.RatioLoc = glGetUniformLocation(shader, "u3DRatio");
    program.HiresCapLoc = glGetUniformLocation(shader, "uHiresCap");
    if (wide)
    {
        program.WideRatioLoc = glGetUniformLocation(shader, "uWideRatio");
        program.WideOffsetLoc = glGetUniformLocation(shader, "uWideOffset");
        program.WideMarginLoc = glGetUniformLocation(shader, "uWideMargin");
    }
    if (filter)
    {
        program.MapScaleLoc = glGetUniformLocation(shader, "uFilterMapScale");
        program.RowOffsetLoc = glGetUniformLocation(shader, "uFilterRowOffset");
    }

    // the bound program is put back, as in BuildCompShader
    GLint bound = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    glUseProgram(shader);
    glUniform1i(glGetUniformLocation(shader, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(shader, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(shader, "HiresBankTex"), HiresBankUnit);
    glUniform1i(glGetUniformLocation(shader, "HiresStreamTex"), HiresStreamUnit);
    if (wide)
    {
        // as PrepareWide's, which made the wing planes' texture before the view got wide
        glUniform1i(glGetUniformLocation(shader, "HeldWingsTex"), 2);
        glUniform1i(glGetUniformLocation(shader, "WingPlanesTex"), WingPlanesUnit);
    }
    if (filter)
        glUniform1i(glGetUniformLocation(shader, "FilterMapTex"), Filter2DMapUnit);
    glUseProgram((GLuint)bound);

    if (mode == 1 || mode == 3)
        MakeDown3DSampler();
    return true;
}

// modes 1 and 3's bilinear taps. a block never reaches past the 3D's edge (see Average3D in
// kCompositorFS_Down3D_Entry), so the wrap mode doesn't matter
void GLCompositor::MakeDown3DSampler() noexcept
{
    if (Down3DSampler)
        return;
    glGenSamplers(1, &Down3DSampler);
    glSamplerParameteri(Down3DSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(Down3DSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(Down3DSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(Down3DSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool GLCompositor::PrepareCapped(bool wide, int mode) noexcept
{
    bool built = PrepareDown3D(wide, mode);
    if (Down3DHiresCapture)
        PrepareHiresCaptureDown3D(built ? mode : 0);
    return built;
}

bool GLCompositor::PrepareCappedFilter(bool wide, int mode) noexcept
{
    return PrepareDown3D(wide, mode, true) || (mode != 0 && PrepareDown3D(wide, 0, true));
}

void GLCompositor::SetOutputCap3D(const GPU& gpu) noexcept
{
    int mode = std::clamp(gpu.OutputCap3D, 0, 3);
    bool hiresCapture = gpu.HiresCapture;
    // the identity mode (3) is never capped (OutputNeed), so it needs nothing here, and a filter that
    // can't run (a program or target that failed, Filter2DState::Failed) draws unfiltered, so the cap
    // doesn't wait on its capped program either
    bool filter2D = gpu.Filter2D != 0 && gpu.Filter2D != 3 && !Filter2D.Failed;
    if (mode == Down3DMode && hiresCapture == Down3DHiresCapture && filter2D == Down3DFilter2D)
        return;

    Down3DMode = mode;
    Down3DHiresCapture = hiresCapture;
    Down3DFilter2D = filter2D;
    // a capped output draws with them from the next frame: SetRenderSettings builds them for the cap
    // asked for, or drops it while filtered 2D's don't build (and caps again once the filter's off).
    // otherwise the next cap builds them
    if (AskedScale != Scale3D)
        SetRenderSettings(AskedScale, Scale3D, ViewWidth, Margin);
}

void GLCompositor::LogOutputCap(const GPU& gpu, int frontendNeed) noexcept
{
    // nothing to say before the renderer's first SetRenderSettings
    if (Scale3D <= 0)
        return;

    OutputCapLogLine line;
    line.Need = OutputNeed(gpu, frontendNeed);
    line.FrontendNeed = frontendNeed;
    line.Scale = Scale;
    line.Scale3D = Scale3D;
    line.ViewWidth = ViewWidth;
    line.Mode = Down3DFilter2D ? Down3DFilterVariant() : Down3DVariant();
    line.AskedMode = gpu.OutputCap3D;
    line.HiresCapture = gpu.HiresCapture ? 1 : 0;
    line.Filter2D = gpu.Filter2D;
    bool anyRatio = OutputAnyRatio(gpu, &line.Ratios);
    // the first reason, in the order they're checked: OutputNeed's, then CappedScale's, then
    // SetRenderSettings'
    if ((gpu.OutputCap & 1) == 0)
        line.Reason = "switch off";
    else if (gpu.Filter2D == 3)
        line.Reason = "filtered 2D identity mode";
    else if (gpu.Shadow2D)
        line.Reason = "GPU 2D shadow";
    else if (line.Need <= 0)
        line.Reason = "need 0";
    else if (line.Need >= Scale3D)
        line.Reason = "need >= scale";
    else if (CappedScale(Scale3D, line.Need, anyRatio) == Scale3D)
        line.Reason = "need above every capped scale";
    else if (Down3D[ViewWidth != 256][0].Failed)
        line.Reason = ViewWidth != 256 ? "wide DOWN3D program failed" : "DOWN3D program failed";
    else if (Down3DFilter2D && Filter2D.Down3D[ViewWidth != 256][Down3DMode].Failed
             && Filter2D.Down3D[ViewWidth != 256][0].Failed)
        line.Reason = ViewWidth != 256 ? "wide filtered DOWN3D program failed" : "filtered DOWN3D program failed";
    else if (Scale == Scale3D)
        line.Reason = "not applied";

    // with the switch off it's said once, whatever the frontend's need does
    if ((gpu.OutputCap & 1) == 0)
    {
        line.Need = line.FrontendNeed = 0;
        line.Ratios = "n/a (switch off)";
    }
    if (line == OutputCapLogged)
        return;
    OutputCapLogged = line;

    const char* source = gpu.OutputCapNeed > 0 ? "outcapneed" : "frontend";
    if (line.Reason)
        Log(LogLevel::Info, "OUTCAP: no cap (%s), output %dx, 3D %dx, outcap %d, outcap3d %d, need %d (%s), frontend need %d, view %d, "
            "hirescap %d, filter2d %d, ratios %s\n",
            line.Reason, Scale, Scale3D, gpu.OutputCap, gpu.OutputCap3D, line.Need, source, frontendNeed, ViewWidth, line.HiresCapture,
            line.Filter2D, line.Ratios);
    else
    {
        // a whole ratio as before ("ratio 2"), another one as a fraction in lowest terms ("ratio 4/3")
        int p, q;
        Down3DRatio(p, q);
        char ratio[24];
        if (q == 1)
            snprintf(ratio, sizeof(ratio), "%d", p);
        else
            snprintf(ratio, sizeof(ratio), "%d/%d", p, q);
        Log(LogLevel::Info, "OUTCAP: capped, output %dx, 3D %dx, ratio %s, outcap3d %d (asked %d), need %d (%s), frontend need %d, view %d, "
            "hirescap %d, filter2d %d, ratios %s\n",
            Scale, Scale3D, ratio, line.Mode, gpu.OutputCap3D, line.Need, source, frontendNeed, ViewWidth, line.HiresCapture,
            line.Filter2D, line.Ratios);
    }
}

void GLCompositor::Down3DRatio(int& p, int& q) const noexcept
{
    int g = std::gcd(Scale3D, Scale);
    p = g > 0 ? Scale3D / g : 1;
    q = g > 0 ? Scale / g : 1;
}

int GLCompositor::CappedScale(int scale, int need, bool anyRatio) noexcept
{
    if (need <= 0)
        return scale;
    // the smallest output scale that's at least the need, at a whole ratio up to 4 (the largest d up to 4
    // that divides scale and leaves scale / d >= need, as before) or, with [anyRatio], at one below 2,
    // where an output pixel covers 2 or 3 3D pixels a side
    for (int s = need; s < scale; s++)
    {
        if ((scale % s == 0 && scale / s <= 4) || (anyRatio && 2 * s > scale))
            return s;
    }
    return scale;
}

bool GLCompositor::OutputAnyRatio(const GPU& gpu, const char** why) noexcept
{
    // the OUTCAP line's words: "any" or "whole", and why
    bool any = (gpu.OutputCap & 2) != 0 || ((gpu.OutputCap & 4) == 0 && gpu.OutputCapDeband);
    const char* reason = (gpu.OutputCap & 2) != 0 ? "any (forced, outcap bit 2)"
                       : (gpu.OutputCap & 4) != 0 ? "whole (forced, outcap bit 4)"
                       : gpu.OutputCapDeband ? "any (deband on)" : "whole (deband off)";
    if (why)
        *why = reason;
    return any;
}

// GPU::OutputCap: what the DOWN3D programs can't draw is decided here, so the output stays at the
// 3D's scale while any of it is on: the GPU 2D shadow's composite (gpu2dwide's too), which draws at
// the output's scale with programs of its own, and filtered 2D's identity mode (Filter2D 3), whose
// 256 view repeats the plain program's mediump maths. the frontend sets them between frames, before it
// calls the renderer's SetOutputNeed, so the output is back at the 3D's scale in the first frame that
// runs them. hi-res capture has a capped pass (HiresCaptureDown3DFS), with its copies at the output's
// scale, and filtered 2D capped programs (FILTER2D with DOWN3D), with the map at the 3D's scale
int GLCompositor::OutputNeed(const GPU& gpu, int frontendNeed) noexcept
{
    if ((gpu.OutputCap & 1) == 0 || gpu.Filter2D == 3 || gpu.Shadow2D)
        return 0;
    return gpu.OutputCapNeed > 0 ? gpu.OutputCapNeed : frontendNeed;
}

void GLCompositor::SetScaleFactor(int scale) noexcept
{
    SetRenderSettings(scale, scale, ViewWidth, Margin);
}

void GLCompositor::SetRenderSettings(int scale, int scale3D, int viewWidth, int margin) noexcept
{
    // what's asked, which SetOutputCap3D asks for again when what a cap needs changes
    AskedScale = scale;

    // GPU::OutputCap: the view's DOWN3D programs, for whole ratios up to 4 and ratios between 1 and 2
    // (CappedScale). GPU::OutputCap3D's (and the capture pass's for it) alongside the exact one, which
    // the draw falls back to if it doesn't build. with filtered 2D on, its own too (PrepareCappedFilter):
    // without one the output stays at the 3D's scale
    bool wide = viewWidth != 256;
    bool ratioFits = scale >= 1 && scale < scale3D
                     && ((scale3D % scale == 0 && scale3D / scale <= 4) || 2 * scale > scale3D);
    if (scale != scale3D && (!ratioFits || !PrepareDown3D(wide, 0)))
        scale = scale3D;
    if (scale != scale3D)
    {
        PrepareCapped(wide, Down3DMode);
        if (Down3DFilter2D && !PrepareCappedFilter(wide, Down3DMode))
            scale = scale3D;
    }

    if (scale == Scale && scale3D == Scale3D && viewWidth == ViewWidth && margin == Margin)
        return;

    if (scale != scale3D || Scale != Scale3D)
        Log(LogLevel::Info, "compositor: output at %dx, 3D at %dx\n", scale, scale3D);

    // the hi-res capture copies are at the scale, a view width change keeps them. the filtered 2D
    // map is at the 3D's (PrepareFilter2D), so it stays while only the output's scale changes, as on a
    // fold or unfold with the cap on (PrepareFilter2D remakes it if the map's own scale changes)
    if (scale != Scale)
        FreeHiresBanks();
    if (scale3D != Scale3D)
        FreeFilter2DTextures();
    // by the 3D's scale: the plain program only draws while the output is at it
    if ((scale3D > 8) != CompShaderForHighScale)
        BuildCompShader(scale3D > 8);

    Scale = scale;
    Scale3D = scale3D;
    ViewWidth = viewWidth;
    ScreenW = viewWidth * scale;
    if (margin != Margin)
    {
        Margin = margin;
        SetupCompVertices(margin);
        // the renderer's own buffer stays bound (a margin change is rare, so the glGet is fine)
        GLint arrayBuffer = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 12 * sizeof(CompVertex), &CompVertices[0]);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer);
    }
    ScreenH = (384+2 + 2*Margin) * scale;

    // held wings are only reused at the width, margin and scale they were made at
    ResetWingHold();

    // a frontend's textures are at the old size, so the buffers go back to the compositor's own
    // until it passes new ones. those are only made, or resized, once a frame is drawn into them
    // (PrepareOutput)
    for (int i = 0; i < 2; i++)
    {
        if (!OutputExternal[i])
            continue;

        glBindFramebuffer(GL_FRAMEBUFFER, CompScreenOutputFB[i]);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, CompScreenOutputTex[i], 0);
        OutputExternal[i] = false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// before a frame is drawn into a buffer: without a frontend texture (SetOutputTexture) it gets
// the compositor's own, made here at the output's size, so the framebuffer is complete either way
void GLCompositor::PrepareOutput(int buf) noexcept
{
    if (OutputExternal[buf] || (OutputTexW[buf] == ScreenW && OutputTexH[buf] == ScreenH))
        return;

    // unit 0 is bound again before anything is drawn (RenderFrame)
    glActiveTexture(GL_TEXTURE0);
    if (!CompScreenOutputTex[buf])
    {
        glGenTextures(1, &CompScreenOutputTex[buf]);
        glBindTexture(GL_TEXTURE_2D, CompScreenOutputTex[buf]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, CompScreenOutputTex[buf]);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    // fill the padding
    u8* zeroPixels = (u8*) calloc(1, ScreenW*2*Scale*4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, TopRows()*Scale, ScreenW, 2*Scale, GL_RGBA, GL_UNSIGNED_BYTE, zeroPixels);
    free(zeroPixels);

    GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
    glBindFramebuffer(GL_FRAMEBUFFER, CompScreenOutputFB[buf]);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, CompScreenOutputTex[buf], 0);
    glDrawBuffers(1, fbassign);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    OutputTexW[buf] = ScreenW;
    OutputTexH[buf] = ScreenH;
}

void GLCompositor::Stop(const GPU& gpu) noexcept
{
    // a buffer that was never drawn into has no texture, so there's nothing to clear (and its
    // framebuffer isn't complete)
    bool hasOutput = OutputExternal[gpu.FrontBuffer] || CompScreenOutputTex[gpu.FrontBuffer];
    for (int i = 0; hasOutput && i < 2; i++)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[gpu.FrontBuffer]);

        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ResetWingHold();
    ResetHiresCapture();
    ResetFilter2D();
    ResetShadow2D();
}

void GLCompositor::ResetWingHold() noexcept
{
    WingHold = {};
    WingCopy = {};
    WingHoldLines = {};
}

void GLCompositor::ResetHiresCapture() noexcept
{
    HiresCap.Banks = {};
    for (HiresNativeRows& native : HiresCap.BankNative)
        native.State = {};
    HiresCap.ScratchNative.State = {};
    HiresCap.ShowBank = -1;
    HiresCap.StreamSource = -1;
    HiresCap.LastScreenSwap = -1;
    HiresCap.StreamMissed = false;
}

// the map only depends on what UpdateFilter2DKeys compares, so a new scene would remake it anyway
// once its keys differ. this is so nothing from before can be reused, whatever the keys
void GLCompositor::ResetFilter2D() noexcept
{
    Filter2D.MapPasses = 0;
}

void GLCompositor::ResetShadow2D() noexcept
{
    Shadow.MemGeneration = 0;
}

// decides, from the control words of the frame being composited, which wing lines are held
// (WingHoldLines, and for the shader WingHeldBit in WingCtrl) and which halves get copied after
// drawing (WingCopy).
// a half is held while unit A's 3D is in the other half this frame (control word bit 22, the
// game moved unit A over), it had its own in a copy made at most 2 frames ago, and it hasn't
// been held for 2 frames in a row yet. only lines that had 3D wings (bit 18) in the copy and
// would be black now are held, so 2D-only lines and white lines stay as they are. bit 22 is
// set whatever the windows do, so a window that hides the wings on every line of unit A's
// screen still counts as unit A being there. games with 3D on one screen never have unit A's
// 3D in both halves, so they never copy or hold
int GLCompositor::PrepareWingHold(const GPU& gpu, int backbuf) noexcept
{
    int topWingLines = 0;
    std::array<bool, 2> live {};
    std::array<std::array<u32, 6>, 2> liveLines {}, blackLines {};
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            u32 ctrl = fb[line * (256*3 + 1) + 256*3];
            if (ctrl & (1 << 22))
                live[half] = true;

            if (ctrl & (1 << 18))
            {
                liveLines[half][line >> 5] |= 1u << (line & 31);
                topWingLines += half == 0;
            }
            else if (!(ctrl & (1 << 21)))
                blackLines[half][line >> 5] |= 1u << (line & 31);
        }

        WingHoldHalf& hold = WingHold[half];
        hold.SinceLive = live[half] ? 0 : std::min(hold.SinceLive + 1, WingHoldNever);
        hold.SinceCopy = std::min(hold.SinceCopy + 1, WingHoldNever);
    }

    // a half's live wings are copied while unit A was in the other half in the last 2 frames
    WingCopy[0] = live[0] && WingHold[1].SinceLive <= 2;
    WingCopy[1] = live[1] && WingHold[0].SinceLive <= 2;

    // with a margin, the output's size, as the margin rows are kept full width (CopyHeldWings)
    int wingW = (ViewWidth - 256) / 2 * Scale;
    int heldW = Margin ? ScreenW : wingW * 2;
    if ((WingCopy[0] || WingCopy[1]) && (HeldWingsW != heldW || HeldWingsH != ScreenH))
    {
        if (!HeldWingsTex)
        {
            glGenTextures(1, &HeldWingsTex);
            glGenFramebuffers(1, &HeldWingsFB);
        }

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, HeldWingsTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, heldW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, HeldWingsFB);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, HeldWingsTex, 0);

        HeldWingsW = heldW;
        HeldWingsH = ScreenH;
        // what was in the old texture is gone
        WingHold[0].SinceCopy = WingHold[1].SinceCopy = WingHoldNever;
    }

    WingHoldLines = {};
    for (int half = 0; half < 2; half++)
    {
        WingHoldHalf& hold = WingHold[half];
        if (live[half])
        {
            hold.HoldRun = 0;
            continue;
        }

        if (WingHold[half ^ 1].SinceLive != 0 || hold.SinceCopy > 2 || hold.HoldRun >= 2)
            continue;

        hold.HoldRun++;
        for (int i = 0; i < 6; i++)
            WingHoldLines[half * 6 + i] = hold.CopyLines[i] & blackLines[half][i];
    }

    // the shader reads the held flag from the control words, so RenderFrame uploads this copy
    // of them over the frame's. the bit is cleared everywhere else, as a game can write any
    // value to MASTER_BRIGHT
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            int outLine = half * 192 + line;
            u32 ctrl = fb[line * (256*3 + 1) + 256*3] & ~WingHeldBit;
            if ((WingHoldLines[outLine >> 5] >> (outLine & 31)) & 1)
                ctrl |= WingHeldBit;
            CtrlWords[outLine] = ctrl;
        }
    }

    for (int half = 0; half < 2; half++)
    {
        if (WingCopy[half])
            WingHold[half].CopyLines = liveLines[half];
    }
    // held lines show 3D wings too (only black lines are held, so they're never counted twice),
    // which keeps a dual-screen 3D game's top screen counted on the frames unit A is away
    for (int i = 0; i < 6; i++)
        topWingLines += __builtin_popcount(WingHoldLines[i]);
    return topWingLines;
}

// copies the wings of the halves PrepareWingHold picked from the output just drawn. the
// wings are the output's first and last wingW columns, and its halves are 192 lines each
// with the 2 padding lines between them. with a margin they're copied where they are, and the
// top half's margin rows too, full width: a margin is held on the frames its edge line is
void GLCompositor::CopyHeldWings(int backbuf) noexcept
{
    int wingW = (ViewWidth - 256) / 2 * Scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, HeldWingsFB);

    for (int half = 0; half < 2; half++)
    {
        if (!WingCopy[half])
            continue;

        int y0 = HalfLineRow(half) * Scale;
        int y1 = y0 + 192 * Scale;
        int rightX = Margin ? ScreenW - wingW : wingW;
        glBlitFramebuffer(0, y0, wingW, y1, 0, y0, wingW, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitFramebuffer(ScreenW - wingW, y0, ScreenW, y1, rightX, y0, rightX + wingW, y1,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (half == 0 && Margin)
        {
            int m = Margin * Scale, bottom = (192 + Margin) * Scale;
            glBlitFramebuffer(0, 0, ScreenW, m, 0, 0, ScreenW, m, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBlitFramebuffer(0, bottom, ScreenW, bottom + m, 0, bottom, ScreenW, bottom + m,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        WingHold[half].SinceCopy = 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
}

void GLCompositor::BeginGPUTimer(int pass) noexcept
{
    if (GPUTimers)
        GPUTimers->BeginGPUTimer(pass);
}

// GPU::CompClear (experiment K2a, docs/plan-gpu-discovery.md): starts the output's render pass
// without its old contents, so a tiled GPU needn't load every tile from memory first. called
// with the output bound, right before RenderFrame's main draw: everything that draws elsewhere
// (the filter's pre-passes, the shadow's OBJ pass) is done by then, and nothing has drawn into the
// output yet. nothing reads the old contents either: that draw covers every pixel of both
// screens (CompVertices, full width, and no compositor program discards), in every frame,
// including those without the frame's lines; the shadow pass, the held wings' copy and the
// hi-res capture come after it and only see this frame's draw. the padding rows are the only
// pixels it leaves, and they're 0 in both kinds of output (PrepareOutput, and the frontend's
// textures start cleared), which both modes keep. (a half GPU::Comp1x draws at 1x leaves the rest
// of its rows, which nothing shows, so there they end up 0 or undefined.) 1 clears the whole
// target to 0, with the scissor test off (it already is here) and the color mask on (RenderFrame
// set it); 2 invalidates it and clears only the padding rows to 0
void GLCompositor::ClearOutput(int mode) noexcept
{
    // glClearBuffer instead of glClear, so the clear color stays what others set (the desktop
    // presenter's is opaque black, which Stop's glClear then uses)
    const GLfloat zero[4] = {0.f, 0.f, 0.f, 0.f};
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (mode == 2)
    {
        GLenum attachment = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);

        GLint box[4];
        glGetIntegerv(GL_SCISSOR_BOX, box);
        if (!scissor)
            glEnable(GL_SCISSOR_TEST);
        glScissor(0, TopRows() * Scale, ScreenW, 2 * Scale);
        glClearBufferfv(GL_COLOR, 0, zero);
        glScissor(box[0], box[1], box[2], box[3]);
        if (!scissor)
            glDisable(GL_SCISSOR_TEST);
        return;
    }

    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glClearBufferfv(GL_COLOR, 0, zero);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
}

// GPU::Comp1x (experiment K2c): the halves (bit 0 the top one) whose full draw is a nearest upscale
// of what the 1x draw makes, from the control words as the shader gets them (CtrlWords when
// ctrlCopied). at 1x every pixel's texture coordinate is the center of its native pixel, and a pixel
// that only reads its native pixel's planes and control word composites as each of its sub-pixels
// does in the full draw. a half is plain when nothing on its lines reads finer than that:
// - no line shows a hi-res capture image (bit 7, once PrepareHiresDisplay rewrote the words);
// - in a wide view: no line has unit A's 3D (bit 22, set whatever the windows do with it), so no 3D
//   pixel in the middle and no 3D wings (bit 18 only comes with it), no line takes held wings
//   (WingHeldBit), and the half's wings aren't copied this frame (only a half with bit 22's are). the
//   wings left are black or white, from the control word alone. (the words are CtrlWords here: the
//   wing hold runs in every wide frame with lines, and only those get this far);
// - in the 256 view, which has no bit 22: no pixel of a display mode 1 line composites the 3D layer
//   (compmode 0-4 in plane 3's top byte, the shader's test).
// a screen that's off (GPU2D::SoftRenderer::FillScreensOff) is black whatever its 3D is; the wide
// view counts it as plain since it clears bit 22, the 256 view doesn't, which only costs the saving
int GLCompositor::PlainHalves(const GPU& gpu, int backbuf, bool wingHold, bool hiresDisplay, bool ctrlCopied) const noexcept
{
    int halves = 0;
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        bool plain = !(wingHold && WingCopy[half]);
        for (int line = 0; plain && line < 192; line++)
        {
            const u32* planes = &fb[line * (256*3 + 1)];
            u32 ctrl = ctrlCopied ? CtrlWords[half * 192 + line] : planes[256*3];
            if (hiresDisplay && (ctrl & HiresDisplayFlag))
                plain = false;
            else if (ViewWidth != 256)
                plain = !(ctrl & ((1u << 22) | (1u << 18) | WingHeldBit));
            else if (((ctrl >> 16) & 0x3) == 1)
            {
                for (int x = 0; x < 256; x++)
                {
                    if (((planes[512 + x] >> 24) & 0xF) <= 4)
                    {
                        plain = false;
                        break;
                    }
                }
            }
        }
        if (plain)
            halves |= 1 << half;
    }
    return halves;
}

// GPU::Comp1x: half [half] at 1x (or, filtered, at the map's scale: draw.DrawScale), ViewWidth x 192
// at that scale from row y of what's bound to draw into, with the main draw's program, uniforms and
// textures. the filter's program works out its sub-pixel from gl_FragCoord, so it's told how far
// the half's rows are from where they'd start in an output at the draw's scale. EndHalvesAt1x puts
// the uniforms and the viewport back
void GLCompositor::DrawHalfAt1x(int half, int y, const SmallHalfDraw& draw) noexcept
{
    glUniform1ui(draw.ScaleLoc, draw.DrawScale);
    if (draw.RowOffsetLoc >= 0)
        glUniform1i(draw.RowOffsetLoc, y - half * 194 * draw.DrawScale);
    glViewport(0, y, ViewWidth * draw.DrawScale, 192 * draw.DrawScale);
    glDrawArrays(GL_TRIANGLES, 12 + half * 6, 6);
}

void GLCompositor::EndHalvesAt1x(const SmallHalfDraw& draw) noexcept
{
    glUniform1ui(draw.ScaleLoc, Scale);
    if (draw.RowOffsetLoc >= 0)
        glUniform1i(draw.RowOffsetLoc, 0);
    glViewport(0, 0, ScreenW, ScreenH);
}

// GPU::Comp1x 3: right after the full draw, draws [halves] at 1x (filtered: at the map's scale) into
// the check target and reads both back: every pixel of each half's full draw should equal the pixel
// it upscales from there. counted for the COMP1X line, with the first few that don't logged
void GLCompositor::CheckComp1x(int backbuf, int halves, const SmallHalfDraw& draw) noexcept
{
    if (OneX.CheckFailed)
        return;
    const int ds = draw.DrawScale;
    if (OneX.CheckW != ViewWidth || OneX.CheckScale != ds)
    {
        FreeComp1xCheck();
        OneX.CheckTex = MakeHiresTexture(GL_RGBA8, ViewWidth * ds, 384 * ds);
        // the draw's 3D frame is on unit 1, which was the active one
        glActiveTexture(GL_TEXTURE1);
        GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
        glGenFramebuffers(1, &OneX.CheckFB);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OneX.CheckFB);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, OneX.CheckTex, 0);
        glDrawBuffers(1, fbassign);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            Log(LogLevel::Error, "COMP1X: the check target is incomplete\n");
            FreeComp1xCheck();
            OneX.CheckFailed = true;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
            return;
        }
        OneX.CheckW = ViewWidth;
        OneX.CheckScale = ds;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OneX.CheckFB);
    for (int half = 0; half < 2; half++)
    {
        if (halves & (1 << half))
            DrawHalfAt1x(half, half * 192 * ds, draw);
    }
    EndHalvesAt1x(draw);

    // each sub-pixel of the full draw upscales the small one's pixel [up] times
    const int smallW = ViewWidth * ds, up = Scale / ds;
    std::vector<u32> small((size_t)smallW * 192 * ds), full((size_t)ScreenW * 192 * Scale);
    std::vector<u8> nativeBad((size_t)ViewWidth * 192);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    for (int half = 0; half < 2; half++)
    {
        if (!(halves & (1 << half)))
            continue;

        glBindFramebuffer(GL_READ_FRAMEBUFFER, OneX.CheckFB);
        glReadPixels(0, half * 192 * ds, smallW, 192 * ds, GL_RGBA, GL_UNSIGNED_BYTE, small.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
        glReadPixels(0, half * 194 * Scale, ScreenW, 192 * Scale, GL_RGBA, GL_UNSIGNED_BYTE, full.data());

        std::fill(nativeBad.begin(), nativeBad.end(), 0);
        u32 mismatches = 0, logged = 0;
        for (int y = 0; y < 192 * Scale; y++)
        {
            const u32* row = &full[(size_t)y * ScreenW];
            const u32* smallRow = &small[(size_t)(y / up) * smallW];
            for (int x = 0; x < ScreenW; x++)
            {
                u32 expected = smallRow[x / up];
                if (row[x] == expected)
                    continue;
                mismatches++;
                nativeBad[(y / Scale) * ViewWidth + x / Scale] = 1;
                if (logged < 8 && OneX.Mismatches + mismatches <= 64)
                {
                    logged++;
                    Log(LogLevel::Info, "COMP1X mismatch half=%d x=%d y=%d native=%d,%d full=%08X small=%08X halfscale=%d\n",
                        half, x, y, x / Scale, y / Scale, row[x], expected, ds);
                }
            }
        }
        OneX.Checks++;
        OneX.CheckPixels += (u32)full.size();
        OneX.Mismatches += mismatches;
        for (u8 bad : nativeBad)
            OneX.MismatchNative += bad;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
    glViewport(0, 0, ScreenW, ScreenH);
}

// GPU::Comp1x, with GPU::SceneStats or GPU::GLTimers on, or in the check mode: once a second, the
// frames composited, how many had only the top half at 1x, only the bottom one or both (in the check
// mode, how many had them plain), why the others couldn't (Comp1xOff; a frame left out of both had no
// plain half), and the check mode's checks, the pixels they compared and how many of those differed,
// and in how many native pixels
void GLCompositor::LogComp1x(const GPU& gpu) noexcept
{
    s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (OneX.LogStartNs == 0)
        OneX.LogStartNs = now;
    OneX.LogFrames++;
    if (now - OneX.LogStartNs < 1000000000)
        return;

    const std::array<u32, Comp1xOff_Count>& off = OneX.LogOff;
    Log(LogLevel::Info, "COMP1X mode=%d scale=%d width=%d frames=%u top=%u bottom=%u both=%u off front=%u scale=%u "
        "view=%u nolines=%u filter=%u shadow=%u checks=%u checkpx=%u mismatches=%u mismatchnative=%u failed=%d "
        "filtered=%u margin=%u\n",
        gpu.Comp1x, Scale, ViewWidth, OneX.LogFrames, OneX.LogHalves[0], OneX.LogHalves[1], OneX.LogHalves[2],
        off[Comp1xOffFront], off[Comp1xOffScale], off[Comp1xOffView], off[Comp1xOffLines], off[Comp1xOffFilter],
        off[Comp1xOffShadow], OneX.Checks, OneX.CheckPixels, OneX.Mismatches, OneX.MismatchNative,
        OneX.CheckFailed ? 1 : 0, OneX.LogFiltered, off[Comp1xOffMargin]);

    OneX.LogStartNs = now;
    OneX.LogFrames = 0;
    OneX.LogHalves = {};
    OneX.LogOff = {};
    OneX.LogFiltered = 0;
    OneX.Checks = OneX.CheckPixels = OneX.Mismatches = OneX.MismatchNative = 0;
}

void GLCompositor::FreeComp1xCheck() noexcept
{
    if (!OneX.CheckTex && !OneX.CheckFB)
        return;
    glDeleteFramebuffers(1, &OneX.CheckFB);
    glDeleteTextures(1, &OneX.CheckTex);
    OneX.CheckFB = OneX.CheckTex = 0;
    OneX.CheckW = OneX.CheckScale = 0;
}

bool GLCompositor::TakeOutputLayout(int buf, OutputLayout& layout) noexcept
{
    if (!OneX.Drawn[buf])
        return false;
    OneX.Drawn[buf] = false;
    layout.HalvesAt1x = OneX.Halves[buf];
    layout.Scale = OneX.Scale[buf];
    layout.HalfScale = OneX.HalfScale[buf];
    layout.TopWingLines = OneX.TopWingLines[buf];
    layout.TopMargin = OneX.TopMargin[buf];
    return true;
}

void GLCompositor::RenderFrame(const GPU& gpu, Renderer3D& renderer) noexcept
{
    int backbuf = gpu.FrontBuffer ^ 1;
    bool hasFramebuffers = gpu.Framebuffer[backbuf][0] && gpu.Framebuffer[backbuf][1];

    // nothing to do while the frontend passes its own output texture
    PrepareOutput(backbuf);

    // hi-res capture: a bank written since the last Blit loses its copy. GPU clears the write
    // masks right after this (GPU::HiresCaptureBlitDone), so they're folded in every frame, whether
    // or not a pass runs: otherwise a write in a frame that doesn't capture would be forgotten
    for (int bank = 0; bank < 4; bank++)
    {
        if (gpu.HiresCapWrites & (1 << bank))
            HiresCap.Banks[bank].Valid = false;
    }

    // before the output is bound, as this can set up the held wings' framebuffer
    bool wingHold = ViewWidth != 256 && hasFramebuffers;
    int topWingLines = 0;
    if (wingHold)
        topWingLines = PrepareWingHold(gpu, backbuf);
    else
        WingHoldLines = {};

    // after the wing hold, which fills CtrlWords when it runs
    bool hiresDisplay = hasFramebuffers && PrepareHiresDisplay(gpu, backbuf, wingHold);

    // before the output is bound too, as this can set up the filter's framebuffers
    int filterPasses = PrepareFilter2D(gpu, hasFramebuffers);
    const Filter2DProgram& filterComp = Filter2D.Comp[ViewWidth != 256];
    // GPU::OutputCap: a capped output's program, the view's in its GPU::OutputCap3D mode, filtered 2D's
    // with the filter. SetRenderSettings only caps once that one's built; without it (a frame where the
    // filter turned on and SetOutputNeed hasn't seen it) the frame is drawn unfiltered
    const bool capped = Scale3D != Scale;
    if (capped && filterPasses > 0 && !Filter2D.Down3D[ViewWidth != 256][Down3DFilterVariant()].Program)
        filterPasses = 0;
    const int capMode = filterPasses > 0 ? Down3DFilterVariant() : Down3DVariant();
    const Down3DProgram& capProgram = filterPasses > 0 ? Filter2D.Down3D[ViewWidth != 256][capMode]
                                                       : Down3D[ViewWidth != 256][capMode];

    // the control words the shader reads are CtrlWords instead of the frame's when either rewrote them
    bool ctrlCopied = wingHold || hiresDisplay;

    // the GPU 2D shadow renderer: which lines it draws, what they read and the OBJ pass, before the
    // output is bound too (it has targets of its own). its composite pass comes after the draw below.
    // debug only: with the compute or OpenGL renderer's timers on, everything from here on is timed,
    // each part as its own pass (ComputePass, from Comp on) and the rest as CompExtra
    const bool timers = gpu.ComputeTimers || gpu.ComputeSync || gpu.GLTimers;
    GPUTimers = timers ? &renderer : nullptr;
    BeginGPUTimer(ComputePass::CompExtra);
    int shadowLines = PrepareShadow2D(gpu, backbuf, hasFramebuffers, hiresDisplay, ctrlCopied);

    // GPU::Comp1x (experiment K2c): the plain halves (see PlainHalves) are drawn at 1x into the first
    // ViewWidth x 192 texels of their own rows, and the frontend shows them from there (see
    // TakeOutputLayout), so a half costs 1/Scale^2 of its full draw. only in frames that draw nothing
    // over the output (the shadow) or with another program (the filter), with the frame's own lines,
    // and above 1x. 1 only in a wide view: its program is highp, so the 1x pixel is exactly what each
    // full-draw sub-pixel gets, where the 256 view's mediump coordinates can round a sub-pixel into the
    // next native pixel on an fp16 GPU. the check mode (3) finds the plain halves the same way (both
    // views, whatever the frontend can do), draws them in full and compares (CheckComp1x).
    // with filtered 2D (experiment K3), a plain half is drawn with the filter's program at the map's
    // scale M instead: there a sub-pixel s at scale S reads map texel (2s+1)M/(2S), which is s/(S/M)
    // when S is a multiple of M, and native pixel s/S, which is that over M, and nothing else it reads
    // depends on s, so its full draw is a nearest upscale by S/M of what the draw at M makes. that
    // program reads gl_FragCoord rather than its varying (except Filter2D 3, whose varying is
    // mediump in the 256 view), so this holds in the 256 view too.
    // capped (GPU::OutputCap), the filter's program is the capped one, which keeps its ratio: at a draw
    // scale D it reads the map at sub-pixel s of scale D * ratio, (2s + 1) M / (2 D ratio), which is
    // s / ratio at D = M: the pixel's own texel, as the uncapped program at M reads it. a plain half
    // reads no 3D, so the 3D's scale doesn't matter there. the filtered halves need an output above the
    // map's scale, so at 4x with the 4x map (16x 3D on the Fold's cover screen) they're drawn in full.
    // that needs a whole ratio too: at another one (GPU::OutputCap bit 2, 8x 3D at 6x) the draw at M
    // would read the map at M * Scale3D / Scale, which isn't a scale, so they're drawn in full there
    int halvesAt1x = 0, checkHalves = 0;
    SmallHalfDraw smallDraw;
    if (filterPasses > 0)
        smallDraw = {Filter2D.MapScale, capped ? capProgram.ScaleLoc : filterComp.ScaleLoc,
                     capped ? capProgram.RowOffsetLoc : filterComp.RowOffsetLoc};
    else
        smallDraw = {1, capped ? capProgram.ScaleLoc : ViewWidth != 256 ? CompWideScaleLoc : CompScaleLoc, -1};
    if (gpu.Comp1x != 0)
    {
        bool check = gpu.Comp1x == 3;
        int off = Comp1xOn;
        if (!check && !gpu.Comp1xFrontend)
            off = Comp1xOffFront;
        else if (Scale < 2)
            off = Comp1xOffScale;
        else if (gpu.Comp1x == 1 && ViewWidth == 256 && filterPasses == 0)
            off = Comp1xOffView;
        else if (!hasFramebuffers)
            off = Comp1xOffLines;
        else if (filterPasses > 0 && (Scale % smallDraw.DrawScale != 0 || Scale <= smallDraw.DrawScale
                                      || (gpu.Filter2D == 3 && ViewWidth == 256) || (capped && Scale3D % Scale != 0)))
            off = Comp1xOffFilter;
        else if (shadowLines > 0)
            off = Comp1xOffShadow;
        else if (Margin > 0)
            off = Comp1xOffMargin;
        OneX.LogOff[off]++;

        if (off == Comp1xOn)
        {
            int plain = PlainHalves(gpu, backbuf, wingHold, hiresDisplay, ctrlCopied);
            if (check)
                checkHalves = plain;
            else
                halvesAt1x = plain;
            if (plain)
            {
                OneX.LogHalves[plain - 1]++;
                if (filterPasses > 0)
                    OneX.LogFiltered++;
            }
        }
    }
    if (gpu.Comp1x != 3)
        FreeComp1xCheck();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glViewport(0, 0, ScreenW, ScreenH);

    // TODO: select more shaders (filtering, etc)
    if (capped)
    {
        // GPU::OutputCap: a capped output, with capProgram
        glUseProgram(capProgram.Program);
        glUniform1ui(capProgram.ScaleLoc, Scale);
        int ratioP, ratioQ;
        Down3DRatio(ratioP, ratioQ);
        glUniform2i(capProgram.RatioLoc, ratioP, ratioQ);
        glUniform1i(capProgram.HiresCapLoc, hiresDisplay);
        if (ViewWidth != 256)
        {
            glUniform1f(capProgram.WideRatioLoc, ViewWidth / 256.0f);
            glUniform1i(capProgram.WideOffsetLoc, (ViewWidth - 256) / 2);
            glUniform1i(capProgram.WideMarginLoc, Margin);
        }
        if (filterPasses > 0)
        {
            glUniform1i(capProgram.MapScaleLoc, Filter2D.MapScale);
            glUniform1i(capProgram.RowOffsetLoc, 0);
        }
    }
    else if (filterPasses > 0)
    {
        glUseProgram(filterComp.Program);
        glUniform1ui(filterComp.ScaleLoc, Scale);
        glUniform1i(filterComp.HiresCapLoc, hiresDisplay);
        glUniform1i(filterComp.MapScaleLoc, Filter2D.MapScale);
        glUniform1i(filterComp.IdentityLoc, gpu.Filter2D == 3);
        glUniform1i(filterComp.RowOffsetLoc, 0);
        if (ViewWidth != 256)
        {
            glUniform1f(filterComp.RatioLoc, ViewWidth / 256.0f);
            glUniform1i(filterComp.OffsetLoc, (ViewWidth - 256) / 2);
            glUniform1i(filterComp.MarginLoc, Margin);
        }
    }
    else if (ViewWidth == 256)
    {
        glUseProgram(CompShader);
        glUniform1ui(CompScaleLoc, Scale);
        glUniform1i(CompHiresCapLoc, hiresDisplay);
    }
    else
    {
        glUseProgram(CompShaderWide);
        glUniform1ui(CompWideScaleLoc, Scale);
        glUniform1f(CompWideRatioLoc, ViewWidth / 256.0f);
        glUniform1i(CompWideOffsetLoc, (ViewWidth - 256) / 2);
        glUniform1i(CompWideHiresCapLoc, hiresDisplay);
        glUniform1i(CompWideMarginLoc, Margin);
    }

    // the frame's wing planes, screen planes and control words
    if (hasFramebuffers)
        BeginGPUTimer(ComputePass::CompUpload);

    if (ViewWidth != 256)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, HeldWingsTex);

        // the frame's wing planes, which come after each screen's lines in its buffer
        glActiveTexture(GL_TEXTURE0 + WingPlanesUnit);
        glBindTexture(GL_TEXTURE_2D, WingPlanesTex);
        if (hasFramebuffers)
        {
            for (int half = 0; half < 2; half++)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, half * 192, GPU2D::WingPlanesWords, 192, GL_RGBA_INTEGER,
                                GL_UNSIGNED_BYTE, &gpu.Framebuffer[backbuf][half][GPU2D::AccelWingPlanesStart]);
        }
    }

    if (hiresDisplay && HiresCap.ShowBank >= 0)
    {
        glActiveTexture(GL_TEXTURE0 + HiresBankUnit);
        glBindTexture(GL_TEXTURE_2D, HiresCap.BankTex[HiresCap.ShowBank]);
    }
    if (hiresDisplay && HiresCap.StreamSource >= 0)
    {
        glActiveTexture(GL_TEXTURE0 + HiresStreamUnit);
        glBindTexture(GL_TEXTURE_2D, HiresCap.StreamSource == HiresStreamScratch
                                     ? HiresCap.ScratchTex : HiresCap.BankTex[HiresCap.StreamSource]);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CompScreenInputTex);

    // the control words again, with the held lines flagged (see PrepareWingHold) and the
    // hi-res capture flag only on the lines that show an image (see PrepareHiresDisplay)
    if (hasFramebuffers)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][0].get());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][1].get());

        if (ctrlCopied)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 256*3, 0, 1, 384, GL_RGBA_INTEGER,
                            GL_UNSIGNED_BYTE, CtrlWords.data());
    }

    // after the upload: the pre-pass reads the frame's planes and the final control words
    if (filterPasses > 0)
    {
        RunFilter2D(gpu, backbuf, hiresDisplay, ctrlCopied, filterPasses);
        glUseProgram(capped ? capProgram.Program : filterComp.Program);
    }

    glActiveTexture(GL_TEXTURE1);
    renderer.SetupAccelFrame();
    // GPU::OutputCap3D 1 and 3: the capped draw's bilinear taps, for this draw only (unbound right after it)
    bool down3DLinear = capped && (capMode == 1 || capMode == 3);
    if (down3DLinear)
        glBindSampler(1, Down3DSampler);

    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glBindVertexArray(CompVertexArrayID);
    // the draw that reads hi-res copies apart, as K1 (docs/plan-gpu-discovery.md) looks at that one
    BeginGPUTimer(hiresDisplay ? ComputePass::CompHires : ComputePass::Comp);
    // in the draw's timer, so an A/B of Comp includes what the clear costs
    if (gpu.CompClear != 0)
        ClearOutput(gpu.CompClear);
    if (halvesAt1x == 0)
    {
        glDrawArrays(GL_TRIANGLES, 0, 4*3);
    }
    else
    {
        // the full halves (vertices 0-5 are half 1's), then the others at 1x (or the map's scale)
        if (!(halvesAt1x & 2))
            glDrawArrays(GL_TRIANGLES, 0, 6);
        if (!(halvesAt1x & 1))
            glDrawArrays(GL_TRIANGLES, 6, 6);
        for (int half = 0; half < 2; half++)
        {
            if (halvesAt1x & (1 << half))
                DrawHalfAt1x(half, half * 194 * Scale, smallDraw);
        }
        EndHalvesAt1x(smallDraw);
    }
    if (down3DLinear)
        glBindSampler(1, 0);
    BeginGPUTimer(ComputePass::CompExtra);

    // what's in the output now, for the frontend (TakeOutputLayout)
    OneX.Halves[backbuf] = halvesAt1x;
    OneX.Scale[backbuf] = Scale;
    OneX.HalfScale[backbuf] = halvesAt1x != 0 ? smallDraw.DrawScale : 1;
    OneX.Drawn[backbuf] = true;
    OneX.TopWingLines[backbuf] = topWingLines;
    OneX.TopMargin[backbuf] = Margin;

    // before anything else draws or reads, with the main draw's state still set
    if (checkHalves != 0 && (OneX.CheckFrames++ % 30) == 0)
        CheckComp1x(backbuf, checkHalves, smallDraw);

    // over the draw, before the held wings are copied, so they hold the shadow's 2D too
    if (shadowLines > 0)
        RunShadow2D(backbuf);

    if (wingHold && (WingCopy[0] || WingCopy[1]))
    {
        BeginGPUTimer(ComputePass::CompWings);
        CopyHeldWings(backbuf);
    }

    // after the draw, which shows the copies as they were before this frame's capture
    if (gpu.HiresCapture)
        RunHiresCapture(gpu, backbuf);
    if (timers)
        renderer.EndGPUTimer();
    GPUTimers = nullptr;
    if (gpu.HiresCaptureDebug)
        LogHiresCapture();
    if (gpu.Filter2DLogging)
        LogFilter2D(gpu, filterPasses);
    if (gpu.Comp1x != 0 && (gpu.SceneStats || gpu.GLTimers || gpu.Comp1x == 3))
        LogComp1x(gpu);

    if (const Shadow2DRecorder* rec = gpu.Shadow2D.get())
    {
        if (shadowLines > 0 && rec->Check && (Shadow.CheckFrames++ % 30) == 0)
            CheckShadow2D(gpu, backbuf);
        if (rec->Logging)
            LogShadow2D(gpu);
    }
}

// filtered 2D: how many MMPX passes this frame gets, 0 for none, and everything they need is made.
// there's nothing to filter at scale 1, and without the frame's lines the screen texture is stale
// the map is at the scale the full-scale draw would use, by the 3D's scale: a capped output
// (GPU::OutputCap) filters what the full-scale draw would, so its capped draw stays a box filter of
// that one's. it can be above the output's scale (the 4x map at 2x), and the capped program reads it
// at the 3D's scale
int GLCompositor::PrepareFilter2D(const GPU& gpu, bool hasFramebuffers) noexcept
{
    if (gpu.Filter2D == 0 || Scale3D < 2)
    {
        FreeFilter2DTextures();
        return 0;
    }
    if (!hasFramebuffers || Filter2D.Failed || !PrepareFilter2DShaders(ViewWidth != 256))
        return 0;

    // scales above 4 read the 4x map, and scale 3 the 2x one
    int passes = (Scale3D >= 4 && gpu.Filter2D != 2) ? 2 : 1;
    int mapScale = passes == 2 ? 4 : 2;
    if (Filter2D.MapScale != mapScale)
    {
        FreeFilter2DTextures();
        bool made = MakeFilter2DTarget(Filter2D.KeyTex, Filter2D.KeyFB, GL_RG32UI, 256, 384);
        if (made && passes == 2)
            made = MakeFilter2DTarget(Filter2D.Map2xTex, Filter2D.Map2xFB, GL_RG32UI, 512, 768);
        if (made)
            made = MakeFilter2DTarget(Filter2D.MapTex, Filter2D.MapFB, GL_R16UI, 256 * mapScale, 384 * mapScale);
        if (!made)
        {
            Log(LogLevel::Error, "FILTER2D: incomplete framebuffer, staying off\n");
            FreeFilter2DTextures();
            Filter2D.Failed = true;
            return 0;
        }
        Filter2D.MapScale = mapScale;
    }

    return passes;
}

bool GLCompositor::PrepareFilter2DShaders(bool wide) noexcept
{
    if (!Filter2D.KeyShader)
    {
        GLuint shader {};
        if (!OpenGL::CompileVertexFragmentProgram(shader, kHiresCaptureVS, kFilter2DKeyFS,
                "Filter2DKeyShader", {}, {{"oTexel", 0}}))
        {
            Filter2D.Failed = true;
            return false;
        }

        Filter2D.KeyShader = shader;
        Filter2D.KeyHiresCapLoc = glGetUniformLocation(shader, "uHiresCap");
        glUseProgram(shader);
        glUniform1i(glGetUniformLocation(shader, "ScreenTex"), 0);
    }

    if (!Filter2D.MmpxShader)
    {
        GLuint shader {};
        if (!OpenGL::CompileVertexFragmentProgram(shader, kHiresCaptureVS, kFilter2DMmpxFS,
                "Filter2DMmpxShader", {}, {{"oTexel", 0}}))
        {
            Filter2D.Failed = true;
            return false;
        }

        Filter2D.MmpxShader = shader;
        Filter2D.MmpxSrcWLoc = glGetUniformLocation(shader, "uSrcW");
        Filter2D.MmpxScreenHLoc = glGetUniformLocation(shader, "uScreenH");
        Filter2D.MmpxIdentityLoc = glGetUniformLocation(shader, "uIdentity");
        glUseProgram(shader);
        glUniform1i(glGetUniformLocation(shader, "SrcTex"), Filter2DMapUnit);
    }

    Filter2DProgram& comp = Filter2D.Comp[wide];
    if (!comp.Program)
    {
        // which has to come after the #version line, as in PrepareWide
        std::string vs = kCompositorVS;
        std::string fs = CompositorFS();
        if (wide)
            vs.insert(vs.find('\n') + 1, "#define WIDE\n");
        fs.insert(fs.find('\n') + 1, wide ? "#define WIDE\n#define FILTER2D\n" : "#define FILTER2D\n");

        GLuint shader {};
        if (!OpenGL::CompileVertexFragmentProgram(shader, vs, fs,
                wide ? "CompositorShaderWideFilter2D" : "CompositorShaderFilter2D",
                {{"vPosition", 0}, {"vTexcoord", 1}},
                {{"oColor", 0}}))
        {
            Filter2D.Failed = true;
            return false;
        }

        comp.Program = shader;
        comp.ScaleLoc = glGetUniformLocation(shader, "u3DScale");
        comp.RatioLoc = glGetUniformLocation(shader, "uWideRatio");
        comp.OffsetLoc = glGetUniformLocation(shader, "uWideOffset");
        comp.MarginLoc = glGetUniformLocation(shader, "uWideMargin");
        comp.HiresCapLoc = glGetUniformLocation(shader, "uHiresCap");
        comp.MapScaleLoc = glGetUniformLocation(shader, "uFilterMapScale");
        comp.IdentityLoc = glGetUniformLocation(shader, "uFilterIdentity");
        comp.RowOffsetLoc = glGetUniformLocation(shader, "uFilterRowOffset");

        glUseProgram(shader);
        glUniform1i(glGetUniformLocation(shader, "ScreenTex"), 0);
        glUniform1i(glGetUniformLocation(shader, "_3DTex"), 1);
        glUniform1i(glGetUniformLocation(shader, "HeldWingsTex"), 2);
        glUniform1i(glGetUniformLocation(shader, "WingPlanesTex"), WingPlanesUnit);
        glUniform1i(glGetUniformLocation(shader, "HiresBankTex"), HiresBankUnit);
        glUniform1i(glGetUniformLocation(shader, "HiresStreamTex"), HiresStreamUnit);
        glUniform1i(glGetUniformLocation(shader, "FilterMapTex"), Filter2DMapUnit);
    }

    return true;
}

bool GLCompositor::MakeFilter2DTarget(GLuint& texture, GLuint& fb, GLenum format, int width, int height) noexcept
{
    texture = MakeHiresTexture(format, width, height);
    glGenFramebuffers(1, &fb);

    GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
    glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
    glDrawBuffers(1, fbassign);
    return glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// filtered 2D: the key the pre-pass (kFilter2DKeyFS) gives each native pixel, from the same planes
// and control words it reads, into Filter2D.Keys. returns how many lines have a key that changed,
// and marks them in Filter2D.LineChanged.
// these keys aren't the shader's, but two of them are equal exactly when the shader's are: the
// pixel's 6-bit color, with bit 31 for a 3D slot, and on a line that stays as it is one value no
// color has (the shader's key there is the pixel's position, the same every frame). the passes
// only read their keys, so a map made from equal keys by the same passes is the one they'd make
int GLCompositor::UpdateFilter2DKeys(const GPU& gpu, int backbuf, bool hiresDisplay, bool ctrlCopied) noexcept
{
    constexpr u32 colorMask = 0x3F3F3F;
    constexpr u32 slot3DKey = 1u << 31;
    constexpr u32 stayKey = 1u << 30;

    Filter2D.Keys.resize(256 * 384);
    int changedLines = 0;
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            int outLine = half * 192 + line;
            const u32* planes = &fb[line * (256*3 + 1)];
            u32 ctrl = ctrlCopied ? CtrlWords[outLine] : planes[256*3];
            u32 dispmode = (ctrl >> 16) & 0x3;
            u32* keys = &Filter2D.Keys[outLine * 256];

            u32 diff = 0;
            if (dispmode >= 2 || (hiresDisplay && (ctrl & HiresDisplayFlag)))
            {
                for (int i = 0; i < 256; i++)
                {
                    diff |= keys[i] ^ stayKey;
                    keys[i] = stayKey;
                }
            }
            else if (dispmode == 0)
            {
                // dispmode 0 only reads plane 0
                for (int i = 0; i < 256; i++)
                {
                    u32 key = planes[i] & colorMask;
                    diff |= keys[i] ^ key;
                    keys[i] = key;
                }
            }
            else
            {
                // plane 1 for the compmodes that show 3D (0-4), plane 0 for the others. both are
                // read either way, so the compiler can vectorize the loop
                for (int i = 0; i < 256; i++)
                {
                    u32 key2D = planes[i] & colorMask;
                    u32 key3D = (planes[256 + i] & colorMask) | slot3DKey;
                    u32 key = ((planes[512 + i] >> 24) & 0xF) <= 4 ? key3D : key2D;
                    diff |= keys[i] ^ key;
                    keys[i] = key;
                }
            }

            Filter2D.LineChanged[outLine] = diff != 0;
            if (diff)
                changedLines++;
        }
    }

    return changedLines;
}

// GPU::Filter2DRows: the rows of a pass's target that can change when the rows [src] of the target it
// reads did (runs in order, none overlapping). an MMPX texel at row y reads source rows y/2 - 3 to
// y/2 + 3 (src()'s offsets in kFilter2DMmpxFS), clamped to its screen's rows, so with source rows
// a-b changed, the rows that can change are those whose y/2 is in a-3 to b+3, kept in the screens
// a and b are in. [screenH] is a screen's rows in the source. runs that touch are merged, also over
// the gap between the screens: redrawing a row that doesn't change only costs the time
static void WidenFilter2DRuns(const std::vector<GLCompositor::Filter2DRowRun>& src, int screenH,
                              std::vector<GLCompositor::Filter2DRowRun>& dst) noexcept
{
    dst.clear();
    for (const GLCompositor::Filter2DRowRun& run : src)
    {
        int screenFirst = run.First / screenH * screenH;
        int screenLast = run.Last / screenH * screenH + screenH - 1;
        int first = 2 * std::max(run.First - 3, screenFirst);
        int last = 2 * std::min(run.Last + 3, screenLast) + 1;
        if (!dst.empty() && first <= dst.back().Last + 1)
            dst.back().Last = std::max(dst.back().Last, last);
        else
            dst.push_back({first, last});
    }
}

// filtered 2D: fills the map the compositor reads planes 0-2 through, from the planes and control
// words just uploaded. a key per native pixel, then MMPX to 2x, then at scale 4 and up to 4x. all
// of it is the frame's own data, so a full redraw doesn't need what a target held before, and
// tiled GPUs are told so. each pass leaves its texture for the next one on the map's unit, and the
// last leaves the map. RenderFrame's state is back afterwards except for the program, which it sets.
// most frames of a 2D scene have the keys of the one before, and then the map from the last run
// is bound as it is. with GPU::Filter2DRows, a frame whose keys changed on some lines redraws only
// the rows those can reach and keeps the rest, which the same passes made from the same keys
void GLCompositor::RunFilter2D(const GPU& gpu, int backbuf, bool hiresDisplay, bool ctrlCopied, int passes) noexcept
{
    auto nowNs = []
    {
        return (s64) std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    s64 checkStart = gpu.Filter2DLogging ? nowNs() : 0;
    int changedLines = UpdateFilter2DKeys(gpu, backbuf, hiresDisplay, ctrlCopied);
    bool identity = gpu.Filter2D == 3;
    // the targets hold what these passes made from the last keys
    bool mapValid = Filter2D.MapPasses == passes && Filter2D.MapIdentity == identity;
    bool reuse = changedLines == 0 && mapValid;
    // GPU::Filter2DRows (experiment K3): otherwise only the rows the changed lines reach are redrawn,
    // over targets that hold the rest. everything is redrawn when they don't hold this mode's map
    bool full = !mapValid || gpu.Filter2DRows == 0;

    // the rows each pass draws: a key per native pixel, so the changed lines; then each MMPX pass
    // widens them (WidenFilter2DRuns)
    std::array<std::vector<Filter2DRowRun>, 3>& runs = Filter2D.Runs;
    if (!reuse)
    {
        for (std::vector<Filter2DRowRun>& passRuns : runs)
            passRuns.clear();
        if (full)
        {
            runs[0].push_back({0, 383});
            runs[1].push_back({0, 767});
            if (passes == 2)
                runs[2].push_back({0, 1535});
        }
        else
        {
            for (int line = 0; line < 384; line++)
            {
                if (!Filter2D.LineChanged[line])
                    continue;
                if (!runs[0].empty() && runs[0].back().Last == line - 1)
                    runs[0].back().Last = line;
                else
                    runs[0].push_back({line, line});
            }
            WidenFilter2DRuns(runs[0], 192, runs[1]);
            if (passes == 2)
                WidenFilter2DRuns(runs[1], 384, runs[2]);
        }
    }

    if (gpu.Filter2DLogging)
    {
        s64 checkNs = nowNs() - checkStart;
        Filter2D.LogCheckNs += checkNs;
        Filter2D.LogCheckMaxNs = std::max(Filter2D.LogCheckMaxNs, checkNs);
        Filter2D.LogChangedLines += changedLines;
        if (reuse)
        {
            Filter2D.LogSkipped++;
        }
        else
        {
            Filter2D.LogRan++;
            if (!full)
                Filter2D.LogPartial++;
            for (int pass = 0; pass < 3; pass++)
            {
                for (const Filter2DRowRun& run : runs[pass])
                    Filter2D.LogRows[pass] += run.Last - run.First + 1;
            }
        }
    }

    if (reuse)
    {
        // the unit may have been used since
        glActiveTexture(GL_TEXTURE0 + Filter2DMapUnit);
        glBindTexture(GL_TEXTURE_2D, Filter2D.MapTex);
        glActiveTexture(GL_TEXTURE0);
        return;
    }

    BeginGPUTimer(ComputePass::CompFilter);

    // kHiresCaptureVS needs no attributes, but the VAO may be the 3D renderer's
    glBindVertexArray(CompVertexArrayID);

    DrawFilter2DPasses(hiresDisplay, identity, passes, full);
    if (!full && gpu.Filter2DRows == 2 && (Filter2D.RowCheckRuns++ % 30) == 0)
        CheckFilter2DRows(hiresDisplay, identity, passes);

    glBindTexture(GL_TEXTURE_2D, Filter2D.MapTex);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
    glViewport(0, 0, ScreenW, ScreenH);

    Filter2D.MapPasses = passes;
    Filter2D.MapIdentity = identity;
}

// RunFilter2D's passes, over the rows in Filter2D.Runs. a full redraw covers every texel of each
// target, so tiled GPUs are told their old contents don't matter; a partial one keeps them, as it
// only draws the rows that can change, scissored to them. leaves the map's unit active, with the
// last pass's source on it
void GLCompositor::DrawFilter2DPasses(bool hiresDisplay, bool identity, int passes, bool full) noexcept
{
    const GLenum attachment = GL_COLOR_ATTACHMENT0;
    GLboolean scissor = GL_FALSE;
    GLint box[4] {};
    if (!full)
    {
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_SCISSOR_BOX, box);
        if (!scissor)
            glEnable(GL_SCISSOR_TEST);
    }
    auto draw = [&](int pass, int width)
    {
        if (full)
        {
            glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            return;
        }
        for (const Filter2DRowRun& run : Filter2D.Runs[pass])
        {
            glScissor(0, run.First, width, run.Last - run.First + 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    };

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Filter2D.KeyFB);
    glViewport(0, 0, 256, 384);
    glUseProgram(Filter2D.KeyShader);
    glUniform1i(Filter2D.KeyHiresCapLoc, hiresDisplay);
    draw(0, 256);

    glUseProgram(Filter2D.MmpxShader);
    glUniform1i(Filter2D.MmpxIdentityLoc, identity);
    glActiveTexture(GL_TEXTURE0 + Filter2DMapUnit);

    glBindTexture(GL_TEXTURE_2D, Filter2D.KeyTex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, passes == 2 ? Filter2D.Map2xFB : Filter2D.MapFB);
    glViewport(0, 0, 512, 768);
    glUniform1i(Filter2D.MmpxSrcWLoc, 256);
    glUniform1i(Filter2D.MmpxScreenHLoc, 192);
    draw(1, 512);

    if (passes == 2)
    {
        glBindTexture(GL_TEXTURE_2D, Filter2D.Map2xTex);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Filter2D.MapFB);
        glViewport(0, 0, 1024, 1536);
        glUniform1i(Filter2D.MmpxSrcWLoc, 512);
        glUniform1i(Filter2D.MmpxScreenHLoc, 384);
        draw(2, 1024);
    }

    if (!full)
    {
        glScissor(box[0], box[1], box[2], box[3]);
        if (!scissor)
            glDisable(GL_SCISSOR_TEST);
    }
}

// GPU::Filter2DRows 2: right after a partial redraw, reads back every target, draws them all again in
// full, reads them back again and counts the texels that differ (the FILTER2D line's rowmismatches),
// logging the first few. the full draw is the map as it should be, so the frame shows that one
void GLCompositor::CheckFilter2DRows(bool hiresDisplay, bool identity, int passes) noexcept
{
    struct Target { GLuint FB; int W, H; };
    std::array<Target, 3> targets {{
        {Filter2D.KeyFB, 256, 384},
        {passes == 2 ? Filter2D.Map2xFB : Filter2D.MapFB, 512, 768},
        {Filter2D.MapFB, 1024, 1536},
    }};
    auto read = [&](std::array<std::vector<u32>, 3>& out)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (int pass = 0; pass < (passes == 2 ? 3 : 2); pass++)
        {
            const Target& t = targets[pass];
            out[pass].resize((size_t)t.W * t.H * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, t.FB);
            glReadPixels(0, 0, t.W, t.H, GL_RGBA_INTEGER, GL_UNSIGNED_INT, out[pass].data());
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    };

    std::array<std::vector<u32>, 3> partial, full;
    read(partial);
    DrawFilter2DPasses(hiresDisplay, identity, passes, true);
    read(full);

    u32 mismatches = 0, logged = 0;
    for (int pass = 0; pass < (passes == 2 ? 3 : 2); pass++)
    {
        const Target& t = targets[pass];
        for (int y = 0; y < t.H; y++)
        {
            for (int x = 0; x < t.W; x++)
            {
                size_t i = ((size_t)y * t.W + x) * 4;
                if (partial[pass][i] == full[pass][i] && partial[pass][i + 1] == full[pass][i + 1])
                    continue;
                mismatches++;
                if (logged < 8 && Filter2D.LogRowMismatches + mismatches <= 64)
                {
                    logged++;
                    Log(LogLevel::Info, "FILTER2D rowcheck mismatch pass=%d x=%d y=%d partial=%08X,%08X full=%08X,%08X\n",
                        pass, x, y, partial[pass][i], partial[pass][i + 1], full[pass][i], full[pass][i + 1]);
                }
            }
        }
    }
    Filter2D.LogRowChecks++;
    Filter2D.LogRowMismatches += mismatches;
}

void GLCompositor::FreeFilter2DTextures() noexcept
{
    if (!Filter2D.KeyTex)
        return;

    glDeleteFramebuffers(1, &Filter2D.KeyFB);
    glDeleteFramebuffers(1, &Filter2D.Map2xFB);
    glDeleteFramebuffers(1, &Filter2D.MapFB);
    glDeleteTextures(1, &Filter2D.KeyTex);
    glDeleteTextures(1, &Filter2D.Map2xTex);
    glDeleteTextures(1, &Filter2D.MapTex);
    Filter2D.KeyTex = Filter2D.KeyFB = 0;
    Filter2D.Map2xTex = Filter2D.Map2xFB = 0;
    Filter2D.MapTex = Filter2D.MapFB = 0;
    Filter2D.MapScale = 0;
    Filter2D.MapPasses = 0;
    Filter2D.Keys = std::vector<u32>();
}

void GLCompositor::FreeFilter2D() noexcept
{
    FreeFilter2DTextures();
    glDeleteProgram(Filter2D.KeyShader);
    glDeleteProgram(Filter2D.MmpxShader);
    for (Filter2DProgram& comp : Filter2D.Comp)
        glDeleteProgram(comp.Program);
    for (std::array<Down3DProgram, 4>& view : Filter2D.Down3D)
    {
        for (Down3DProgram& program : view)
            glDeleteProgram(program.Program);
    }
    Filter2D.KeyShader = 0;
    Filter2D.MmpxShader = 0;
    Filter2D.Comp = {};
    Filter2D.Down3D = {};
}

// debug only (GPU::Filter2DLogging): the mode, the scale and how many passes frames run, once a second.
// of the frames, ran made the map and skipped reused it. changedlines is how many lines' keys changed
// and checkus how long UpdateFilter2DKeys took, on average over those two, and checkmaxus its longest.
// rows is GPU::Filter2DRows, partial how many of the frames that ran redrew only some rows, passrows
// the rows each pass drew (key, first MMPX pass, second; of 384, 768 and 1536) on average over the
// frames that ran, and rowchecks and rowmismatches the Filter2DRows 2 checks and the texels they found
// differ
void GLCompositor::LogFilter2D(const GPU& gpu, int passes) noexcept
{
    s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (Filter2D.LogStartNs == 0)
        Filter2D.LogStartNs = now;
    Filter2D.LogFrames++;
    if (now - Filter2D.LogStartNs < 1000000000)
        return;

    u32 checked = Filter2D.LogRan + Filter2D.LogSkipped;
    double perChecked = checked ? 1.0 / checked : 0.0;
    double perRan = Filter2D.LogRan ? 1.0 / Filter2D.LogRan : 0.0;
    Log(LogLevel::Info, "FILTER2D mode=%d scale=%d passes=%d mapscale=%d frames=%u ran=%u skipped=%u "
        "changedlines=%.1f checkus=%.1f checkmaxus=%.1f failed=%d rows=%d partial=%u passrows=%.1f/%.1f/%.1f "
        "rowchecks=%u rowmismatches=%u\n",
        gpu.Filter2D, Scale, passes, Filter2D.MapScale, Filter2D.LogFrames, Filter2D.LogRan, Filter2D.LogSkipped,
        Filter2D.LogChangedLines * perChecked, Filter2D.LogCheckNs * perChecked / 1000.0,
        Filter2D.LogCheckMaxNs / 1000.0, Filter2D.Failed ? 1 : 0, gpu.Filter2DRows, Filter2D.LogPartial,
        Filter2D.LogRows[0] * perRan, Filter2D.LogRows[1] * perRan, Filter2D.LogRows[2] * perRan,
        Filter2D.LogRowChecks, Filter2D.LogRowMismatches);

    Filter2D.LogStartNs = now;
    Filter2D.LogFrames = 0;
    Filter2D.LogRan = Filter2D.LogSkipped = Filter2D.LogChangedLines = 0;
    Filter2D.LogCheckNs = Filter2D.LogCheckMaxNs = 0;
    Filter2D.LogPartial = 0;
    Filter2D.LogRows = {};
    Filter2D.LogRowChecks = Filter2D.LogRowMismatches = 0;
}

// hi-res display capture: decides which VRAM display lines show a hi-res bank copy. the 2D
// renderer flags the candidates (control word bit 7, the bank in bits 5-6), and the flag is
// cleared on the lines that fail any of:
// - the bank's copy is valid and covers the line's row (VRAM display line L shows bank row L);
// - no earlier capture wrote the bank without a pass redoing it (the record's StaleMask);
// - this frame doesn't capture into the bank at a non-zero offset. at offset 0, display line L
//   reads row L before capture line L writes it, so it sees the last pass's copy, as native does.
// one copy is bound (unit 3), the bank of the first candidate line: other banks stay native.
// bits 5-7 are cleared on every other line too, in case stale ones got through. bits 5-6 are
// cleared on the lines that stay too, as the shader reads bit 5 as the stream flag.
// regular display lines can then show a hi-res image as well (see PrepareHiresStream).
// a game with 3D on both screens can send one captured image to a screen twice in a row: by VRAM
// display, then, once the screens swapped, on unit B some way the stream can't see (a scaled BG,
// say). if regular display lines don't match a stream, the VRAM display lines of a frame that
// swapped stay native too, or a screen would alternate between hi-res and native. a frame whose
// regular display lines are all one color says nothing, so it goes by the last one that did: with
// one screen black, unit B has nothing to match every other frame, and the other screen still
// needs the same answer on both. without a swap the other frame shows that screen the same way,
// so there's no reason to give the hi-res image up.
// returns whether any line shows a hi-res image, and then CtrlWords is what the shader gets
bool GLCompositor::PrepareHiresDisplay(const GPU& gpu, int backbuf, bool ctrlCopied) noexcept
{
    HiresCap.ShowBank = -1;
    HiresCap.StreamSource = -1;
    // without the pass no copy is valid (they're dropped on a scale change), and the 2D renderer
    // flags no lines (GPU::Start2DJob)
    if (!gpu.HiresCapture || !RunsHiresCapture(gpu.HiresCaptureDebug))
        return false;

    const HiresCaptureRecord* record = gpu.HiresCapRecord.get();
    u8 stale = record ? record->StaleMask : gpu.NativeCapturedSincePass;
    u8 offsetCapture = record ? record->OffsetMask : 0;

    // debug only, see GPU_CaptureReads.h: the banks whose lines show the copy or stay native
    u8 hiresBanks = 0, nativeBanks = 0;

    int firstBank = -1;
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            int outLine = half * 192 + line;
            u32& ctrl = CtrlWords[outLine];
            if (!ctrlCopied)
                ctrl = fb[line * (256*3 + 1) + 256*3];

            if (!(ctrl & HiresDisplayFlag))
            {
                ctrl &= ~HiresDisplayBits;
                continue;
            }

            HiresCap.LinesCandidate++;
            int bank = (ctrl >> 5) & 0x3;
            if (firstBank < 0)
                firstBank = bank;

            if (bank == firstBank && HiresCap.BankTex[bank] && HiresCap.Banks[bank].Covers(line) &&
                !(stale & (1 << bank)) && !(offsetCapture & (1 << bank)))
            {
                HiresCap.ShowBank = bank;
                HiresCap.LinesShown++;
                ctrl &= ~(HiresDisplayBits & ~HiresDisplayFlag);
                hiresBanks |= 1 << bank;
            }
            else
            {
                ctrl &= ~HiresDisplayBits;
                nativeBanks |= 1 << bank;
            }
        }
    }

    PrepareHiresStream(gpu, backbuf);

    bool swapped = HiresCap.LastScreenSwap >= 0 && HiresCap.LastScreenSwap != (int)gpu.ScreenSwap;
    HiresCap.LastScreenSwap = gpu.ScreenSwap;
    if (swapped && HiresCap.ShowBank >= 0 && HiresCap.StreamMissed)
    {
        for (u32& ctrl : CtrlWords)
        {
            if (ctrl & HiresDisplayFlag)
            {
                ctrl &= ~HiresDisplayBits;
                HiresCap.LinesShown--;
                HiresCap.LinesSwapNative++;
            }
        }
        HiresCap.ShowBank = -1;
        nativeBanks |= hiresBanks;
        hiresBanks = 0;
    }

    if (gpu.CapReads)
        gpu.CapReads->NoteHiresDisplay(hiresBanks, nativeBanks);

    return HiresCap.ShowBank >= 0 || HiresCap.StreamSource >= 0;
}

// hi-res display capture, regular display lines: a line that's byte for byte a row of a capture
// shows that capture's hi-res image, however the pixels got there (see GPU_HiresStream.h). what
// matches is one image per frame, the one most lines match, its row L on line L only, and the lines
// that don't stay native.
// the images are the banks' copies and the scratch one, the capture before them: as the pass runs
// after the draw, frame F sees captures F-1 and F-2 of a bank that's captured into every frame.
// CtrlWords has the frame's control words, with bits 5-7 already rewritten for VRAM display
void GLCompositor::PrepareHiresStream(const GPU& gpu, int backbuf) noexcept
{
    auto source = [](const HiresNativeRows& native, GLuint tex) -> HiresStream::Source
    {
        if (!tex || !native.State.Valid)
            return {};
        return {native.Words.data(), native.State.RowStart, native.State.Rows};
    };

    HiresStream::Source sources[5];
    for (int i = 0; i < 4; i++)
        sources[i] = source(HiresCap.BankNative[i], HiresCap.BankTex[i]);
    sources[HiresStreamScratch] = source(HiresCap.ScratchNative, HiresCap.ScratchTex);

    const u32* const screens[2] = {gpu.Framebuffer[backbuf][0].get(), gpu.Framebuffer[backbuf][1].get()};
    bool anyDetail;
    int found = HiresStream::FindSource(screens, sources, 5, anyDetail);
    if (found < 0)
    {
        if (anyDetail)
            HiresCap.StreamMissed = true;
        return;
    }

    HiresCap.StreamMissed = false;
    HiresCap.StreamSource = found;
    for (int half = 0; half < 2; half++)
    {
        for (int line = 0; line < 192; line++)
        {
            const u32* fbline = &screens[half][line * HiresStream::LineWords];
            if (HiresStream::IsRegularDisplay(fbline) && sources[found].MatchesLine(fbline, line))
            {
                CtrlWords[half * 192 + line] |= HiresDisplayFlag | HiresStreamBit;
                HiresCap.LinesStream++;
            }
        }
    }
}

// the capture pass: redoes this frame's display capture at the 3D scale, from the record the 2D
// renderer made (GPU::HiresCapRecord), the hi-res 3D frame (still bound on unit 1 by
// SetupAccelFrame: the next 3D frame only replaces it at line 215, after this in GL command order,
// or with GPU::BlitAfterReadback draws into the other color buffer while this reads the kept one)
// and the source B copy, into the destination bank's copy. the copies of the banks this frame
// captured into are dropped first, so a capture that can't be redone leaves its destination native
void GLCompositor::RunHiresCapture(const GPU& gpu, int backbuf) noexcept
{
    const HiresCaptureRecord* record = gpu.HiresCapRecord.get();
    if (!record || !record->DstMask)
        return;

    HiresCap.Captures++;

    u32 skip = record->Skip;
    int dst = (record->Layout >> 16) & 0x3;
    int dstRow = ((record->Layout >> 18) & 0x3) * 64;
    int size = (record->Layout >> 20) & 0x3;
    int rows = size * 64;

    if (size == 0)
        skip |= HiresCapSkip_Size;
    else if (dstRow + rows > 256)
        skip |= HiresCapSkip_Wrap;
    else
    {
        for (int line = 0; line < rows; line++)
        {
            if (!record->HasLine(line))
                skip |= HiresCapSkip_Missing;
        }
    }

    if (gpu.HiresCapWritesInCapture & (1 << dst))
        skip |= HiresCapSkip_Written;
    // the check mode runs the pass at 1x too, to compare it with the native capture. GPU records
    // nothing at 1x otherwise (RunsHiresCapture), so this only catches lines held across a change
    if (Scale == 1 && gpu.HiresCaptureDebug < 2)
        skip |= HiresCapSkip_Native1x;
    // GPU::OutputCap: a capped output's pass reads the 3D at its own scale, in the main draw's mode
    // (or the exact one's if that mode's program didn't build)
    int capMode = Down3DVariant();
    if (!skip && Scale3D != Scale)
    {
        if (!PrepareHiresCaptureDown3D(capMode))
            capMode = 0;
        if (capMode == 0 && !PrepareHiresCaptureDown3D(0))
            skip |= HiresCapSkip_Shader;
    }
    else if (!skip && !PrepareHiresCaptureShader())
        skip |= HiresCapSkip_Shader;

    // where each line's source B comes from, decided with the copies as they were before this
    // pass: the source bank's copy if it's valid over the row read, no earlier capture wrote the
    // bank without a pass, and, for the destination bank itself, the row read is the row written
    // (capture line L reads it before writing it, so it reads the last pass's copy). one source B
    // copy is bound, the first one a line can use. otherwise the native words, and always those
    // for the display FIFO
    int bankB = -1;
    bool anyPlanes = false, anyNativeB = false;
    u8 hiresBanksB = 0, nativeBanksB = 0; // debug only, see GPU_CaptureReads.h
    if (!skip)
    {
        for (int line = 0; line < rows; line++)
        {
            const HiresCaptureRecord::Line& entry = record->Lines[line];
            u32 capcnt = entry.CaptureCnt;
            u32 mode = (capcnt >> 29) & 0x3;

            bool hiresB = false;
            if (mode != 0 && entry.SrcBPresent)
            {
                int bank = entry.SrcBBank;
                int row = entry.SrcBRow;
                hiresB = !entry.SrcBFIFO && HiresCap.BankTex[bank] && HiresCap.Banks[bank].Covers(row) &&
                         !(record->StaleMask & (1 << bank)) && (bank != dst || row == dstRow + line) &&
                         (bankB < 0 || bank == bankB);
                if (hiresB)
                {
                    bankB = bank;
                    HiresCap.LinesHiresB++;
                    hiresBanksB |= 1 << bank;
                }
                else
                {
                    anyNativeB = true;
                    HiresCap.LinesNativeB++;
                    nativeBanksB |= 1 << bank;
                }
            }
            if (mode != 1 && !(capcnt & (1 << 24)))
                anyPlanes = true;

            HiresCapCtrl[line * 2] = capcnt;
            HiresCapCtrl[line * 2 + 1] = (entry.RenderXPos & 0x1FF) | (hiresB << 9) |
                                         (entry.SrcBPresent << 10) | (entry.SrcBRow << 16);
        }

        if (gpu.CapReads)
            gpu.CapReads->NoteHiresSrcB(hiresBanksB, nativeBanksB);
    }

    for (int bank = 0; bank < 4; bank++)
    {
        if (record->DstMask & (1 << bank))
            HiresCap.Banks[bank].Valid = false;
    }

    if (skip)
    {
        for (int i = 0; i < HiresCapSkipCount; i++)
        {
            if (skip & (1 << i))
                HiresCap.Skips[i]++;
        }
        return;
    }

    BeginGPUTimer(ComputePass::CompHiCap);
    if (!HiresCap.PlanesTex)
        HiresCap.PlanesTex = MakeHiresTexture(GL_RGBA8UI, 256*3 + 2, 192);
    if (!HiresCap.SrcBTex)
        HiresCap.SrcBTex = MakeHiresTexture(GL_R16UI, 256, 192);
    if (!HiresCap.ScratchTex)
        HiresCap.ScratchTex = MakeHiresTexture(GL_R16UI, 256 * Scale, 256 * Scale);
    if (!HiresCap.PassFB)
        glGenFramebuffers(1, &HiresCap.PassFB);

    // only the captured lines, and the planes and source B words only when a line reads them
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, HiresCap.PlanesTex);
    if (anyPlanes)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256*3, rows, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, record->Planes);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 256*3, 0, 2, rows, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, HiresCapCtrl.data());

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, HiresCap.SrcBTex);
    if (anyNativeB)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, rows, GL_RED_INTEGER, GL_UNSIGNED_SHORT, record->SrcB);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, bankB >= 0 ? HiresCap.BankTex[bankB] : 0);
    // units 3 and 8 can still have the scratch texture from an earlier frame's draw. the capture
    // program doesn't read them, but no texture the pass draws into stays bound
    glActiveTexture(GL_TEXTURE0 + HiresBankUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + HiresStreamUnit);
    glBindTexture(GL_TEXTURE_2D, 0);

    // the captured rows only, with the viewport: classic GL leaves the scissor test off and
    // calls glScissor assuming it is
    GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, HiresCap.PassFB);
    glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, HiresCap.ScratchTex, 0);
    glDrawBuffers(1, fbassign);
    glViewport(0, dstRow * Scale, 256 * Scale, rows * Scale);

    if (Scale3D != Scale)
    {
        const HiresCaptureState::Down3DShader& shader = HiresCap.Down3D[capMode];
        glUseProgram(shader.Program);
        glUniform1i(shader.ScaleLoc, Scale);
        glUniform1i(shader.WideOffsetLoc, (ViewWidth - 256) / 2);
        glUniform1i(shader.WideMarginLoc, Margin);
        glUniform1i(shader.DstRowLoc, dstRow);
        int ratioP, ratioQ;
        Down3DRatio(ratioP, ratioQ);
        glUniform2i(shader.RatioLoc, ratioP, ratioQ);
        // GPU::OutputCap3D 1 and 3's bilinear taps on the 3D's unit, for this pass only, as in the main draw
        bool linear = capMode == 1 || capMode == 3;
        if (linear)
            glBindSampler(1, Down3DSampler);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (linear)
            glBindSampler(1, 0);
    }
    else
    {
        glUseProgram(HiresCap.Shader);
        glUniform1i(HiresCap.ScaleLoc, Scale);
        glUniform1i(HiresCap.WideOffsetLoc, (ViewWidth - 256) / 2);
        glUniform1i(HiresCap.WideMarginLoc, Margin);
        glUniform1i(HiresCap.DstRowLoc, dstRow);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // the destination's old image becomes the scratch one, and stays usable for the next frame's
    // stream with the rows it covered, until the next pass draws over it. the native capture is
    // finished: every capture line is drawn before the Blit (GPU::StartScanline at line 192), and
    // a write to the bank during it skipped this pass (HiresCapSkip_Written). a Blit that waited
    // for line 215 (GPU::BlitAfterReadback) reads the bank as line 192 left it
    const u8* dstNative = record->DstNativeKept ? (const u8*)record->DstNative : gpu.VRAM[dst];
    std::swap(HiresCap.BankTex[dst], HiresCap.ScratchTex);
    std::swap(HiresCap.BankNative[dst], HiresCap.ScratchNative);
    HiresNativeRows& native = HiresCap.BankNative[dst];
    native.State = {true, dstRow, rows};
    native.Words.resize(256 * 192);
    if (dstRow < 192)
        memcpy(&native.Words[dstRow * 256], &dstNative[dstRow * 512],
               (std::min(dstRow + rows, 192) - dstRow) * 512);

    HiresCap.Banks[dst] = {true, dstRow, rows};
    HiresCap.Passes++;

#ifdef MELONADE_DEBUG_TOOLS
    // not while capped (GPU::OutputCap): the copy is then made from the 3D at its own scale, which
    // the native capture never matches
    if (gpu.HiresCaptureDebug >= 2 && Scale == 1 && Scale3D == Scale && (HiresCap.CheckCount++ % 30) == 0)
        CheckHiresCapture(dstNative, dst, dstRow, rows);
#endif

    // back to what RenderFrame leaves bound, like CopyHeldWings. the viewport too, which
    // CopyHeldWings never changes
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
    glViewport(0, 0, ScreenW, ScreenH);
    glActiveTexture(GL_TEXTURE1);
}

bool GLCompositor::PrepareHiresCaptureShader() noexcept
{
    if (HiresCap.Shader || HiresCap.ShaderFailed)
        return HiresCap.Shader != 0;

    GLuint shader {};
    if (!OpenGL::CompileVertexFragmentProgram(shader,
            kHiresCaptureVS, HiresCaptureFS(),
            "HiresCaptureShader",
            {},
            {{"oWord", 0}}))
    {
        HiresCap.ShaderFailed = true;
        return false;
    }

    HiresCap.Shader = shader;
    HiresCap.ScaleLoc = glGetUniformLocation(shader, "uScale");
    HiresCap.WideOffsetLoc = glGetUniformLocation(shader, "uWideOffset");
    HiresCap.WideMarginLoc = glGetUniformLocation(shader, "uWideMargin");
    HiresCap.DstRowLoc = glGetUniformLocation(shader, "uDstRow");

    glUseProgram(shader);
    glUniform1i(glGetUniformLocation(shader, "PlanesTex"), 4);
    glUniform1i(glGetUniformLocation(shader, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(shader, "SrcBTex"), 5);
    glUniform1i(glGetUniformLocation(shader, "BankBTex"), 6);
    return true;
}

// GPU::OutputCap: the capped pass's program in GPU::OutputCap3D mode [mode], tried once. the bound
// program is put back, as it can be built outside the pass (PrepareCapped)
bool GLCompositor::PrepareHiresCaptureDown3D(int mode) noexcept
{
    HiresCaptureState::Down3DShader& shader = HiresCap.Down3D[mode];
    if (shader.Program || shader.Failed)
        return shader.Program != 0;

    GLuint program {};
    if (!OpenGL::CompileVertexFragmentProgram(program,
            kHiresCaptureVS, HiresCaptureDown3DFS(mode),
            "HiresCaptureShaderDown3D",
            {},
            {{"oWord", 0}}))
    {
        Log(LogLevel::Warn, "compositor: the capped hi-res capture program (mode %d) didn't build%s\n", mode,
            mode == 0 ? ", capped captures stay native" : "");
        shader.Failed = true;
        return false;
    }

    shader.Program = program;
    shader.ScaleLoc = glGetUniformLocation(program, "uScale");
    shader.WideOffsetLoc = glGetUniformLocation(program, "uWideOffset");
    shader.WideMarginLoc = glGetUniformLocation(program, "uWideMargin");
    shader.DstRowLoc = glGetUniformLocation(program, "uDstRow");
    shader.RatioLoc = glGetUniformLocation(program, "u3DRatio");

    GLint bound = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "PlanesTex"), 4);
    glUniform1i(glGetUniformLocation(program, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(program, "SrcBTex"), 5);
    glUniform1i(glGetUniformLocation(program, "BankBTex"), 6);
    glUseProgram((GLuint)bound);

    if (mode == 1 || mode == 3)
        MakeDown3DSampler();
    return true;
}

// an integer texture for the capture pass, made on unit 7 so no binding the frame uses changes
GLuint GLCompositor::MakeHiresTexture(GLenum format, int width, int height) noexcept
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

// the bank copies and the scratch texture are at the scale, so a scale change frees them
void GLCompositor::FreeHiresBanks() noexcept
{
    glDeleteTextures(HiresCap.BankTex.size(), HiresCap.BankTex.data());
    glDeleteTextures(1, &HiresCap.ScratchTex);
    HiresCap.BankTex = {};
    HiresCap.ScratchTex = 0;
    HiresCap.BankNative = {};
    HiresCap.ScratchNative = {};
    ResetHiresCapture();
}

void GLCompositor::FreeHiresCapture() noexcept
{
    FreeHiresBanks();
    glDeleteTextures(1, &HiresCap.PlanesTex);
    glDeleteTextures(1, &HiresCap.SrcBTex);
    glDeleteFramebuffers(1, &HiresCap.PassFB);
    glDeleteProgram(HiresCap.Shader);
    for (HiresCaptureState::Down3DShader& shader : HiresCap.Down3D)
        glDeleteProgram(shader.Program);
    HiresCap.PlanesTex = 0;
    HiresCap.SrcBTex = 0;
    HiresCap.PassFB = 0;
    HiresCap.Shader = 0;
    HiresCap.Down3D = {};
}

// debug only (GPU::HiresCaptureDebug): once a second, how many captures there were and passes
// ran, how many VRAM display lines were candidates and showed a copy, how many regular display
// lines showed a stream image, how many VRAM display lines a screen swap kept native, how many
// capture lines took source B from a copy or the native words, and why captures were skipped
void GLCompositor::LogHiresCapture() noexcept
{
    static_assert(HiresCapSkipCount == HiresCapSkip_Count);

    s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (HiresCap.LogStartNs == 0)
        HiresCap.LogStartNs = now;
    if (now - HiresCap.LogStartNs < 1000000000)
        return;

    const std::array<u32, HiresCapSkipCount>& s = HiresCap.Skips;
    Log(LogLevel::Info, "HIRESCAP scale=%d captures=%u passes=%u shown=%u/%u stream=%u swapnative=%u srcb=%u/%u skip "
        "layout=%u vcount=%u abort=%u nolcdc=%u size=%u wrap=%u missing=%u written=%u 1x=%u shader=%u\n",
        Scale, HiresCap.Captures, HiresCap.Passes, HiresCap.LinesShown, HiresCap.LinesCandidate,
        HiresCap.LinesStream, HiresCap.LinesSwapNative,
        HiresCap.LinesHiresB, HiresCap.LinesHiresB + HiresCap.LinesNativeB,
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9]);

    HiresCap.LogStartNs = now;
    HiresCap.Captures = HiresCap.Passes = 0;
    HiresCap.LinesShown = HiresCap.LinesCandidate = 0;
    HiresCap.LinesStream = HiresCap.LinesSwapNative = 0;
    HiresCap.LinesHiresB = HiresCap.LinesNativeB = 0;
    HiresCap.Skips = {};
}

#ifdef MELONADE_DEBUG_TOOLS
// check mode (GPU::HiresCaptureDebug 2, at 1x): reads the copy the pass just made back and
// compares it with the native capture (the bank's VRAM, or its copy from line 192), which it
// should match word for word
void GLCompositor::CheckHiresCapture(const u8* bankNative, int bank, int rowStart, int rows) noexcept
{
    // R16UI reads back as GL_RGBA_INTEGER/GL_UNSIGNED_INT, which GLES 3 always allows for it.
    // the pass framebuffer still has the copy attached
    std::vector<u32> texels(256 * rows * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, HiresCap.PassFB);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(0, rowStart, 256, rows, GL_RGBA_INTEGER, GL_UNSIGNED_INT, texels.data());

    const u16* vram = (const u16*)bankNative;
    int mismatches = 0, firstX = -1, firstY = -1;
    u32 firstHires = 0, firstNative = 0;
    for (int y = 0; y < rows; y++)
    {
        for (int x = 0; x < 256; x++)
        {
            u32 hires = texels[(y * 256 + x) * 4];
            u32 native = vram[(rowStart + y) * 256 + x];
            if (hires == native)
                continue;

            if (mismatches++ == 0)
            {
                firstX = x;
                firstY = rowStart + y;
                firstHires = hires;
                firstNative = native;
            }
        }
    }

    Log(LogLevel::Info, "HIRESCAP check bank=%c rows=%d-%d mismatches=%d first=(%d,%d) hires=%04X native=%04X\n",
        'A' + bank, rowStart, rowStart + rows - 1, mismatches, firstX, firstY, firstHires, firstNative);
}
#endif

// GPU 2D shadow renderer (GPU_Shadow2D.h): which lines it draws this frame, from each line's block and
// what the compositor knows. a line is drawn if the recorder can vouch for it (feature-eligible, this
// frame's, not before a real mid-frame change of its unit's memory, its sprites fit), it doesn't show
// a hi-res capture image, its wings aren't held, and the mode wants it: mode 1 only where it gains
// something, which in increment 1 is a line whose 2D the wide config sends into a wide view's wings.
// the decision goes into the uploaded copy of the block (F_Drawn), never a uniform mask (the Adreno
// gotcha in MODS.md). then the drawn lines' memory and sprites are uploaded and the OBJ pass runs.
// returns how many lines are drawn
int GLCompositor::PrepareShadow2D(const GPU& gpu, int backbuf, bool hasFramebuffers, bool hiresDisplay, bool ctrlCopied) noexcept
{
    using namespace Shadow2D;

    Shadow2DRecorder* rec = gpu.Shadow2D.get();
    Shadow.DrawnLines = 0;
    if (!rec)
    {
        FreeShadow2DTextures();
        return 0;
    }
    if (!hasFramebuffers)
        return 0;

    // the sprite runs are closed at line 192 already, unless a VCount write skipped it
    rec->Finalize();

    const bool wideView = ViewWidth != 256;
    Shadow.State.resize(384 * GPU2D::ShadowStateWords);
    int drawn = 0;
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            int outLine = half * 192 + line;
            const u32* block = &fb[GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords];
            u32* copy = &Shadow.State[outLine * GPU2D::ShadowStateWords];
            memcpy(copy, block, GPU2D::ShadowStateWords * sizeof(u32));

            u32 ctrl = ctrlCopied ? CtrlWords[outLine] : fb[line * (256*3 + 1) + 256*3];
            u32 reason = Shadow.Failed ? (u32)R_Failed : rec->LineReason(block);
            if (reason == R_None && hiresDisplay && (ctrl & HiresDisplayFlag))
                reason = R_Hires;
            if (reason == R_None && ((WingHoldLines[outLine >> 5] >> (outLine & 31)) & 1))
                reason = R_Held;
            if (reason == R_None && rec->Mode == 1 && !(wideView && (block[W_Flags] & F_Wide)))
                reason = R_Policy;

            copy[W_Flags] = (copy[W_Flags] & ~F_Drawn) | (reason == R_None ? F_Drawn : 0);
            if (rec->Logging)
                Shadow.LogReasons[reason]++;
            drawn += reason == R_None;
        }
    }

    // nothing to draw: the dirty pages wait for a frame that draws something
    if (!drawn)
        return 0;
    if (!PrepareShadow2DResources() || !PrepareShadow2DProgram(wideView ? Shadow2DCompWide : Shadow2DComp)
        || !PrepareShadow2DProgram(wideView ? Shadow2DPlanesWide : Shadow2DPlanes))
    {
        Log(LogLevel::Error, "GPU2D: the shadow renderer couldn't be set up, staying on the CPU\n");
        Shadow.Failed = true;
        return 0;
    }

    BeginGPUTimer(ComputePass::CompShadowObj);
    UploadShadow2D(gpu, *rec);
    RunShadow2DObj(*rec);
    // RenderFrame checks this frame (CheckShadow2D) by the same test
    RunShadow2DPlanes(rec->Check && (Shadow.CheckFrames % 30) == 0);
    Shadow.DrawnLines = drawn;
    return drawn;
}

bool GLCompositor::PrepareShadow2DResources() noexcept
{
    using namespace Shadow2D;

    if (!Shadow.MemTex)
    {
        Shadow.MemTex = MakeHiresTexture(GL_R32UI, 256, MemRows);
        Shadow.StateTex = MakeHiresTexture(GL_RGBA32UI, GPU2D::ShadowStateWords / 4, 384);
        Shadow.MemGeneration = 0;
    }

    if (Shadow.ObjW != ViewWidth)
    {
        glDeleteFramebuffers(1, &Shadow.ObjFB);
        glDeleteRenderbuffers(1, &Shadow.ObjDepth);
        glDeleteTextures(1, &Shadow.ObjTex);

        Shadow.ObjTex = MakeHiresTexture(GL_RG32UI, ViewWidth, 384);
        glGenRenderbuffers(1, &Shadow.ObjDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, Shadow.ObjDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, ViewWidth, 384);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
        glGenFramebuffers(1, &Shadow.ObjFB);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Shadow.ObjFB);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, Shadow.ObjTex, 0);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, Shadow.ObjDepth);
        glDrawBuffers(1, fbassign);
        bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (!complete)
            return false;
        Shadow.ObjW = ViewWidth;
    }

    if (Shadow.PlanesW != ViewWidth)
    {
        glDeleteFramebuffers(1, &Shadow.PlanesFB);
        glDeleteTextures(1, &Shadow.PlanesTex);

        Shadow.PlanesTex = MakeHiresTexture(GL_RGBA32UI, ViewWidth, 384);
        GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
        glGenFramebuffers(1, &Shadow.PlanesFB);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Shadow.PlanesFB);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, Shadow.PlanesTex, 0);
        glDrawBuffers(1, fbassign);
        bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (!complete)
            return false;
        Shadow.PlanesW = ViewWidth;
    }

    if (!Shadow.InstanceBuffer)
    {
        glGenBuffers(1, &Shadow.InstanceBuffer);
        glGenVertexArrays(1, &Shadow.InstanceVAO);
        glGenBuffers(1, &Shadow.QuadBuffer);
        glGenVertexArrays(1, &Shadow.QuadVAO);

        glBindVertexArray(Shadow.QuadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, Shadow.QuadBuffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

        glBindVertexArray(Shadow.InstanceVAO);
        glBindBuffer(GL_ARRAY_BUFFER, Shadow.InstanceBuffer);
        for (GLuint i = 0; i < 3; i++)
        {
            glEnableVertexAttribArray(i);
            glVertexAttribDivisor(i, 1);
        }

        glBindVertexArray(CompVertexArrayID);
        glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    }

    if (!Shadow.ObjProgram)
    {
        GLuint shader {};
        if (!OpenGL::CompileVertexFragmentProgram(shader, kShadow2DObjVS, Shadow2DObjFS(), "Shadow2DObjShader",
                {{"iGeom", 0}, {"iSprite", 1}, {"iMisc", 2}}, {{"oObj", 0}}))
            return false;

        Shadow.ObjProgram = shader;
        Shadow.ObjWLoc = glGetUniformLocation(shader, "uObjW");
        Shadow.ObjWideOffsetLoc = glGetUniformLocation(shader, "uWideOffset");
        glUseProgram(shader);
        glUniform1i(glGetUniformLocation(shader, "MemTex"), Shadow2DMemUnit);
    }

    return true;
}

bool GLCompositor::PrepareShadow2DProgram(int variant) noexcept
{
    Shadow2DProgram& comp = Shadow.Comp[variant];
    if (comp.Program)
        return true;

    GLuint shader {};
    static const char* const names[Shadow2DPrograms] = {"Shadow2DShader", "Shadow2DShaderWide", "Shadow2DPlanesShader",
                                                        "Shadow2DPlanesShaderWide"};
    const bool planes = variant == Shadow2DPlanes || variant == Shadow2DPlanesWide;
    if (!OpenGL::CompileVertexFragmentProgram(shader, kShadow2DVS, Shadow2DFS(variant), names[variant],
            {{"vPosition", 0}}, {{planes ? "oPlanes" : "oColor", 0}}))
        return false;

    comp.Program = shader;
    comp.ScaleLoc = glGetUniformLocation(shader, "u3DScale");
    comp.WideOffsetLoc = glGetUniformLocation(shader, "uWideOffset");
    comp.ObjOffsetLoc = glGetUniformLocation(shader, "uObjOffset");
    comp.WideMarginLoc = glGetUniformLocation(shader, "uWideMargin");

    glUseProgram(shader);
    glUniform1i(glGetUniformLocation(shader, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(shader, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(shader, "HeldWingsTex"), 2);
    glUniform1i(glGetUniformLocation(shader, "WingPlanesTex"), WingPlanesUnit);
    glUniform1i(glGetUniformLocation(shader, "HiresBankTex"), HiresBankUnit);
    glUniform1i(glGetUniformLocation(shader, "HiresStreamTex"), HiresStreamUnit);
    glUniform1i(glGetUniformLocation(shader, "MemTex"), Shadow2DMemUnit);
    glUniform1i(glGetUniformLocation(shader, "LineStateTex"), Shadow2DStateUnit);
    glUniform1i(glGetUniformLocation(shader, "ObjTex"), Shadow2DObjUnit);
    glUniform1i(glGetUniformLocation(shader, "PlanesTex"), Shadow2DPlanesUnit);
    return true;
}

// MemTex: everything after a reset, a load or a new compositor (the recorder's Generation), otherwise
// the pages written since the last upload, from the flats as of the frame's last line. then the
// blocks and the instances. every row uploaded is a multiple of 128 bytes, so whatever unpack
// alignment the 3D renderer left works
void GLCompositor::UploadShadow2D(const GPU& gpu, Shadow2DRecorder& rec) noexcept
{
    using namespace Shadow2D;

    u64 bytes = 0;
    glActiveTexture(GL_TEXTURE0 + Shadow2DMemUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.MemTex);

    bool full = Shadow.MemGeneration != rec.Generation;
    rec.ForEachUpload(gpu, full, [&](u32 x, u32 row, u32 width, u32 rows, const u8* source)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, row, width, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, source);
        bytes += width * rows * 4;
    });
    Shadow.MemGeneration = rec.Generation;
    memset(rec.Dirty, 0, sizeof(rec.Dirty));

    glActiveTexture(GL_TEXTURE0 + Shadow2DStateUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.StateTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GPU2D::ShadowStateWords / 4, 384, GL_RGBA_INTEGER, GL_UNSIGNED_INT, Shadow.State.data());
    bytes += Shadow.State.size() * sizeof(u32);

    size_t sprites = rec.Sprites.size();
    if (sprites)
    {
        glBindBuffer(GL_ARRAY_BUFFER, Shadow.InstanceBuffer);
        glBufferData(GL_ARRAY_BUFFER, sprites * sizeof(Instance), rec.Sprites.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
        bytes += sprites * sizeof(Instance);
    }

    glActiveTexture(GL_TEXTURE0);
    Shadow.LogUploadBytes += bytes;
    Shadow.LogInstances += sprites;
}

// the OBJ pass: the frame's sprite instances into ObjTex (R; G, the OBJ window, stays 0 until
// increment 4). it leaves the state RenderFrame sets up afterwards as it found it, or sets it again
void GLCompositor::RunShadow2DObj(const Shadow2DRecorder& rec) noexcept
{
    using namespace Shadow2D;

    // ObjTex isn't read while it's drawn
    glActiveTexture(GL_TEXTURE0 + Shadow2DObjUnit);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Shadow.ObjFB);
    // a new query for the OBJ pass, still cx_sobj: the uploads, this pass and the planes pass are
    // three back-to-back queries, so a tiler running the two independent passes out of order can't
    // put one query's start after its end (GLTIME's bad=)
    BeginGPUTimer(ComputePass::CompShadowObj);
    glViewport(0, 0, Shadow.ObjW, 384);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    const GLuint zero[4] = {0, 0, 0, 0};
    const GLfloat farDepth = 1.0f;
    glClearBufferuiv(GL_COLOR, 0, zero);
    glClearBufferfv(GL_DEPTH, 0, &farDepth);

    size_t sprites = rec.Sprites.size();
    if (sprites)
    {
        glUseProgram(Shadow.ObjProgram);
        glUniform1i(Shadow.ObjWLoc, Shadow.ObjW);
        glUniform1i(Shadow.ObjWideOffsetLoc, (ViewWidth - 256) / 2);
        glBindVertexArray(Shadow.InstanceVAO);
        glBindBuffer(GL_ARRAY_BUFFER, Shadow.InstanceBuffer);
        glVertexAttribIPointer(0, 4, GL_INT, sizeof(Instance), (void*)offsetof(Instance, X));
        glVertexAttribIPointer(1, 4, GL_UNSIGNED_INT, sizeof(Instance), (void*)offsetof(Instance, FirstRow));
        glVertexAttribIPointer(2, 4, GL_UNSIGNED_INT, sizeof(Instance), (void*)offsetof(Instance, Index));

        // lowest (priority, OAM index) wins, as SoftRenderer::DrawSpritePixel decides it
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)sprites);

        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(CompVertexArrayID);
        glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    }

    const GLenum depth = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &depth);

    glActiveTexture(GL_TEXTURE0 + Shadow2DObjUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.ObjTex);
    glActiveTexture(GL_TEXTURE0);
}

// two triangles per run of drawn lines in a screen that are alike in whether they cover the wings: in
// the output (rows of Scale pixels, the 2 padding lines between the screens, the top screen's lines
// below its margin, whose rows are the compositor's) or PlanesTex (ViewWidth x 384 at 1x, no padding)
void GLCompositor::BuildShadow2DQuads(bool planes) noexcept
{
    using namespace Shadow2D;

    Shadow.Quads.clear();
    const bool wideView = ViewWidth != 256;
    const int offset = (ViewWidth - 256) / 2;
    const int scale = planes ? 1 : Scale;
    const float w = planes ? (float)ViewWidth : (float)ScreenW;
    const float h = planes ? 384.0f : (float)ScreenH;

    for (int half = 0; half < 2; half++)
    {
        const int firstRow = planes ? half * 192 : HalfLineRow(half);
        int line = 0;
        while (line < 192)
        {
            const u32* block = &Shadow.State[(half * 192 + line) * GPU2D::ShadowStateWords];
            if (!(block[W_Flags] & F_Drawn))
            {
                line++;
                continue;
            }

            bool wide = wideView && (block[W_Flags] & F_Wide);
            int end = line + 1;
            while (end < 192)
            {
                u32 flags = Shadow.State[(half * 192 + end) * GPU2D::ShadowStateWords + W_Flags];
                if (!(flags & F_Drawn) || (wideView && (flags & F_Wide)) != wide)
                    break;
                end++;
            }

            float x0 = wide ? 0.0f : (float)(offset * scale);
            float x1 = wide ? w : (float)((offset + 256) * scale);            float y0 = (float)((firstRow + line) * scale);
            float y1 = (float)((firstRow + end) * scale);
            x0 = x0 / w * 2.0f - 1.0f;
            x1 = x1 / w * 2.0f - 1.0f;
            y0 = y0 / h * 2.0f - 1.0f;
            y1 = y1 / h * 2.0f - 1.0f;
            const float quad[12] = {x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1};
            Shadow.Quads.insert(Shadow.Quads.end(), quad, quad + 12);

            line = end;
        }
    }
}

// the planes pass, right after the OBJ pass and before the output is bound, so a tiled GPU doesn't
// store and load the output around it: each drawn native pixel's planes 0-2, and whether a 2D layer
// drew it, into PlanesTex at 1x (the wings too on lines that draw them). the composite reads them,
// so the layers are worked out once per native pixel rather than at every sub-pixel. [checked]: the
// frame CheckShadow2D reads back, which starts from a sentinel instead of the last frame's planes, so
// a texel this pass misses shows up as a mismatch
void GLCompositor::RunShadow2DPlanes(bool checked) noexcept
{
    BuildShadow2DQuads(true);

    // PlanesTex isn't read while it's drawn, and the composite only reads texels this draws, so
    // nothing needs loading first
    glActiveTexture(GL_TEXTURE0 + Shadow2DPlanesUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Shadow.PlanesFB);
    // the planes pass's own query, still cx_sobj (see RunShadow2DObj)
    BeginGPUTimer(ComputePass::CompShadowObj);
    if (checked)
    {
        const GLuint sentinel[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
        glClearBufferuiv(GL_COLOR, 0, sentinel);
    }
    else
    {
        const GLenum color = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &color);
    }
    glViewport(0, 0, ViewWidth, 384);

    const Shadow2DProgram& comp = Shadow.Comp[ViewWidth != 256 ? Shadow2DPlanesWide : Shadow2DPlanes];
    glUseProgram(comp.Program);
    glUniform1i(comp.WideOffsetLoc, (ViewWidth - 256) / 2);
    glUniform1i(comp.ObjOffsetLoc, (ViewWidth - 256) / 2);
    glActiveTexture(GL_TEXTURE0 + Shadow2DMemUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.MemTex);
    glActiveTexture(GL_TEXTURE0 + Shadow2DStateUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.StateTex);

    glBindVertexArray(Shadow.QuadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, Shadow.QuadBuffer);
    glBufferData(GL_ARRAY_BUFFER, Shadow.Quads.size() * sizeof(float), Shadow.Quads.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(Shadow.Quads.size() / 2));

    glBindVertexArray(CompVertexArrayID);
    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glActiveTexture(GL_TEXTURE0 + Shadow2DPlanesUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.PlanesTex);
    glActiveTexture(GL_TEXTURE0);
}

// the composite pass, over the compositor's output just drawn (bound, with its viewport), its
// screen texture on unit 0 and the 3D frame on unit 1. drawn lines lose the filter for now (the
// shadow draws over them). the state is RenderFrame's again afterwards, the program aside
void GLCompositor::RunShadow2D(int backbuf) noexcept
{
    const bool wideView = ViewWidth != 256;
    const Shadow2DProgram& comp = Shadow.Comp[wideView ? Shadow2DCompWide : Shadow2DComp];
    BuildShadow2DQuads(false);
    if (Shadow.Quads.empty())
        return;

    BeginGPUTimer(ComputePass::CompShadow);
    glUseProgram(comp.Program);
    glUniform1ui(comp.ScaleLoc, Scale);
    glUniform1i(comp.WideOffsetLoc, (ViewWidth - 256) / 2);
    glUniform1i(comp.WideMarginLoc, Margin);

    glActiveTexture(GL_TEXTURE0 + Shadow2DPlanesUnit);
    glBindTexture(GL_TEXTURE_2D, Shadow.PlanesTex);

    glBindVertexArray(Shadow.QuadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, Shadow.QuadBuffer);
    glBufferData(GL_ARRAY_BUFFER, Shadow.Quads.size() * sizeof(float), Shadow.Quads.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(Shadow.Quads.size() / 2));

    glBindVertexArray(CompVertexArrayID);
    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glActiveTexture(GL_TEXTURE1);
}

// check mode: the middle 256 columns of the planes pass's output read back and compared word for word
// with what the CPU drew into the frame's buffers, on the drawn lines. a synchronous readback, every
// 30 frames that draw
void GLCompositor::CheckShadow2D(const GPU& gpu, int backbuf) noexcept
{
    using namespace Shadow2D;

    std::vector<u32> texels(256 * 384 * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, Shadow.PlanesFB);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels((ViewWidth - 256) / 2, 0, 256, 384, GL_RGBA_INTEGER, GL_UNSIGNED_INT, texels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    u32 mismatches = 0, lines = 0;
    for (int half = 0; half < 2; half++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][half].get();
        for (int line = 0; line < 192; line++)
        {
            int row = half * 192 + line;
            if (!(Shadow.State[row * GPU2D::ShadowStateWords + W_Flags] & F_Drawn))
                continue;
            lines++;
            const u32* planes = &fb[line * (256*3 + 1)];
            for (int x = 0; x < 256; x++)
            {
                for (int plane = 0; plane < 3; plane++)
                {
                    u32 expected = planes[plane * 256 + x];
                    u32 got = texels[(row * 256 + x) * 4 + plane];
                    if (expected == got)
                        continue;
                    if (mismatches++ < 8 && Shadow.Mismatches < 64)
                        Log(LogLevel::Info, "GPU2D mismatch screen=%d line=%d x=%d plane=%d expected=%08X got=%08X unit=%c\n",
                            half, line, x, plane, expected, got,
                            (Shadow.State[row * GPU2D::ShadowStateWords + W_Flags] & F_UnitB) ? 'B' : 'A');
                }
            }
        }
    }
    Shadow.Checks++;
    Shadow.CheckLines += lines;
    Shadow.Mismatches += mismatches;
}

// debug only (Shadow2DRecorder::Logging): once a second, the frames, the lines drawn out of all of
// theirs, the lines on the CPU by reason, the upload per frame, the sprite instances per frame, the
// page log's pages (none before increment 1b), the recorder's emulator thread time per frame, and
// in check mode how many checks ran over how many lines, with how many words differing
void GLCompositor::LogShadow2D(const GPU& gpu) noexcept
{
    using namespace Shadow2D;

    s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (Shadow.LogStartNs == 0)
        Shadow.LogStartNs = now;
    Shadow.LogFrames++;
    if (now - Shadow.LogStartNs < 1000000000)
        return;

    Shadow2DRecorder* rec = gpu.Shadow2D.get();
    double perFrame = Shadow.LogFrames ? 1.0 / Shadow.LogFrames : 0.0;
    char reasons[512];
    int length = 0;
    reasons[0] = '\0';
    for (u32 i = R_None + 1; i < R_Count && length < (int)sizeof(reasons) - 32; i++)
    {
        if (Shadow.LogReasons[i])
            length += snprintf(reasons + length, sizeof(reasons) - length, "%s%s:%u", length ? "," : "", ReasonName(i), Shadow.LogReasons[i]);
    }

    Log(LogLevel::Info, "GPU2D mode=%s scale=%d width=%d frames=%u drawn=%u/%u cpu=%s upkb=%.1f inst=%.1f logpages=0 "
        "recus=%.1f checks=%u checklines=%u mismatches=%u failed=%d\n",
        rec->Check ? "check" : rec->Mode == 2 ? "all" : "1", Scale, ViewWidth, Shadow.LogFrames,
        Shadow.LogReasons[R_None], Shadow.LogFrames * 384, length ? reasons : "-",
        Shadow.LogUploadBytes * perFrame / 1024.0, Shadow.LogInstances * perFrame,
        rec->RecorderNs * perFrame / 1000.0, Shadow.Checks, Shadow.CheckLines, Shadow.Mismatches, Shadow.Failed ? 1 : 0);

    Shadow.LogStartNs = now;
    Shadow.LogFrames = 0;
    Shadow.LogReasons = {};
    Shadow.LogUploadBytes = 0;
    Shadow.LogInstances = 0;
    Shadow.Checks = Shadow.CheckLines = Shadow.Mismatches = 0;
    rec->RecorderNs = 0;
}

// when the recorder is off: the textures and buffers go, the programs stay
void GLCompositor::FreeShadow2DTextures() noexcept
{
    if (!Shadow.MemTex && !Shadow.ObjTex && !Shadow.PlanesTex && !Shadow.InstanceBuffer)
        return;

    glDeleteTextures(1, &Shadow.MemTex);
    glDeleteTextures(1, &Shadow.StateTex);
    glDeleteFramebuffers(1, &Shadow.ObjFB);
    glDeleteRenderbuffers(1, &Shadow.ObjDepth);
    glDeleteTextures(1, &Shadow.ObjTex);
    glDeleteFramebuffers(1, &Shadow.PlanesFB);
    glDeleteTextures(1, &Shadow.PlanesTex);
    glDeleteBuffers(1, &Shadow.InstanceBuffer);
    glDeleteVertexArrays(1, &Shadow.InstanceVAO);
    glDeleteBuffers(1, &Shadow.QuadBuffer);
    glDeleteVertexArrays(1, &Shadow.QuadVAO);
    Shadow.MemTex = Shadow.StateTex = 0;
    Shadow.ObjFB = Shadow.ObjDepth = Shadow.ObjTex = 0;
    Shadow.ObjW = 0;
    Shadow.PlanesFB = Shadow.PlanesTex = 0;
    Shadow.PlanesW = 0;
    Shadow.InstanceBuffer = Shadow.InstanceVAO = 0;
    Shadow.QuadBuffer = Shadow.QuadVAO = 0;
    Shadow.MemGeneration = 0;
    Shadow.State = std::vector<u32>();
    Shadow.Quads = std::vector<float>();
}

void GLCompositor::FreeShadow2D() noexcept
{
    FreeShadow2DTextures();
    glDeleteProgram(Shadow.ObjProgram);
    for (Shadow2DProgram& comp : Shadow.Comp)
        glDeleteProgram(comp.Program);
    Shadow.ObjProgram = 0;
    Shadow.Comp = {};
}

void GLCompositor::SetOutputTexture(int buf, GLuint texture)
{
    // without a texture the buffer goes back to the compositor's own (see PrepareOutput)
    OutputExternal[buf] = texture != 0;
    // GPU::Comp1x: a layout is only told for a draw into this texture
    OneX.Drawn[buf] = false;

    GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
    glBindFramebuffer(GL_FRAMEBUFFER, CompScreenOutputFB[buf]);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, OutputExternal[buf] ? texture : CompScreenOutputTex[buf], 0);
    glDrawBuffers(1, fbassign);

    // the buffer's own texture, if a frame had to be drawn into it since the last size change,
    // isn't needed anymore
    if (OutputExternal[buf] && CompScreenOutputTex[buf])
    {
        glDeleteTextures(1, &CompScreenOutputTex[buf]);
        CompScreenOutputTex[buf] = 0;
        OutputTexW[buf] = OutputTexH[buf] = 0;
    }
}

void GLCompositor::BindOutputTexture(int buf)
{
    glBindTexture(GL_TEXTURE_2D, CompScreenOutputTex[buf]);
}

}
