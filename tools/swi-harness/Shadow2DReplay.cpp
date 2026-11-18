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

// The GPU 2D shadow renderer's shaders, replayed on the CPU (swi-harness --shadow2d): a line for line
// C++ copy of kShadow2DObjFS_Main and of kShadow2DFS_Main's SHADOW2D_PLANES and SHADOW2D_AFFINE paths
// (GPU_OpenGL_shaders.h), run on what GLCompositor::UploadShadow2D would upload at the Blit (the
// flats, the recorder's palette copy, the blocks and the sprite instances), at 1x. Each line the
// recorder lets through gets its planes 0-2 compared word for word with what SoftRenderer drew, as
// the check mode does on the phone. On lines with a rotated or scaled BG, the affine composite's
// planes at each native pixel's first sub-pixel are compared with them too (they have to match at
// any scale), and its other sub-pixels are only counted. It proves the shaders' maths and the
// recorder's rules, not the GL driver: keep it in step with the shaders when they change.

#include "Shadow2DReplay.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "GPU.h"
#include "GPU_Shadow2D.h"

using namespace melonDS;
using namespace melonDS::Shadow2D;

namespace
{

// MemTex, as GLCompositor::UploadShadow2D fills it: by the same uploads (Shadow2DRecorder::ForEachUpload),
// kept from frame to frame like the texture, and what a full upload would hold now, to compare
std::vector<u32> Mem, MemFull;
u32 MemGeneration = 0;

const int MemABG = 0, MemBBG = 512, MemAOBJ = 640, MemBOBJ = 896;
const int MemABGExt = 1024, MemBBGExt = 1056, MemAOBJExt = 1088, MemBOBJExt = 1096, MemPal = 1104;

u32 MemWord(int base, int addr)
{
    return Mem[(base + (addr >> 10)) * 256 + ((addr >> 2) & 255)];
}

u32 MemByte(int base, int addr)
{
    return (MemWord(base, addr) >> ((addr & 3) * 8)) & 0xFFu;
}

u32 MemHalf(int base, int addr)
{
    return (MemWord(base, addr) >> ((addr & 2) * 8)) & 0xFFFFu;
}

u32 ColorWord(u32 color, u32 flag)
{
    u32 r = (color & 0x1Fu) << 1;
    u32 g = ((color & 0x3E0u) >> 4) | ((color & 0x8000u) >> 15);
    u32 b = (color & 0x7C00u) >> 9;
    return r | (g << 8) | (b << 16) | flag;
}

// kShadow2DObjFS_Main for the pixel at native X nx of the instance's line ObjTex row: false if discarded.
// out is what goes into R, or for an OBJ window sprite into G (1)
bool ObjPixel(const Instance& inst, int nx, int row, u32& out)
{
    int width = (int)inst.Width;
    int xoff = nx - inst.X;
    if (xoff < 0 || xoff >= width)
        return false;

    int height = (int)inst.Height;
    int ypos = (int)inst.FirstRow + (row - (int)inst.Row);
    u32 attr0 = inst.Attr0 & 0xFFFFu;
    u32 attr1 = inst.Attr1 & 0xFFFFu;
    u32 attr2 = inst.Attr2 & 0xFFFFu;
    u32 dispcnt = inst.DispCnt;
    bool unitB = (inst.Flags & 1u) != 0u;
    bool window = (inst.Flags & 2u) != 0u;
    int base = unitB ? MemBOBJ : MemAOBJ;
    int mask = unitB ? 0x1FFFF : 0x3FFFF;

    if ((attr1 & 0x2000u) != 0u)
        ypos = height - 1 - ypos;
    int x = (attr1 & 0x1000u) != 0u ? width - 1 - xoff : xoff;

    u32 pixelattr = ((attr2 & 0x0C00u) << 6) | 0xC0000u;
    u32 tilenum = attr2 & 0x03FFu;
    u32 mode = window ? 0u : ((attr0 >> 10) & 0x3u);
    u32 pixel;

    if (mode == 3u)
    {
        u32 alpha = (attr2 >> 12) & 0xFu;
        pixelattr |= 0xC0000000u | ((alpha + 1u) << 24);

        int addr;
        if ((dispcnt & 0x40u) != 0u)
            addr = (int)(tilenum << (7u + ((dispcnt >> 22) & 0x1u))) + ypos * width * 2;
        else if ((dispcnt & 0x20u) != 0u)
            addr = (int)(((tilenum & 0x01Fu) << 4) + ((tilenum & 0x3E0u) << 7)) + ypos * 256 * 2;
        else
            addr = (int)(((tilenum & 0x00Fu) << 4) + ((tilenum & 0x3F0u) << 7)) + ypos * 128 * 2;
        addr += x * 2;

        u32 color = MemHalf(base, addr & mask);
        if ((color & 0x8000u) == 0u)
            return false;
        pixel = color | pixelattr;
    }
    else
    {
        bool color256 = (attr0 & 0x2000u) != 0u;
        int tile = (int)tilenum;
        if ((dispcnt & 0x10u) != 0u)
        {
            tile <<= (int)((dispcnt >> 20) & 0x3u);
            tile += ((ypos >> 3) * (width >> 3)) << (color256 ? 1 : 0);
        }
        else
            tile += (ypos >> 3) * 0x20;

        pixelattr |= (mode == 1u) ? 0x80000000u : 0x10000000u;

        u32 color;
        if (color256)
        {
            int addr = (tile << 5) + ((ypos & 0x7) << 3) + ((x >> 3) << 6) + (x & 0x7);
            color = MemByte(base, addr & mask);
            if ((dispcnt & 0x80000000u) == 0u)
                pixelattr |= 0x1000u;
            else
                pixelattr |= (attr2 & 0xF000u) >> 4;
        }
        else
        {
            int addr = (tile << 5) + ((ypos & 0x7) << 2) + ((x >> 3) << 5) + ((x & 0x7) >> 1);
            u32 twoPixels = MemByte(base, addr & mask);
            color = (x & 1) != 0 ? (twoPixels >> 4) : (twoPixels & 0xFu);
            pixelattr |= 0x1000u | ((attr2 & 0xF000u) >> 8);
        }

        if (color == 0u)
            return false;
        pixel = color | pixelattr;
    }

    out = window ? 1u : pixel;
    return true;
}

u32 TextBGPixel(int bg, int nx, int vcount, bool unitB, u32 dispcnt, u32 bgcnt, u32 pos)
{
    int xoff = ((int)(pos & 0xFFFFu) + nx) & 0x1FF;
    int yoff = ((int)((pos >> 16) & 0xFFFFu) + vcount) & 0x1FF;
    int widexmask = (bgcnt & 0x4000u) != 0u ? 0x100 : 0;

    int tileset, tilemap, mask, base;
    if (unitB)
    {
        tileset = (int)((bgcnt & 0x003Cu) << 12);
        tilemap = (int)((bgcnt & 0x1F00u) << 3);
        mask = 0x1FFFF;
        base = MemBBG;
    }
    else
    {
        tileset = (int)(((dispcnt & 0x07000000u) >> 8) + ((bgcnt & 0x003Cu) << 12));
        tilemap = (int)(((dispcnt & 0x38000000u) >> 11) + ((bgcnt & 0x1F00u) << 3));
        mask = 0x7FFFF;
        base = MemABG;
    }

    if ((bgcnt & 0x8000u) != 0u)
    {
        tilemap += (yoff & 0x1F8) << 3;
        if ((bgcnt & 0x4000u) != 0u)
            tilemap += (yoff & 0x100) << 3;
    }
    else
        tilemap += (yoff & 0xF8) << 3;

    int tile = (int)MemHalf(base, (tilemap + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & mask);
    int ty = (tile & 0x0800) != 0 ? 7 - (yoff & 0x7) : (yoff & 0x7);
    int tx = (tile & 0x0400) != 0 ? 7 - (xoff & 0x7) : (xoff & 0x7);
    int palBase = unitB ? 0x400 : 0;

    u32 color;
    if ((bgcnt & 0x0080u) != 0u)
    {
        int pixels = tileset + ((tile & 0x03FF) << 6) + (ty << 3);
        int c = (int)MemByte(base, (pixels + tx) & mask);
        if (c == 0)
            return 0u;
        if ((dispcnt & 0x40000000u) != 0u)
        {
            int slot = (bg < 2 && (bgcnt & 0x2000u) != 0u) ? (2 + bg) : bg;
            color = MemHalf(unitB ? MemBBGExt : MemABGExt, slot * 8192 + ((tile >> 12) & 0xF) * 512 + c * 2);
        }
        else
            color = MemHalf(MemPal, palBase + c * 2);
    }
    else
    {
        int pixels = tileset + ((tile & 0x03FF) << 5) + (ty << 2);
        int b = (int)MemByte(base, (pixels + (tx >> 1)) & mask);
        int c = (tx & 0x1) != 0 ? (b >> 4) : (b & 0xF);
        if (c == 0)
            return 0u;
        color = MemHalf(MemPal, palBase + ((((tile & 0xF000) >> 8) + c) * 2));
    }

    return ColorWord(color, 0x01000000u << (u32)bg);
}

// kShadow2DFS_Main's AsInt and S16
int AsInt(u32 w)
{
    return (w & 0x80000000u) != 0u ? -(int)(~w) - 1 : (int)w;
}

int S16(u32 w)
{
    int v = (int)(w & 0xFFFFu);
    return v >= 0x8000 ? v - 0x10000 : v;
}

u32 RotBGPixel(int bg, int x, int y, bool ext, bool unitB, u32 dispcnt, u32 bgcnt)
{
    int size = (int)((bgcnt >> 14) & 0x3u);
    int coordmask = (0x8000 << size) - 0x800;
    if ((bgcnt & 0x2000u) == 0u && ((x | y) & ~(coordmask | 0x7FF)) != 0)
        return 0u;

    int tileset, tilemap, mask, base;
    if (unitB)
    {
        tileset = (int)((bgcnt & 0x003Cu) << 12);
        tilemap = (int)((bgcnt & 0x1F00u) << 3);
        mask = 0x1FFFF;
        base = MemBBG;
    }
    else
    {
        tileset = (int)(((dispcnt & 0x07000000u) >> 8) + ((bgcnt & 0x003Cu) << 12));
        tilemap = (int)(((dispcnt & 0x38000000u) >> 11) + ((bgcnt & 0x1F00u) << 3));
        mask = 0x7FFFF;
        base = MemABG;
    }

    int map = (((y & coordmask) >> 11) << (4 + size)) + ((x & coordmask) >> 11);
    int tx = (x >> 8) & 0x7;
    int ty = (y >> 8) & 0x7;
    int palBase = unitB ? 0x400 : 0;

    u32 color;
    if (ext)
    {
        int tile = (int)MemHalf(base, (tilemap + (map << 1)) & mask);
        if ((tile & 0x0400) != 0)
            tx = 7 - tx;
        if ((tile & 0x0800) != 0)
            ty = 7 - ty;
        int c = (int)MemByte(base, (tileset + ((tile & 0x03FF) << 6) + (ty << 3) + tx) & mask);
        if (c == 0)
            return 0u;
        if ((dispcnt & 0x40000000u) != 0u)
            color = MemHalf(unitB ? MemBBGExt : MemABGExt, bg * 8192 + ((tile >> 12) & 0xF) * 512 + c * 2);
        else
            color = MemHalf(MemPal, palBase + c * 2);
    }
    else
    {
        int tile = (int)MemByte(base, (tilemap + map) & mask);
        int c = (int)MemByte(base, (tileset + (tile << 6) + (ty << 3) + tx) & mask);
        if (c == 0)
            return 0u;
        color = MemHalf(MemPal, palBase + c * 2);
    }

    return ColorWord(color, 0x01000000u << (u32)bg);
}

u32 SpritePixel(u32 obj, bool unitB, u32 dispcnt)
{
    u32 color;
    if ((obj & 0x8000u) != 0u)
        color = obj & 0x7FFFu;
    else if ((dispcnt & 0x80000000u) != 0u && (obj & 0x1000u) == 0u)
        color = MemHalf(unitB ? MemBOBJExt : MemAOBJExt, (int)(obj & 0xFFFu) * 2);
    else
        color = MemHalf(MemPal, (unitB ? 0x600 : 0x200) + (int)(obj & 0xFFu) * 2);
    return ColorWord(color, obj & 0xFF000000u);
}

// kShadow2DFS_Main's copies of SoftRenderer's blend functions and ColorComposite
u32 ColorBlend4(u32 val1, u32 val2, u32 eva, u32 evb)
{
    u32 r =  (((val1 & 0x00003Fu) * eva) + ((val2 & 0x00003Fu) * evb) + 0x000008u) >> 4;
    u32 g = ((((val1 & 0x003F00u) * eva) + ((val2 & 0x003F00u) * evb) + 0x000800u) >> 4) & 0x007F00u;
    u32 b = ((((val1 & 0x3F0000u) * eva) + ((val2 & 0x3F0000u) * evb) + 0x080000u) >> 4) & 0x7F0000u;
    r = std::min(r, 0x00003Fu);
    g = std::min(g, 0x003F00u);
    b = std::min(b, 0x3F0000u);
    return r | g | b | 0xFF000000u;
}

u32 ColorBlend5(u32 val1, u32 val2)
{
    u32 eva = ((val1 >> 24) & 0x1Fu) + 1u;
    u32 evb = 32u - eva;
    if (eva == 32u)
        return val1;

    u32 r =  (((val1 & 0x00003Fu) * eva) + ((val2 & 0x00003Fu) * evb) + 0x000010u) >> 5;
    u32 g = ((((val1 & 0x003F00u) * eva) + ((val2 & 0x003F00u) * evb) + 0x001000u) >> 5) & 0x007F00u;
    u32 b = ((((val1 & 0x3F0000u) * eva) + ((val2 & 0x3F0000u) * evb) + 0x100000u) >> 5) & 0x7F0000u;
    r = std::min(r, 0x00003Fu);
    g = std::min(g, 0x003F00u);
    b = std::min(b, 0x3F0000u);
    return r | g | b | 0xFF000000u;
}

u32 ColorBrightnessUp(u32 val, u32 factor, u32 bias)
{
    u32 rb = val & 0x3F003Fu;
    u32 g = val & 0x003F00u;
    rb += ((((0x3F003Fu - rb) * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g +=  ((((0x003F00u - g ) * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g | 0xFF000000u;
}

u32 ColorBrightnessDown(u32 val, u32 factor, u32 bias)
{
    u32 rb = val & 0x3F003Fu;
    u32 g = val & 0x003F00u;
    rb -= (((rb * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g -=  (((g  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g | 0xFF000000u;
}

u32 BlendTarget2(u32 flag)
{
    if ((flag & 0x80u) != 0u) return 0x1000u;
    if ((flag & 0x40u) != 0u) return 0x0100u;
    return flag << 8;
}

u32 ColorComposite(u32 val1, u32 val2, u32 blend, u32 evy, u32 win)
{
    u32 coloreffect = 0u;
    u32 eva = 0u, evb = 0u;
    u32 flag1 = val1 >> 24;
    u32 flag2 = val2 >> 24;
    u32 blendCnt = blend & 0xFFFFu;
    u32 target2 = BlendTarget2(flag2);

    if ((flag1 & 0x80u) != 0u && (blendCnt & target2) != 0u)
    {
        coloreffect = 1u;
        if ((flag1 & 0x40u) != 0u)
        {
            eva = flag1 & 0x1Fu;
            evb = 16u - eva;
        }
        else
        {
            eva = (blend >> 16) & 0xFFu;
            evb = (blend >> 24) & 0xFFu;
        }
    }
    else if ((flag1 & 0x40u) != 0u && (blendCnt & target2) != 0u)
    {
        coloreffect = 4u;
    }
    else
    {
        if      ((flag1 & 0x80u) != 0u) flag1 = 0x10u;
        else if ((flag1 & 0x40u) != 0u) flag1 = 0x01u;

        if ((blendCnt & flag1) != 0u && (win & 0x20u) != 0u)
        {
            coloreffect = (blendCnt >> 6) & 0x3u;
            if (coloreffect == 1u)
            {
                if ((blendCnt & target2) != 0u)
                {
                    eva = (blend >> 16) & 0xFFu;
                    evb = (blend >> 24) & 0xFFu;
                }
                else
                    coloreffect = 0u;
            }
        }
    }

    if (coloreffect == 1u) return ColorBlend4(val1, val2, eva, evb);
    if (coloreffect == 2u) return ColorBrightnessUp(val1, evy, 0x8u);
    if (coloreffect == 3u) return ColorBrightnessDown(val1, evy, 0x7u);
    if (coloreffect == 4u) return ColorBlend5(val1, val2);
    return val1;
}

// kShadow2DFS_Main's InsideWindow and WindowMask (the closed form of CalculateWindowMask's scan)
bool InsideWindow(int x, u32 coords, u32 active)
{
    int x1 = (int)(coords & 0xFFu);
    int x2 = (int)((coords >> 8) & 0xFFu);
    bool open = (active & 0x2u) != 0u;
    bool opened = x1 <= x && x1 != x2;
    bool closed = x2 <= x;
    if (opened && closed)
        open = x1 > x2;
    else if (opened)
        open = true;
    else if (closed)
        open = false;
    return open && (active & 0x1u) != 0u;
}

u32 WindowMask(int x, u32 dispcnt, u32 win0, u32 win1, u32 winCnt, u32 active, bool objWin)
{
    u32 mask = (winCnt >> 16) & 0xFFu;
    if ((dispcnt & 0x8000u) != 0u && objWin)
        mask = (winCnt >> 24) & 0xFFu;
    if ((dispcnt & 0x4000u) != 0u && InsideWindow(x, win1, (active >> 16) & 0xFFFFu))
        mask = (winCnt >> 8) & 0xFFu;
    if ((dispcnt & 0x2000u) != 0u && InsideWindow(x, win0, active & 0xFFFFu))
        mask = winCnt & 0xFFu;
    return mask;
}

bool Is2D(u32 word)
{
    return ((word >> 24) & 0x9Fu) != 0u;
}

// kShadow2DFS_Main's StackPlanes
void StackPlanes(u32 top, u32 second, u32 third, bool unitB, u32 blend, u32 evy, u32 win, u32* p)
{
    u32 blendCnt = blend & 0xFFFFu;
    u32 eva = (blend >> 16) & 0xFFu;
    u32 evb = (blend >> 24) & 0xFFu;
    u32 flag1 = top >> 24;
    u32 flag2 = second >> 24;
    if (!unitB && (flag1 & 0xC0u) == 0x40u && (blendCnt & BlendTarget2(flag2)) != 0u)
    {
        p[0] = second;
        p[1] = ColorComposite(second, third, blend, evy, win);
        p[2] = 0x04000000u;
    }
    else if (!unitB && (flag1 & 0xC0u) == 0x40u)
    {
        u32 effect = (blendCnt >> 6) & 0x3u;
        if (effect == 1u || (blendCnt & 0x0001u) == 0u || (win & 0x20u) == 0u)
            effect = 0u;
        p[0] = second;
        p[1] = ColorComposite(second, third, blend, evy, win);
        p[2] = (effect << 24) | (evy << 8);
    }
    else if (!unitB && (flag2 & 0xC0u) == 0x40u && (blendCnt & 0x01C0u) == 0x0140u)
    {
        u32 effect = 1u;
        u32 target1;
        if      ((flag1 & 0x80u) != 0u) target1 = 0x0010u;
        else if ((flag1 & 0x40u) != 0u) target1 = 0x0001u;
        else                            target1 = flag1;
        if ((flag1 & 0xC0u) != 0xC0u && !(((blendCnt & target1) != 0u && (win & 0x20u) != 0u) || (flag1 & 0xC0u) == 0x80u))
            effect = 7u;
        p[0] = top;
        p[1] = ColorComposite(top, third, blend, evy, win);
        p[2] = (effect << 24) | (evb << 16) | (eva << 8);
    }
    else
    {
        p[0] = ColorComposite(top, second, blend, evy, win);
        p[1] = 0u;
        p[2] = 0x07000000u;
    }
}

// the affine composite's FloorDiv, StackInsert and what its main() makes of a sub-pixel's stack
int FloorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((b - 1 - a) / b);
}

void StackInsert(u32& t0, u32& t1, u32& t2, u32 v, int at)
{
    if (at == 0)
    {
        t2 = t1;
        t1 = t0;
        t0 = v;
    }
    else if (at == 1)
    {
        t2 = t1;
        t1 = v;
    }
    else if (at == 2)
        t2 = v;
}

// a line's constants as the affine composite has them: its run's (the uniforms GLCompositor::RunShadow2D
// sets from the run's first line, Shadow2D::PackAffineLine), with the reference points moved on by
// lines B and D, as the shader does
struct AffineLine
{
    u32 Flags, DispCnt, BGCnt23, Blend, EVY;
    int Ref[4];         // BG2 X, Y, BG3 X, Y
    int Rot[2][4];      // A, B, C, D of BG2 and BG3
};

AffineLine ReadAffineLine(const u32* c, int lines)
{
    AffineLine l;
    for (int i = 0; i < 2; i++)
    {
        l.Rot[i][0] = S16(c[4 + i * 2]);
        l.Rot[i][1] = S16(c[4 + i * 2] >> 16);
        l.Rot[i][2] = S16(c[5 + i * 2]);
        l.Rot[i][3] = S16(c[5 + i * 2] >> 16);
        l.Ref[i * 2] = AsInt(c[i * 2]) + lines * l.Rot[i][1];
        l.Ref[i * 2 + 1] = AsInt(c[i * 2 + 1]) + lines * l.Rot[i][3];
    }
    l.Flags = c[8];
    l.DispCnt = c[9] & 0xFF000000u;
    l.EVY = c[9] & 0xFFu;
    l.BGCnt23 = c[10];
    l.Blend = c[11];
    return l;
}

// the affine composite's planes for the sub-pixel (subX, subY) of native pixel nx at this scale, from
// the planes pass's stack texel (stack[0-3])
void AffinePlanes(const AffineLine& l, const u32* stack, int nx, int subX, int subY, int scale, u32* p)
{
    u32 t0 = stack[0], t1 = stack[1], t2 = stack[2];
    u32 win = stack[3] & 0xFFu;
    int at2 = (int)((stack[3] >> 8) & 0x3u);
    int at3 = (int)((stack[3] >> 10) & 0x3u);
    u32 bgcnt2 = l.BGCnt23 & 0xFFFFu;
    u32 bgcnt3 = (l.BGCnt23 >> 16) & 0xFFFFu;
    bool unitB = (l.Flags & 0x1u) != 0u;
    u32 affineMask = (l.Flags >> 25) & 0x3u;

    u32 a2 = 0u, a3 = 0u;
    if (at2 < 3)
        a2 = RotBGPixel(2, l.Ref[0] + nx * l.Rot[0][0] + FloorDiv(subX * l.Rot[0][0] + subY * l.Rot[0][1], scale),
                           l.Ref[1] + nx * l.Rot[0][2] + FloorDiv(subX * l.Rot[0][2] + subY * l.Rot[0][3], scale),
                        (affineMask & 0x1u) == 0u, unitB, l.DispCnt, bgcnt2);
    if (at3 < 3)
        a3 = RotBGPixel(3, l.Ref[2] + nx * l.Rot[1][0] + FloorDiv(subX * l.Rot[1][0] + subY * l.Rot[1][1], scale),
                           l.Ref[3] + nx * l.Rot[1][2] + FloorDiv(subX * l.Rot[1][2] + subY * l.Rot[1][3], scale),
                        (affineMask & 0x2u) == 0u, unitB, l.DispCnt, bgcnt3);

    if ((bgcnt3 & 0x3u) >= (bgcnt2 & 0x3u))
    {
        if (a3 != 0u) StackInsert(t0, t1, t2, a3, at3);
        if (a2 != 0u) StackInsert(t0, t1, t2, a2, at2);
    }
    else
    {
        if (a2 != 0u) StackInsert(t0, t1, t2, a2, at2);
        if (a3 != 0u) StackInsert(t0, t1, t2, a3, at3);
    }
    StackPlanes(t0, t1, t2, unitB, l.Blend, l.EVY, win, p);
}

}

void Shadow2DReplay::Frame(const GPU& gpu, int backbuf, void* user)
{
    Shadow2DReplay& replay = *(Shadow2DReplay*)user;
    Shadow2DRecorder& rec = *gpu.Shadow2D;

    // MemTex as the compositor keeps it up to date (it uploads every frame that draws a line, as
    // this does every frame), then the same uploads into a full copy, which it has to match
    bool full = MemGeneration != rec.Generation;
    if (full)
        Mem.assign(MemRows * 256, 0);
    auto upload = [](std::vector<u32>& mem)
    {
        return [&mem](u32 x, u32 row, u32 width, u32 rows, const u8* source)
        {
            for (u32 r = 0; r < rows; r++)
                memcpy(&mem[(row + r) * 256 + x], source + r * width * 4, width * 4);
        };
    };
    rec.ForEachUpload(gpu, full, upload(Mem));
    MemGeneration = rec.Generation;
    memset(rec.Dirty, 0, sizeof(rec.Dirty));
    MemFull.assign(MemRows * 256, 0);
    rec.ForEachUpload(gpu, true, upload(MemFull));
    for (size_t i = 0; i < Mem.size(); i++)
    {
        if (Mem[i] == MemFull[i])
            continue;
        if (replay.MemMismatches++ < 10 && replay.Log)
            fprintf(replay.Log, "REPLAY stale MemTex word row=%zu x=%zu has=%08X should=%08X\n", i / 256, i % 256, Mem[i], MemFull[i]);
    }

    // the OBJ pass at 1x: per ObjTex row (unit * 192 + line) and native X 0..255, the lowest
    // (priority, OAM index) of the opaque pixels, as the depth test keeps it, in R. the OBJ window
    // sprites come after the sprites in the instance buffer and draw G in rows 384 on
    static u32 objTex[ObjRows][256][2];
    static u32 objDepth[ObjRows][256];
    memset(objTex, 0, sizeof(objTex));
    for (auto& row : objDepth)
        for (u32& d : row)
            d = ~0u;
    for (const std::vector<Instance>* list : {&rec.Sprites, &rec.WindowSprites})
    {
        for (const Instance& inst : *list)
        {
            u32 key = ((inst.Attr2 >> 10) & 0x3u) * 128u + (inst.Index & 0x7Fu);
            u32 channel = (inst.Flags & 2u) ? 1 : 0;
            for (u32 r = inst.Row; r < inst.Row + inst.Lines && r < ObjRows; r++)
            {
                for (int nx = std::max(inst.X, 0); nx < std::min(inst.X + (int)inst.Width, 256); nx++)
                {
                    u32 pixel;
                    if (key >= objDepth[r][nx] || !ObjPixel(inst, nx, (int)r, pixel))
                        continue;
                    objDepth[r][nx] = key;
                    objTex[r][nx][channel] = pixel;
                }
            }
        }
    }

    // every 8th frame, the affine composite over all sub-pixels at SubScale too
    const bool subFrame = (replay.Frames++ % 8) == 0;

    for (int screen = 0; screen < 2; screen++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][screen].get();
        // the affine composite's runs, as GLCompositor::BuildShadow2DQuads makes them: by the lines'
        // constants (Shadow2D::AffineRunContinues), or past 8 runs by the lines alone, the shader
        // reading each line's own constants (per line)
        auto lineConstants = [&](int line, u32* c)
        {
            const u32* b = &fb[GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords];
            PackAffineLine(b, b[W_Flags] | F_Drawn | (HiresBGs(b) << F_HiresShift), c);
        };
        auto affineLine = [&](int line)
        {
            const u32* b = &fb[GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords];
            return rec.LineReason(b) == R_None && HiresBGs(b) != 0;
        };
        int runStart[192];
        bool perLine = false;
        for (int pass = 0; pass < 2; pass++)
        {
            perLine = pass != 0;
            int runs = 0;
            for (int line = 0; line < 192; )
            {
                if (!affineLine(line))
                {
                    runStart[line++] = -1;
                    continue;
                }
                u32 last[AffineWords];
                lineConstants(line, last);
                int start = line;
                runStart[line++] = start;
                runs++;
                while (line < 192 && affineLine(line))
                {
                    if (!perLine)
                    {
                        u32 c[AffineWords];
                        lineConstants(line, c);
                        if (!AffineRunContinues(last, c))
                            break;
                        memcpy(last, c, sizeof(last));
                    }
                    runStart[line++] = start;
                }
            }
            if (perLine || runs <= 8)
            {
                replay.AffineRuns += runs;
                replay.PerLineRuns += perLine ? runs : 0;
                break;
            }
        }

        for (int line = 0; line < 192; line++)
        {
            const u32* s = &fb[GPU2D::AccelShadowStateStart + line * GPU2D::ShadowStateWords];
            if (rec.LineReason(s) != R_None)
                continue;
            replay.Lines++;

            u32 flags = s[W_Flags];
            bool unitB = (flags & 0x1u) != 0u;
            u32 dispcnt = s[W_DispCnt];
            u32 bgcnt[4] = {s[W_BGCnt01] & 0xFFFFu, (s[W_BGCnt01] >> 16) & 0xFFFFu, s[W_BGCnt23] & 0xFFFFu, (s[W_BGCnt23] >> 16) & 0xFFFFu};
            u32 pos[4] = {s[W_BGPos0], s[W_BGPos1], s[W_BGPos2], s[W_BGPos3]};
            int vcount = (int)(s[W_Lines] & 0xFFFFu);
            u32 textMask = (flags >> 12) & 0xFu;
            bool bg0Is3D = (flags & 0x800u) != 0u;
            bool objWinLine = false;

            // as the compositor flags the line at 2x and up (Shadow2D::HiresBGs into F_HiresShift)
            u32 affineMask = ((flags >> F_AffineShift) & 0x3u) << 2;
            u32 rotMask = affineMask | (((flags >> F_ExtShift) & 0x3u) << 2);
            u32 hiresMask = HiresBGs(s) << 2;
            replay.AffineLines += affineMask != 0u;
            replay.ExtLines += (rotMask & ~affineMask) != 0u;
            replay.HiresLines += hiresMask != 0u;
            // the line's own constants, which the planes pass reads from the block; and its run's,
            // which the affine composite gets
            u32 constants[AffineWords];
            PackAffineLine(s, flags | F_Drawn | ((hiresMask >> 2) << F_HiresShift), constants);
            AffineLine affine = ReadAffineLine(constants, 0);
            AffineLine affineRun = affine;
            if (hiresMask && !perLine)
            {
                u32 runConstants[AffineWords];
                lineConstants(runStart[line], runConstants);
                affineRun = ReadAffineLine(runConstants, line - runStart[line]);
            }
            for (int i = 0; i < 2 && replay.Log; i++)
            {
                if (!(hiresMask & (4u << i)))
                    continue;
                bool ext = !(affineMask & (4u << i));
                u64 key = (u64)(s[W_RotAB2 + i * 2]) | ((u64)(s[W_RotCD2 + i * 2]) << 32);
                key ^= ((u64)((unitB ? 4 : 0) | (i << 1) | (ext ? 1 : 0))) << 61;
                bool seen = false;
                for (int k = 0; k < replay.SeenRotCount; k++)
                    seen |= replay.SeenRot[k] == key;
                if (seen || replay.SeenRotCount >= 64)
                    continue;
                replay.SeenRot[replay.SeenRotCount++] = key;
                u32 cnt = bgcnt[2 + i];
                fprintf(replay.Log, "REPLAY rotbg frame=%u unit=%c bg=%d kind=%s A=%d B=%d C=%d D=%d size=%u wrap=%u extpal=%u screen=%d line=%d\n",
                    replay.Frames, unitB ? 'B' : 'A', 2 + i, ext ? "ext" : "affine", affine.Rot[i][0], affine.Rot[i][1],
                    affine.Rot[i][2], affine.Rot[i][3], (cnt >> 14) & 0x3u, (cnt >> 13) & 0x1u,
                    (dispcnt >> 30) & 0x1u, screen, line);
            }

            for (int nx = 0; nx < 256; nx++)
            {
                u32 obj = 0u;
                if ((flags & (0x10u << 3)) != 0u)
                {
                    obj = objTex[(unitB ? 192 : 0) + line][nx][0];
                    if ((obj & 0x40000u) == 0u)
                        obj = 0u;
                }

                u32 win = 0xFFu;
                if ((dispcnt & 0xE000u) != 0u)
                {
                    int wx = std::clamp(nx, 0, 255);
                    if ((flags & F_WinEdge) != 0u)
                        wx = std::clamp(wx, 1, 254);
                    bool objWin = false;
                    if ((dispcnt & 0x8000u) != 0u)
                        objWin = objTex[ObjWindowRow + (unitB ? 192 : 0) + line][wx][1] != 0u;
                    win = WindowMask(wx, dispcnt, s[W_Win0], s[W_Win1], s[W_WinCnt], s[W_WinActive], objWin);
                    replay.WindowLines += nx == 0;
                    replay.ObjWindowOnLines += nx == 0 && (dispcnt & 0x8000u) != 0u;
                    objWinLine |= objWin;
                }

                // the planes pass's two stacks: DrawPixel_Accel's, and the one without the BGs
                // sampled at sub-pixels with where they come in it (3 for nowhere)
                u32 top = 0u, second = 0u, third = 0u;
                int found = 0;
                u32 na[3] = {0u, 0u, 0u};
                int foundNA = 0;
                int at[2] = {3, 3};
                auto push = [&](u32 v) { if (found == 0) top = v; else if (found == 1) second = v; else if (found == 2) third = v; found++; };
                auto pushNA = [&](u32 v) { if (foundNA < 3) na[foundNA] = v; foundNA++; };
                for (int prio = 0; prio < 4 && (found < 3 || foundNA < 3); prio++)
                {
                    if (obj != 0u && (int)((obj >> 16) & 0x3u) == prio && (win & 0x10u) != 0u)
                    {
                        u32 pixel = SpritePixel(obj, unitB, dispcnt);
                        push(pixel);
                        pushNA(pixel);
                    }
                    for (int bg = 0; bg < 4 && (found < 3 || foundNA < 3); bg++)
                    {
                        if ((int)(bgcnt[bg] & 0x3u) != prio || ((win >> (u32)bg) & 0x1u) == 0u)
                            continue;
                        if (bg == 0 && bg0Is3D)
                        {
                            push(0x40000000u);
                            pushNA(0x40000000u);
                            continue;
                        }
                        u32 bit = 1u << (u32)bg;
                        if ((textMask & bit) == 0u && (rotMask & bit) == 0u)
                            continue;
                        bool hires = (hiresMask & bit) != 0u;
                        if (hires)
                        {
                            at[bg - 2] = std::min(foundNA, 3);
                            if (found >= 3)
                                continue;
                        }

                        u32 pixel;
                        if ((textMask & bit) != 0u)
                            pixel = TextBGPixel(bg, nx, vcount, unitB, dispcnt, bgcnt[bg], pos[bg]);
                        else
                        {
                            int i = bg - 2;
                            pixel = RotBGPixel(bg, affine.Ref[i * 2] + nx * affine.Rot[i][0], affine.Ref[i * 2 + 1] + nx * affine.Rot[i][2],
                                               (affineMask & bit) == 0u, unitB, dispcnt, bgcnt[bg]);
                        }
                        if (pixel != 0u)
                        {
                            push(pixel);
                            if (!hires)
                                pushNA(pixel);
                        }
                    }
                }
                u32 backdrop = ColorWord((s[W_EVYBackdrop] >> 16) & 0xFFFFu, 0x20000000u);
                push(backdrop);
                pushNA(backdrop);
                // plane 2 as the pass starts is 0, except unit B's without EngineBNoHold (unit A's
                // leftover), whose lines that could read it the recorder keeps on the CPU
                push(0u);
                pushNA(0u);

                u32 p[3];
                StackPlanes(top, second, third, unitB, s[W_Blend], s[W_EVYBackdrop] & 0xFFu, win, p);

                const u32* planes = &fb[line * (256*3 + 1)];
                for (int plane = 0; plane < 3; plane++)
                {
                    u32 expected = planes[plane * 256 + nx];
                    if (expected == p[plane])
                        continue;
                    if (replay.Mismatches++ < 20 && replay.Log)
                        fprintf(replay.Log, "REPLAY mismatch screen=%d line=%d x=%d plane=%d expected=%08X got=%08X unit=%c\n",
                            screen, line, nx, plane, expected, p[plane], unitB ? 'B' : 'A');
                }

                if (!hiresMask)
                    continue;

                // the affine composite from this texel of StackTex: at the native pixel's first
                // sub-pixel it has to give the planes just written, at any scale
                const u32 stack[4] = {na[0], na[1], na[2], win | ((u32)at[0] << 8) | ((u32)at[1] << 10)};
                u32 q[3];
                AffinePlanes(affineRun, stack, nx, 0, 0, replay.SubScale, q);
                for (int plane = 0; plane < 3; plane++)
                {
                    if (q[plane] == p[plane])
                        continue;
                    if (replay.StackMismatches++ < 20 && replay.Log)
                        fprintf(replay.Log, "REPLAY stack mismatch screen=%d line=%d x=%d plane=%d planes=%08X composite=%08X unit=%c\n",
                            screen, line, nx, plane, p[plane], q[plane], unitB ? 'B' : 'A');
                }
                if (!subFrame)
                    continue;
                for (int subY = 0; subY < replay.SubScale; subY++)
                {
                    for (int subX = 0; subX < replay.SubScale; subX++)
                    {
                        AffinePlanes(affineRun, stack, nx, subX, subY, replay.SubScale, q);
                        replay.SubPixels++;
                        replay.SubDiffers += q[0] != p[0] || q[1] != p[1] || q[2] != p[2];
                    }
                }
            }
            replay.ObjWindowLines += objWinLine;
        }
    }
}
