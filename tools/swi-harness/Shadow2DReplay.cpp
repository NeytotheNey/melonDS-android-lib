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
// C++ copy of kShadow2DObjFS_Main and of kShadow2DFS_Main's SHADOW2D_PLANES path (GPU_OpenGL_shaders.h),
// run on what GLCompositor::UploadShadow2D would upload at the Blit (the flats, the recorder's palette
// copy, the blocks and the sprite instances), at 1x. Each line the recorder lets through gets its
// planes 0-2 compared word for word with what SoftRenderer drew, as the check mode does on the phone.
// It proves the shaders' maths and the recorder's rules, not the GL driver: keep it in step with the
// shaders when they change.

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

// kShadow2DObjFS_Main for the pixel at native X nx of the instance's line ObjTex row: false if discarded
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

    out = window ? 0u : pixel;
    return !window;
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
    // (priority, OAM index) of the opaque pixels, as the depth test keeps it. window sprites only set
    // G, which nothing reads in increment 1
    static u32 objTex[384][256];
    static u32 objDepth[384][256];
    memset(objTex, 0, sizeof(objTex));
    for (auto& row : objDepth)
        for (u32& d : row)
            d = ~0u;
    for (const Instance& inst : rec.Sprites)
    {
        u32 key = ((inst.Attr2 >> 10) & 0x3u) * 128u + (inst.Index & 0x7Fu);
        for (u32 r = inst.Row; r < inst.Row + inst.Lines && r < 384; r++)
        {
            for (int nx = std::max(inst.X, 0); nx < std::min(inst.X + (int)inst.Width, 256); nx++)
            {
                u32 pixel;
                if (key >= objDepth[r][nx] || !ObjPixel(inst, nx, (int)r, pixel))
                    continue;
                objDepth[r][nx] = key;
                objTex[r][nx] = pixel;
            }
        }
    }

    for (int screen = 0; screen < 2; screen++)
    {
        const u32* fb = gpu.Framebuffer[backbuf][screen].get();
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

            for (int nx = 0; nx < 256; nx++)
            {
                u32 obj = 0u;
                if ((flags & (0x10u << 3)) != 0u)
                {
                    obj = objTex[(unitB ? 192 : 0) + line][nx];
                    if ((obj & 0x40000u) == 0u)
                        obj = 0u;
                }

                u32 top = 0u, second = 0u;
                int found = 0;
                auto push = [&](u32 v) { if (found == 0) top = v; else second = v; found++; };
                for (int prio = 0; prio < 4 && found < 2; prio++)
                {
                    if (obj != 0u && (int)((obj >> 16) & 0x3u) == prio)
                        push(SpritePixel(obj, unitB, dispcnt));
                    for (int bg = 0; bg < 4 && found < 2; bg++)
                    {
                        if ((int)(bgcnt[bg] & 0x3u) != prio)
                            continue;
                        if (bg == 0 && bg0Is3D)
                        {
                            push(0x40000000u);
                            continue;
                        }
                        if (((textMask >> (u32)bg) & 0x1u) == 0u)
                            continue;
                        u32 pixel = TextBGPixel(bg, nx, vcount, unitB, dispcnt, bgcnt[bg], pos[bg]);
                        if (pixel != 0u)
                            push(pixel);
                    }
                }
                if (found < 2)
                    push(ColorWord((s[W_EVYBackdrop] >> 16) & 0xFFFFu, 0x20000000u));

                u32 p[3];
                if (!unitB && ((top >> 24) & 0xC0u) == 0x40u)
                {
                    p[0] = second;
                    p[1] = second;
                    p[2] = (s[W_EVYBackdrop] & 0xFFu) << 8;
                }
                else
                {
                    p[0] = top;
                    p[1] = 0u;
                    p[2] = 0x07000000u;
                }

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
            }
        }
    }
}
