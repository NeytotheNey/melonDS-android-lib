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

#ifndef GPU_HIRESSTREAM_H
#define GPU_HIRESSTREAM_H

#include "types.h"

// hi-res display capture, regular display lines (see GLCompositor::PrepareHiresStream): a game can
// send a captured image to a screen by any route (DMA through main RAM, CPU copies, remapping the
// bank), and what proves a line shows that image is its pixels. these are the comparisons, with no
// GL in them, on a frame's lines in the accelerated format: 3 planes of 256 words, then the
// control word (GPU2D::SoftRenderer::DrawScanline)
namespace melonDS::HiresStream
{

constexpr int LineWords = 256*3 + 1;
constexpr int Rows = 192;

inline bool IsRegularDisplay(const u32* line) noexcept
{
    return ((line[256*3] >> 16) & 0x3) == 1;
}

// the color a captured word shows as: unpacked to 6 bits like the native VRAM display line, so
// the top bit (the capture's alpha) is ignored
inline u32 WordColor(u16 word) noexcept
{
    return ((word & 0x001F) << 1) | ((word & 0x03E0) << 4) | ((word & 0x7C00) << 7);
}

// whether a regular display line shows exactly this row of captured words. a pixel only qualifies
// when the compositor's final color for it is plane 0's: plane 2's compmode is above 4
// (kCompositePixelFS), so it reads no 3D and blends nothing. plane 0's top byte is the 2D layer
// flags, which never reach the screen
inline bool LineMatchesRow(const u32* line, const u16* row) noexcept
{
    for (int x = 0; x < 256; x++)
    {
        if (((line[512 + x] >> 24) & 0xF) <= 4)
            return false;
        if ((line[x] & 0xFFFFFF) != WordColor(row[x]))
            return false;
    }
    return true;
}

// all one color: such a line says nothing about where it came from
inline bool LineIsUniform(const u32* line) noexcept
{
    u32 color = line[0] & 0xFFFFFF;
    for (int x = 1; x < 256; x++)
    {
        if ((line[x] & 0xFFFFFF) != color)
            return false;
    }
    return true;
}

// a hi-res image's native side: Words has 256 per bank row, for the rows from RowStart on below
// 192 (display line L can only show row L). no Words = no image
struct Source
{
    const u16* Words = nullptr;
    int RowStart = 0;
    int RowCount = 0;

    bool Covers(int row) const noexcept { return Words && row >= RowStart && row < RowStart + RowCount; }
    bool MatchesLine(const u32* line, int row) const noexcept
    {
        return Covers(row) && LineMatchesRow(line, &Words[row * 256]);
    }
};

// whether the compositor shows plane 0 as it is all along the line, so whether the line can tell
// anything: under live 3D it's a 2D layer that may not even be visible
inline bool LineIsPlain(const u32* line) noexcept
{
    for (int x = 0; x < 256; x++)
    {
        if (((line[512 + x] >> 24) & 0xF) <= 4)
            return false;
    }
    return true;
}

// the one source a frame's lines are streamed from, or -1. screens are the frame's two halves,
// and count is at most MaxSources. the lines that can tell are the regular display ones that are
// plain and not uniform (uniform lines still show the image once it's picked), and the source
// that most of them match wins. one line can't decide it: a static HUD row can equal an older
// capture, and the top of two captures can be the same. a tie means the images look the same
// over those lines, and the first one wins, so a bank's copy over the older scratch one.
// a source that's matched by less than half of them isn't a stream either, as most of what's on
// screen came from somewhere else.
// anyDetail says whether there were such lines, so whether a -1 means the frame's lines came from
// somewhere else or there was nothing to tell by. it stays false without any source, as nothing
// is scanned then
constexpr int MaxSources = 8;
inline int FindSource(const u32* const screens[2], const Source* sources, int count, bool& anyDetail) noexcept
{
    anyDetail = false;

    bool anySource = false;
    for (int i = 0; i < count; i++)
        anySource = anySource || sources[i].Words;
    if (!anySource)
        return -1;

    int detail = 0;
    int matches[MaxSources] = {};
    for (int half = 0; half < 2; half++)
    {
        for (int row = 0; row < Rows; row++)
        {
            const u32* line = &screens[half][row * LineWords];
            if (!IsRegularDisplay(line) || LineIsUniform(line) || !LineIsPlain(line))
                continue;

            detail++;
            for (int i = 0; i < count; i++)
            {
                if (sources[i].MatchesLine(line, row))
                    matches[i]++;
            }
        }
    }
    anyDetail = detail > 0;

    int best = -1;
    int bestMatches = 0;
    for (int i = 0; i < count; i++)
    {
        if (matches[i] > bestMatches)
        {
            best = i;
            bestMatches = matches[i];
        }
    }

    return bestMatches * 2 >= detail ? best : -1;
}

}

#endif // GPU_HIRESSTREAM_H
