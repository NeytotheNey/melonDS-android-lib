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
#include <cstdio>
#include <cstring>
#include <string>

#include "NDS.h"
#include "GPU.h"
#include "GPU3D_OpenGL.h"
#include "OpenGLSupport.h"
#include "GPU_OpenGL_shaders.h"

namespace melonDS
{

using namespace OpenGL;

std::optional<GLCompositor> GLCompositor::New() noexcept
{
    assert(glBindAttribLocation != nullptr);
    GLuint CompShader {};

    if (!OpenGL::CompileVertexFragmentProgram(CompShader,
            kCompositorVS, kCompositorFS_Nearest,
            "CompositorShader",
            {{"vPosition", 0}, {"vTexcoord", 1}},
            {{"oColor", 0}}))
        return std::nullopt;

    return { GLCompositor(CompShader) };
}

GLCompositor::GLCompositor(GLuint compShader) noexcept : CompShader(compShader)
{
    CompScaleLoc = glGetUniformLocation(CompShader, "u3DScale");

    glUseProgram(CompShader);
    GLuint screenTextureUniform = glGetUniformLocation(CompShader, "ScreenTex");
    glUniform1i(screenTextureUniform, 0);
    GLuint _3dTextureUniform = glGetUniformLocation(CompShader, "_3DTex");
    glUniform1i(_3dTextureUniform, 1);

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
    CompShaderWide(other.CompShaderWide),
    CompShaderWideFailed(other.CompShaderWideFailed),
    CompWideScaleLoc(other.CompWideScaleLoc),
    CompWideRatioLoc(other.CompWideRatioLoc),
    CompWideOffsetLoc(other.CompWideOffsetLoc),
    CompWideHoldLoc(other.CompWideHoldLoc),
    WingHold(other.WingHold),
    WingCopy(other.WingCopy),
    WingHoldLines(other.WingHoldLines),
    HeldWingsTex(other.HeldWingsTex),
    HeldWingsFB(other.HeldWingsFB),
    HeldWingsW(other.HeldWingsW),
    HeldWingsH(other.HeldWingsH),
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
        CompShaderWideFailed = other.CompShaderWideFailed;
        CompWideScaleLoc = other.CompWideScaleLoc;
        CompWideRatioLoc = other.CompWideRatioLoc;
        CompWideOffsetLoc = other.CompWideOffsetLoc;
        CompWideHoldLoc = other.CompWideHoldLoc;
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

        other.HeldWingsTex = {};
        other.HeldWingsFB = {};
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
    std::string fs = kCompositorFS_Nearest;
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
    CompWideHoldLoc = glGetUniformLocation(CompShaderWide, "uWingHold");

    glUseProgram(CompShaderWide);
    glUniform1i(glGetUniformLocation(CompShaderWide, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(CompShaderWide, "_3DTex"), 1);
    glUniform1i(glGetUniformLocation(CompShaderWide, "HeldWingsTex"), 2);

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
}

void GLCompositor::ResetWingHold() noexcept
{
    WingHold = {};
    WingCopy = {};
    WingHoldLines = {};
}

// decides, from the control words of the frame being composited, which wing lines are held
// (WingHoldLines, for the shader) and which halves get copied after drawing (WingCopy).
// a half is held while unit A's 3D wings are in the other half this frame (the game moved
// unit A over), it had its own in a copy made at most 2 frames ago, and it hasn't been held
// for 2 frames in a row yet. only lines that had 3D wings in the copy and would be black
// now are held, so 2D-only lines and white lines stay as they are. games with 3D on one
// screen never have 3D wings in both halves, so they never copy or hold
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
            if (ctrl & (1 << 18))
            {
                liveLines[half][line >> 5] |= 1u << (line & 31);
                live[half] = true;
            }
            else if (!(ctrl & (1 << 21)))
                blackLines[half][line >> 5] |= 1u << (line & 31);
        }

        WingHoldHalf& hold = WingHold[half];
        hold.SinceLive = live[half] ? 0 : std::min(hold.SinceLive + 1, WingHoldNever);
        hold.SinceCopy = std::min(hold.SinceCopy + 1, WingHoldNever);
    }

    // a half's live wings are copied while the other half had 3D wings in the last 2 frames
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

    // before the output is bound, as this can set up the held wings' framebuffer
    bool wingHold = ViewWidth != 256 && hasFramebuffers;
    if (wingHold)
        PrepareWingHold(gpu, backbuf);
    else
        WingHoldLines = {};

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
    }
    else
    {
        glUseProgram(CompShaderWide);
        glUniform1ui(CompWideScaleLoc, Scale);
        glUniform1f(CompWideRatioLoc, ViewWidth / 256.0f);
        glUniform1i(CompWideOffsetLoc, (ViewWidth - 256) / 2);
        glUniform4uiv(CompWideHoldLoc, WingHoldLines.size() / 4, WingHoldLines.data());

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, HeldWingsTex);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, CompScreenInputTex);

    if (hasFramebuffers)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][0].get());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192, 256*3 + 1, 192, GL_RGBA_INTEGER,
                        GL_UNSIGNED_BYTE, gpu.Framebuffer[backbuf][1].get());
    }

    glActiveTexture(GL_TEXTURE1);
    renderer.SetupAccelFrame();

    glBindBuffer(GL_ARRAY_BUFFER, CompVertexBufferID);
    glBindVertexArray(CompVertexArrayID);
    glDrawArrays(GL_TRIANGLES, 0, 4*3);

    if (wingHold && (WingCopy[0] || WingCopy[1]))
        CopyHeldWings(backbuf);
}

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
