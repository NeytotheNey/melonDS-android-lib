// WideMelon widescreen helpers.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace WideMelon
{

constexpr int NativeWidth = 256;
constexpr int MaxWidth = 768;

// Anything outside 256..768 or odd falls back to the native width.
inline int SanitizeWidth(int width)
{
    if (width < NativeWidth || width > MaxWidth || (width & 1))
        return NativeWidth;
    return width;
}

// Squeezes clip-space X so the native frustum only covers the centre 256/width
// of the screen. Everything the game would have clipped at the sides now lands
// in the extra space instead. Rounds to nearest, symmetrically around 0.
inline int32_t ProjectX(int32_t x, int width)
{
    int64_t n = (int64_t)x * NativeWidth;
    return (int32_t)((n + (n < 0 ? -(width / 2) : (width / 2))) / width);
}

}
