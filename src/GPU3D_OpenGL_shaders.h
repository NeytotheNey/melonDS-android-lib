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

#ifndef GPU3D_OPENGL_SHADERS_H
#define GPU3D_OPENGL_SHADERS_H

#define kShaderHeader "#version 320 es"

namespace melonDS
{
const char* kClearVS = kShaderHeader R"(

in vec2 vPosition;

uniform uint uDepth;

void main()
{
    float fdepth = (float(uDepth) / 8388608.0) - 1.0;
    gl_Position = vec4(vPosition, fdepth, 1.0);
}
)";

const char* kClearFS = kShaderHeader R"(

precision highp float;

uniform uvec4 uColor;
uniform uint uOpaquePolyID;
uniform uint uFogFlag;

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;

void main()
{
    oColor = vec4(uColor).bgra / 31.0;
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0.0;
    oAttr.b = float(uFogFlag);
    oAttr.a = 1.0;
}
)";

// clear bitmap (DISP3DCNT bit 14): the colour and depth bitmaps are slots 2 and 3 of texture
// VRAM, which TexMem already holds (R8UI, 1024 bytes per row), so they are fetched from there
// instead of being converted and uploaded separately like upstream's GL renderer does
const char* kClearBitmapVS = kShaderHeader R"(

in vec2 vPosition;

void main()
{
    gl_Position = vec4(vPosition, 0.0, 1.0);
}
)";

const char* kClearBitmapFS = kShaderHeader R"(

precision highp float;
precision highp int;
precision mediump usampler2D;

uniform usampler2D TexMem;

uniform int uScale;
uniform ivec2 uOffset;
uniform uint uOpaquePolyID;

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;

uint ReadTexVRAM16(int addr)
{
    ivec2 coord = ivec2(addr & 0x3FF, addr >> 10);
    uint lo = texelFetch(TexMem, coord, 0).r;
    uint hi = texelFetch(TexMem, coord + ivec2(1, 0), 0).r;
    return lo | (hi << 8);
}

void main()
{
    // native pixel, scrolled by the clear attributes, wrapping at 256 both ways.
    // our targets keep DS line 0 at row 0, so no Y flip
    int x = ((int(gl_FragCoord.x) / uScale) + uOffset.x) & 0xFF;
    int y = ((int(gl_FragCoord.y) / uScale) + uOffset.y) & 0xFF;
    int addr = ((y << 8) + x) << 1;

    uint color = ReadTexVRAM16(0x40000 + addr);
    uint depth = ReadTexVRAM16(0x60000 + addr);

    // same 5->6 bit expansion as the software renderer
    uint r = (color << 1) & 0x3Eu; if (r != 0u) r++;
    uint g = (color >> 4) & 0x3Eu; if (g != 0u) g++;
    uint b = (color >> 9) & 0x3Eu; if (b != 0u) b++;
    uint a = ((color & 0x8000u) != 0u) ? 31u : 0u;

    uint z = ((depth & 0x7FFFu) * 0x200u) + 0x1FFu;

    oColor = (vec4(float(r), float(g), float(b), float(a)) / vec4(63.0, 63.0, 63.0, 31.0)).bgra;
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0.0;
    oAttr.b = float((depth >> 15) & 1u);
    oAttr.a = 1.0;
    gl_FragDepth = float(z) / 16777216.0;
}
)";



const char* kFinalPassVS = kShaderHeader R"(

in vec2 vPosition;

void main()
{
    // heh
    gl_Position = vec4(vPosition, 0.0, 1.0);
}
)";

const char* kFinalPassEdgeFS = kShaderHeader R"(

precision highp float;

uniform highp sampler2D DepthBuffer;
uniform sampler2D AttrBuffer;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

layout(location = 0) out vec4 oColor;

// make up for crapo zbuffer precision
bool isless(float a, float b)
{
    return a < b;

    // a < b
    float diff = a - b;
    return diff < (256.0 / 16777216.0);
}

bool isgood(vec4 attr, float depth, int refPolyID, float refDepth)
{
    int polyid = int(attr.r * 63.0);

    if (polyid != refPolyID && isless(refDepth, depth))
        return true;

    return false;
}

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    int scale = 1;//int(uScreenSize.x / 256);

    vec4 ret = vec4(0,0,0,0);
    vec4 depth = texelFetch(DepthBuffer, coord, 0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);

    int polyid = int(attr.r * 63.0);

    if (attr.g != 0.0)
    {
        vec4 depthU = texelFetch(DepthBuffer, coord + ivec2(0,-scale), 0);
        vec4 attrU = texelFetch(AttrBuffer, coord + ivec2(0,-scale), 0);
        vec4 depthD = texelFetch(DepthBuffer, coord + ivec2(0,scale), 0);
        vec4 attrD = texelFetch(AttrBuffer, coord + ivec2(0,scale), 0);
        vec4 depthL = texelFetch(DepthBuffer, coord + ivec2(-scale,0), 0);
        vec4 attrL = texelFetch(AttrBuffer, coord + ivec2(-scale,0), 0);
        vec4 depthR = texelFetch(DepthBuffer, coord + ivec2(scale,0), 0);
        vec4 attrR = texelFetch(AttrBuffer, coord + ivec2(scale,0), 0);

        if (isgood(attrU, depthU.r, polyid, depth.r) ||
            isgood(attrD, depthD.r, polyid, depth.r) ||
            isgood(attrL, depthL.r, polyid, depth.r) ||
            isgood(attrR, depthR.r, polyid, depth.r))
        {
            // mark this pixel!

            ret.rgb = uEdgeColors[polyid >> 3].bgr;

            // this isn't quite accurate, but it will have to do
            if ((uDispCnt & (1<<4)) != 0)
                ret.a = 0.5;
            else
                ret.a = 1.0;
        }
    }

    oColor = ret;
}
)";

const char* kFinalPassFogFS = kShaderHeader R"(

precision highp float;
precision highp int;

uniform highp sampler2D DepthBuffer;
uniform sampler2D AttrBuffer;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

layout(location = 0) out vec4 oColor;

vec4 CalculateFog(float depth)
{
    int idepth = int(depth * 16777216.0);
    int densityid, densityfrac;

    if (idepth < uFogOffset)
    {
        densityid = 0;
        densityfrac = 0;
    }
    else
    {
        uint udepth = uint(idepth);
        udepth -= uint(uFogOffset);
        udepth = (udepth >> 2) << uint(uFogShift);

        densityid = int(udepth >> 17);
        if (densityid >= 32)
        {
            densityid = 32;
            densityfrac = 0;
        }
        else
            densityfrac = int(udepth & uint(0x1FFFF));
    }

    float density = mix(uFogDensity[densityid], uFogDensity[densityid+1], float(densityfrac)/131072.0);

    return vec4(density, density, density, density);
}

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);

    vec4 ret = vec4(0,0,0,0);
    vec4 depth = texelFetch(DepthBuffer, coord, 0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);

    if (attr.b != 0.0) ret = CalculateFog(depth.r);

    oColor = ret;
}
)";



const char* kRenderVSCommon = R"(

precision highp int;
precision highp float;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

in uvec4 vPosition;
in uvec4 vColor;
in ivec2 vTexcoord;
in ivec3 vPolygonAttr;

smooth out vec4 fColor;
smooth out vec2 fTexcoord;
flat out ivec3 fPolygonAttr;

// shifts the polygon's depth, in DS depth units. only set for the depth-equal margin of shadow
// masks and shadows (GLRenderer::SetDepthBias), 0 otherwise
uniform float uDepthBias;
)";

const char* kRenderFSCommon = R"(

precision highp int;
precision highp float;
precision mediump usampler2D;

uniform usampler2D TexMem;
uniform sampler2D TexPalMem;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in ivec3 fPolygonAttr;

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAttr;

int TexcoordWrap(int c, int maxc, int mode)
{
    if ((mode & (1<<0)) != 0)
    {
        if ((mode & (1<<2)) != 0 && (c & maxc) != 0)
            return (maxc-1) - (c & (maxc-1));
        else
            return (c & (maxc-1));
    }
    else
        return clamp(c, 0, maxc-1);
}

vec4 TextureFetch_A3I5(ivec2 addr, ivec4 st, int wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));

    pixel.a = (pixel.r & 0xE0);
    pixel.a = (pixel.a >> 3) + (pixel.a >> 6);
    pixel.r &= 0x1F;

    addr.y = (addr.y << 3) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, float(pixel.a)/31.0);
}

vec4 TextureFetch_I2(ivec2 addr, ivec4 st, int wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) >> 2;
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));
    pixel.r >>= (2 * (st.x & 3));
    pixel.r &= 0x03;

    addr.y = (addr.y << 2) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, (pixel.r>0) ? 1.0 : alpha0);
}

vec4 TextureFetch_I4(ivec2 addr, ivec4 st, int wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) >> 1;
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));
    if ((st.x & 1) != 0) pixel.r >>= 4;
    else                 pixel.r &= 0x0F;

    addr.y = (addr.y << 3) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, (pixel.r>0) ? 1.0 : alpha0);
}

vec4 TextureFetch_I8(ivec2 addr, ivec4 st, int wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));

    addr.y = (addr.y << 3) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, (pixel.r>0) ? 1.0 : alpha0);
}

vec4 TextureFetch_Compressed(ivec2 addr, ivec4 st, int wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y & 0x3FC) * (st.z>>2)) + (st.x & 0x3FC) + (st.y & 0x3);
    ivec4 p = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));
    int val = (p.r >> (2 * (st.x & 0x3))) & 0x3;

    int slot1addr = 0x20000 + ((addr.x & 0x1FFFC) >> 1);
    if (addr.x >= 0x40000) slot1addr += 0x10000;

    int palinfo;
    p = ivec4(texelFetch(TexMem, ivec2(slot1addr&0x3FF, slot1addr>>10), 0));
    palinfo = p.r;
    slot1addr++;
    p = ivec4(texelFetch(TexMem, ivec2(slot1addr&0x3FF, slot1addr>>10), 0));
    palinfo |= (p.r << 8);

    addr.y = (addr.y << 3) + ((palinfo & 0x3FFF) << 1);
    palinfo >>= 14;

    if (val == 0)
    {
        vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
        return vec4(color.rgb, 1.0);
    }
    else if (val == 1)
    {
        addr.y++;
        vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
        return vec4(color.rgb, 1.0);
    }
    else if (val == 2)
    {
        if (palinfo == 1)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb + color1.rgb) / 2.0, 1.0);
        }
        else if (palinfo == 3)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb*5.0 + color1.rgb*3.0) / 8.0, 1.0);
        }
        else
        {
            addr.y += 2;
            vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4(color.rgb, 1.0);
        }
    }
    else
    {
        if (palinfo == 2)
        {
            addr.y += 3;
            vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4(color.rgb, 1.0);
        }
        else if (palinfo == 3)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb*3.0 + color1.rgb*5.0) / 8.0, 1.0);
        }
        else
        {
            return vec4(0.0);
        }
    }
}

vec4 TextureFetch_A5I3(ivec2 addr, ivec4 st, int wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));

    pixel.a = (pixel.r & 0xF8) >> 3;
    pixel.r &= 0x07;

    addr.y = (addr.y << 3) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, float(pixel.a)/31.0);
}

vec4 TextureFetch_Direct(ivec2 addr, ivec4 st, int wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) << 1;
    ivec4 pixelL = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));
    addr.x++;
    ivec4 pixelH = ivec4(texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0));

    vec4 color;
    color.r = float(pixelL.r & 0x1F) / 31.0;
    color.g = float((pixelL.r >> 5) | ((pixelH.r & 0x03) << 3)) / 31.0;
    color.b = float((pixelH.r & 0x7C) >> 2) / 31.0;
    color.a = float(pixelH.r >> 7);

    return color;
}

vec4 TextureLookup_Nearest(vec2 st)
{
    int vramOffset = int(fPolygonAttr.y);
    int attr = int(fPolygonAttr.z << 16); // Shift just to reuse same code as in original source. Same below
    int paladdr = int(fPolygonAttr.z >> 16);

    float alpha0;
    if ((attr & (1<<29)) != 0) alpha0 = 0.0;
    else                       alpha0 = 1.0;

    int tw = 8 << ((attr >> 20) & 0x7);
    int th = 8 << ((attr >> 23) & 0x7);
    ivec4 st_full = ivec4(ivec2(st), tw, th);

    ivec2 vramaddr = ivec2(vramOffset << 3, paladdr);
    int wrapmode = (attr >> 16);

    int type = (attr >> 26) & 0x7;
    if      (type == 5) return TextureFetch_Compressed(vramaddr, st_full, wrapmode);
    else if (type == 2) return TextureFetch_I2        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 3) return TextureFetch_I4        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 4) return TextureFetch_I8        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 1) return TextureFetch_A3I5      (vramaddr, st_full, wrapmode);
    else if (type == 6) return TextureFetch_A5I3      (vramaddr, st_full, wrapmode);
    else                return TextureFetch_Direct    (vramaddr, st_full, wrapmode);
}

vec4 TextureLookup_Linear(vec2 texcoord)
{
    ivec2 intpart = ivec2(texcoord);
    vec2 fracpart = fract(texcoord);

    int vramOffset = int(fPolygonAttr.y);
    int attr = int(fPolygonAttr.z << 16);  // Shift just to reuse same code as in original source. Same below
    int paladdr = int(fPolygonAttr.z >> 16);

    float alpha0;
    if ((attr & (1<<29)) != 0) alpha0 = 0.0;
    else                       alpha0 = 1.0;

    int tw = 8 << ((attr >> 20) & 0x7);
    int th = 8 << ((attr >> 23) & 0x7);
    ivec4 st_full = ivec4(intpart, tw, th);

    ivec2 vramaddr = ivec2(vramOffset << 3, paladdr);
    int wrapmode = (attr >> 16);

    vec4 A, B, C, D;
    int type = (attr >> 26) & 0x7;
    if (type == 5)
    {
        A = TextureFetch_Compressed(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_Compressed(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_Compressed(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_Compressed(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else if (type == 2)
    {
        A = TextureFetch_I2(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I2(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I2(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I2(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 3)
    {
        A = TextureFetch_I4(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I4(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I4(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I4(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 4)
    {
        A = TextureFetch_I8(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I8(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I8(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I8(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 1)
    {
        A = TextureFetch_A3I5(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_A3I5(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_A3I5(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_A3I5(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else if (type == 6)
    {
        A = TextureFetch_A5I3(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_A5I3(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_A5I3(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_A5I3(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else
    {
        A = TextureFetch_Direct(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_Direct(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_Direct(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_Direct(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }

    float fx = fracpart.x;
    vec4 AB;
    if (A.a < (0.5/31.0) && B.a < (0.5/31.0))
        AB = vec4(0);
    else
    {
        //if (A.a < (0.5/31.0) || B.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        AB = mix(A, B, fx);
    }

    fx = fracpart.x;
    vec4 CD;
    if (C.a < (0.5/31.0) && D.a < (0.5/31.0))
        CD = vec4(0);
    else
    {
        //if (C.a < (0.5/31.0) || D.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        CD = mix(C, D, fx);
    }

    fx = fracpart.y;
    vec4 ret;
    if (AB.a < (0.5/31.0) && CD.a < (0.5/31.0))
        ret = vec4(0);
    else
    {
        //if (AB.a < (0.5/31.0) || CD.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        ret = mix(AB, CD, fx);
    }

    return ret;
}

vec4 FinalColor()
{
    vec4 col;
    vec4 vcol = fColor;
    int blendmode = (fPolygonAttr.x >> 4) & 0x3;

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) == 0)
        {
            // toon
            vec3 tooncolor = uToonColors[int(vcol.r * 31.0)].rgb;
            vcol.rgb = tooncolor;
        }
        else
        {
            // highlight
            vcol.rgb = vcol.rrr;
        }
    }

    if ((((fPolygonAttr.z >> 10) & 0x7) == 0) || ((uDispCnt & (1<<0)) == 0))
    {
        // no texture
        col = vcol;
    }
    else
    {
        vec4 tcol = TextureLookup_Nearest(fTexcoord);
        //vec4 tcol = TextureLookup_Linear(fTexcoord);

        if ((blendmode & 1) != 0)
        {
            // decal
            col.rgb = (tcol.rgb * tcol.a) + (vcol.rgb * (1.0-tcol.a));
            col.a = vcol.a;
        }
        else
        {
            // modulate
            col = vcol * tcol;
        }
    }

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) != 0)
        {
            vec3 tooncolor = uToonColors[int(vcol.r * 31.0)].rgb;
            col.rgb = min(col.rgb + tooncolor, 1.0);
        }
    }

    return col.bgra;
}
)";


// the render shaders come in a Z-buffer and a W-buffer build (WBuffer defined by
// BuildRenderShader); the pass (opaque / translucent / shadow mask) is picked per draw
// through uRenderMode, as upstream does, instead of a program per combination
const char* kRenderVS = R"(

#ifdef WBuffer
smooth out float fZ;
#endif

void main()
{
    int attr = vPolygonAttr.x;
    int zshift = (attr >> 16) & 0x1F;

    vec4 fpos;
    fpos.xy = (((vec2(vPosition.xy) ) * 2.0) / uScreenSize) - 1.0;
#ifdef WBuffer
    fpos.z = 0.0;
    fZ = (float(vPosition.z << zshift) + uDepthBias) / 16777216.0;
#else
    fpos.z = ((float(vPosition.z << zshift) + uDepthBias) / 8388608.0) - 1.0;
#endif
    fpos.w = float(vPosition.w) / 65536.0f;
    fpos.xyz *= fpos.w;

    fColor = vec4(vColor) / vec4(255.0,255.0,255.0,31.0);
    fTexcoord = vec2(vTexcoord) / 16.0;
    fPolygonAttr = vPolygonAttr;

    gl_Position = fpos;
}
)";

const char* kRenderFS = R"(

#ifdef WBuffer
smooth in float fZ;
#endif

uniform int uRenderMode; // 0=opaque 1=translucent 2=shadowmask

void main()
{
    if (uRenderMode == 2)
    {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
    else
    {
        vec4 col = FinalColor();
        if (uRenderMode == 0)
        {
            // opaque pixels
            if (col.a < 30.5/31.0) discard;

            oAttr.r = float((fPolygonAttr.x >> 24) & 0x3F) / 63.0;
            oAttr.g = 0.0;
            oAttr.b = float((fPolygonAttr.x >> 15) & 0x1);
            oAttr.a = 1.0;
        }
        else
        {
            // translucent pixels
            if (col.a < 0.5/31.0) discard;
            if (col.a >= 30.5/31.0) discard;

            oAttr.b = 0.0;
            oAttr.a = 1.0;
        }

        oColor = col;
    }

#ifdef WBuffer
    gl_FragDepth = fZ;
#endif
}
)";

#ifdef MELONADE_DEBUG_TOOLS
// debug only (GPU::SceneStats 2, GLFragCounter): GLRenderer::BuildRenderShader's counting build puts
// this between kRenderFSCommon and kRenderFS, and kRenderFSFragCountHit first in kRenderFS's main.
// the normal build's source stays as it is
const char* kRenderFSFragCount = R"(

layout(std430, binding = 0) buffer FragCounts
{
    uint uFragCount[];
};
uniform int uFragPass;
)";

const char* kRenderFSFragCountHit = "\n    atomicAdd(uFragCount[uFragPass], 1u);\n";

// the covered pixel count: drawn over the whole target with the stencil test set to pass where a
// polygon drew, and early fragment tests so only those run
const char* kCoverVS = kShaderHeader R"(

in vec2 vPosition;

void main()
{
    gl_Position = vec4(vPosition, 0.0, 1.0);
}
)";

const char* kCoverFS = kShaderHeader R"(

precision highp float;

layout(early_fragment_tests) in;

layout(std430, binding = 0) buffer FragCounts
{
    uint uFragCount[];
};

layout(location = 0) out vec4 oColor;

void main()
{
    atomicAdd(uFragCount[3], 1u);
    oColor = vec4(0.0);
}
)";

// the covered pixel count when the clear plane's alpha is 0, where the shadow masks of pass 2 leave
// stencil values the count above can't tell from polygons: every polygon pixel has alpha 1 or more
// then and a clear pixel 0, so it counts the color target's pixels with alpha, before fog changes it
const char* kCoverAlphaCS = kShaderHeader R"(

precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16) in;

uniform highp sampler2D uColor;
uniform ivec2 uSize;

layout(std430, binding = 0) buffer FragCounts
{
    uint uFragCount[];
};

shared uint sCovered;

void main()
{
    if (gl_LocalInvocationIndex == 0u)
        sCovered = 0u;
    memoryBarrierShared();
    barrier();

    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x < uSize.x && pos.y < uSize.y && texelFetch(uColor, pos, 0).a > 0.0)
        atomicAdd(sCovered, 1u);

    memoryBarrierShared();
    barrier();
    if (gl_LocalInvocationIndex == 0u)
        atomicAdd(uFragCount[3], sCovered);
}
)";
#endif
}
#endif // GPU3D_OPENGL_SHADERS_H
