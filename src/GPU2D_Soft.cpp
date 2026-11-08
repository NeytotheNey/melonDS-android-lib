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

#include "GPU2D_Soft.h"
#include "GPU.h"
#include "GPU3D.h"
#include "GPU_CaptureReads.h"

namespace melonDS
{
namespace GPU2D
{
SoftRenderer::SoftRenderer(melonDS::GPU& gpu)
    : Renderer2D(), GPU(gpu)
{
    // mosaic table is initialized at compile-time
}

u32 SoftRenderer::ColorComposite(int i, u32 val1, u32 val2) const
{
    u32 coloreffect = 0;
    u32 eva, evb;

    u32 flag1 = val1 >> 24;
    u32 flag2 = val2 >> 24;

    u32 blendCnt = CurUnit->BlendCnt;

    u32 target2;
    if      (flag2 & 0x80) target2 = 0x1000;
    else if (flag2 & 0x40) target2 = 0x0100;
    else                   target2 = flag2 << 8;

    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        // sprite blending

        coloreffect = 1;

        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        else
        {
            eva = CurUnit->EVA;
            evb = CurUnit->EVB;
        }
    }
    else if ((flag1 & 0x40) && (blendCnt & target2))
    {
        // 3D layer blending

        coloreffect = 4;
    }
    else
    {
        if      (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;

        if ((blendCnt & flag1) && (WindowMask[i] & 0x20))
        {
            coloreffect = (blendCnt >> 6) & 0x3;

            if (coloreffect == 1)
            {
                if (blendCnt & target2)
                {
                    eva = CurUnit->EVA;
                    evb = CurUnit->EVB;
                }
                else
                    coloreffect = 0;
            }
        }
    }

    switch (coloreffect)
    {
    case 0: return val1;
    case 1: return ColorBlend4(val1, val2, eva, evb);
    case 2: return ColorBrightnessUp(val1, CurUnit->EVY, 0x8);
    case 3: return ColorBrightnessDown(val1, CurUnit->EVY, 0x7);
    case 4: return ColorBlend5(val1, val2);
    }

    return val1;
}

void SoftRenderer::RunJob(const LineJob& job, u32 engines)
{
    CurJob = &job;

    if (job.DrawBG)
    {
        if (engines & EngineA) DrawScanline(job.Line, &job.BG[0]);
        if (engines & EngineB) DrawScanline(job.Line, &job.BG[1]);
    }

    // sprites are pre-rendered one scanline in advance
    if (job.DrawOBJ)
    {
        if (engines & EngineA) DrawSprites(job.SpriteLine, &job.OBJ[0]);
        if (engines & EngineB) DrawSprites(job.SpriteLine, &job.OBJ[1]);
    }
}

u16* SoftRenderer::GetBGExtPal(u32 slot, u32 pal) const
{
    const u32 PaletteSize = 256 * 2;
    const u32 SlotSize = PaletteSize * 16;
    return (u16*)&(CurUnit->Num == 0
         ? GPU.VRAMFlat_ABGExtPal
         : GPU.VRAMFlat_BBGExtPal)[slot * SlotSize + pal * PaletteSize];
}

u16* SoftRenderer::GetOBJExtPal() const
{
    return CurUnit->Num == 0
         ? (u16*)GPU.VRAMFlat_AOBJExtPal
         : (u16*)GPU.VRAMFlat_BOBJExtPal;
}

void SoftRenderer::GetBGVRAM(u8*& data, u32& mask) const
{
    if (CurUnit->Num == 0)
    {
        data = GPU.VRAMFlat_ABG;
        mask = 0x7FFFF;
    }
    else
    {
        data = GPU.VRAMFlat_BBG;
        mask = 0x1FFFF;
    }
}

void SoftRenderer::GetOBJVRAM(u8*& data, u32& mask) const
{
    if (CurUnit->Num == 0)
    {
        data = GPU.VRAMFlat_AOBJ;
        mask = 0x3FFFF;
    }
    else
    {
        data = GPU.VRAMFlat_BOBJ;
        mask = 0x1FFFF;
    }
}

void SoftRenderer::DrawScanline(u32 line, const UnitState* unit)
{
    CurUnit = unit;

    int stride = CurJob->Accelerated ? (256*3 + 1) : 256;
    u32* dst = &CurJob->Framebuffer[CurUnit->Num][stride * line];

    line = CurJob->VCount;

    // the flat VRAM copies are made coherent by GPU::MakeVRAMFlat_2DBGCoherent

    bool forceblank = false;

    // scanlines that end up outside of the GPU drawing range
    // (as a result of writing to VCount) are filled white
    if (line > 192) forceblank = true;

    // a unit disabled by POWCNT1 outputs a fixed colour instead (see DrawScanline_BGOBJ)

    // CaptureLatch is set for line 0 by Unit::PreDrawScanline

    // the 3D line is fetched when the job is built (GPU::Snapshot2DScanline), or just before a held
    // display capture line is drawn (GPU::Flush2DDeferred), as the GL renderers read it back from a
    // GL buffer. lines that don't fetch one keep the last one, as before
    if (CurUnit->Num == 0 && CurJob->Line3D)
        _3DLine = CurJob->Line3D;

    if (forceblank)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFFFFFFFF;

        if (CurJob->Accelerated)
        {
            dst[256*3] = 0;

            // white wings next to the white line (see below)
            if (CurJob->WideView)
                dst[256*3] = (1 << 21);
        }

        if (!CurJob->ScreensEnabled)
            FillScreensOff(dst);
        return;
    }

    u32 dispmode = CurUnit->DispCnt >> 16;
    dispmode &= (CurUnit->Num ? 0x1 : 0x3);

    // always render regular graphics
    DrawScanline_BGOBJ(line);

    switch (dispmode)
    {
    case 0: // screen off
        {
            for (int i = 0; i < 256; i++)
                dst[i] = 0x003F3F3F;
        }
        break;

    case 1: // regular display
        {
            int i = 0;
            for (; i < (stride & ~1); i+=2)
                *(u64*)&dst[i] = *(u64*)&BGOBJLine[i];
        }
        break;

    case 2: // VRAM display
        {
            u32 vrambank = (CurUnit->DispCnt >> 18) & 0x3;
            if (CurJob->VRAMMap_LCDC & (1<<vrambank))
            {
                u16* vram = (u16*)GPU.VRAM[vrambank];
                vram = &vram[line * 256];

                for (int i = 0; i < 256; i++)
                {
                    u16 color = vram[i];
                    u8 r = (color & 0x001F) << 1;
                    u8 g = (color & 0x03E0) >> 4;
                    u8 b = (color & 0x7C00) >> 9;

                    dst[i] = r | (g << 8) | (b << 16);
                }
            }
            else
            {
                for (int i = 0; i < 256; i++)
                {
                    dst[i] = 0;
                }
            }
        }
        break;

    case 3: // FIFO display
        {
            for (int i = 0; i < 256; i++)
            {
                u16 color = CurJob->DispFIFOBuffer[i];
                u8 r = (color & 0x001F) << 1;
                u8 g = (color & 0x03E0) >> 4;
                u8 b = (color & 0x7C00) >> 9;

                dst[i] = r | (g << 8) | (b << 16);
            }
        }
        break;
    }

    // capture
    if ((CurUnit->Num == 0) && CurUnit->CaptureLatch)
    {
        u32 capwidth, capheight;
        switch ((CurUnit->CaptureCnt >> 20) & 0x3)
        {
        case 0: capwidth = 128; capheight = 128; break;
        case 1: capwidth = 256; capheight = 64;  break;
        case 2: capwidth = 256; capheight = 128; break;
        case 3: capwidth = 256; capheight = 192; break;
        }

        if (line < capheight)
            DoCapture(line, capwidth);
    }

    u32 masterBrightness = CurUnit->MasterBrightness;

    if (CurJob->Accelerated)
    {
        u32 xpos = CurJob->RenderXPos;

        dst[256*3] = masterBrightness |
                     (CurUnit->DispCnt & 0x30000) |
                     (xpos << 24) | ((xpos & 0x100) << 15);

        if (CurJob->HiresCapture)
        {
            // hi-res display capture: bit 7 = a VRAM display line the GL compositor may show from
            // its hi-res copy of the bank in bits 5-6 (see GLCompositor::PrepareHiresDisplay). only
            // when the bank is in LCDC mode and no VCount write moved the line, so the line shows
            // bank row Line. MasterBrightness is masked again first: a savestate loads it unmasked,
            // and older ones may have these bits set
            dst[256*3] &= ~0x3FE0;
            u32 vrambank = (CurUnit->DispCnt >> 18) & 0x3;
            if (dispmode == 2 && (CurJob->VRAMMap_LCDC & (1<<vrambank)) && CurJob->VCount == CurJob->Line)
                dst[256*3] |= 0x80 | (vrambank << 5);
        }

        if (CurJob->WideView)
        {
            // wide 3D view: bit 18 = this line shows unit A's 3D layer in the wings, bits 19-20 = its
            // BLDCNT fade (BG0 as 1st target) and bits 8-12 = EVY for it, bit 21 = white wings, for
            // a white line (screen off or forced blank), bit 22 = unit A shows 3D on BG0 on this
            // line, whatever the windows do with it (the wing hold reads it to tell which half
            // unit A is in). the fade bits are only read with bit 18: a line whose wings a window
            // hid has black wings, and a held line takes the copied wings with their fade as
            // drawn. the master brightness register doesn't use bits 5-13: 5-7 are the hi-res
            // capture flag above, and the GL compositor flags held wing lines with bit 13 in its
            // own copy (see GLCompositor::PrepareWingHold)
            dst[256*3] &= ~0x1F00;

            if ((dispmode == 0) || ((dispmode == 1) &&
                (CurUnit->ForcedBlank || (CurUnit->Num && !CurUnit->Enabled))))
            {
                dst[256*3] |= (1 << 21);
            }
            else if ((CurUnit->Num == 0) && CurUnit->Enabled && (dispmode == 1) &&
                !CurUnit->ForcedBlank && (CurUnit->LayerEnable & 0x01) && (CurUnit->DispCnt & 0x8))
            {
                dst[256*3] |= (1 << 22);

                // with windows on, the wings follow BG0 at the edge columns (the mask built by
                // DrawScanline_BGOBJ for this line, with WINOUT and the OBJ window in it), so a
                // window letterbox blacks them out with the middle. the control word has one
                // wings bit, so both show 3D if either edge does
                if (!(CurUnit->DispCnt & 0xE000) || ((WindowMask[0] | WindowMask[255]) & 0x01))
                {
                    u32 effect = (CurUnit->BlendCnt & 0x0001) ? ((CurUnit->BlendCnt >> 6) & 0x3) : 0;
                    u32 evy = CurUnit->EVY;
                    if (evy > 16) evy = 16;

                    dst[256*3] |= (1 << 18) | (effect << 19) | (evy << 8);
                }
            }
        }

        if (!CurJob->ScreensEnabled)
            FillScreensOff(dst);
        return;
    }

    // master brightness
    if (dispmode != 0)
    {
        if ((masterBrightness >> 14) == 1)
        {
            // up
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessUp(dst[i], factor, 0x0);
            }
        }
        else if ((masterBrightness >> 14) == 2)
        {
            // down
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessDown(dst[i], factor, 0xF);
            }
        }
    }

    // convert to 32-bit BGRA
    // note: 32-bit RGBA would be more straightforward, but
    // BGRA seems to be more compatible (Direct2D soft, cairo...)
    for (int i = 0; i < 256; i+=2)
    {
        u64 c = *(u64*)&dst[i];

        u64 r = (c << 18) & 0xFC000000FC0000;
        u64 g = (c << 2) & 0xFC000000FC00;
        u64 b = (c >> 14) & 0xFC000000FC;
        c = r | g | b;

        *(u64*)&dst[i] = c | ((c & 0x00C0C0C000C0C0C0) >> 6) | 0xFF000000FF000000;
    }

    if (!CurJob->ScreensEnabled)
        FillScreensOff(dst);
}

// POWCNT1 bit 0 clear: the screens show black, whatever the units output. display capture
// is done by then, so it still sees the units' output
void SoftRenderer::FillScreensOff(u32* dst)
{
    if (CurJob->Accelerated)
    {
        // the compositor shader composites the line as usual, then applies the control word's
        // master brightness: regular display with brightness down by 16 ((x*16+0xF)>>4 == x)
        // yields black for every 6-bit value, so no extra shader bit is needed. the wings bits
        // are cleared, so a wide view's wings go black too
        dst[256*3] = 0x00018010;
    }
    else
    {
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFF000000;
    }
}

void SoftRenderer::VBlankEnd(Unit* unitA, Unit* unitB)
{
#ifdef OGLRENDERER_ENABLED
    if (Renderer3D& renderer3d = GPU.GPU3D.GetCurrentRenderer(); renderer3d.Accelerated)
    {
        // read back already, right after the render at line 215 (GPU::BlitAfterReadback), into a
        // buffer of its own, and nothing has drawn into the 3D frame since: it becomes the one
        // GetLine maps. dropped whether this frame captures or not, so it's never used past the
        // line 0 it was made for, and one that isn't used here leaves GetLine's buffer as it was,
        // for a capture armed later in this line
        bool captures3D = (unitA->CaptureCnt & (1<<31)) && (((unitA->CaptureCnt >> 29) & 0x3) != 1);
        bool earlyReadback = renderer3d.TakeEarlyCaptureFrame(captures3D);
        if (GPU.CapReads && earlyReadback)
            GPU.CapReads->NoteEarlyReadbackUsed();

        if (captures3D && !earlyReadback)
        {
            renderer3d.PrepareCaptureFrame();

            // the lines this arms are held until line 192 (see GPU::Begin2DJob), so submit the
            // readback now instead of leaving it to the map there
            if (GPU.DeferCapture2D && GPU.EarlyFlushCapture)
                renderer3d.FlushCommands();
        }
    }
#endif
}

void SoftRenderer::DoCapture(u32 line, u32 width)
{
    u32 captureCnt = CurUnit->CaptureCnt;
    u32 dstvram = (captureCnt >> 16) & 0x3;
    u32 capmode = (captureCnt >> 29) & 0x3;

    // hi-res display capture: what this line captures from, for the GL compositor to redo it at
    // the 3D scale (see GPU::HiresCapture). the native capture below is unchanged
    HiresCaptureRecord* record = nullptr;
    HiresCaptureRecord::Line* recordLine = nullptr;
    if (CurJob->HiresCapture)
    {
        record = &GPU.RecordHiresCaptureLine(line, captureCnt);
        recordLine = &record->Lines[line];
        recordLine->RenderXPos = CurJob->RenderXPos;

        if (CurJob->VCount != CurJob->Line)
            record->Skip |= HiresCapSkip_VCount;
        if (capmode != 1 && CurJob->Abort3DLine)
            record->Skip |= HiresCapSkip_Abort;
    }

    // TODO: confirm this
    // it should work like VRAM display mode, which requires VRAM to be mapped to LCDC
    if (!(CurJob->VRAMMap_LCDC & (1<<dstvram)))
    {
        if (record)
            record->Skip |= HiresCapSkip_NoLCDC;
        if (GPU.CapReads)
            GPU.CapReads->NoteNoLCDCLine();
        return;
    }

    u16* dst = (u16*)GPU.VRAM[dstvram];
    u32 dstaddr = (((captureCnt >> 18) & 0x3) << 14) + (line * width);

    // TODO: handle 3D in GPU3D::CurrentRenderer->Accelerated mode!!

    u32* srcA;
    if (captureCnt & (1<<24))
    {
        srcA = _3DLine;
    }
    else
    {
        srcA = BGOBJLine;

        // the planes as the compositor gets them, before plane 0 is overwritten below
        if (record && capmode != 1)
            memcpy(record->Planes[line], BGOBJLine, sizeof(record->Planes[line]));

        if (CurJob->Accelerated)
        {
            // in GPU3D::CurrentRenderer->Accelerated mode, compositing is normally done on the GPU
            // but when doing display capture, we do need the composited output
            // so we do it here

            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 compmode = (val3 >> 24) & 0xF;

                if (compmode == 4)
                {
                    // 3D on top, blending

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                        val1 = ColorBlend5(_3dval, val1);
                    else
                        val1 = val2;
                }
                else if (compmode == 1)
                {
                    // 3D on bottom, blending

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 eva = (val3 >> 8) & 0x1F;
                        u32 evb = (val3 >> 16) & 0x1F;

                        val1 = ColorBlend4(val1, _3dval, eva, evb);
                    }
                    else
                        val1 = val2;
                }
                else if (compmode <= 3)
                {
                    // 3D on top, normal/fade

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 evy = (val3 >> 8) & 0x1F;

                        val1 = _3dval;
                        if      (compmode == 2) val1 = ColorBrightnessUp(val1, evy, 0x8);
                        else if (compmode == 3) val1 = ColorBrightnessDown(val1, evy, 0x7);
                    }
                    else
                        val1 = val2;
                }

                BGOBJLine[i] = val1;
            }
        }
    }

    const u16* srcB = NULL;
    u32 srcBaddr = line * 256;

    if (captureCnt & (1<<25))
    {
        srcB = &CurJob->DispFIFOBuffer[0];
        srcBaddr = 0;
    }
    else
    {
        u32 srcvram = (CurUnit->DispCnt >> 18) & 0x3;
        if (CurJob->VRAMMap_LCDC & (1<<srcvram))
            srcB = (u16*)GPU.VRAM[srcvram];

        if (((CurUnit->DispCnt >> 16) & 0x3) != 2)
            srcBaddr += ((captureCnt >> 26) & 0x3) << 14;
    }

    dstaddr &= 0xFFFF;
    srcBaddr &= 0xFFFF;

    // debug only, see GPU_CaptureReads.h. the addresses are in halfwords, so 256 of them make a row
    if (GPU.CapReads)
    {
        bool readsBank = capmode != 0 && srcB && !(captureCnt & (1<<25));
        GPU.CapReads->NoteCaptureLine(line, captureCnt, dstaddr >> 8,
                                      readsBank ? (s32)((CurUnit->DispCnt >> 18) & 0x3) : -1, srcBaddr >> 8);
    }

    if (record && capmode != 0)
    {
        recordLine->SrcBPresent = srcB != NULL;
        recordLine->SrcBFIFO = (captureCnt & (1<<25)) != 0;
        recordLine->SrcBBank = (CurUnit->DispCnt >> 18) & 0x3;
        recordLine->SrcBRow = srcBaddr >> 8;
        if (srcB)
        {
            for (u32 i = 0; i < 256; i++)
                record->SrcB[line][i] = srcB[(srcBaddr + i) & 0xFFFF];
        }
    }

    static_assert(VRAMDirtyGranularity == 512);
    GPU.VRAMDirty[dstvram][(dstaddr * 2) / VRAMDirtyGranularity] = true;

    switch ((captureCnt >> 29) & 0x3)
    {
    case 0: // source A
        {
            for (u32 i = 0; i < width; i++)
            {
                u32 val = srcA[i];

                // TODO: check what happens when alpha=0

                u32 r = (val >> 1) & 0x1F;
                u32 g = (val >> 9) & 0x1F;
                u32 b = (val >> 17) & 0x1F;
                u32 a = ((val >> 24) != 0) ? 0x8000 : 0;

                dst[dstaddr] = r | (g << 5) | (b << 10) | a;
                dstaddr = (dstaddr + 1) & 0xFFFF;
            }
        }
        break;

    case 1: // source B
        {
            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = srcB[srcBaddr];
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = 0;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;

    case 2: // sources A+B
    case 3:
        {
            u32 eva = captureCnt & 0x1F;
            u32 evb = (captureCnt >> 8) & 0x1F;

            // checkme
            if (eva > 16) eva = 16;
            if (evb > 16) evb = 16;

            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    val = srcB[srcBaddr];

                    u32 rB = val & 0x1F;
                    u32 gB = (val >> 5) & 0x1F;
                    u32 bB = (val >> 10) & 0x1F;
                    u32 aB = val >> 15;

                    u32 rD = ((rA * aA * eva) + (rB * aB * evb) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + (gB * aB * evb) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + (bB * aB * evb) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0) | (evb>0 ? aB : 0);

                    if (rD > 0x1F) rD = 0x1F;
                    if (gD > 0x1F) gD = 0x1F;
                    if (bD > 0x1F) bD = 0x1F;

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    u32 rD = ((rA * aA * eva) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0);

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;
    }
}

#define DoDrawBG(type, line, num) \
    do \
    { \
        if ((bgCnt[num] & 0x0040) && (CurUnit->BGMosaicSize[0] > 0)) \
        { \
            if (CurJob->Accelerated) DrawBG_##type<true, DrawPixel_Accel>(line, num); \
            else DrawBG_##type<true, DrawPixel_Normal>(line, num); \
        } \
        else \
        { \
            if (CurJob->Accelerated) DrawBG_##type<false, DrawPixel_Accel>(line, num); \
            else DrawBG_##type<false, DrawPixel_Normal>(line, num); \
        } \
    } while (false)

#define DoDrawBG_Large(line) \
    do \
    { \
        if ((bgCnt[2] & 0x0040) && (CurUnit->BGMosaicSize[0] > 0)) \
        { \
            if (CurJob->Accelerated) DrawBG_Large<true, DrawPixel_Accel>(line); \
            else DrawBG_Large<true, DrawPixel_Normal>(line); \
        } \
        else \
        { \
            if (CurJob->Accelerated) DrawBG_Large<false, DrawPixel_Accel>(line); \
            else DrawBG_Large<false, DrawPixel_Normal>(line); \
        } \
    } while (false)

#define DoInterleaveSprites(prio) \
    if (CurJob->Accelerated) InterleaveSprites<DrawPixel_Accel>(prio); else InterleaveSprites<DrawPixel_Normal>(prio);

template<u32 bgmode>
void SoftRenderer::DrawScanlineBGMode(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u32 layerEnable = CurUnit->LayerEnable;
    const u16* bgCnt = CurUnit->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[3] & 0x3) == i)
        {
            if (layerEnable & 0x08)
            {
                if (bgmode >= 3)
                    DoDrawBG(Extended, line, 3);
                else if (bgmode >= 1)
                    DoDrawBG(Affine, line, 3);
                else
                    DoDrawBG(Text, line, 3);
            }
        }
        if ((bgCnt[2] & 0x3) == i)
        {
            if (layerEnable & 0x04)
            {
                if (bgmode == 5)
                    DoDrawBG(Extended, line, 2);
                else if (bgmode == 4 || bgmode == 2)
                    DoDrawBG(Affine, line, 2);
                else
                    DoDrawBG(Text, line, 2);
            }
        }
        if ((bgCnt[1] & 0x3) == i)
        {
            if (layerEnable & 0x02)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (layerEnable & 0x01)
            {
                if (!CurUnit->Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((layerEnable & 0x10) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16));
        }

    }
}

void SoftRenderer::DrawScanlineBGMode6(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u32 layerEnable = CurUnit->LayerEnable;
    const u16* bgCnt = CurUnit->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[2] & 0x3) == i)
        {
            if (layerEnable & 0x04)
            {
                DoDrawBG_Large(line);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (layerEnable & 0x01)
            {
                if ((!CurUnit->Num) && (dispCnt & 0x8))
                    DrawBG_3D();
            }
        }
        if ((layerEnable & 0x10) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanlineBGMode7(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u32 layerEnable = CurUnit->LayerEnable;
    const u16* bgCnt = CurUnit->BGCnt;
    // mode 7 only has text-mode BG0 and BG1

    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[1] & 0x3) == i)
        {
            if (layerEnable & 0x02)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (layerEnable & 0x01)
            {
                if (!CurUnit->Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((layerEnable & 0x10) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanline_BGOBJ(u32 line)
{
    if (!CurUnit->Enabled)
    {
        // a unit disabled by POWCNT1 outputs a fixed colour: black for unit A, white for unit B.
        // display mode, master brightness and display capture still apply to it
        u32 fillcolor = (CurUnit->Num == 0) ? 0xFF000000 : 0xFF3F3F3F;
        for (int i = 0; i < 256; i++)
            BGOBJLine[i] = fillcolor;

        if (CurJob->Accelerated)
        {
            // no 3D pixel involved (see the compositing below)
            for (int i = 0; i < 256; i++)
            {
                BGOBJLine[256+i] = 0;
                BGOBJLine[512+i] = 0x07000000;
            }
        }

        return;
    }

    // forced blank disables BG/OBJ compositing (leaving it takes effect two lines later, see
    // Unit::PreDrawScanline)
    if (CurUnit->ForcedBlank)
    {
        for (int i = 0; i < 256; i++)
            BGOBJLine[i] = 0xFF3F3F3F;

        if (CurJob->Accelerated)
        {
            // no 3D pixel involved (see the compositing below)
            for (int i = 0; i < 256; i++)
            {
                BGOBJLine[256+i] = 0;
                BGOBJLine[512+i] = 0x07000000;
            }
        }

        return;
    }

    u64 backdrop;
    if (CurUnit->Num) backdrop = *(u16*)&CurJob->Palette[0x400];
    else     backdrop = *(u16*)&CurJob->Palette[0];

    {
        u8 r = (backdrop & 0x001F) << 1;
        u8 g = ((backdrop & 0x03E0) >> 4) | ((backdrop & 0x8000) >> 15);
        u8 b = (backdrop & 0x7C00) >> 9;

        backdrop = r | (g << 8) | (b << 16) | 0x20000000;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i+=2)
            *(u64*)&BGOBJLine[i] = backdrop;
    }

    // a pass only resets the first plane, so a column no layer draws in keeps the last pass's second
    // plane as what's under the backdrop. engine B's pass leaves it 0, and engine A's first pass after
    // it depends on that: with engine A's parts held (GPU::EngineBNoHold) they can follow each other,
    // and what they leave would reach the 3D compositing and display capture. this also stops engine
    // B from seeing what engine A leaves, which only shows with the backdrop as a blend's first target
    if (CurJob->Accelerated && CurJob->EngineBNoHold)
        memset(&BGOBJLine[256], 0, 256 * sizeof(u32));

    if (CurUnit->DispCnt & 0xE000)
    {
        CurUnit->CalculateWindowMask(line, WindowMask, OBJWindow[CurUnit->Num]);

        // wide 3D view: x2 is exclusive and at most 255, so column 255 is only inside a window
        // that wraps (x1 > x2), and a game may start one at x1 = 1. on hardware that's a hidden
        // edge pixel, but here the wings carry on past it, so it shows as a 1-px seam (backdrop)
        // between the middle and the wings. give the edge columns their neighbour's mask, for
        // both units (unit B can sit next to held wings, see GLCompositor::PrepareWingHold).
        // this also goes into display capture, and the wings rule in DrawScanline reads the
        // patched edges. the wide check does it in both runs, as it changes the middle on purpose
        if (CurJob->WideView || GPU.GPU3D.WideCheck)
        {
            WindowMask[0] = WindowMask[1];
            WindowMask[255] = WindowMask[254];
        }
    }
    else
        memset(WindowMask, 0xFF, 256);

    ApplySpriteMosaicX();
    CurBGXMosaicTable = MosaicTable[CurUnit->BGMosaicSize[0]].data();

    switch (CurUnit->DispCnt & 0x7)
    {
    case 0: DrawScanlineBGMode<0>(line); break;
    case 1: DrawScanlineBGMode<1>(line); break;
    case 2: DrawScanlineBGMode<2>(line); break;
    case 3: DrawScanlineBGMode<3>(line); break;
    case 4: DrawScanlineBGMode<4>(line); break;
    case 5: DrawScanlineBGMode<5>(line); break;
    case 6: DrawScanlineBGMode6(line); break;
    case 7: DrawScanlineBGMode7(line); break;
    }

    // color special effects
    // can likely be optimized

    if (!CurJob->Accelerated)
    {
        for (int i = 0; i < 256; i++)
        {
            u32 val1 = BGOBJLine[i];
            u32 val2 = BGOBJLine[256+i];

            BGOBJLine[i] = ColorComposite(i, val1, val2);
        }
    }
    else
    {
        if (CurUnit->Num == 0)
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 flag1 = val1 >> 24;
                u32 flag2 = val2 >> 24;

                u32 bldcnteffect = (CurUnit->BlendCnt >> 6) & 0x3;

                u32 target1;
                if      (flag1 & 0x80) target1 = 0x0010;
                else if (flag1 & 0x40) target1 = 0x0001;
                else                   target1 = flag1;

                u32 target2;
                if      (flag2 & 0x80) target2 = 0x1000;
                else if (flag2 & 0x40) target2 = 0x0100;
                else                   target2 = flag2 << 8;

                if (((flag1 & 0xC0) == 0x40) && (CurUnit->BlendCnt & target2))
                {
                    // 3D on top, blending

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(i, val2, val3);
                    BGOBJLine[512+i] = 0x04000000;
                }
                else if ((flag1 & 0xC0) == 0x40)
                {
                    // 3D on top, normal/fade

                    if (bldcnteffect == 1)             bldcnteffect = 0;
                    if (!(CurUnit->BlendCnt & 0x0001)) bldcnteffect = 0;
                    if (!(WindowMask[i] & 0x20))       bldcnteffect = 0;

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(i, val2, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (CurUnit->EVY << 8);
                }
                else if (((flag2 & 0xC0) == 0x40) && ((CurUnit->BlendCnt & 0x01C0) == 0x0140))
                {
                    // 3D on bottom, blending

                    u32 eva, evb;
                    if ((flag1 & 0xC0) == 0xC0)
                    {
                        eva = flag1 & 0x1F;
                        evb = 16 - eva;
                    }
                    else if (((CurUnit->BlendCnt & target1) && (WindowMask[i] & 0x20)) ||
                            ((flag1 & 0xC0) == 0x80))
                    {
                        eva = CurUnit->EVA;
                        evb = CurUnit->EVB;
                    }
                    else
                        bldcnteffect = 7;

                    BGOBJLine[i]     = val1;
                    BGOBJLine[256+i] = ColorComposite(i, val1, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (CurUnit->EVB << 16) | (CurUnit->EVA << 8);
                }
                else
                {
                    // no potential 3D pixel involved

                    BGOBJLine[i]     = ColorComposite(i, val1, val2);
                    BGOBJLine[256+i] = 0;
                    BGOBJLine[512+i] = 0x07000000;
                }
            }
        }
        else
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];

                BGOBJLine[i]     = ColorComposite(i, val1, val2);
                BGOBJLine[256+i] = 0;
                BGOBJLine[512+i] = 0x07000000;
            }
        }
    }

    // the BG and OBJ mosaic counters, affine reference points and window X state
    // advance in Unit::PostDrawScanline

    /*if (OBJMosaicY >= OBJMosaicYMax)
    {
        OBJMosaicY = 0;
        OBJMosaicYMax = OBJMosaicSize[1];
    }
    else
        OBJMosaicY++;*/
}


void SoftRenderer::DrawPixel_Normal(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = ((color & 0x03E0) >> 4) | ((color & 0x8000) >> 15);
    u8 b = (color & 0x7C00) >> 9;

    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawPixel_Accel(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = ((color & 0x03E0) >> 4) | ((color & 0x8000) >> 15);
    u8 b = (color & 0x7C00) >> 9;

    *(dst+512) = *(dst+256);
    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawBG_3D()
{
    int i = 0;

    if (CurJob->Accelerated)
    {
        for (i = 0; i < 256; i++)
        {
            if (!(WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+512] = BGOBJLine[i+256];
            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = 0x40000000; // 3D-layer placeholder
        }
    }
    else
    {
        for (i = 0; i < 256; i++)
        {
            u32 c = _3DLine[i];

            if ((c >> 24) == 0) continue;
            if (!(WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = c | 0x40000000;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Text(u32 line, u32 bgnum)
{
    // workaround for backgrounds missing on aarch64 with lto build
    asm volatile ("" : : : "memory");

    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal, extpalslot;

    u16 xoff = CurUnit->BGXPos[bgnum];
    u16 yoff = CurUnit->BGYPos[bgnum];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic: the line the mosaic row started on
        yoff += CurUnit->BGMosaicLine;
    }
    else
        yoff += line;

    u32 widexmask = (bgcnt & 0x4000) ? 0x100 : 0;

    extpal = (CurUnit->DispCnt & 0x40000000);
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(bgvram, bgvrammask);
    if (CurUnit->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&CurJob->Palette[0x400];
    }
    else
    {
        tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&CurJob->Palette[0];
    }

    // adjust Y position in tilemap
    if (bgcnt & 0x8000)
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & 0x4000)
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u16 curtile;
    u16* curpal;
    u32 pixelsaddr;
    u8 color;
    u32 lastxpos;

    if (bgcnt & 0x0080)
    {
        // 256-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask];

            if (extpal) curpal = GetBGExtPal(extpalslot, curtile>>12);
            else        curpal = pal;

            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];

                if (extpal) curpal = GetBGExtPal(extpalslot, curtile>>12);
                else        curpal = pal;

                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
    else
    {
        // 16-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[((tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3))) & bgvrammask];
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                if (tilexoff & 0x1)
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4;
                }
                else
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F;
                }

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Affine(u32 line, u32 bgnum)
{
    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;

    u32 coordmask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: coordmask = 0x07800; yshift = 7; break;
    case 0x4000: coordmask = 0x0F800; yshift = 8; break;
    case 0x8000: coordmask = 0x1F800; yshift = 9; break;
    case 0xC000: coordmask = 0x3F800; yshift = 10; break;
    }

    u32 overflowmask;
    if (bgcnt & 0x2000) overflowmask = 0;
    else                overflowmask = ~(coordmask | 0x7FF);

    s16 rotA = CurUnit->BGRotA[bgnum-2];
    s16 rotC = CurUnit->BGRotC[bgnum-2];

    // vertical mosaic is in the latched reference points (see Unit::PreDrawScanline)
    s32 rotX = CurUnit->BGXRefInternal[bgnum-2];
    s32 rotY = CurUnit->BGYRefInternal[bgnum-2];

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(bgvram, bgvrammask);

    if (CurUnit->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&CurJob->Palette[0x400];
    }
    else
    {
        tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&CurJob->Palette[0];
    }

    u16 curtile;
    u8 color;

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<bgnum))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if ((!((finalX|finalY) & overflowmask)))
            {
                curtile = bgvram[(tilemapaddr + ((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11))) & bgvrammask];

                // draw pixel
                u32 tilexoff = (finalX >> 8) & 0x7;
                u32 tileyoff = (finalY >> 8) & 0x7;

                color = bgvram[(tilesetaddr + (curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Extended(u32 line, u32 bgnum)
{
    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal;

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(bgvram, bgvrammask);

    extpal = (CurUnit->DispCnt & 0x40000000);

    s16 rotA = CurUnit->BGRotA[bgnum-2];
    s16 rotC = CurUnit->BGRotC[bgnum-2];

    // vertical mosaic is in the latched reference points (see Unit::PreDrawScanline)
    s32 rotX = CurUnit->BGXRefInternal[bgnum-2];
    s32 rotY = CurUnit->BGYRefInternal[bgnum-2];

    if (bgcnt & 0x0080)
    {
        // bitmap modes

        u32 xmask, ymask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
        case 0x4000: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
        case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        u32 ofxmask, ofymask;
        if (bgcnt & 0x2000)
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        if (CurUnit->Num) tilemapaddr = ((bgcnt & 0x1F00) << 6);
        else              tilemapaddr = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & 0x0004)
        {
            // direct color bitmap

            u16 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = *(u16*)&bgvram[(tilemapaddr + (((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) << 1)) & bgvrammask];

                        if (color & 0x8000)
                            drawPixel(&BGOBJLine[i], color & 0x7FFF, 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            // 256-color bitmap

            if (CurUnit->Num) pal = (u16*)&CurJob->Palette[0x400];
            else              pal = (u16*)&CurJob->Palette[0];

            u8 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = bgvram[(tilemapaddr + (((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                        if (color)
                            drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        // mixed affine/text mode

        u32 coordmask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: coordmask = 0x07800; yshift = 7; break;
        case 0x4000: coordmask = 0x0F800; yshift = 8; break;
        case 0x8000: coordmask = 0x1F800; yshift = 9; break;
        case 0xC000: coordmask = 0x3F800; yshift = 10; break;
        }

        u32 overflowmask;
        if (bgcnt & 0x2000) overflowmask = 0;
        else                overflowmask = ~(coordmask | 0x7FF);

        if (CurUnit->Num)
        {
            tilesetaddr = ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&CurJob->Palette[0x400];
        }
        else
        {
            tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&CurJob->Palette[0];
        }

        u16 curtile;
        u16* curpal;
        u8 color;

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (WindowMask[i] & (1<<bgnum))
            {
                s32 finalX, finalY;
                if (mosaic)
                {
                    int im = CurBGXMosaicTable[i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if ((!((finalX|finalY) & overflowmask)))
                {
                    curtile = *(u16*)&bgvram[(tilemapaddr + (((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11)) << 1)) & bgvrammask];

                    if (extpal) curpal = GetBGExtPal(bgnum, curtile>>12);
                    else        curpal = pal;

                    // draw pixel
                    u32 tilexoff = (finalX >> 8) & 0x7;
                    u32 tileyoff = (finalY >> 8) & 0x7;

                    if (curtile & 0x0400) tilexoff = 7-tilexoff;
                    if (curtile & 0x0800) tileyoff = 7-tileyoff;

                    color = bgvram[(tilesetaddr + ((curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                    if (color)
                        drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
                }
            }

            rotX += rotA;
            rotY += rotC;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Large(u32 line) // BG is always BG2
{
    u16 bgcnt = CurUnit->BGCnt[2];

    u16* pal;

    // large BG sizes:
    // 0: 512x1024
    // 1: 1024x512
    // 2: 512x256
    // 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
    case 0x4000: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
    case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
    case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    u32 ofxmask, ofymask;
    if (bgcnt & 0x2000)
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    s16 rotA = CurUnit->BGRotA[0];
    s16 rotC = CurUnit->BGRotC[0];

    // vertical mosaic is in the latched reference points (see Unit::PreDrawScanline)
    s32 rotX = CurUnit->BGXRefInternal[0];
    s32 rotY = CurUnit->BGYRefInternal[0];

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(bgvram, bgvrammask);

    // 256-color bitmap

    if (CurUnit->Num) pal = (u16*)&CurJob->Palette[0x400];
    else     pal = (u16*)&CurJob->Palette[0];

    u8 color;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<2))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!(finalX & ofxmask) && !(finalY & ofymask))
            {
                color = bgvram[((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<2);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

// OBJ line buffer:
// * bit0-15: color (bit15=1: direct color, bit15=0: palette index, bit12=0 to indicate extpal)
// * bit16-17: BG-relative priority
// * bit18: non-transparent sprite pixel exists here
// * bit19: a sprite covers this pixel (transparent or not)
// * bit20: X mosaic should be applied here
// * bit24-31: compositor flags

void SoftRenderer::ApplySpriteMosaicX()
{
    // apply X mosaic if needed
    // X mosaic for sprites is applied after all sprites are rendered
    //
    // rules:
    // pixels are processed from left to right
    // the current pixel value is latched if:
    // - the X mosaic counter is 0
    // - the current pixel doesn't receive sprite mosaic
    // - the latched pixel doesn't receive sprite mosaic
    // - the current BG-relative priority value is lower than the latched one

    if (CurUnit->OBJMosaicSize[0] == 0) return;

    u32* objLine = OBJLine[CurUnit->Num];

    u8* curOBJXMosaicTable = MosaicTable[CurUnit->OBJMosaicSize[0]].data();

    u32 latchcolor = 0;

    for (u32 i = 0; i < 256; i++)
    {
        u32 curcolor = objLine[i];
        bool latch = false;

        if (curOBJXMosaicTable[i] == 0)
            latch = true;
        else if (!(curcolor & 0x100000))
            latch = true;
        else if (!(latchcolor & 0x100000))
            latch = true;
        else if ((curcolor & 0x30000) < (latchcolor & 0x30000))
            latch = true;

        if (latch)
            latchcolor = curcolor;

        objLine[i] = latchcolor;
    }
}

template <SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::InterleaveSprites(u32 prio)
{
    u32* objLine = OBJLine[CurUnit->Num];
    u16* pal = (u16*)&CurJob->Palette[CurUnit->Num ? 0x600 : 0x200];

    if (CurUnit->DispCnt & 0x80000000)
    {
        u16* extpal = GetOBJExtPal();

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else if (pixel & 0x1000)
                color = pal[pixel & 0xFF];
            else
                color = extpal[pixel & 0xFFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
    else
    {
        // optimized no-extpal version

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else
                color = pal[pixel & 0xFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
}

#define DoDrawSprite(type, ...) \
    if (iswin) \
    { \
        DrawSprite_##type<true>(__VA_ARGS__); \
    } \
    else \
    { \
        DrawSprite_##type<false>(__VA_ARGS__); \
    }

void SoftRenderer::DrawSprites(u32 line, const UnitState* unit)
{
    CurUnit = unit;

    // the OBJ mosaic counters are reset for line 0 by Unit::ResetOBJMosaicCounters,
    // and the flat VRAM copies are made coherent by GPU::MakeVRAMFlat_2DOBJCoherent

    NumSprites[CurUnit->Num] = 0;
    memset(OBJLine[CurUnit->Num], 0, 256*4);
    memset(OBJWindow[CurUnit->Num], 0, 256);
    // the OBJ buffers don't get updated at all if the 2D engine is disabled
    if (!CurUnit->Enabled) return;
    // OBJ enable as latched for this line's sprite pass (see Unit::PreDrawScanline)
    if (!CurUnit->OBJEnable) return;

    u16* oam = (u16*)&CurJob->OAM[CurUnit->Num ? 0x400 : 0];

    const s32 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    const s32 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    // sprites are drawn in OAM order: a pixel replaces the one already there only if that one
    // is transparent or has a strictly lower priority (see DrawSpritePixel)
    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        u16* attrib = &oam[sprnum*4];

        bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

        u32 sprline;
        if ((attrib[0] & 0x1000) && !iswin)
        {
            // apply Y mosaic
            sprline = CurUnit->OBJMosaicY;
        }
        else
            sprline = line;

        if (attrib[0] & 0x0100)
        {
            u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
            s32 width = spritewidth[sizeparam];
            s32 height = spriteheight[sizeparam];
            s32 boundwidth = width;
            s32 boundheight = height;

            if (attrib[0] & 0x0200)
            {
                boundwidth <<= 1;
                boundheight <<= 1;
            }

            u32 ypos = attrib[0] & 0xFF;
            if (((line - ypos) & 0xFF) >= (u32)boundheight)
                continue;
            ypos = (sprline - ypos) & 0xFF;
            // a ypos past the sprite height means we underflowed, because OBJMosaicY was
            // latched before the sprite's top: clamp to the first row
            if (ypos >= (u32)boundheight) ypos = 0;

            s32 xpos = (s32)(attrib[1] << 23) >> 23;
            if (xpos <= -boundwidth)
                continue;

            DoDrawSprite(Rotscale, sprnum, boundwidth, boundheight, width, height, xpos, ypos);

            NumSprites[CurUnit->Num]++;
        }
        else
        {
            if (attrib[0] & 0x0200)
                continue;

            u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
            s32 width = spritewidth[sizeparam];
            s32 height = spriteheight[sizeparam];

            u32 ypos = attrib[0] & 0xFF;
            if (((line - ypos) & 0xFF) >= (u32)height)
                continue;
            ypos = (sprline - ypos) & 0xFF;
            if (ypos >= (u32)height) ypos = 0;

            s32 xpos = (s32)(attrib[1] << 23) >> 23;
            if (xpos <= -width)
                continue;

            DoDrawSprite(Normal, sprnum, width, height, xpos, ypos);

            NumSprites[CurUnit->Num]++;
        }
    }
}

// one sprite pixel into the OBJ line, in OAM order. an opaque pixel only replaces an opaque one of
// strictly lower priority. a transparent pixel over no opaque pixel still marks the pixel as
// covered by a sprite and takes over its priority and mosaic flag (the last sprite in OAM order
// wins), which is what the sprite X mosaic and the OBJ blending see there
template<bool window>
void SoftRenderer::DrawSpritePixel(u32* objLine, u8* objWindow, int color, u32 pixelattr, s32 xpos)
{
    if (window)
    {
        if (color != -1)
            objWindow[xpos] = 1;
        return;
    }

    u32 oldpixel = objLine[xpos];
    bool oldisopaque = !!(oldpixel & 0x40000);

    if (color != -1)
    {
        if (!oldisopaque || ((pixelattr & 0x30000) < (oldpixel & 0x30000)))
            objLine[xpos] = color | pixelattr;
    }
    else if (!oldisopaque)
    {
        objLine[xpos] = (oldpixel & ~0x130000) | (pixelattr & 0x1B0000);
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&CurJob->OAM[CurUnit->Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(objvram, objvrammask);

    u32* objLine = OBJLine[CurUnit->Num];
    u8* objWindow = OBJWindow[CurUnit->Num];

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((xoff-centerX) * rotA) + ((ypos-centerY) * rotB) + (width << 7);
    s32 rotY = ((xoff-centerX) * rotC) + ((ypos-centerY) * rotD) + (height << 7);

    width <<= 8;
    height <<= 8;

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (CurUnit->DispCnt & 0x40)
        {
            if (CurUnit->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((CurUnit->DispCnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (CurUnit->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < boundwidth;)
        {
            if ((u32)rotX < width && (u32)rotY < height)
            {
                color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];

                DrawSpritePixel<window>(objLine, objWindow, (color & 0x8000) ? color : -1, pixelattr, xpos);
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x10)
        {
            pixelsaddr <<= ((CurUnit->DispCnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & 0x2000)
        {
            // 256-color

            if (!window)
            {
                if (!(CurUnit->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];

                    DrawSpritePixel<window>(objLine, objWindow, color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;

                    DrawSpritePixel<window>(objLine, objWindow, color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&CurJob->OAM[CurUnit->Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 wmask = width - 8; // really ((width - 1) & ~0x7)

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(objvram, objvrammask);

    u32* objLine = OBJLine[CurUnit->Num];
    u8* objWindow = OBJWindow[CurUnit->Num];

    // yflip
    if (attrib[1] & 0x2000)
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        // bitmap sprite

        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x40)
        {
            if (CurUnit->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((CurUnit->DispCnt >> 22) & 0x1));
                pixelsaddr += (ypos * width * 2);
            }
        }
        else
        {
            if (CurUnit->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (ypos * 128 * 2);
            }
        }

        s32 pixelstride;

        if (attrib[1] & 0x1000) // xflip
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend;)
        {
            color = *(u16*)&objvram[pixelsaddr & objvrammask];

            pixelsaddr += pixelstride;

            DrawSpritePixel<window>(objLine, objWindow, (color & 0x8000) ? color : -1, pixelattr, xpos);

            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x10)
        {
            pixelsaddr <<= ((CurUnit->DispCnt >> 20) & 0x3);
            pixelsaddr += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            pixelsaddr += ((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        if (attrib[0] & 0x2000)
        {
            // 256-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 3);
            s32 pixelstride;

            if (!window)
            {
                if (!(CurUnit->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 3);
                pixelsaddr += ((width-1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                color = objvram[pixelsaddr & objvrammask];

                pixelsaddr += pixelstride;

                DrawSpritePixel<window>(objLine, objWindow, color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }
        }
        else
        {
            // 16-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 2);
            s32 pixelstride;

            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            // TODO: optimize VRAM access!!
            // TODO: do xflip better? the 'two pixels per byte' thing makes it a bit shitty

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 2);
                pixelsaddr += (((width-1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                if (attrib[1] & 0x1000)
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr--; }
                    else              color = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr++; }
                    else              color = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                DrawSpritePixel<window>(objLine, objWindow, color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }
        }
    }
}

}
}
