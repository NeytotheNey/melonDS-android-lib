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

#ifndef GPU3D_COMPUTESTATS_H
#define GPU3D_COMPUTESTATS_H

namespace melonDS::ComputePass
{
// what the compute renderer's GPU timers tell apart (GPU::ComputeTimers): the passes of
// ComputeRenderer::RenderFrame, then the GL compositor's draw and everything else it runs
enum
{
    Clear, Interp, Bin, Offs, Sort, Rast, Depth, Final,
    Comp, CompExtra,
    Count,
    FirstBlit = Comp,
};
}

#ifdef MELONADE_DEBUG_TOOLS

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include "types.h"
#include "Platform.h"
#include "PlatformOGL.h"

namespace melonDS
{

// debug builds only, compute renderer: the COMPTIME, COMPSYNC and COMPPEAK logs
// (debug.melonade.computetimers, computesync and computepeaks, see DebugProperties.h). the renderer
// makes one at the first RenderFrame with a switch on, so while all are off it costs nothing.
// emulator thread only, GL context current.
//
// COMPTIME, once a second, is GPU time from GL_EXT_disjoint_timer_query. only one
// GL_TIME_ELAPSED_EXT query can run at a time, so the passes are timed back to back: a pass is its
// dispatches and the barrier after them. nothing waits for a result. each RenderFrame and each Blit
// fills a set of queries from a ring, and a set is read once its last query is available, some
// frames later. while the ring is full, sets go untimed
//   frames skipped  RenderFrame calls that rendered, and that took the identical-frame skip
//   clear..final    ms per timed render. clear is ClearCoarseBinMask and ClearIndirectWorkCount,
//                   rast every variant's dispatch
//   comp compx      ms per timed Blit: the compositor's draw, and everything else it runs from the
//                   GPU 2D shadow's OBJ pass on (uploads, filtered 2D, the shadow's composite, the
//                   held wings, hi-res capture)
//   total           the sum of those
//   polys variants  per rendered frame
//   timed blits     the render and Blit sets read this second, which the averages are over
//   untimed         sets left out because the ring was full
//   disjoint        times the GPU reported a disjoint operation, which drops every set not read yet
//
// COMPSYNC is the same passes timed on the CPU instead, for drivers whose queries read 0 around
// compute dispatches (Adreno 640): a glFinish before the first pass and after each one, and a pass
// is the time between two of them, so its submit plus the GPU finishing it. every render and Blit
// is timed, and it's much slower than normal. it turns the other two logs off
//   rastdispatches  Rasterise dispatches per rendered frame, one per variant
//
// COMPPEAK, once a second, is the most a frame used of each work buffer:
//   worktiles a/b   work tiles (polygon and tile pairs) binning wrote, of MaxWorkTiles
//   wanted          what binning asked for (VariantWorkCount[0].w), anything past b was dropped
//   overflow        frames where it asked for more than b
//   bound           the CPU's bounding box estimate of wanted (the direct dispatch sizes)
//   room            the work tiles the tile memory has room for now (GPU::ComputeGrow, b at most)
//   pastroom        frames whose work tiles went past the room they had. should stay 0: the room
//                   grows to the bound first
//   pastbound       frames where wanted went past bound. should stay 0, the room counts on it
//   xspans a/b      setup indices, which is one X span each, of the most the polygon setup takes
//   xroom           the X spans their buffers have room for now (b at most)
//   yspans a/b      Y span setups, of MaxYSpanSetups
//   variants a/b    of MaxVariants
//   polys           polygons drawn
//   cut             frames that dropped polygons because the variants or a span buffer ran out
// wanted is read back from the GPU every frame, which waits for it, so the two logs don't mix
struct ComputeStats
{
    static constexpr int SetCount = 16;
    static constexpr int SetQueries = 8;

    struct QuerySet
    {
        GLuint Query[SetQueries] {};
        u8 Pass[SetQueries] {};
        int Count = 0;
        bool Blit = false;
        bool Pending = false;
    };

    // zeroed with every log line
    struct Counters
    {
        u32 Frames = 0, Skipped = 0;
        u64 Polys = 0, Variants = 0, RastDispatches = 0;
        u32 TimedRenders = 0, TimedBlits = 0, Untimed = 0, Disjoint = 0;
        u64 PassNs[ComputePass::Count] {};

        u32 PeakWanted = 0, PeakBound = 0, PeakXSpans = 0, PeakYSpans = 0, PeakVariants = 0, PeakPolys = 0;
        u32 Overflowed = 0, Cut = 0, PastRoom = 0, PastBound = 0;
    };

    // the switches as they were when this was made. Sync is never on with the other two
    const bool WantTimers, WantPeaks, Sync;
    // the queries exist. false without the extension
    bool Timers = false;
    Counters Sec;

    ComputeStats(bool timers, bool peaks, bool sync) : WantTimers(timers), WantPeaks(peaks), Sync(sync)
    {
        Timers = timers && InitTimers();
    }

    ComputeStats(const ComputeStats&) = delete;
    ComputeStats& operator=(const ComputeStats&) = delete;

    ~ComputeStats()
    {
        if (!Timers)
            return;

        End();
        for (QuerySet& set : Sets)
            DeleteQueries(SetQueries, set.Query);
    }

    // at the start of every RenderFrame
    void FrameStart(s64 now, int scale, int tileSize, u32 maxWorkTiles, u32 workTileRoom, u32 maxXSpans, u32 xSpanRoom,
                    u32 maxYSpans, u32 maxVariants)
    {
        if (Timers)
            Collect();

        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Timers || Sync)
            LogTimes(scale, tileSize);
        if (WantPeaks)
        {
            Platform::Log(Platform::LogLevel::Info, "COMPPEAK frames=%u worktiles=%u/%u wanted=%u overflow=%u bound=%u "
                "room=%u pastroom=%u pastbound=%u xspans=%u/%u xroom=%u yspans=%u/%u variants=%u/%u polys=%u cut=%u "
                "scale=%d tile=%d\n",
                Sec.Frames, std::min(Sec.PeakWanted, maxWorkTiles), maxWorkTiles, Sec.PeakWanted, Sec.Overflowed, Sec.PeakBound,
                workTileRoom, Sec.PastRoom, Sec.PastBound, Sec.PeakXSpans, maxXSpans, xSpanRoom, Sec.PeakYSpans, maxYSpans,
                Sec.PeakVariants, maxVariants, Sec.PeakPolys, Sec.Cut, scale, tileSize);
        }

        Sec = {};
        LogStartNs = now;
    }

    // a frame that renders, once its polygons are set up
    void NoteFrame(u32 polys, u32 variants, u32 rastDispatches, u32 xspans, u32 yspans, u32 bound, bool cut)
    {
        Sec.Frames++;
        Sec.Polys += polys;
        Sec.Variants += variants;
        Sec.RastDispatches += rastDispatches;
        Sec.PeakPolys = std::max(Sec.PeakPolys, polys);
        Sec.PeakVariants = std::max(Sec.PeakVariants, variants);
        Sec.PeakXSpans = std::max(Sec.PeakXSpans, xspans);
        Sec.PeakYSpans = std::max(Sec.PeakYSpans, yspans);
        Sec.PeakBound = std::max(Sec.PeakBound, bound);
        Sec.Cut += cut;
    }

    // what binning asked for, against what the frame had: the most binning writes, the room the tile
    // memory had and the CPU's bound
    void NoteWorkTiles(u32 wanted, u32 maxWorkTiles, u32 workTileRoom, u32 bound)
    {
        Sec.PeakWanted = std::max(Sec.PeakWanted, wanted);
        Sec.Overflowed += wanted > maxWorkTiles;
        Sec.PastRoom += std::min(wanted, maxWorkTiles) > workTileRoom;
        Sec.PastBound += wanted > bound;
    }

    // the queries of one RenderFrame or one Blit, from here to EndSet
    void BeginSet(bool blit)
    {
        if (Sync)
        {
            SyncOpen = true;
            SyncBlit = blit;
            SyncPass = -1;
            return;
        }
        if (!Timers)
            return;

        QuerySet& set = Sets[Head];
        if (set.Pending)
        {
            // the GPU is a whole ring behind
            Sec.Untimed++;
            return;
        }
        set.Count = 0;
        set.Blit = blit;
        Open = true;
    }

    // ends the pass before it. outside of a set it only does that
    void Begin(int pass)
    {
        if (Sync)
        {
            SyncMark(pass);
            return;
        }

        End();
        if (!Open)
            return;

        QuerySet& set = Sets[Head];
        if (set.Count == SetQueries)
            return;
        set.Pass[set.Count] = (u8)pass;
        BeginQuery(GL_TIME_ELAPSED_EXT, set.Query[set.Count++]);
        Active = true;
    }

    void End()
    {
        if (Sync)
            SyncMark(-1);
        if (Active)
        {
            EndQuery(GL_TIME_ELAPSED_EXT);
            Active = false;
        }
    }

    void EndSet()
    {
        End();
        if (SyncOpen)
        {
            SyncOpen = false;
            if (SyncBlit)
                Sec.TimedBlits++;
            else
                Sec.TimedRenders++;
        }
        if (!Open)
            return;

        Open = false;
        QuerySet& set = Sets[Head];
        if (set.Count == 0)
            return;
        set.Pending = true;
        Head = (Head + 1) % SetCount;
    }

private:
    // GL_EXT_disjoint_timer_query's entry points, which the NDK's libGLESv3 doesn't export
    PFNGLGENQUERIESEXTPROC GenQueries = nullptr;
    PFNGLDELETEQUERIESEXTPROC DeleteQueries = nullptr;
    PFNGLBEGINQUERYEXTPROC BeginQuery = nullptr;
    PFNGLENDQUERYEXTPROC EndQuery = nullptr;
    PFNGLGETQUERYIVEXTPROC GetQueryiv = nullptr;
    PFNGLGETQUERYOBJECTUIVEXTPROC GetQueryObjectuiv = nullptr;
    PFNGLGETQUERYOBJECTUI64VEXTPROC GetQueryObjectui64v = nullptr;

    // the pending sets run from Tail up to Head, in the order the GPU gets them
    QuerySet Sets[SetCount];
    int Head = 0, Tail = 0;
    bool Open = false;
    bool Active = false;
    s64 LogStartNs = 0;

    // Sync: the pass being timed, -1 for none, and when the glFinish before it returned
    bool SyncOpen = false, SyncBlit = false;
    int SyncPass = -1;
    s64 SyncStartNs = 0;

    // waits for the GPU to finish everything so far, which ends the pass before and starts this
    // one (-1 for none). the first of a set has no pass before it, so older work isn't charged
    void SyncMark(int pass)
    {
        if (!SyncOpen || (pass < 0 && SyncPass < 0))
            return;

        glFinish();
        s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (SyncPass >= 0)
            Sec.PassNs[SyncPass] += (u64)(now - SyncStartNs);
        SyncPass = pass;
        SyncStartNs = now;
    }

    template <typename T>
    static bool Load(T& function, const char* name)
    {
        function = reinterpret_cast<T>(eglGetProcAddress(name));
        return function != nullptr;
    }

    bool InitTimers()
    {
        const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "GL_EXT_disjoint_timer_query"))
        {
            Platform::Log(Platform::LogLevel::Warn, "COMPTIME: no GL_EXT_disjoint_timer_query, staying off\n");
            return false;
        }

        bool loaded = Load(GenQueries, "glGenQueriesEXT");
        loaded &= Load(DeleteQueries, "glDeleteQueriesEXT");
        loaded &= Load(BeginQuery, "glBeginQueryEXT");
        loaded &= Load(EndQuery, "glEndQueryEXT");
        loaded &= Load(GetQueryiv, "glGetQueryivEXT");
        loaded &= Load(GetQueryObjectuiv, "glGetQueryObjectuivEXT");
        loaded &= Load(GetQueryObjectui64v, "glGetQueryObjectui64vEXT");
        // a driver can list the extension and still count no bits
        GLint bits = 0;
        if (loaded)
            GetQueryiv(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
        if (bits <= 0)
        {
            Platform::Log(Platform::LogLevel::Warn, "COMPTIME: GL_TIME_ELAPSED_EXT can't be used (loaded=%d bits=%d), staying off\n",
                loaded ? 1 : 0, bits);
            return false;
        }

        for (QuerySet& set : Sets)
            GenQueries(SetQueries, set.Query);

        // reading the flag clears it, so what happened before the first query doesn't count
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        return true;
    }

    // reads the sets the GPU has finished, oldest first, and never waits for one
    void Collect()
    {
        bool checked = false;
        while (Sets[Tail].Pending)
        {
            QuerySet& set = Sets[Tail];
            GLuint available = 0;
            GetQueryObjectuiv(set.Query[set.Count - 1], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
            if (!available)
                break;

            if (!checked)
            {
                // a disjoint operation (the GPU changing its clock, say) since the flag was last read
                // makes the times wrong. any set not read yet could have run across it
                GLint disjoint = 0;
                glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
                checked = true;
                if (disjoint)
                {
                    for (QuerySet& pending : Sets)
                        pending.Pending = false;
                    Tail = Head;
                    Sec.Disjoint++;
                    break;
                }
            }

            for (int i = 0; i < set.Count; i++)
            {
                GLuint64 ns = 0;
                GetQueryObjectui64v(set.Query[i], GL_QUERY_RESULT_EXT, &ns);
                Sec.PassNs[set.Pass[i]] += ns;
            }
            if (set.Blit)
                Sec.TimedBlits++;
            else
                Sec.TimedRenders++;

            set.Pending = false;
            Tail = (Tail + 1) % SetCount;
        }
    }

    void LogTimes(int scale, int tileSize)
    {
        double perRender = Sec.TimedRenders ? 1e-6 / Sec.TimedRenders : 0.0;
        double perBlit = Sec.TimedBlits ? 1e-6 / Sec.TimedBlits : 0.0;
        double perFrame = Sec.Frames ? 1.0 / Sec.Frames : 0.0;
        double ms[ComputePass::Count];
        double total = 0.0;
        for (int i = 0; i < ComputePass::Count; i++)
        {
            ms[i] = Sec.PassNs[i] * (i >= ComputePass::FirstBlit ? perBlit : perRender);
            total += ms[i];
        }

        char tail[96];
        if (Sync)
            snprintf(tail, sizeof(tail), "rastdispatches=%.1f blits=%u", Sec.RastDispatches * perFrame, Sec.TimedBlits);
        else
            snprintf(tail, sizeof(tail), "timed=%u blits=%u untimed=%u disjoint=%u", Sec.TimedRenders, Sec.TimedBlits,
                Sec.Untimed, Sec.Disjoint);

        using namespace ComputePass;
        Platform::Log(Platform::LogLevel::Info, "%s frames=%u skipped=%u clear=%.3f interp=%.3f bin=%.3f offs=%.3f "
            "sort=%.3f rast=%.3f depth=%.3f final=%.3f comp=%.3f compx=%.3f total=%.3f polys=%.0f variants=%.1f "
            "scale=%d tile=%d %s\n",
            Sync ? "COMPSYNC" : "COMPTIME", Sec.Frames, Sec.Skipped, ms[Clear], ms[Interp], ms[Bin], ms[Offs], ms[Sort],
            ms[Rast], ms[Depth], ms[Final], ms[Comp], ms[CompExtra], total, Sec.Polys * perFrame, Sec.Variants * perFrame,
            scale, tileSize, tail);
    }
};

}

#endif

#endif
