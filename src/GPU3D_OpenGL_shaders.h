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
    float uPosScale;
};

layout(location = 0) out vec4 oColor;

// GPU::GLEdgeMark (E1, GLRenderer::RenderSceneChunk): uEdgeScale, the neighbor offset, one DS
// pixel in target pixels (the 3D scale, wide views too). uClearPolyID and uClearDepth: what a
// neighbor outside the target reads, the clear plane's ID and depth, as the software renderer
// fills its screen borders with (texelFetch outside a texture is undefined in GLES). uDepthUnbias:
// what SetPolygonDepthBias moved a depth-equal polygon's depth nearer by (0x200 Z, 0xFF W, in
// depth units), added back where attr G says the opaque draw was biased (kRenderFS's EdgeAttrG),
// so a coplanar decal compares with its own depth, as the software renderer stores it
uniform int uEdgeScale;
uniform float uDepthUnbias;
// GPU::GLEdgeFast bit 0: pixels without the flag are discarded right after the attr fetch, before
// the depth fetch (the same result: the blend leaves them as they are)
uniform int uEdgeFast;
uniform int uClearPolyID;
uniform float uClearDepth;

// make up for crapo zbuffer precision
bool isless(float a, float b)
{
    return a < b;

    // a < b
    float diff = a - b;
    return diff < (256.0 / 16777216.0);
}

// the attr target holds ID/63 in 8 bits, which truncating can read back one low (ID 1 as 0)
int PolyID(vec4 attr)
{
    return int(attr.r * 63.0 + 0.5);
}

// attr G (kRenderFS's EdgeAttrG): (2 * edge flag + depth biased) / 3
int AttrG(vec4 attr)
{
    return int(attr.g * 3.0 + 0.5);
}

float Depth(float depth, vec4 attr)
{
    return ((AttrG(attr) & 1) != 0) ? depth + uDepthUnbias : depth;
}

bool isgood(ivec2 coord, int refPolyID, float refDepth)
{
    int polyid;
    float depth;
    if (any(lessThan(coord, ivec2(0))) || any(greaterThanEqual(coord, ivec2(uScreenSize))))
    {
        polyid = uClearPolyID;
        depth = uClearDepth;
    }
    else
    {
        vec4 attr = texelFetch(AttrBuffer, coord, 0);
        polyid = PolyID(attr);
        depth = Depth(texelFetch(DepthBuffer, coord, 0).r, attr);
    }

    if (polyid != refPolyID && isless(refDepth, depth))
        return true;

    return false;
}

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    int scale = uEdgeScale;

    vec4 ret = vec4(0,0,0,0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);
    if (uEdgeFast != 0 && (AttrG(attr) & 2) == 0)
        discard;
    vec4 depth = texelFetch(DepthBuffer, coord, 0);

    int polyid = PolyID(attr);

    if ((AttrG(attr) & 2) != 0)
    {
        float z = Depth(depth.r, attr);
        if (isgood(coord + ivec2(0,-scale), polyid, z) ||
            isgood(coord + ivec2(0,scale), polyid, z) ||
            isgood(coord + ivec2(-scale,0), polyid, z) ||
            isgood(coord + ivec2(scale,0), polyid, z))
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
    float uPosScale;
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
    float uPosScale;
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
#ifdef TexCache
// GPU::GLTexCache (T2, GLRenderer::BuildRenderShader): the decoded texel heap (RGBA8UI, 1024 wide,
// mediump like TexMem) and the palette 256 entries a row, declared like TexPalMem so its fetches
// come back at the same precision
uniform usampler2D TexHeap;
uniform sampler2D TexPalLinear;
#ifdef TexColorPlane
// GPU::GLTexCache 128 (T2 step 4a, measurement only): the heap's color plane, the same size and
// offsets as TexHeap, each texel's palette word as TexPalLinear holds it (RGB5_A1, the same 16 bits)
// with A the record's 0/1 alpha. declared like TexPalMem and TexPalLinear, the same precision
uniform sampler2D TexColor;
#endif
#ifdef TexWrapVS
// GPU::GLTexCache 64 (T2 step 3): kRenderVS's per-polygon wrap constants, (P.x, P.y, R.x, R.y).
// all within [-1, 2047], so mediump (16 bits on the phones) holds them exactly
flat in mediump ivec4 fTexWrap;
#endif
#endif

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
    float uPosScale;
};

smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in ivec3 fPolygonAttr;

#ifdef FramebufferFetch
// GPU::GLTransSkip bit 2 (GLRenderer::BuildRenderShader's fetch build): oColor starts out as the
// color target's value (GL_EXT_shader_framebuffer_fetch)
layout(location = 0) inout highp vec4 oColor;
#else
layout(location = 0) out vec4 oColor;
#endif
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

#if defined(TexProbeFetch)
// GPU::GLTexProbe 1 (T2 measurement probe, GLRenderer::BuildRenderShader, renders wrong on purpose):
// what a lookup into decoded RGBA textures would cost at most, one texelFetch from a float sampler
// that's bound anyway (TexPalMem, 1024 x 48), at the texcoord's texel masked into its first 32 rows so
// neighboring fragments read neighboring texels as with a real texture. no wrap, no format decode,
// no palette fetch. alpha 1: the per-texel alpha (color 0, A3I5/A5I3, direct's bit, 4x4 mode 3) needs
// the decoded texel, and the polygon's own alpha still decides the pass through modulate and decal
vec4 TextureProbe(vec2 st)
{
    return vec4(texelFetch(TexPalMem, ivec2(st) & ivec2(0x3FF, 0x1F), 0).rgb, 1.0);
}
#elif defined(TexProbeConst)
// GPU::GLTexProbe 2: as 1 without the fetch, so 1 against 2 is the fetch and 0 against 1 the decode
vec4 TextureProbe(vec2 st)
{
    return vec4(1.0);
}
#elif defined(TexProbeWrap) || defined(TexProbeChain)
// GPU::GLTexProbe 3 and 4 (T2 measurement probes, render wrong on purpose): both wrap the texcoord
// with the old path's code first. 3 then makes probe 1's one fetch at the wrapped texel (masked into
// TexPalMem's first 32 rows), so 1 against 3 is the wrap. 4 instead makes two dependent fetches like
// T2's: a TexMem byte at I8's address from the wrapped texel and the VRAM offset, then the palette
// entry it indexes as I8 does, alpha 1, with no kind branches or alpha decode
vec4 TextureProbe(vec2 st)
{
    int attr = int(fPolygonAttr.z << 16);
    int tw = 8 << ((attr >> 20) & 0x7);
    int th = 8 << ((attr >> 23) & 0x7);
    int wrapmode = (attr >> 16);
    ivec2 c = ivec2(st);
    c.x = TexcoordWrap(c.x, tw, wrapmode>>0);
    c.y = TexcoordWrap(c.y, th, wrapmode>>1);
#ifdef TexProbeWrap
    return vec4(texelFetch(TexPalMem, c & ivec2(0x3FF, 0x1F), 0).rgb, 1.0);
#else
    int addr = (int(fPolygonAttr.y) << 3) + ((c.y * tw) + c.x);
    ivec4 pixel = ivec4(texelFetch(TexMem, ivec2(addr&0x3FF, addr>>10), 0));
    int pal = (int(fPolygonAttr.z >> 16) << 3) + pixel.r;
    vec4 color = texelFetch(TexPalMem, ivec2(pal&0x3FF, pal>>10), 0);
    return vec4(color.rgb, 1.0);
#endif
}
#endif

#ifdef TexCache
// GPU::GLTexCache (T2 step 1): a polygon whose texture is in the heap (fPolygonAttr.y is
// 0x40000000 | its block's offset) reads the texel's record, decoded on the CPU from the integer
// code of the TextureFetch_* above (GPU3D_OpenGLTexHeap.cpp), and runs that fetch's float tail on
// it, written as it is there: the same wrap, the same palette words fetched at the same precision
// and the same expressions, so the color comes out the same. a record's A & 7 is its kind:
// 0 palette with alpha 0/1 in B, 1 palette with alpha 0..31 in B (A3I5, A5I3), 2 direct color,
// 3 4x4 half and half, 4 4x4 5:3, 5 4x4 3:5 (color 1 is the entry after color 0), 6 transparent
vec4 TextureLookup_Cached(vec2 st)
{
#ifdef TexWrapVS
    // GPU::GLTexCache 64 (T2 step 3): TexcoordWrap without branches, from kRenderVS's constants.
    // per axis, P is the mask (m = size-1 for repeat, 2m+1 for repeat and flip, -1 for clamp), R the
    // mirror (2m+1 for flip, else size) and H = R + P the clamp's top (4m+2, 2m+1, m). repeat:
    // c & m. flip: u = c & (2m+1) is mirrored by min(u, u ^ (2m+1)) = min(u, 2m+1-u). clamp:
    // clamp(c, 0, m), and u ^ size = u + size leaves the min alone. the same texel as TexcoordWrap
    // for every int c (checked for all c in [-2^24, 2^24] and 2^24 random ints, every size and
    // mode). R.x's top bit is log2(tw) in every mode, so the row is a shift
    ivec4 w = fTexWrap;
    ivec2 c = ivec2(st);
    ivec2 u = clamp(c & w.xy, ivec2(0), w.zw + w.xy);
    c = min(u, u ^ w.zw);

    int a = (fPolygonAttr.y & 0x3FFFFF) + ((c.y << findMSB(w.z)) + c.x);
#else
    int attr = int(fPolygonAttr.z << 16);
    int tw = 8 << ((attr >> 20) & 0x7);
    int th = 8 << ((attr >> 23) & 0x7);
    int wrapmode = (attr >> 16);
    ivec2 c = ivec2(st);
    c.x = TexcoordWrap(c.x, tw, wrapmode>>0);
    c.y = TexcoordWrap(c.y, th, wrapmode>>1);

    int a = (fPolygonAttr.y & 0x3FFFFF) + ((c.y * tw) + c.x);
#endif
#ifdef TexColorPlane
    // GPU::GLTexCache 128 (T2 step 4a): a polygon whose texture has a color plane (I2, I4, I8: bit
    // 22 of fPolygonAttr.y; A3I5, A5I3: bits 22 and 23) reads the palette word straight from it, so
    // there's no dependent fetch. rgb is kind 0's and 1's (the same word, format and precision as
    // their TexPalLinear fetch), A is kind 0's 0/1 alpha; with bit 23 the alpha is kind 1's, from
    // the record at the same address
    ivec2 ta = ivec2(a&0x3FF, a>>10);
#ifdef TexColorOnly
    // GPU::GLTexCache 256 (debug, measurement): built without the record path. polygons without the
    // color plane never draw with this build (AssignTexCache gives them RenderKey_TexFull)
    {
#else
    if ((fPolygonAttr.y & 0x400000) != 0)
    {
#endif
        vec4 color = texelFetch(TexColor, ta, 0);
        if ((fPolygonAttr.y & 0x800000) != 0)
        {
            ivec4 rec = ivec4(texelFetch(TexHeap, ta, 0));
            return vec4(color.rgb, float(rec.b)/31.0);
        }
        return color;
    }
#endif
#ifndef TexColorOnly
    ivec4 rec = ivec4(texelFetch(TexHeap, ivec2(a&0x3FF, a>>10), 0));
    int kind = rec.a & 0x7;

    if (kind == 2)
    {
        vec4 color;
        color.r = float(rec.r) / 31.0;
        color.g = float(rec.g) / 31.0;
        color.b = float(rec.b) / 31.0;
        color.a = float(rec.a >> 7);

        return color;
    }
    else if (kind == 6)
    {
        return vec4(0.0);
    }
    else if (kind == 0)
    {
        vec4 color = texelFetch(TexPalLinear, rec.rg, 0);
        return vec4(color.rgb, (rec.b>0) ? 1.0 : 0.0);
    }
    else if (kind == 1)
    {
        vec4 color = texelFetch(TexPalLinear, rec.rg, 0);
        return vec4(color.rgb, float(rec.b)/31.0);
    }
    else
    {
        vec4 color0 = texelFetch(TexPalLinear, rec.rg, 0);
        vec4 color1 = texelFetch(TexPalLinear, rec.rg + ivec2(1, 0), 0);
        if (kind == 3)      return vec4((color0.rgb + color1.rgb) / 2.0, 1.0);
        else if (kind == 4) return vec4((color0.rgb*5.0 + color1.rgb*3.0) / 8.0, 1.0);
        else                return vec4((color0.rgb*3.0 + color1.rgb*5.0) / 8.0, 1.0);
    }
#endif
}
#endif

vec4 TextureLookup_Nearest(vec2 st)
{
#if defined(TexProbeFetch) || defined(TexProbeConst) || defined(TexProbeWrap) || defined(TexProbeChain)
    return TextureProbe(st);
#elif defined(TexCacheOnly)
    // GPU::GLTexCache step 2 (GLRenderer::BuildRenderShader): no old path here. only polygons the heap
    // serves (and untextured ones) draw with this build; the others carry RenderKey_TexFull and draw
    // with the twin built without TexCacheOnly
    return TextureLookup_Cached(st);
#else
#ifdef TexCache
    if (fPolygonAttr.y >= 0x40000000) return TextureLookup_Cached(st);
#endif
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
#endif
}

// not called (FinalColor's call is commented out). with GPU::GLTexCache, fPolygonAttr.y can be a heap
// word (0x40000000 | offset) instead of a VRAM offset, which this reads as an offset: before using it
// again, give it TextureLookup_Nearest's TexCache branch or keep the cache off
vec4 TextureLookup_Linear(vec2 texcoord)
{
#if defined(TexProbeFetch) || defined(TexProbeConst) || defined(TexProbeWrap) || defined(TexProbeChain)
    // one fetch (or none), not four
    return TextureProbe(texcoord);
#else
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
#endif
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

#ifdef EdgeMark
// GPU::GLEdgeMark 1 (E1): where the polygon's outline is in EdgeTex, the same for each of its
// vertices (GLRenderer::BuildEdges), for kRenderFS's EdgeFlag
in uint vEdgeInfo;
flat out highp uint fEdgeInfo;
#ifdef EdgeLines
// GPU::GLEdgeFast bit 1: the outline of a polygon with up to 4 edges, as kRenderFS's EdgeFlag reads
// it from EdgeTex otherwise, (nx, ny, c) an edge packed into 12 floats (GLRenderer::BuildEdges)
in vec4 vEdgeLine0;
in vec4 vEdgeLine1;
in vec4 vEdgeLine2;
flat out highp vec4 fEdgeLine0;
flat out highp vec4 fEdgeLine1;
flat out highp vec4 fEdgeLine2;
#endif
#endif

#ifdef TexWrapVS
// GPU::GLTexCache 64 (T2 step 3): the per-polygon part of TexcoordWrap for the heap's lookup
// (kRenderFSCommon's TextureLookup_Cached), once a vertex instead of once a fragment. (P, R) for
// one axis of size 1 << k, mode as TexcoordWrap's (bit 0 repeat, bit 2 flip). the same for every
// vertex of a polygon, so flat. polygons the heap doesn't serve get them too, unused
flat out mediump ivec4 fTexWrap;

ivec2 TexWrapAxis(int k, int mode)
{
    int m = (1 << k) - 1;
    if ((mode & (1<<0)) == 0) return ivec2(-1, m + 1);
    if ((mode & (1<<2)) == 0) return ivec2(m, m + 1);
    return ivec2(2*m + 1, 2*m + 1);
}
#endif

void main()
{
    int attr = vPolygonAttr.x;
    int zshift = (attr >> 16) & 0x1F;

    vec4 fpos;
    // GPU::GLSeams: positions in 1/(1 << PosFrac) target pixels, uPosScale = 1/(1 << PosFrac)
    fpos.xy = (((vec2(vPosition.xy) * uPosScale) * 2.0) / uScreenSize) - 1.0;
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
#ifdef TexWrapVS
    // the same fields TextureLookup_Cached's old code reads from fPolygonAttr.z
    int tattr = vPolygonAttr.z << 16;
    int wrapmode = (tattr >> 16);
    ivec2 sx = TexWrapAxis(3 + ((tattr >> 20) & 0x7), wrapmode>>0);
    ivec2 sy = TexWrapAxis(3 + ((tattr >> 23) & 0x7), wrapmode>>1);
    fTexWrap = ivec4(sx.x, sy.x, sx.y, sy.y);
#endif
#ifdef EdgeMark
    fEdgeInfo = vEdgeInfo;
#ifdef EdgeLines
    fEdgeLine0 = vEdgeLine0;
    fEdgeLine1 = vEdgeLine1;
    fEdgeLine2 = vEdgeLine2;
#endif
#endif

    gl_Position = fpos;
}
)";

const char* kRenderFS = R"(

#ifdef WBuffer
smooth in float fZ;
#endif

#ifdef EdgeMark
// GPU::GLEdgeMark (E1, GLRenderer::BuildEdges): the edge flag of an opaque fragment. set
// when one of its four neighbors one DS pixel away (uEdgeScale target pixels, the edge pass's
// offset) is outside the polygon, the hi-res form of the software renderer's flags (span ends, top
// and bottom rows). EdgeTex holds the polygon's outline only, so the fan's inner diagonals don't
// count: one texel an edge, (nx, ny, c), its inward unit normal and c = -dot(n, v0) - reach, reach
// being how far the farthest of the four neighbors is from the edge's line, scale * max(|nx|, |ny|),
// plus a little for neighbors right on it. a neighbor is outside an edge's half plane when
// dot(n, p) + c < 0. fEdgeInfo: the first texel (low 16 bits) and the edge count (high bits);
// a count of 0 (lines, polygons without area, every polygon with GPU::GLEdgeMark 2) flags every
// fragment
flat in highp uint fEdgeInfo;
uniform highp sampler2D EdgeTex;
#ifdef EdgeLines
// GPU::GLEdgeFast bit 1: polygons with up to 4 edges have them here instead of in EdgeTex
flat in highp vec4 fEdgeLine0;
flat in highp vec4 fEdgeLine1;
flat in highp vec4 fEdgeLine2;
#endif
// the vertex shader's: nonzero for a depth-equal polygon's margin (GLRenderer::SetPolygonDepthBias)
uniform float uDepthBias;

// one edge's test, the same expression for both ways the edges come in
bool EdgeOutside(vec3 e, vec2 p)
{
    return dot(e.xy, p) + e.z < 0.0;
}

float EdgeFlag()
{
    uint count = fEdgeInfo >> 16;
    if (count == 0u) return 1.0;
    vec2 p = gl_FragCoord.xy;
#ifdef EdgeLines
    if (count <= 4u)
    {
        if (EdgeOutside(fEdgeLine0.xyz, p)) return 1.0;
        if (count > 1u && EdgeOutside(vec3(fEdgeLine0.w, fEdgeLine1.xy), p)) return 1.0;
        if (count > 2u && EdgeOutside(vec3(fEdgeLine1.zw, fEdgeLine2.x), p)) return 1.0;
        if (count > 3u && EdgeOutside(fEdgeLine2.yzw, p)) return 1.0;
        return 0.0;
    }
#endif
    int base = int(fEdgeInfo & 0xFFFFu);
    for (int k = 0; k < int(count); k++)
    {
        int idx = base + k;
        vec4 e = texelFetch(EdgeTex, ivec2(idx & 1023, idx >> 10), 0);
        if (EdgeOutside(e.xyz, p)) return 1.0;
    }
    return 0.0;
}

// attr G: (2 * edge flag + depth biased) / 3, which the edge pass decodes (kFinalPassEdgeFS's
// AttrG). the bias bit lets it compare a depth-equal polygon with the depth the DS would store
float EdgeAttrG()
{
    return (EdgeFlag() * 2.0 + (uDepthBias != 0.0 ? 1.0 : 0.0)) / 3.0;
}
#define EDGE_ATTR_G EdgeAttrG()
#else
#define EDGE_ATTR_G 0.0
#endif

#ifdef NoDiscard
// GPU::GLNoDiscard (R1 step 1, GLRenderer::BuildRenderShader's nodiscard build, Z-buffer only): the
// opaque pass of polygons whose fragments always come out at alpha 31, so it's the opaque branch
// below without its discard, and no other branch, so nothing in the program discards and the GPU
// can test depth before the shader runs
void main()
{
    vec4 col = FinalColor();

    oAttr.r = float((fPolygonAttr.x >> 24) & 0x3F) / 63.0;
    oAttr.g = EDGE_ATTR_G;
    oAttr.b = float((fPolygonAttr.x >> 15) & 0x1);
    oAttr.a = 1.0;

    oColor = col;
}
#else
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
            oAttr.g = EDGE_ATTR_G;
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

#ifdef FramebufferFetch
            // the blend GLRenderer's fixed-function state does (GL_FUNC_ADD with SRC_ALPHA,
            // ONE_MINUS_SRC_ALPHA when DISP3DCNT bit 3 is on, ONE, ZERO when off; GL_MAX on alpha),
            // done here with blending off so a destination alpha of 0 can write the source as it is,
            // like the software renderer's AlphaBlend and the clear0 pass this replaces. the same
            // expression as the blend equation, so rounding should match it
            vec4 dst = oColor;
            if (dst.a > 0.0 && (uDispCnt & (1<<3)) != 0)
                col.rgb = col.rgb * col.a + dst.rgb * (1.0 - col.a);
            col.a = max(col.a, dst.a);
#endif
        }

        oColor = col;
    }

#ifdef WBuffer
    gl_FragDepth = fZ;
#endif
}
#endif
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
