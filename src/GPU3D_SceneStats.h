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

#ifndef GPU3D_SCENESTATS_H
#define GPU3D_SCENESTATS_H

#include "types.h"

namespace melonDS
{

// GPU::SceneStats (debug.melonade.scenestats): what the scenes line 215 hands the renderer are made
// of, from the CPU's polygon data, whatever the renderer (GPU3D::NoteSceneStats). zeroed with each
// SCENESTATS line, once a second:
//   frames identical   line 215 renders, and of those the ones the latched state said were the same as
//                      the last (RenderFrameIdentical; a renderer can skip more, see COMPWHY)
//   nopolys            renders with no polygon
//   bg3d               of the frames, the ones with engine A's BG0 showing 3D (DISPCNT bit 3)
//   tex hl atest blend aa edge fog fogalpha bitmap
//                      the ones with that DISP3DCNT bit on: texture mapping (0), highlight shading (1),
//                      alpha test (2), alpha blending (3), anti-aliasing (4), edge marking (5), fog
//                      alpha only (6), fog (7), clear bitmap (14)
//   wbuffer            the ones whose polygons were W-buffered (the scene's first polygon's flag, which
//                      the GL renderers use for the whole scene)
//   polys              per frame, polygons that passed setup (not Degenerate), split into opaque, trans
//                      (translucent, no shadow), shadow (translucent shadows) and mask (shadow masks),
//                      and into textured (DISP3DCNT bit 0 on and a texture format) and untextured.
//                      lines are the line polygons among them
//   od od_opaque od_trans od_mask
//                      per frame, the summed screen area of those polygons from the CPU's final vertex
//                      positions (shoelace) over the screen's: rasterized fragments per displayed 3D
//                      pixel. FinalPosition is the native 256x192 frame, with a WideMelon view squeezed
//                      into it by 256/width, so it's over 256x192 and means the same at any width (the
//                      share of the whole wide view). od is opaque + trans (shadows in trans), masks are
//                      left out of it
//   peakpolys peakod   the most of each in one frame
//   width              the view width the last scene was built for
struct SceneStatsCounters
{
    s64 LogStartNs = 0;
    u32 Frames = 0, Identical = 0, NoPolys = 0;
    u32 BG3D = 0, Tex = 0, Highlight = 0, AlphaTest = 0, Blend = 0, AA = 0, Edge = 0, Fog = 0, FogAlpha = 0, Bitmap = 0;
    u32 WBuffer = 0;
    u64 Polys = 0, Opaque = 0, Trans = 0, Shadow = 0, Mask = 0, Textured = 0, Untextured = 0, Lines = 0;
    double OverdrawOpaque = 0, OverdrawTrans = 0, OverdrawMask = 0;
    u32 PeakPolys = 0;
    double PeakOverdraw = 0;
    int Width = 256;
};

}

#endif
