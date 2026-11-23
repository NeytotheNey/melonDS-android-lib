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

// n / d rounded to nearest, symmetrically around 0, kept within int32 (a shifted view's terms can add up past it)
inline int32_t RoundedRatio(int64_t n, int d)
{
    int64_t q = (n + (n < 0 ? -(d / 2) : (d / 2))) / d;
    return (int32_t)(q < INT32_MIN ? INT32_MIN : q > INT32_MAX ? INT32_MAX : q);
}

// Squeezes clip-space X so the native frustum only covers 256 of the screen's
// [width] columns, starting (width - 256) / 2 + shiftX in from the left (shiftX
// positive moves it right). Everything the game would have clipped at the sides
// now lands in the extra space instead. [w] is the vertex's clip W.
inline int32_t ProjectX(int32_t x, int32_t w, int width, int shiftX)
{
    return RoundedRatio((int64_t)x * NativeWidth + (int64_t)2 * shiftX * w, width);
}

// the margin only applies to a wide view, and is clamped to 0..MaxMargin
inline int SanitizeMargin(int margin, int viewWidth)
{
    if (viewWidth == NativeWidth || margin < 0)
        return 0;
    return margin > MaxMargin ? MaxMargin : margin;
}

// ProjectX for clip-space Y: squeezes the native frustum into 192 rows of a view [height] rows
// tall (192 + 2 * margin), starting margin + shiftY rows down (shiftY positive moves it down, and
// clip Y points up), rounded the same way
inline int32_t ProjectY(int32_t y, int32_t w, int height, int shiftY)
{
    return RoundedRatio((int64_t)y * NativeHeight - (int64_t)2 * shiftY * w, height);
}

// the view shift that fits a view: native column 0 at (width - 256) / 2 + shiftX and row 0 at margin + shiftY,
// so each side keeps 0 or more columns and rows
inline int ClampShift(int shift, int room)
{
    return shift < -room ? -room : shift > room ? room : shift;
}

}
