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
#include <string>
#include <utility>
#include <vector>

#include "NDS.h"
#include "GPU.h"
#include "GPU3D_OpenGL.h"
#include "GPU_HiresStream.h"
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
    return std::string(kCompositorFS_Nearest_Head) + kCompositePixelFS + kCompositorFS_Nearest_Main;
}

std::string GLCompositor::HiresCaptureFS()
{
    return std::string(kHiresCaptureFS_Head) + kCompositePixelFS + kHiresCaptureFS_Main;
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

GLCompositor::GLCompositor(GLuint compShader) noexcept : CompShader(compShader)
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

    glGenTextures(CompScreenOutputTex.size(), &CompScreenOutputTex[0]);
    for (GLuint i : CompScreenOutputTex)
    {
        glBindTexture(GL_TEXTURE_2D, i);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

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
    FreeHiresCapture();

    glDeleteVertexArrays(1, &CompVertexArrayID);
    glDeleteBuffers(1, &CompVertexBufferID);

    glDeleteProgram(CompShader);
    glDeleteProgram(CompShaderWide);
}


GLCompositor::GLCompositor(GLCompositor&& other) noexcept :
    Scale(other.Scale),
    ViewWidth(other.ViewWidth),
    ScreenH(other.ScreenH),
    ScreenW(other.ScreenW),
    CompScaleLoc(other.CompScaleLoc),
    CompHiresCapLoc(other.CompHiresCapLoc),
    CompShaderWide(other.CompShaderWide),
    CompShaderWideFailed(other.CompShaderWideFailed),
    CompWideScaleLoc(other.CompWideScaleLoc),
    CompWideRatioLoc(other.CompWideRatioLoc),
    CompWideOffsetLoc(other.CompWideOffsetLoc),
    CompWideHiresCapLoc(other.CompWideHiresCapLoc),
    WingHold(other.WingHold),
    WingCopy(other.WingCopy),
    WingHoldLines(other.WingHoldLines),
    HeldWingsTex(other.HeldWingsTex),
    HeldWingsFB(other.HeldWingsFB),
    HeldWingsW(other.HeldWingsW),
    HeldWingsH(other.HeldWingsH),
    HiresCap(other.HiresCap),
    CompVertices(other.CompVertices),
    CompShader(other.CompShader),
    CompVertexBufferID(other.CompVertexBufferID),
    CompVertexArrayID(other.CompVertexArrayID),
    CompScreenInputTex(other.CompScreenInputTex),
    CompScreenOutputTex(other.CompScreenOutputTex),
    CompScreenOutputFB(other.CompScreenOutputFB)
{
    other.HeldWingsTex = {};
    other.HeldWingsFB = {};
    other.HiresCap = {};
    other.CompScreenOutputFB = {};
    other.CompScreenInputTex = {};
    other.CompScreenOutputTex = {};
    other.CompVertexArrayID = {};
    other.CompVertexBufferID = {};
    other.CompShader = {};
    other.CompShaderWide = {};
}

GLCompositor& GLCompositor::operator=(GLCompositor&& other) noexcept
{
    if (this != &other)
    {
        Scale = other.Scale;
        ViewWidth = other.ViewWidth;
        ScreenH = other.ScreenH;
        ScreenW = other.ScreenW;
        CompScaleLoc = other.CompScaleLoc;
        CompHiresCapLoc = other.CompHiresCapLoc;
        CompShaderWideFailed = other.CompShaderWideFailed;
        CompWideScaleLoc = other.CompWideScaleLoc;
        CompWideRatioLoc = other.CompWideRatioLoc;
        CompWideOffsetLoc = other.CompWideOffsetLoc;
        CompWideHiresCapLoc = other.CompWideHiresCapLoc;
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

        glDeleteBuffers(1, &CompVertexBufferID);
        CompVertexBufferID = other.CompVertexBufferID;

        glDeleteVertexArrays(1, &CompVertexArrayID);
        CompVertexArrayID = other.CompVertexArrayID;

        glDeleteTextures(1, &CompScreenInputTex);
        CompScreenInputTex = other.CompScreenInputTex;

        glDeleteTextures(CompScreenOutputTex.size(), &CompScreenOutputTex[0]);
        CompScreenOutputTex = other.CompScreenOutputTex;

        glDeleteFramebuffers(CompScreenOutputFB.size(), &CompScreenOutputFB[0]);
        CompScreenOutputFB = other.CompScreenOutputFB;

        glDeleteTextures(1, &HeldWingsTex);
        HeldWingsTex = other.HeldWingsTex;

        glDeleteFramebuffers(1, &HeldWingsFB);
        HeldWingsFB = other.HeldWingsFB;

        FreeHiresCapture();
        HiresCap = other.HiresCap;

        other.HeldWingsTex = {};
        other.HeldWingsFB = {};
        other.HiresCap = {};
        other.CompScreenOutputFB = {};
        other.CompScreenInputTex = {};
        other.CompScreenOutputTex = {};
        other.CompVertexArrayID = {};
        other.CompVertexBufferID = {};
        other.CompShader = {};
        other.CompShaderWide = {};
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

    glUseProgram(CompShaderWide);
    glUniform1i(glGetUniformLocation(CompShaderWide, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(CompShaderWide, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HeldWingsTex"), 2);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HiresBankTex"), HiresBankUnit);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HiresStreamTex"), HiresStreamUnit);

    return true;
}

void GLCompositor::SetScaleFactor(int scale) noexcept
{
    SetRenderSettings(scale, ViewWidth);
}

void GLCompositor::SetRenderSettings(int scale, int viewWidth) noexcept
{
    if (scale == Scale && viewWidth == ViewWidth)
        return;

    // the hi-res capture copies are at the scale, a view width change keeps them
    if (scale != Scale)
        FreeHiresBanks();

    Scale = scale;
    ViewWidth = viewWidth;
    ScreenW = viewWidth * scale;
    ScreenH = (384+2) * scale;

    // held wings are only reused at the width and scale they were made at
    ResetWingHold();

    for (int i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, CompScreenOutputTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        // fill the padding
        u8* zeroPixels = (u8*) calloc(1, ScreenW*2*scale*4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192*scale, ScreenW, 2*scale, GL_RGBA, GL_UNSIGNED_BYTE, zeroPixels);

        GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
        glBindFramebuffer(GL_FRAMEBUFFER, CompScreenOutputFB[i]);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, CompScreenOutputTex[i], 0);
        glDrawBuffers(1, fbassign);
        free(zeroPixels);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLCompositor::Stop(const GPU& gpu) noexcept
{
    for (int i = 0; i < 2; i++)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[gpu.FrontBuffer]);

        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ResetWingHold();
    ResetHiresCapture();
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
void GLCompositor::PrepareWingHold(const GPU& gpu, int backbuf) noexcept
{
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
                liveLines[half][line >> 5] |= 1u << (line & 31);
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

    int wingW = (ViewWidth - 256) / 2 * Scale;
    if ((WingCopy[0] || WingCopy[1]) && (HeldWingsW != wingW * 2 || HeldWingsH != ScreenH))
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wingW * 2, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, HeldWingsFB);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, HeldWingsTex, 0);

        HeldWingsW = wingW * 2;
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
}

// copies the wings of the halves PrepareWingHold picked from the output just drawn. the
// wings are the output's first and last wingW columns, and its halves are 192 lines each
// with the 2 padding lines between them
void GLCompositor::CopyHeldWings(int backbuf) noexcept
{
    int wingW = (ViewWidth - 256) / 2 * Scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, HeldWingsFB);

    for (int half = 0; half < 2; half++)
    {
        if (!WingCopy[half])
            continue;

        int y0 = half * 194 * Scale;
        int y1 = y0 + 192 * Scale;
        glBlitFramebuffer(0, y0, wingW, y1, 0, y0, wingW, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBlitFramebuffer(ScreenW - wingW, y0, ScreenW, y1, wingW, y0, wingW * 2, y1,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        WingHold[half].SinceCopy = 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);
}

void GLCompositor::RenderFrame(const GPU& gpu, Renderer3D& renderer) noexcept
{
    int backbuf = gpu.FrontBuffer ^ 1;
    bool hasFramebuffers = gpu.Framebuffer[backbuf][0] && gpu.Framebuffer[backbuf][1];

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
    if (wingHold)
        PrepareWingHold(gpu, backbuf);
    else
        WingHoldLines = {};

    // after the wing hold, which fills CtrlWords when it runs
    bool hiresDisplay = hasFramebuffers && PrepareHiresDisplay(gpu, backbuf, wingHold);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, CompScreenOutputFB[backbuf]);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glViewport(0, 0, ScreenW, ScreenH);

    // TODO: select more shaders (filtering, etc)
    if (ViewWidth == 256)
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

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, HeldWingsTex);
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

    if (hasFramebuffers)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][0].get());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][1].get());

        // the control words again, with the held lines flagged (see PrepareWingHold) and the
        // hi-res capture flag only on the lines that show an image (see PrepareHiresDisplay)
        if (wingHold || hiresDisplay)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 256*3, 0, 1, 384, GL_RGBA_INTEGER,
                            GL_UNSIGNED_BYTE, CtrlWords.data());
    }

    glActiveTexture(GL_TEXTURE1);
    renderer.SetupAccelFrame();

    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glBindVertexArray(CompVertexArrayID);
    glDrawArrays(GL_TRIANGLES, 0, 4*3);

    if (wingHold && (WingCopy[0] || WingCopy[1]))
        CopyHeldWings(backbuf);

    // after the draw, which shows the copies as they were before this frame's capture
    if (gpu.HiresCapture)
        RunHiresCapture(gpu, backbuf);
    if (gpu.HiresCaptureDebug)
        LogHiresCapture();
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
            }
            else
                ctrl &= ~HiresDisplayBits;
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
    }

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
// SetupAccelFrame: the next 3D frame only replaces it at line 215, after this in GL command order)
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
    if (!skip && !PrepareHiresCaptureShader())
        skip |= HiresCapSkip_Shader;

    // where each line's source B comes from, decided with the copies as they were before this
    // pass: the source bank's copy if it's valid over the row read, no earlier capture wrote the
    // bank without a pass, and, for the destination bank itself, the row read is the row written
    // (capture line L reads it before writing it, so it reads the last pass's copy). one source B
    // copy is bound, the first one a line can use. otherwise the native words, and always those
    // for the display FIFO
    int bankB = -1;
    bool anyPlanes = false, anyNativeB = false;
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
                }
                else
                {
                    anyNativeB = true;
                    HiresCap.LinesNativeB++;
                }
            }
            if (mode != 1 && !(capcnt & (1 << 24)))
                anyPlanes = true;

            HiresCapCtrl[line * 2] = capcnt;
            HiresCapCtrl[line * 2 + 1] = (entry.RenderXPos & 0x1FF) | (hiresB << 9) |
                                         (entry.SrcBPresent << 10) | (entry.SrcBRow << 16);
        }
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

    glUseProgram(HiresCap.Shader);
    glUniform1i(HiresCap.ScaleLoc, Scale);
    glUniform1i(HiresCap.WideOffsetLoc, (ViewWidth - 256) / 2);
    glUniform1i(HiresCap.DstRowLoc, dstRow);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // the destination's old image becomes the scratch one, and stays usable for the next frame's
    // stream with the rows it covered, until the next pass draws over it. the native capture is
    // finished: every capture line is drawn before the Blit (GPU::StartScanline at line 192), and
    // a write to the bank during it skipped this pass (HiresCapSkip_Written)
    std::swap(HiresCap.BankTex[dst], HiresCap.ScratchTex);
    std::swap(HiresCap.BankNative[dst], HiresCap.ScratchNative);
    HiresNativeRows& native = HiresCap.BankNative[dst];
    native.State = {true, dstRow, rows};
    native.Words.resize(256 * 192);
    if (dstRow < 192)
        memcpy(&native.Words[dstRow * 256], &gpu.VRAM[dst][dstRow * 512],
               (std::min(dstRow + rows, 192) - dstRow) * 512);

    HiresCap.Banks[dst] = {true, dstRow, rows};
    HiresCap.Passes++;

#ifdef MELONADE_DEBUG_TOOLS
    if (gpu.HiresCaptureDebug >= 2 && Scale == 1 && (HiresCap.CheckCount++ % 30) == 0)
        CheckHiresCapture(gpu, dst, dstRow, rows);
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
    HiresCap.DstRowLoc = glGetUniformLocation(shader, "uDstRow");

    glUseProgram(shader);
    glUniform1i(glGetUniformLocation(shader, "PlanesTex"), 4);
    glUniform1i(glGetUniformLocation(shader, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(shader, "SrcBTex"), 5);
    glUniform1i(glGetUniformLocation(shader, "BankBTex"), 6);
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
    HiresCap.PlanesTex = 0;
    HiresCap.SrcBTex = 0;
    HiresCap.PassFB = 0;
    HiresCap.Shader = 0;
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
// compares it with the native capture in VRAM, which it should match word for word
void GLCompositor::CheckHiresCapture(const GPU& gpu, int bank, int rowStart, int rows) noexcept
{
    // R16UI reads back as GL_RGBA_INTEGER/GL_UNSIGNED_INT, which GLES 3 always allows for it.
    // the pass framebuffer still has the copy attached
    std::vector<u32> texels(256 * rows * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, HiresCap.PassFB);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(0, rowStart, 256, rows, GL_RGBA_INTEGER, GL_UNSIGNED_INT, texels.data());

    const u16* vram = (const u16*)gpu.VRAM[bank];
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

void GLCompositor::SetOutputTexture(int buf, GLuint texture)
{
    GLenum fbassign[] = {GL_COLOR_ATTACHMENT0};
    glBindFramebuffer(GL_FRAMEBUFFER, CompScreenOutputFB[buf]);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
    glDrawBuffers(1, fbassign);
}

void GLCompositor::BindOutputTexture(int buf)
{
    glBindTexture(GL_TEXTURE_2D, CompScreenOutputTex[buf]);
}

}
