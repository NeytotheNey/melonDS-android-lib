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

#ifndef GPU_OPENGL_SHADERS_H
#define GPU_OPENGL_SHADERS_H

namespace melonDS
{
// unit A's display mode 1 composite of one pixel, from the three 2D planes the 2D renderer drew in
// the accelerated format and the 3D pixel (only read for compmode 0-4). the compositor and the
// hi-res capture program both use it, put after their precision statements (see
// GLCompositor::CompositorFS). opaque is what the CPU composite in GPU2D::SoftRenderer::DoCapture
// leaves in the alpha byte, which capture turns into the alpha bit: its blend functions always
// return opaque, while the alpha returned here goes through the blend maths
const char* kCompositePixelFS = R"(
ivec4 CompositePixel(ivec4 val1, ivec4 val2, ivec4 val3, ivec4 _3dpix, out bool opaque)
{
    int compmode = val3.a & 0xF;
    int eva, evb, evy;
    opaque = val1.a != 0;

    if (compmode == 4)
    {
        // 3D on top, blending

        if (_3dpix.a > 0)
        {
            eva = (_3dpix.a & 0x1F) + 1;
            evb = 32 - eva;

            val1 = ((_3dpix * eva) + (val1 * evb) + 0x10) >> 5;
            val1 = min(val1, 0x3F);
            opaque = true;
        }
        else
        {
            val1 = val2;
            opaque = val2.a != 0;
        }
    }
    else if (compmode == 1)
    {
        // 3D on bottom, blending

        if (_3dpix.a > 0)
        {
            eva = val3.g;
            evb = val3.b;

            val1 = ((val1 * eva) + (_3dpix * evb) + 0x8) >> 4;
            val1 = min(val1, 0x3F);
            opaque = true;
        }
        else
        {
            val1 = val2;
            opaque = val2.a != 0;
        }
    }
    else if (compmode <= 3)
    {
        // 3D on top, normal/fade

        if (_3dpix.a > 0)
        {
            evy = val3.g;

            val1 = _3dpix;
            if      (compmode == 2) val1 += (((0x3F - val1) * evy) + 0x8) >> 4;
            else if (compmode == 3) val1 -= ((val1 * evy) + 0x7) >> 4;
            opaque = true;
        }
        else
        {
            val1 = val2;
            opaque = val2.a != 0;
        }
    }

    return val1;
}
)";

const char* kCompositorVS = R"(#version 320 es

in vec2 vPosition;
in vec2 vTexcoord;

#ifdef WIDE
uniform float uWideRatio;
#endif

smooth out vec2 fTexcoord;

void main()
{
    vec4 fpos;
    fpos.xy = vPosition;
    fpos.z = 0.0;
    fpos.w = 1.0;

    gl_Position = fpos;
#ifdef WIDE
    // spread the native 0..256 over the wide output, what ends up below 0 or past 256 is a wing
    fTexcoord = vec2((vTexcoord.x - 128.0) * uWideRatio + 128.0, vTexcoord.y);
#else
    fTexcoord = vTexcoord;
#endif
}
)";

// the compositor's fragment shader is kCompositorFS_Nearest_Head + kCompositePixelFS +
// kCompositorFS_Nearest_Main (see GLCompositor::CompositorFS)
const char* kCompositorFS_Nearest_Head = R"(#version 320 es

#if defined(WIDE) || defined(FILTER2D)
// wide and filtered coordinates need more precision than mediump (which can be fp16) has
precision highp float;
#else
precision mediump float;
#endif
precision mediump usampler2D;

uniform uint u3DScale;
#ifdef WIDE
uniform int uWideOffset;
uniform mediump sampler2D HeldWingsTex;
#endif

uniform usampler2D ScreenTex;
uniform sampler2D _3DTex;

// hi-res display capture (see GLCompositor::PrepareHiresDisplay): lines flagged in the control word
// (bit 7) show one of these images instead, HiresStreamTex if bit 5 is set too: VRAM display lines
// the bank copy, regular display lines the stream's. they hold bank words, so they're highp:
// 0x8000 doesn't fit a mediump int
uniform int uHiresCap;
uniform highp usampler2D HiresBankTex;
uniform highp usampler2D HiresStreamTex;

#ifdef FILTER2D
// filtered 2D (see GLCompositor::RunFilter2D): for each sub-pixel, the native pixel of its screen
// to read planes 0-2 from. uFilterMapScale is how many map texels a native pixel has.
// uFilterIdentity is debug.melonade.filter2d 3, see FilterRowCoord
uniform int uFilterMapScale;
uniform int uFilterIdentity;
uniform highp usampler2D FilterMapTex;
#endif

#if defined(FILTER2D) && !defined(WIDE)
// as with the filter off, for the identity mode, which has to round the same way
smooth in mediump vec2 fTexcoord;
#else
smooth in vec2 fTexcoord;
#endif

layout(location = 0) out vec4 oColor;
)";

// the rest of the compositor's fragment shader, after kCompositePixelFS
const char* kCompositorFS_Nearest_Main = R"(
ivec4 Fetch3D(float xpos, float ypos)
{
#ifdef WIDE
    // xpos is in native pixels, and the 3D target is uWideOffset wider on each side.
    // past its edges there's no 3D, like with a scrolled native 3D layer
    ivec2 coord = ivec2(floor(vec2(xpos + float(uWideOffset), ypos) * float(u3DScale)));
    if (coord.x < 0 || coord.x >= textureSize(_3DTex, 0).x)
        return ivec4(0);
    return ivec4(texelFetch(_3DTex, coord, 0).bgra * vec4(63,63,63,31));
#else
    return ivec4(texelFetch(_3DTex, ivec2(vec2(xpos, ypos) * float(u3DScale)), 0).bgra
         * vec4(63,63,63,31));
#endif
}

#ifdef WIDE
// the wide output's extra columns. only unit A's 3D layer goes there, on the lines where it
// shows it (control word bit 18), and the 2D layers stay native. unit A writes its control
// words into whichever screen it's on, so the wings follow the 3D to the bottom half too (the
// frontend shows that half wide when the player swaps the screens). lines that are all white
// (bit 21, from either unit) get white wings
ivec4 WingPixel()
{
    ivec4 mbright = ivec4(texelFetch(ScreenTex, ivec2(256*3, int(fTexcoord.y)), 0));
    int dispmode = mbright.b & 0x3;

    ivec4 pixel = ivec4(0);
    if ((mbright.b & 0x20) != 0)
    {
        pixel = ivec4(0x3F);
    }
    else if ((mbright.b & 0x4) != 0)
    {
        // same X scroll as the middle
        float _3dxpos = float(mbright.a - ((mbright.b & 0x80) * 2));
        pixel = Fetch3D(fTexcoord.x + _3dxpos, mod(fTexcoord.y, 192.0));

        // BLDCNT fade, as for 3D on top in the middle (bits 19-20, EVY in bits 8-12)
        int effect = (mbright.b >> 3) & 0x3;
        int evy = mbright.g & 0x1F;
        if      (effect == 2) pixel += (((0x3F - pixel) * evy) + 0x8) >> 4;
        else if (effect == 3) pixel -= ((pixel * evy) + 0x7) >> 4;
    }

    if (dispmode != 0)
    {
        int brightmode = mbright.g >> 6;
        int evy = min(mbright.r & 0x1F, 16);
        if      (brightmode == 1) pixel += ((0x3F - pixel) * evy) >> 4;
        else if (brightmode == 2) pixel -= ((pixel * evy) + 0xF) >> 4;
    }

    return pixel;
}
#endif

#ifdef FILTER2D
// this sub-pixel's position in the map's rows, which have no gap between the screens, unlike the
// output, in sub-pixels of the middle 256. fTexcoord can't give it: as fp16 it can round a
// sub-pixel into the next native pixel
highp ivec2 FilterSubPixel()
{
    highp ivec2 coord = ivec2(gl_FragCoord.xy);
    highp int scale = int(u3DScale);
    if (coord.y >= 194 * scale)
        coord.y -= 2 * scale;
#ifdef WIDE
    coord.x -= uWideOffset * scale;
#endif
    return clamp(coord, ivec2(0), ivec2(256 * scale - 1, 384 * scale - 1));
}

// what the sub-pixel reads when the map doesn't move it. the real native pixel, except in the
// identity mode, which has to match the filter off to the byte, rounding included
ivec2 FilterRowCoord(highp ivec2 native)
{
    return uFilterIdentity != 0 ? ivec2(fTexcoord) : native;
}

// where the sub-pixel reads planes 0-2: the map's entry, unless that's on a line with another
// display mode, as not every mode writes planes 1 and 2
ivec2 FilterPlaneCoord(highp ivec2 subPixel, highp ivec2 native, ivec2 rowCoord, int dispmode)
{
    // by the sub-pixel's center, so at scales the map doesn't divide evenly every sub-pixel still
    // lands in its own native pixel. the mask is for Adreno, see the end of kHiresCaptureFS_Main
    highp ivec2 mapCoord = (2 * subPixel + 1) * uFilterMapScale / (2 * int(u3DScale));
    highp uint entry = texelFetch(FilterMapTex, mapCoord, 0).r;
    highp ivec2 source = ivec2(int(entry & 0xFFu), int((entry >> 8) & 0xFFu) + (native.y >= 192 ? 192 : 0));

    if (source == native)
        return rowCoord;
    if (source.y != native.y && (int(texelFetch(ScreenTex, ivec2(256*3, source.y), 0).b) & 0x3) != dispmode)
        return rowCoord;
    return source;
}

// the 3D pixel at this sub-pixel. in the middle-only program, from its exact position, except that
// the identity mode repeats the filter off's maths at mediump. the wide program is already highp
ivec4 FilterFetch3D(float xscroll, highp ivec2 subPixel)
{
#ifdef WIDE
    return Fetch3D(fTexcoord.x + xscroll, mod(fTexcoord.y, 192.0));
#else
    if (uFilterIdentity != 0)
    {
        mediump float scroll = xscroll;
        mediump float scale = float(u3DScale);
        mediump vec2 pos = vec2(fTexcoord.x + scroll, mod(fTexcoord.y, 192.0)) * scale;
        return ivec4(texelFetch(_3DTex, ivec2(pos), 0).bgra * vec4(63,63,63,31));
    }

    highp vec2 pos = (vec2(subPixel) + 0.5) / float(u3DScale);
    return Fetch3D(pos.x + xscroll, mod(pos.y, 192.0));
#endif
}
#endif

void main()
{
#ifdef WIDE
    if (fTexcoord.x < 0.0 || fTexcoord.x >= 256.0)
    {
        // a held line (control word bit 13, set by GLCompositor::PrepareWingHold in the copy
        // of the control words it uploads) takes the wings from an earlier frame's output as
        // they are. HeldWingsTex has the left wing, then the right one, at the output's rows.
        // the flag isn't a uniform bit mask: Adreno dropped the hold for lines 0-31 when that
        // was read as uWingHold[line >> 7][(line >> 5) & 3]
        ivec4 ctrl = ivec4(texelFetch(ScreenTex, ivec2(256*3, int(fTexcoord.y)), 0));
        if ((ctrl.g & 0x20) != 0)
        {
            ivec2 coord = ivec2(gl_FragCoord.xy);
            int wingW = uWideOffset * int(u3DScale);
            if (coord.x >= wingW)
                coord.x -= 256 * int(u3DScale);
            oColor = vec4(texelFetch(HeldWingsTex, coord, 0).rgb, 1.0);
            return;
        }

        ivec4 wing = WingPixel();
        wing.rgb <<= 2;
        wing.rgb |= (wing.rgb >> 6);
        oColor = vec4(vec3(wing.bgr) / 255.0, 1.0);
        return;
    }
#endif

#ifdef FILTER2D
    highp ivec2 subPixel = FilterSubPixel();
    highp ivec2 native = subPixel / int(u3DScale);
    ivec2 rowCoord = FilterRowCoord(native);
    ivec4 mbright = ivec4(texelFetch(ScreenTex, ivec2(256*3, rowCoord.y), 0));
    int dispmode = mbright.b & 0x3;
    ivec2 planeCoord = FilterPlaneCoord(subPixel, native, rowCoord, dispmode);
#else
    ivec4 mbright = ivec4(texelFetch(ScreenTex, ivec2(256*3, int(fTexcoord.y)), 0));
    int dispmode = mbright.b & 0x3;
    ivec2 planeCoord = ivec2(fTexcoord);
#endif
    ivec4 pixel = ivec4(texelFetch(ScreenTex, planeCoord, 0));

    // mbright.a == HOFS bit0..7
    // mbright.b bit7 == HOFS bit8 (sign)
    float _3dxpos = float(mbright.a - ((mbright.b & 0x80) * 2));

    if (uHiresCap != 0 && (mbright.r & 0x80) != 0)
    {
        // from a hi-res image, whose rows are the bank rows (display line L shows row L). the
        // output has 2 padding lines between the halves, so the second half's rows start at 194
        // lines, and the wide output's wing comes before the middle
        highp ivec2 coord = ivec2(gl_FragCoord.xy);
        int scale = int(u3DScale);
        if (coord.y >= 194 * scale)
            coord.y -= 194 * scale;
#ifdef WIDE
        coord.x -= uWideOffset * scale;
#endif
        coord = clamp(coord, ivec2(0), ivec2(256 * scale - 1, 192 * scale - 1));

        // unpacked to 6 bits like the native VRAM display line (GPU2D::SoftRenderer::DrawScanline)
        highp uint word;
        if ((mbright.r & 0x20) != 0)
            word = texelFetch(HiresStreamTex, coord, 0).r;
        else
            word = texelFetch(HiresBankTex, coord, 0).r;
        pixel = ivec4(int(word & 0x1Fu) << 1, int((word >> 5) & 0x1Fu) << 1, int((word >> 10) & 0x1Fu) << 1, 0);
    }
    else if (dispmode == 1)
    {
        ivec4 val2 = ivec4(texelFetch(ScreenTex, planeCoord + ivec2(256,0), 0));
        ivec4 val3 = ivec4(texelFetch(ScreenTex, planeCoord + ivec2(512,0), 0));

        // the 3D pixel, for the compmodes that show 3D (0-4)
        ivec4 _3dpix = ivec4(0);
        if ((val3.a & 0xF) <= 4)
#ifdef FILTER2D
            _3dpix = FilterFetch3D(_3dxpos, subPixel);
#else
            _3dpix = Fetch3D(fTexcoord.x + _3dxpos, mod(fTexcoord.y, 192.0));
#endif

        bool opaque;
        pixel = CompositePixel(pixel, val2, val3, _3dpix, opaque);
    }

    if (dispmode != 0)
    {
        int brightmode = mbright.g >> 6;
        if (brightmode == 1)
        {
            // up
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel += ((0x3F - pixel) * evy) >> 4;
        }
        else if (brightmode == 2)
        {
            // down
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel -= ((pixel * evy) + 0xF) >> 4;
        }
    }

    pixel.rgb <<= 2;
    pixel.rgb |= (pixel.rgb >> 6);

    // TODO: filters

    oColor = vec4(vec3(pixel.bgr) / 255.0, 1.0);
}
)";

// hi-res display capture: the capture pass (GLCompositor::RunHiresCapture) redoes a frame's display
// capture at the 3D scale into a bank copy, a GL_R16UI texture of 256 x 256 bank rows at the
// scale. it draws one triangle over the viewport, which covers the captured rows
const char* kHiresCaptureVS = R"(#version 320 es

void main()
{
    vec2 pos = vec2(float((gl_VertexID & 1) << 2), float((gl_VertexID & 2) << 1)) - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

// the capture program's fragment shader is kHiresCaptureFS_Head + kCompositePixelFS +
// kHiresCaptureFS_Main. it makes the bank word DoCapture (GPU2D_Soft.cpp) would make for this
// hi-res texel, with the same integer maths, so at 1x it should match the native capture exactly
const char* kHiresCaptureFS_Head = R"(#version 320 es

precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;

uniform int uScale;
uniform int uWideOffset;    // native columns the 3D target has on each side of the middle 256
uniform int uDstRow;        // the bank row the capture starts at

// per capture line: the 3 planes (columns 0-767) as GPU::HiresCapRecord has them, then 2 control
// texels: DISPCAPCNT, and the line's info from GLCompositor::RunHiresCapture
uniform usampler2D PlanesTex;
uniform sampler2D _3DTex;
uniform usampler2D SrcBTex;     // the native source B words of each line
uniform usampler2D BankBTex;    // the hi-res copy of the source B bank, for lines that use it

layout(location = 0) out uvec4 oWord;
)";

const char* kHiresCaptureFS_Main = R"(
// the 3D pixel at hi-res column x of the middle 256 and hi-res line y, decoded to 6-bit colour and
// 5-bit alpha as the capture readbacks do (GLRenderer::GetLine shifts the bytes right by 2 and 3,
// the compute renderer writes c/63 and a/31). classic GL draws polygons in float, so the bytes
// are arbitrary and a c/63 round trip isn't exact. outside the middle 256 there's no 3D, like the
// scrolled native line (GPU3D::GetLine)
ivec4 Fetch3DCapture(int x, int y)
{
    if (x < 0 || x >= 256 * uScale)
        return ivec4(0);

    vec4 texel = texelFetch(_3DTex, ivec2(x + uWideOffset * uScale, y), 0);
    return ivec4(round(texel.bgra * 255.0)) >> ivec4(2, 2, 2, 3);
}

uint TexelWord(uvec4 texel)
{
    return texel.r | (texel.g << 8) | (texel.b << 16) | (texel.a << 24);
}

void main()
{
    ivec2 frag = ivec2(gl_FragCoord.xy);
    int row = frag.y - uDstRow * uScale;    // hi-res row in the captured rectangle
    int line = row / uScale;                // the capture line
    int subrow = row - line * uScale;
    int x = frag.x / uScale;                // native column

    uint capcnt = TexelWord(texelFetch(PlanesTex, ivec2(768, line), 0));
    // bits 0-8: the 3D X scroll, 9: source B from BankBTex, 10: DoCapture had a source B,
    // 16-23: the BankBTex row
    uint linectrl = TexelWord(texelFetch(PlanesTex, ivec2(769, line), 0));
    uint mode = (capcnt >> 29) & 0x3u;

    // source A: the 3D layer alone, or the composite of the 2D planes and the 3D
    ivec4 colA = ivec4(0);
    bool alphaA = false;
    if (mode != 1u)
    {
        int xscroll = int(linectrl & 0x1FFu);
        if (xscroll >= 256)
            xscroll -= 512;
        int x3d = frag.x + xscroll * uScale;

        if ((capcnt & (1u << 24)) != 0u)
        {
            colA = Fetch3DCapture(x3d, row);
            alphaA = colA.a != 0;
        }
        else
        {
            ivec4 val1 = ivec4(texelFetch(PlanesTex, ivec2(x, line), 0));
            ivec4 val2 = ivec4(texelFetch(PlanesTex, ivec2(x + 256, line), 0));
            ivec4 val3 = ivec4(texelFetch(PlanesTex, ivec2(x + 512, line), 0));

            ivec4 _3dpix = ivec4(0);
            if ((val3.a & 0xF) <= 4)
                _3dpix = Fetch3DCapture(x3d, row);

            colA = CompositePixel(val1, val2, val3, _3dpix, alphaA);
        }
    }

    // source B: the hi-res copy of the bank, or the native words upscaled
    bool hasB = (linectrl & (1u << 10)) != 0u;
    uint wordB = 0u;
    if (mode != 0u && hasB)
    {
        if ((linectrl & (1u << 9)) != 0u)
            wordB = texelFetch(BankBTex, ivec2(frag.x, int((linectrl >> 16) & 0xFFu) * uScale + subrow), 0).r;
        else
            wordB = texelFetch(SrcBTex, ivec2(x, line), 0).r;
    }

    uint rA = uint(colA.r >> 1) & 0x1Fu;
    uint gA = uint(colA.g >> 1) & 0x1Fu;
    uint bA = uint(colA.b >> 1) & 0x1Fu;
    uint aA = alphaA ? 1u : 0u;

    uint word;
    if (mode == 0u)
    {
        word = rA | (gA << 5) | (bA << 10) | (aA << 15);
    }
    else if (mode == 1u)
    {
        // zero without a source B, as DoCapture writes then
        word = wordB;
    }
    else
    {
        uint eva = min(capcnt & 0x1Fu, 16u);
        uint evb = min((capcnt >> 8) & 0x1Fu, 16u);

        if (hasB)
        {
            uint rB = wordB & 0x1Fu;
            uint gB = (wordB >> 5) & 0x1Fu;
            uint bB = (wordB >> 10) & 0x1Fu;
            uint aB = wordB >> 15;

            uint rD = min(((rA * aA * eva) + (rB * aB * evb) + 8u) >> 4, 0x1Fu);
            uint gD = min(((gA * aA * eva) + (gB * aB * evb) + 8u) >> 4, 0x1Fu);
            uint bD = min(((bA * aA * eva) + (bB * aB * evb) + 8u) >> 4, 0x1Fu);
            uint aD = (eva > 0u ? aA : 0u) | (evb > 0u ? aB : 0u);

            word = rD | (gD << 5) | (bD << 10) | (aD << 15);
        }
        else
        {
            // DoCapture's branch for a source B bank that isn't in LCDC mode: no B term, and the
            // alpha only from A
            uint rD = ((rA * aA * eva) + 8u) >> 4;
            uint gD = ((gA * aA * eva) + 8u) >> 4;
            uint bD = ((bA * aA * eva) + 8u) >> 4;
            uint aD = eva > 0u ? aA : 0u;

            word = rD | (gD << 5) | (bD << 10) | (aD << 15);
        }
    }

    // the mask is for Adreno 840 (driver 0842.19.11): without it, a word with bit 15 set has high
    // bits set by here, which the R16UI target saturates to 0xFFFF, so anything opaque came out white
    oWord = uvec4(word & 0xFFFFu, 0u, 0u, 0u);
}
)";

// filtered 2D (GLCompositor::RunFilter2D). both programs draw kHiresCaptureVS's triangle over their
// target, and write a texel of two words:
// - r: the native pixel it shows, as x | (y << 8), y counted from the top of its screen, since a
//   map entry never leaves the sub-pixel's screen. this keeps a map in R16UI.
// - g: the key MMPX compares, the color in 6 bits per channel (bits 0-17) and:
//     bit 18: a 3D slot, where the color is what shows if the 3D is transparent there;
//     bit 19: a line that stays as it is. the rest of the key is then the pixel's position,
//             so no two pixels on such lines compare equal and MMPX can't pick them or change them.
const char* kFilter2DKeyFS = R"(#version 320 es

precision highp float;
precision highp int;
precision highp usampler2D;

uniform usampler2D ScreenTex;
uniform int uHiresCap;

layout(location = 0) out uvec4 oTexel;

void main()
{
    ivec2 pos = ivec2(gl_FragCoord.xy);
    uvec4 ctrl = texelFetch(ScreenTex, ivec2(256*3, pos.y), 0);
    uint dispmode = ctrl.b & 0x3u;

    uint key;
    if (dispmode >= 2u || (uHiresCap != 0 && (ctrl.r & 0x80u) != 0u))
    {
        key = 0x80000u | uint(pos.x) | (uint(pos.y) << 8);
    }
    else
    {
        // dispmode 0 only reads plane 0
        uvec4 color = texelFetch(ScreenTex, pos, 0);
        uint slot3D = 0u;
        if (dispmode == 1u && (texelFetch(ScreenTex, pos + ivec2(512, 0), 0).a & 0xFu) <= 4u)
        {
            color = texelFetch(ScreenTex, pos + ivec2(256, 0), 0);
            slot3D = 0x40000u;
        }

        key = (color.r & 0x3Fu) | ((color.g & 0x3Fu) << 6) | ((color.b & 0x3Fu) << 12) | slot3D;
    }

    oTexel = uvec4(uint(pos.x) | (uint(pos.y % 192) << 8), key, 0u, 0u);
}
)";

// MMPX
// by Morgan McGuire and Mara Gagiu
// https://casual-effects.com/research/McGuire2021PixelArt/
// License: MIT
//
// Copyright 2020 Morgan McGuire & Mara Gagiu
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// adapted for glsl by hunterk
//
// the rules are the same as the app's MmpxPrePassShader (ShaderProgramSource.kt), which only ever
// assigns a neighbor to J, K, L or M and never blends. here a "color" is a texel from the previous
// pass (see above): the comparisons and the luminances use its key, and what gets picked carries
// the native pixel with it. the output is twice the size of SrcTex.
const char* kFilter2DMmpxFS = R"(#version 320 es

precision highp float;
precision highp int;
precision highp usampler2D;

uniform usampler2D SrcTex;
uniform int uSrcW;      // SrcTex's width, 256 or 512
uniform int uScreenH;   // the rows one screen has in SrcTex, half its height
uniform int uIdentity;  // write every sub-pixel's own native pixel (debug.melonade.filter2d 3)

layout(location = 0) out uvec4 oTexel;

ivec2 gFrag;    // this texel in the output
ivec2 gPos;     // this texel's pixel in SrcTex
int gRowMin;    // SrcTex's first row of this pixel's screen

// the source texel x, y texels away from this one (y grows downwards, rows are DS lines), kept
// inside this pixel's screen. x is the native pixel, y the key
uvec2 src(int x, int y)
{
    ivec2 pos = clamp(gPos + ivec2(x, y), ivec2(0, gRowMin), ivec2(uSrcW - 1, gRowMin + uScreenH - 1));
    return texelFetch(SrcTex, pos, 0).rg;
}

// BT.709 scaled to integers. a line that stays as it is never counts as darker, so the rules that
// pick a darker neighbor without comparing it to anything else (the tips below) can't pick it
int luma(uvec2 c)
{
    if ((c.y & 0x80000u) != 0u)
        return 0x7FFFFFFF;
    return int(c.y & 0x3Fu) * 2126 + int((c.y >> 6) & 0x3Fu) * 7152 + int((c.y >> 12) & 0x3Fu) * 722;
}

bool same(uvec2 B, uvec2 A0) { return B.y == A0.y; }
bool notsame(uvec2 B, uvec2 A0) { return B.y != A0.y; }
bool all_eq2(uvec2 B, uvec2 A0, uvec2 A1) { return same(B, A0) && same(B, A1); }
bool all_eq3(uvec2 B, uvec2 A0, uvec2 A1, uvec2 A2) { return same(B, A0) && same(B, A1) && same(B, A2); }
bool all_eq4(uvec2 B, uvec2 A0, uvec2 A1, uvec2 A2, uvec2 A3) { return same(B, A0) && same(B, A1) && same(B, A2) && same(B, A3); }
bool any_eq3(uvec2 B, uvec2 A0, uvec2 A1, uvec2 A2) { return same(B, A0) || same(B, A1) || same(B, A2); }
bool none_eq2(uvec2 B, uvec2 A0, uvec2 A1) { return notsame(B, A0) && notsame(B, A1); }
bool none_eq4(uvec2 B, uvec2 A0, uvec2 A1, uvec2 A2, uvec2 A3) { return notsame(B, A0) && notsame(B, A1) && notsame(B, A2) && notsame(B, A3); }

void writeTexel(uvec2 picked)
{
    uint coord = picked.x;
    if (uIdentity != 0)
    {
        ivec2 native = gFrag / (uSrcW / 128);
        coord = uint(native.x) | (uint(native.y % 192) << 8);
    }

    // the mask for the R16UI maps: see the Adreno note at the end of kHiresCaptureFS_Main
    oTexel = uvec4(coord & 0xFFFFu, picked.y, 0u, 0u);
}

void main()
{
    gFrag = ivec2(gl_FragCoord.xy);
    gPos = gFrag >> 1;
    gRowMin = gPos.y >= uScreenH ? uScreenH : 0;

    // Our current pixel
    uvec2 E = src(0, 0);

    // Input: A-I central 3x3 grid
    uvec2 A = src(-1, -1);
    uvec2 B = src(0, -1);
    uvec2 C = src(1, -1);

    uvec2 D = src(-1, 0);
    uvec2 F = src(1, 0);

    uvec2 G = src(-1, 1);
    uvec2 H = src(0, 1);
    uvec2 I = src(1, 1);

    // Default to nearest magnification
    uvec2 J = E;
    uvec2 K = E;
    uvec2 L = E;
    uvec2 M = E;

    // Skip constant 3x3 centers and just use nearest-neighbor them
    if (same(E, A) && same(E, B) && same(E, C) && same(E, D) && same(E, F) && same(E, G) && same(E, H) && same(E, I))
    {
        writeTexel(E);
        return;
    }

    // Read additional values at the tips of the diamond pattern
    uvec2 P = src(0, -2);
    uvec2 Q = src(-2, 0);
    uvec2 R = src(2, 0);
    uvec2 S = src(0, 2);

    // Precompute luminances
    int Bl = luma(B);
    int Dl = luma(D);
    int El = luma(E);
    int Fl = luma(F);
    int Hl = luma(H);

    // Round some corners and fill in 1:1 slopes, but preserve sharp right angles. In each expression the 1st clause is from EPX, the 2nd
    // avoids double diagonal filling on 1:1 slopes (luminance breaks the tie for thick features), the 3rd preserves square corners and the
    // 4th keeps 1-pixel bumps on straight lines that are darker than their background (like the tail of a pixel art 4)
    if (((same(D, B) && notsame(D, H) && notsame(D, F))) && ((El >= Dl) || same(E, A)) && any_eq3(E, A, C, G) && ((El < Dl) || notsame(A, D) || notsame(E, P) || notsame(E, Q))) J = D;
    if (((same(B, F) && notsame(B, D) && notsame(B, H))) && ((El >= Bl) || same(E, C)) && any_eq3(E, A, C, I) && ((El < Bl) || notsame(C, B) || notsame(E, P) || notsame(E, R))) K = B;
    if (((same(H, D) && notsame(H, F) && notsame(H, B))) && ((El >= Hl) || same(E, G)) && any_eq3(E, A, G, I) && ((El < Hl) || notsame(G, H) || notsame(E, S) || notsame(E, Q))) L = H;
    if (((same(F, H) && notsame(F, B) && notsame(F, D))) && ((El >= Fl) || same(E, I)) && any_eq3(E, C, G, I) && ((El < Fl) || notsame(I, H) || notsame(E, R) || notsame(E, S))) M = F;

    // Clean up disconnected line intersections. The 1st clause recognizes being on the inside of a diagonal corner, the 2nd makes sure
    // that it is not a notch at the edge of a checkerboard dithering pattern
    if ((notsame(E, F) && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && notsame(F, src(3, 0))) K = M = F;
    if ((notsame(E, D) && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && notsame(D, src(-3, 0))) J = L = D;
    if ((notsame(E, H) && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && notsame(H, src(0, 3))) L = M = H;
    if ((notsame(E, B) && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && notsame(B, src(0, -3))) J = K = B;

    // Remove tips of bright triangles on dark backgrounds
    if ((Bl < El) && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
    if ((Hl < El) && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
    if ((Fl < El) && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
    if ((Dl < El) && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

    // Further neighborhood peeking to identify 2:1 and 1:2 slopes of constant color. These extend a color assigned by the rules above
    // instead of drawing from the source image
    if (notsame(H, B))
    {
        // Above a 2:1 slope or -2:1 slope
        if (notsame(H, A) && notsame(H, E) && notsame(H, C))
        {
            if (all_eq3(H, G, F, R) && none_eq2(H, D, src(2, -1))) L = M;
            if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(-2, -1))) M = L;
        }

        // Below a 2:1 or -2:1 slope (the above patterns reflected vertically)
        if (notsame(B, I) && notsame(B, G) && notsame(B, E))
        {
            if (all_eq3(B, A, F, R) && none_eq2(B, D, src(2, 1))) J = K;
            if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(-2, 1))) K = J;
        }
    }

    if (notsame(F, D))
    {
        // Right of a -1:2 or 1:2 slope (the left patterns below reflected horizontally)
        if (notsame(D, I) && notsame(D, E) && notsame(D, C))
        {
            if (all_eq3(D, A, H, S) && none_eq2(D, B, src(1, 2))) J = L;
            if (all_eq3(D, G, B, P) && none_eq2(D, H, src(1, -2))) L = J;
        }

        // Left of a 1:2 or -1:2 slope (the 2:1 patterns above transposed)
        if (notsame(F, E) && notsame(F, A) && notsame(F, G))
        {
            if (all_eq3(F, C, H, S) && none_eq2(F, B, src(-1, 2))) K = M;
            if (all_eq3(F, I, B, P) && none_eq2(F, H, src(-1, -2))) M = K;
        }
    }

    // Determine which of our 4 output pixels we need to use
    bool left = (gFrag.x & 1) == 0;
    bool top = (gFrag.y & 1) == 0;
    writeTexel(left ? (top ? J : L) : (top ? K : M));
}
)";



const char* kCompositorFS_Linear = R"(#version 140

uniform uint u3DScale;

uniform usampler2D ScreenTex;
uniform sampler2D _3DTex;

smooth in vec2 fTexcoord;

out vec4 oColor;

ivec4 Get3DPixel(vec2 pos)
{
    return ivec4(texelFetch(_3DTex, ivec2(pos*u3DScale), 0).bgra
         * vec4(63,63,63,31));
}

ivec4 GetFullPixel(ivec4 val1, ivec4 val2, ivec4 val3, ivec4 _3dpix)
{
    int compmode = val3.a & 0xF;
    int eva, evb, evy;

    if (compmode == 4)
    {
        // 3D on top, blending

        if (_3dpix.a > 0)
        {
            eva = (_3dpix.a & 0x1F) + 1;
            evb = 32 - eva;

            val1 = ((_3dpix * eva) + (val1 * evb)) >> 5;
            if (eva <= 16) val1 += ivec4(1,1,1,0);
            val1 = min(val1, 0x3F);
        }
        else
            val1 = val2;
    }
    else if (compmode == 1)
    {
        // 3D on bottom, blending

        if (_3dpix.a > 0)
        {
            eva = val3.g;
            evb = val3.b;

            val1 = ((val1 * eva) + (_3dpix * evb)) >> 4;
            val1 = min(val1, 0x3F);
        }
        else
            val1 = val2;
    }
    else if (compmode <= 3)
    {
        // 3D on top, normal/fade

        if (_3dpix.a > 0)
        {
            evy = val3.g;

            val1 = _3dpix;
            if      (compmode == 2) val1 += ((ivec4(0x3F,0x3F,0x3F,0) - val1) * evy) >> 4;
            else if (compmode == 3) val1 -= (val1 * evy) >> 4;
        }
        else
            val1 = val2;
    }

    return val1;
}

ivec4 imix(ivec4 a, ivec4 b, float x)
{
    return ivec4(vec4(a)*(1-x) + vec4(b)*x);
}

void main()
{
    ivec4 pixel = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord), 0));

    ivec4 mbright = ivec4(texelFetch(ScreenTex, ivec2(256*3, int(fTexcoord.y)), 0));
    int dispmode = mbright.b & 0x3;

    if (dispmode == 1)
    {
        ivec4 val1 = pixel;
        ivec4 val2 = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(256,0), 0));
        ivec4 val3 = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(512,0), 0));

        float xfract = fract(fTexcoord.x);
        float yfract = fract(fTexcoord.y);

        float xpos = val3.r + xfract;
        float ypos = mod(fTexcoord.y, 192);
        ivec4 _3dpix = Get3DPixel(vec2(xpos,ypos));

        ivec4 p00 = GetFullPixel(val1, val2, val3, _3dpix);

        int xdisp = 1 - int(step(255, fTexcoord.x));
        int ydisp = 1 - int(step(191, ypos));

        ivec4 p01 = GetFullPixel(ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+0  ,0), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+256,0), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+512,0), 0)),
                                 _3dpix);

        ivec4 p10 = GetFullPixel(ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(0+0  ,ydisp), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(0+256,ydisp), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(0+512,ydisp), 0)),
                                 _3dpix);

        ivec4 p11 = GetFullPixel(ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+0  ,ydisp), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+256,ydisp), 0)),
                                 ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(xdisp+512,ydisp), 0)),
                                 _3dpix);

        ivec4 pa = imix(p00, p01, xfract);
        ivec4 pb = imix(p10, p11, xfract);

        pixel = imix(pa, pb, yfract);
    }

    if (dispmode != 0)
    {
        int brightmode = mbright.g >> 6;
        if (brightmode == 1)
        {
            // up
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel += ((ivec4(0x3F,0x3F,0x3F,0) - pixel) * evy) >> 4;
        }
        else if (brightmode == 2)
        {
            // down
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel -= (pixel * evy) >> 4;
        }
    }

    pixel.rgb <<= 2;
    pixel.rgb |= (pixel.rgb >> 6);

    // TODO: filters

    oColor = vec4(vec3(pixel.bgr) / 255.0, 1.0);
}
)";






// HUGE TEST ZONE ARRLGD

const char* kCompositorVS_xBRZ = R"(#version 140

#define BLEND_NONE 0
#define BLEND_NORMAL 1
#define BLEND_DOMINANT 2
#define LUMINANCE_WEIGHT 1.0
#define EQUAL_COLOR_TOLERANCE 30.0/255.0
#define STEEP_DIRECTION_THRESHOLD 2.2
#define DOMINANT_DIRECTION_THRESHOLD 3.6

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec2 vPosition;
COMPAT_VARYING vec4 TEX0;
COMPAT_VARYING vec4 t1;
COMPAT_VARYING vec4 t2;
COMPAT_VARYING vec4 t3;
COMPAT_VARYING vec4 t4;
COMPAT_VARYING vec4 t5;
COMPAT_VARYING vec4 t6;
COMPAT_VARYING vec4 t7;

uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;

// vertex compatibility #defines
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define outsize vec4(OutputSize, 1.0 / OutputSize)

void main()
{
    vec4 fpos;
    fpos.xy = vPosition;
    fpos.z = 0.0;
    fpos.w = 1.0;

    gl_Position = fpos;
    vec2 TexCoord = (vPosition + vec2(1.0, 1.0)) * (vec2(256.0, 384.0) / 2.0);


    //gl_Position = MVPMatrix * VertexCoord;
    //COL0 = COLOR;
    TEX0.xy = TexCoord.xy;
	vec2 ps = vec2(1,1);//vec2(SourceSize.z, SourceSize.w);
	float dx = ps.x;
	float dy = ps.y;

	 //  A1 B1 C1
	// A0 A  B  C C4
	// D0 D  E  F F4
	// G0 G  H  I I4
	 //  G5 H5 I5

	t1 = vTexCoord.xxxy + vec4( -dx, 0.0, dx,-2.0*dy); // A1 B1 C1
	t2 = vTexCoord.xxxy + vec4( -dx, 0.0, dx, -dy);    //  A  B  C
	t3 = vTexCoord.xxxy + vec4( -dx, 0.0, dx, 0.0);    //  D  E  F
	t4 = vTexCoord.xxxy + vec4( -dx, 0.0, dx, dy);     //  G  H  I
	t5 = vTexCoord.xxxy + vec4( -dx, 0.0, dx, 2.0*dy); // G5 H5 I5
	t6 = vTexCoord.xyyy + vec4(-2.0*dx,-dy, 0.0, dy);  // A0 D0 G0
	t7 = vTexCoord.xyyy + vec4( 2.0*dx,-dy, 0.0, dy);  // C4 F4 I4
}
)";

const char* kCompositorFS_xBRZ = R"(#version 140

#define BLEND_NONE 0
#define BLEND_NORMAL 1
#define BLEND_DOMINANT 2
#define LUMINANCE_WEIGHT 1.0
#define EQUAL_COLOR_TOLERANCE 30.0/255.0
#define STEEP_DIRECTION_THRESHOLD 2.2
#define DOMINANT_DIRECTION_THRESHOLD 3.6

#if __VERSION__ >= 130
#define COMPAT_VARYING in
//#define COMPAT_TEXTURE texture
#define FragColor oColor
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
//#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

uniform uint u3DScale;

uniform usampler2D ScreenTex;
uniform sampler2D _3DTex;

smooth in vec2 fTexcoord;

out vec4 oColor;

//uniform COMPAT_PRECISION vec2 OutputSize;
//uniform COMPAT_PRECISION vec2 TextureSize;
#define TextureSize vec2(256,384)
//uniform COMPAT_PRECISION vec2 InputSize;
//uniform sampler2D Texture;
#define Texture 1312
COMPAT_VARYING vec4 TEX0;
COMPAT_VARYING vec4 t1;
COMPAT_VARYING vec4 t2;
COMPAT_VARYING vec4 t3;
COMPAT_VARYING vec4 t4;
COMPAT_VARYING vec4 t5;
COMPAT_VARYING vec4 t6;
COMPAT_VARYING vec4 t7;

// fragment compatibility #defines
#define Source Texture
#define vTexCoord TEX0.xy

#define SourceSize vec4(TextureSize, 1.0 / TextureSize) //either TextureSize or InputSize
#define outsize vec4(OutputSize, 1.0 / OutputSize)

	const float  one_sixth = 1.0 / 6.0;
	const float  two_sixth = 2.0 / 6.0;
	const float four_sixth = 4.0 / 6.0;
	const float five_sixth = 5.0 / 6.0;

vec4 Get2DPixel(vec2 texcoord, int level)
{
    ivec4 pixel = ivec4(texelFetch(ScreenTex, ivec2(texcoord) + ivec2(level*256,0), 0));

    return vec4(pixel) / vec4(63.0, 63.0, 63.0, 31.0);
}

ivec4 Get3DPixel(vec2 pos)
{
    return ivec4(texelFetch(_3DTex, ivec2(pos*u3DScale), 0).bgra
         * vec4(63,63,63,31));
}

float reduce(const vec3 color)
{
    return dot(color, vec3(65536.0, 256.0, 1.0));
}

float DistYCbCr(const vec3 pixA, const vec3 pixB)
{
    const vec3 w = vec3(0.2627, 0.6780, 0.0593);
    const float scaleB = 0.5 / (1.0 - w.b);
    const float scaleR = 0.5 / (1.0 - w.r);
    vec3 diff = pixA - pixB;
    float Y = dot(diff, w);
    float Cb = scaleB * (diff.b - Y);
    float Cr = scaleR * (diff.r - Y);

    return sqrt( ((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr) );
}

bool IsPixEqual(const vec3 pixA, const vec3 pixB)
{
    return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);
}

bool IsBlendingNeeded(const ivec4 blend)
{
    return any(notEqual(blend, ivec4(BLEND_NONE)));
}

//---------------------------------------
// Input Pixel Mapping:    --|21|22|23|--
//                         19|06|07|08|09
//                         18|05|00|01|10
//                         17|04|03|02|11
//                         --|15|14|13|--
//
// Output Pixel Mapping: 20|21|22|23|24|25
//                       19|06|07|08|09|26
//                       18|05|00|01|10|27
//                       17|04|03|02|11|28
//                       16|15|14|13|12|29
//                       35|34|33|32|31|30

ivec4 GetFiltered2DPixel(int level)
{
    vec2 f = fract(vTexCoord.xy);// * SourceSize.xy);

    //---------------------------------------
    // Input Pixel Mapping:  20|21|22|23|24
    //                       19|06|07|08|09
    //                       18|05|00|01|10
    //                       17|04|03|02|11
    //                       16|15|14|13|12

    vec3 src[25];

    src[21] = Get2DPixel(t1.xw, level).rgb;
    src[22] = Get2DPixel(t1.yw, level).rgb;
    src[23] = Get2DPixel(t1.zw, level).rgb;
    src[ 6] = Get2DPixel(t2.xw, level).rgb;
    src[ 7] = Get2DPixel(t2.yw, level).rgb;
    src[ 8] = Get2DPixel(t2.zw, level).rgb;
    src[ 5] = Get2DPixel(t3.xw, level).rgb;
    src[ 0] = Get2DPixel(t3.yw, level).rgb;
    src[ 1] = Get2DPixel(t3.zw, level).rgb;
    src[ 4] = Get2DPixel(t4.xw, level).rgb;
    src[ 3] = Get2DPixel(t4.yw, level).rgb;
    src[ 2] = Get2DPixel(t4.zw, level).rgb;
    src[15] = Get2DPixel(t5.xw, level).rgb;
    src[14] = Get2DPixel(t5.yw, level).rgb;
    src[13] = Get2DPixel(t5.zw, level).rgb;
    src[19] = Get2DPixel(t6.xy, level).rgb;
    src[18] = Get2DPixel(t6.xz, level).rgb;
    src[17] = Get2DPixel(t6.xw, level).rgb;
    src[ 9] = Get2DPixel(t7.xy, level).rgb;
    src[10] = Get2DPixel(t7.xz, level).rgb;
    src[11] = Get2DPixel(t7.xw, level).rgb;

    float v[9];
    v[0] = reduce(src[0]);
    v[1] = reduce(src[1]);
    v[2] = reduce(src[2]);
    v[3] = reduce(src[3]);
    v[4] = reduce(src[4]);
    v[5] = reduce(src[5]);
    v[6] = reduce(src[6]);
    v[7] = reduce(src[7]);
    v[8] = reduce(src[8]);

    ivec4 blendResult = ivec4(BLEND_NONE);

    // Preprocess corners
    // Pixel Tap Mapping: --|--|--|--|--
    //                    --|--|07|08|--
    //                    --|05|00|01|10
    //                    --|04|03|02|11
    //                    --|--|14|13|--
    // Corner (1, 1)
    if ( ((v[0] == v[1] && v[3] == v[2]) || (v[0] == v[3] && v[1] == v[2])) == false)
    {
        float dist_03_01 = DistYCbCr(src[ 4], src[ 0]) + DistYCbCr(src[ 0], src[ 8]) + DistYCbCr(src[14], src[ 2]) + DistYCbCr(src[ 2], src[10]) + (4.0 * DistYCbCr(src[ 3], src[ 1]));
        float dist_00_02 = DistYCbCr(src[ 5], src[ 3]) + DistYCbCr(src[ 3], src[13]) + DistYCbCr(src[ 7], src[ 1]) + DistYCbCr(src[ 1], src[11]) + (4.0 * DistYCbCr(src[ 0], src[ 2]));
        bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_03_01) < dist_00_02;
        blendResult[2] = ((dist_03_01 < dist_00_02) && (v[0] != v[1]) && (v[0] != v[3])) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }

    // Pixel Tap Mapping: --|--|--|--|--
    //                    --|06|07|--|--
    //                    18|05|00|01|--
    //                    17|04|03|02|--
    //                    --|15|14|--|--
    // Corner (0, 1)
    if ( ((v[5] == v[0] && v[4] == v[3]) || (v[5] == v[4] && v[0] == v[3])) == false)
    {
        float dist_04_00 = DistYCbCr(src[17], src[ 5]) + DistYCbCr(src[ 5], src[ 7]) + DistYCbCr(src[15], src[ 3]) + DistYCbCr(src[ 3], src[ 1]) + (4.0 * DistYCbCr(src[ 4], src[ 0]));
        float dist_05_03 = DistYCbCr(src[18], src[ 4]) + DistYCbCr(src[ 4], src[14]) + DistYCbCr(src[ 6], src[ 0]) + DistYCbCr(src[ 0], src[ 2]) + (4.0 * DistYCbCr(src[ 5], src[ 3]));
        bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_05_03) < dist_04_00;
        blendResult[3] = ((dist_04_00 > dist_05_03) && (v[0] != v[5]) && (v[0] != v[3])) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }

    // Pixel Tap Mapping: --|--|22|23|--
    //                    --|06|07|08|09
    //                    --|05|00|01|10
    //                    --|--|03|02|--
    //                    --|--|--|--|--
    // Corner (1, 0)
    if ( ((v[7] == v[8] && v[0] == v[1]) || (v[7] == v[0] && v[8] == v[1])) == false)
    {
        float dist_00_08 = DistYCbCr(src[ 5], src[ 7]) + DistYCbCr(src[ 7], src[23]) + DistYCbCr(src[ 3], src[ 1]) + DistYCbCr(src[ 1], src[ 9]) + (4.0 * DistYCbCr(src[ 0], src[ 8]));
        float dist_07_01 = DistYCbCr(src[ 6], src[ 0]) + DistYCbCr(src[ 0], src[ 2]) + DistYCbCr(src[22], src[ 8]) + DistYCbCr(src[ 8], src[10]) + (4.0 * DistYCbCr(src[ 7], src[ 1]));
        bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_07_01) < dist_00_08;
        blendResult[1] = ((dist_00_08 > dist_07_01) && (v[0] != v[7]) && (v[0] != v[1])) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }

    // Pixel Tap Mapping: --|21|22|--|--
    //                    19|06|07|08|--
    //                    18|05|00|01|--
    //                    --|04|03|--|--
    //                    --|--|--|--|--
    // Corner (0, 0)
    if ( ((v[6] == v[7] && v[5] == v[0]) || (v[6] == v[5] && v[7] == v[0])) == false)
    {
        float dist_05_07 = DistYCbCr(src[18], src[ 6]) + DistYCbCr(src[ 6], src[22]) + DistYCbCr(src[ 4], src[ 0]) + DistYCbCr(src[ 0], src[ 8]) + (4.0 * DistYCbCr(src[ 5], src[ 7]));
        float dist_06_00 = DistYCbCr(src[19], src[ 5]) + DistYCbCr(src[ 5], src[ 3]) + DistYCbCr(src[21], src[ 7]) + DistYCbCr(src[ 7], src[ 1]) + (4.0 * DistYCbCr(src[ 6], src[ 0]));
        bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_05_07) < dist_06_00;
        blendResult[0] = ((dist_05_07 < dist_06_00) && (v[0] != v[5]) && (v[0] != v[7])) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }

    vec3 dst[16];
    dst[ 0] = src[0];
    dst[ 1] = src[0];
    dst[ 2] = src[0];
    dst[ 3] = src[0];
    dst[ 4] = src[0];
    dst[ 5] = src[0];
    dst[ 6] = src[0];
    dst[ 7] = src[0];
    dst[ 8] = src[0];
    dst[ 9] = src[0];
    dst[10] = src[0];
    dst[11] = src[0];
    dst[12] = src[0];
    dst[13] = src[0];
    dst[14] = src[0];
    dst[15] = src[0];

    // Scale pixel
    if (IsBlendingNeeded(blendResult) == true)
    {
        float dist_01_04 = DistYCbCr(src[1], src[4]);
        float dist_03_08 = DistYCbCr(src[3], src[8]);
        bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_01_04 <= dist_03_08) && (v[0] != v[4]) && (v[5] != v[4]);
        bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_03_08 <= dist_01_04) && (v[0] != v[8]) && (v[7] != v[8]);
        bool needBlend = (blendResult[2] != BLEND_NONE);
        bool doLineBlend = (  blendResult[2] >= BLEND_DOMINANT ||
                           ((blendResult[1] != BLEND_NONE && !IsPixEqual(src[0], src[4])) ||
                             (blendResult[3] != BLEND_NONE && !IsPixEqual(src[0], src[8])) ||
                             (IsPixEqual(src[4], src[3]) && IsPixEqual(src[3], src[2]) && IsPixEqual(src[2], src[1]) && IsPixEqual(src[1], src[8]) && IsPixEqual(src[0], src[2]) == false) ) == false );

        vec3 blendPix = ( DistYCbCr(src[0], src[1]) <= DistYCbCr(src[0], src[3]) ) ? src[1] : src[3];
        dst[ 2] = mix(dst[ 2], blendPix, (needBlend && doLineBlend) ? ((haveShallowLine) ? ((haveSteepLine) ? 1.0/3.0 : 0.25) : ((haveSteepLine) ? 0.25 : 0.00)) : 0.00);
        dst[ 9] = mix(dst[ 9], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.25 : 0.00);
        dst[10] = mix(dst[10], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.75 : 0.00);
        dst[11] = mix(dst[11], blendPix, (needBlend) ? ((doLineBlend) ? ((haveSteepLine) ? 1.00 : ((haveShallowLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[12] = mix(dst[12], blendPix, (needBlend) ? ((doLineBlend) ? 1.00 : 0.6848532563) : 0.00);
        dst[13] = mix(dst[13], blendPix, (needBlend) ? ((doLineBlend) ? ((haveShallowLine) ? 1.00 : ((haveSteepLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[14] = mix(dst[14], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.75 : 0.00);
        dst[15] = mix(dst[15], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.25 : 0.00);

        dist_01_04 = DistYCbCr(src[7], src[2]);
        dist_03_08 = DistYCbCr(src[1], src[6]);
        haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_01_04 <= dist_03_08) && (v[0] != v[2]) && (v[3] != v[2]);
        haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_03_08 <= dist_01_04) && (v[0] != v[6]) && (v[5] != v[6]);
        needBlend = (blendResult[1] != BLEND_NONE);
        doLineBlend = (  blendResult[1] >= BLEND_DOMINANT ||
                      !((blendResult[0] != BLEND_NONE && !IsPixEqual(src[0], src[2])) ||
                        (blendResult[2] != BLEND_NONE && !IsPixEqual(src[0], src[6])) ||
                        (IsPixEqual(src[2], src[1]) && IsPixEqual(src[1], src[8]) && IsPixEqual(src[8], src[7]) && IsPixEqual(src[7], src[6]) && !IsPixEqual(src[0], src[8])) ) );

        blendPix = ( DistYCbCr(src[0], src[7]) <= DistYCbCr(src[0], src[1]) ) ? src[7] : src[1];
        dst[ 1] = mix(dst[ 1], blendPix, (needBlend && doLineBlend) ? ((haveShallowLine) ? ((haveSteepLine) ? 1.0/3.0 : 0.25) : ((haveSteepLine) ? 0.25 : 0.00)) : 0.00);
        dst[ 6] = mix(dst[ 6], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.25 : 0.00);
        dst[ 7] = mix(dst[ 7], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.75 : 0.00);
        dst[ 8] = mix(dst[ 8], blendPix, (needBlend) ? ((doLineBlend) ? ((haveSteepLine) ? 1.00 : ((haveShallowLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[ 9] = mix(dst[ 9], blendPix, (needBlend) ? ((doLineBlend) ? 1.00 : 0.6848532563) : 0.00);
        dst[10] = mix(dst[10], blendPix, (needBlend) ? ((doLineBlend) ? ((haveShallowLine) ? 1.00 : ((haveSteepLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[11] = mix(dst[11], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.75 : 0.00);
        dst[12] = mix(dst[12], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.25 : 0.00);

        dist_01_04 = DistYCbCr(src[5], src[8]);
        dist_03_08 = DistYCbCr(src[7], src[4]);
        haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_01_04 <= dist_03_08) && (v[0] != v[8]) && (v[1] != v[8]);
        haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_03_08 <= dist_01_04) && (v[0] != v[4]) && (v[3] != v[4]);
        needBlend = (blendResult[0] != BLEND_NONE);
        doLineBlend = (  blendResult[0] >= BLEND_DOMINANT ||
                      !((blendResult[3] != BLEND_NONE && !IsPixEqual(src[0], src[8])) ||
                        (blendResult[1] != BLEND_NONE && !IsPixEqual(src[0], src[4])) ||
                        (IsPixEqual(src[8], src[7]) && IsPixEqual(src[7], src[6]) && IsPixEqual(src[6], src[5]) && IsPixEqual(src[5], src[4]) && !IsPixEqual(src[0], src[6])) ) );

        blendPix = ( DistYCbCr(src[0], src[5]) <= DistYCbCr(src[0], src[7]) ) ? src[5] : src[7];
        dst[ 0] = mix(dst[ 0], blendPix, (needBlend && doLineBlend) ? ((haveShallowLine) ? ((haveSteepLine) ? 1.0/3.0 : 0.25) : ((haveSteepLine) ? 0.25 : 0.00)) : 0.00);
        dst[15] = mix(dst[15], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.25 : 0.00);
        dst[ 4] = mix(dst[ 4], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.75 : 0.00);
        dst[ 5] = mix(dst[ 5], blendPix, (needBlend) ? ((doLineBlend) ? ((haveSteepLine) ? 1.00 : ((haveShallowLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[ 6] = mix(dst[ 6], blendPix, (needBlend) ? ((doLineBlend) ? 1.00 : 0.6848532563) : 0.00);
        dst[ 7] = mix(dst[ 7], blendPix, (needBlend) ? ((doLineBlend) ? ((haveShallowLine) ? 1.00 : ((haveSteepLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[ 8] = mix(dst[ 8], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.75 : 0.00);
        dst[ 9] = mix(dst[ 9], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.25 : 0.00);


        dist_01_04 = DistYCbCr(src[3], src[6]);
        dist_03_08 = DistYCbCr(src[5], src[2]);
        haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_01_04 <= dist_03_08) && (v[0] != v[6]) && (v[7] != v[6]);
        haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_03_08 <= dist_01_04) && (v[0] != v[2]) && (v[1] != v[2]);
        needBlend = (blendResult[3] != BLEND_NONE);
        doLineBlend = (  blendResult[3] >= BLEND_DOMINANT ||
                      !((blendResult[2] != BLEND_NONE && !IsPixEqual(src[0], src[6])) ||
                        (blendResult[0] != BLEND_NONE && !IsPixEqual(src[0], src[2])) ||
                        (IsPixEqual(src[6], src[5]) && IsPixEqual(src[5], src[4]) && IsPixEqual(src[4], src[3]) && IsPixEqual(src[3], src[2]) && !IsPixEqual(src[0], src[4])) ) );

        blendPix = ( DistYCbCr(src[0], src[3]) <= DistYCbCr(src[0], src[5]) ) ? src[3] : src[5];
        dst[ 3] = mix(dst[ 3], blendPix, (needBlend && doLineBlend) ? ((haveShallowLine) ? ((haveSteepLine) ? 1.0/3.0 : 0.25) : ((haveSteepLine) ? 0.25 : 0.00)) : 0.00);
        dst[12] = mix(dst[12], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.25 : 0.00);
        dst[13] = mix(dst[13], blendPix, (needBlend && doLineBlend && haveSteepLine) ? 0.75 : 0.00);
        dst[14] = mix(dst[14], blendPix, (needBlend) ? ((doLineBlend) ? ((haveSteepLine) ? 1.00 : ((haveShallowLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[15] = mix(dst[15], blendPix, (needBlend) ? ((doLineBlend) ? 1.00 : 0.6848532563) : 0.00);
        dst[ 4] = mix(dst[ 4], blendPix, (needBlend) ? ((doLineBlend) ? ((haveShallowLine) ? 1.00 : ((haveSteepLine) ? 0.75 : 0.50)) : 0.08677704501) : 0.00);
        dst[ 5] = mix(dst[ 5], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.75 : 0.00);
        dst[ 6] = mix(dst[ 6], blendPix, (needBlend && doLineBlend && haveShallowLine) ? 0.25 : 0.00);
    }

    vec3 res = mix( mix( mix( mix(dst[ 6], dst[ 7], step(0.25, f.x)), mix(dst[ 8], dst[ 9], step(0.75, f.x)), step(0.50, f.x)),
                                 mix( mix(dst[ 5], dst[ 0], step(0.25, f.x)), mix(dst[ 1], dst[10], step(0.75, f.x)), step(0.50, f.x)), step(0.25, f.y)),
                            mix( mix( mix(dst[ 4], dst[ 3], step(0.25, f.x)), mix(dst[ 2], dst[11], step(0.75, f.x)), step(0.50, f.x)),
                                 mix( mix(dst[15], dst[14], step(0.25, f.x)), mix(dst[13], dst[12], step(0.75, f.x)), step(0.50, f.x)), step(0.75, f.y)),
                                                                                                                                        step(0.50, f.y));

    return ivec4(res * vec3(63,63,63), 0);
}


void main()
{
    vec2 fTexcoord = vTexCoord.xy;

    ivec4 pixel;// = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord), 0));

    ivec4 mbright = ivec4(texelFetch(ScreenTex, ivec2(256*3, int(fTexcoord.y)), 0));
    int dispmode = mbright.b & 0x3;

    if (dispmode == 1)
    {
        ivec4 val1;// = pixel;
        //ivec4 val2 = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(256,0), 0));
        ivec4 val3 = ivec4(texelFetch(ScreenTex, ivec2(fTexcoord) + ivec2(512,0), 0));

        int compmode = val3.a & 0xF;
        int eva, evb, evy;

        float xpos = val3.r + fract(fTexcoord.x);
        float ypos = mod(fTexcoord.y, 192);
        ivec4 _3dpix = Get3DPixel(vec2(xpos, ypos));

        if (compmode == 4)
        {
            // 3D on top, blending

            if (_3dpix.a > 0)
            {
                eva = (_3dpix.a & 0x1F) + 1;
                if (eva == 32)
                {
                    val1 = _3dpix;
                }
                else
                {
                    evb = 32 - eva;

                    val1 = GetFiltered2DPixel(0);

                    val1 = ((_3dpix * eva) + (val1 * evb)) >> 5;
                    if (eva <= 16) val1 += ivec4(1,1,1,0);
                    val1 = min(val1, 0x3F);
                }
            }
            else
                val1 = GetFiltered2DPixel(1);
        }
        else if (compmode == 1)
        {
            // 3D on bottom, blending

            if (_3dpix.a > 0)
            {
                eva = val3.g;
                evb = val3.b;

                val1 = GetFiltered2DPixel(0);

                val1 = ((val1 * eva) + (_3dpix * evb)) >> 4;
                val1 = min(val1, 0x3F);
            }
            else
                val1 = GetFiltered2DPixel(1);
        }
        else if (compmode <= 3)
        {
            // 3D on top, normal/fade

            if (_3dpix.a > 0)
            {
                evy = val3.g;

                val1 = _3dpix;
                if      (compmode == 2) val1 += ((ivec4(0x3F,0x3F,0x3F,0) - val1) * evy) >> 4;
                else if (compmode == 3) val1 -= (val1 * evy) >> 4;
            }
            else
                val1 = GetFiltered2DPixel(1);
        }
        else
            val1 = GetFiltered2DPixel(0);

        pixel = val1;
    }
    else
    {
        pixel = GetFiltered2DPixel(0);
    }

    if (dispmode != 0)
    {
        int brightmode = mbright.g >> 6;
        if (brightmode == 1)
        {
            // up
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel += ((ivec4(0x3F,0x3F,0x3F,0) - pixel) * evy) >> 4;
        }
        else if (brightmode == 2)
        {
            // down
            int evy = mbright.r & 0x1F;
            if (evy > 16) evy = 16;

            pixel -= (pixel * evy) >> 4;
        }
    }

    pixel.rgb <<= 2;
    pixel.rgb |= (pixel.rgb >> 6);

    FragColor = vec4(vec3(pixel.bgr) / 255.0, 1.0);
}
)";





}

#endif // GPU_OPENGL_SHADERS_H
