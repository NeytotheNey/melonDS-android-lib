// WideMelon widescreen helpers.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace WideMelon
{

constexpr int NativeWidth = 256;
constexpr int MaxWidth = 768;
constexpr int NativeHeight = 192;
// the vertical margin: extra 3D rows above and below the top screen's 192 (camera stage 2)
constexpr int MaxMargin = 128;

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

// the margin only applies to a wide view, and is clamped to 0..MaxMargin
inline int SanitizeMargin(int margin, int viewWidth)
{
    if (viewWidth == NativeWidth || margin < 0)
        return 0;
    return margin > MaxMargin ? MaxMargin : margin;
}

// ProjectX for clip-space Y: squeezes the native frustum into the middle 192 rows of a view
// [height] rows tall (192 + 2 * margin), rounded the same way
inline int32_t ProjectY(int32_t y, int height)
{
    int64_t n = (int64_t)y * NativeHeight;
    return (int32_t)((n + (n < 0 ? -(height / 2) : (height / 2))) / height);
}

}
