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

#ifndef GPU3D_GLSTATS_H
#define GPU3D_GLSTATS_H

#if defined(MELONADE_DEBUG_TOOLS) && defined(OGLRENDERER_ENABLED)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "PlatformOGL.h"
// a PC build's GL header names its own lookup (desktop/gl/pc_gl.h), Android has EGL's, as in GPU3D_ComputeStats.h
#ifndef MELONDS_GL_GET_PROC_ADDRESS
#include <EGL/egl.h>
#define MELONDS_GL_GET_PROC_ADDRESS eglGetProcAddress
#endif
#include <GLES2/gl2ext.h>

#include "types.h"
#include "Platform.h"
#include "OpenGLSupport.h"
#include "GPU3D_OpenGLTexHeap.h"

namespace melonDS
{

static inline s64 GLStatsNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// what the OpenGL renderer's GPU timers tell apart (GPU::GLTimers): GLRenderer::RenderFrame's passes,
// the capture readback, then the compositor's parts, from Comp on in the order of ComputePass's
// (GPU3D_ComputeStats.h), which GPU_OpenGL.cpp passes to BeginGPUTimer
namespace GLPass
{
enum
{
    Upload, Clear, Opaque, Clear0, Trans, Edge, Fog,
    Capture,
    Comp, CompExtra,
    CompHires, CompShadowObj, CompUpload, CompFilter, CompShadow, CompWings, CompHiCap,
    Count,
};
}

// debug builds only, OpenGL renderer: the GLTIME log (debug.melonade.gltimers, see DebugProperties.h).
// the renderer makes one at the first RenderFrame with the switch on. emulator thread only, GL context
// current.
//
// gltimers 1, GPU time from GL_EXT_disjoint_timer_query, as the compute renderer's COMPTIME does it:
// one GL_TIME_ELAPSED_EXT query at a time, so the passes are timed back to back (a pass runs up to the
// next Begin). each RenderFrame that draws, each capture readback and each Blit fills a set of queries
// from a ring, and a set is read once its last query is available, some frames later. nothing waits.
// while the ring is full, sets go untimed. a disjoint operation drops every set not read yet.
// gltimers 2, the same passes timed on the CPU between glFinish calls, like COMPSYNC: a glFinish
// before a set's first pass and after each one, and a pass is the time between two. every set is
// timed, the emulator runs much slower and every pass reads longer than it is, so read its shares.
//
// GLTIME, once a second:
//   mode           queries, or glfinish (the times are inflated, compare the share list)
//   frames skipped line 215 renders, and of those the ones RenderFrame left out as identical
//   timed          the render, capture and Blit sets read this second, which the averages are over
//   upload..fog    µs per timed render: upload is the UBO, texture and vertex uploads, clear the clear
//                  quad (or bitmap), opaque pass 1, clear0 pass 2 (translucent polygons over a clear
//                  plane with alpha 0, only then), trans pass 3 (translucent, shadow masks and
//                  shadows), edge and fog the final passes with the depth buffer's detach
//   capture        µs per timed capture readback (the downscale blit and its glReadPixels)
//   comp compx     µs per timed Blit: the compositor's draw, and everything else it runs (the GPU 2D
//                  shadow, filtered 2D, held wings, hi-res capture)
//   total          upload..fog + capture + comp + compx
//   untimed        sets left out because the ring was full, disjoint the disjoint operations seen
//   comp_hd .. cx_hicap
//                  the compositor's parts (GLCompositor::RenderFrame), µs per timed Blit like comp
//                  and compx, a Blit a part didn't run in counting 0. comp_hd is the part of comp
//                  from Blits that showed hi-res captures (the draw reads the copies then), and the
//                  cx_ ones add up to compx: cx_misc the state setup and CPU work between the parts
//                  (and before the first one), cx_sobj the GPU 2D shadow's uploads and OBJ pass,
//                  cx_up the screen texture uploads (planes, control words, wing planes), cx_filter
//                  filtered 2D's key and MMPX passes (not on a frame that reuses the map), cx_shadow
//                  the shadow's composite, cx_wings the held wings' copy, cx_hicap the hi-res
//                  capture pass with its uploads. each but cx_misc has an _n, the timed Blits it ran
//                  in, so µs per run is the value x blits (timed's third) / _n. a part's pass runs up
//                  to the next one, so on a tiled GPU a draw's resolve can land in the part after it.
//                  with glfinish, the CPU time between two parts goes to the first
//   share          glfinish only: each pass's part of total, in %
struct GLTimerStats
{
    enum SetKind { Set_Render, Set_Capture, Set_Blit, Set_Kinds };

    static constexpr int SetCount = 24;
    // a render uses up to 7, a capture readback 1, a Blit up to 9 (CompExtra twice and each other
    // compositor part once)
    static constexpr int SetQueries = 12;
    static_assert(GLPass::Count <= 32, "the passes a set ran are a u32 mask");

    struct QuerySet
    {
        GLuint Query[SetQueries] {};
        u8 Pass[SetQueries] {};
        int Count = 0;
        u8 Kind = 0;
        bool Pending = false;
    };

    // zeroed with every log line
    struct Counters
    {
        u32 Frames = 0, Skipped = 0;
        u32 Timed[Set_Kinds] {};
        u32 Untimed = 0, Disjoint = 0;
        u64 PassNs[GLPass::Count] {};
        // the timed sets each pass ran in
        u32 PassSets[GLPass::Count] {};
    };

    // the switch as it was when this was made: 1 queries, 2 glFinish
    const int Mode;
    // the queries exist (mode 1 only)
    bool Timers = false;
    Counters Sec;

    explicit GLTimerStats(int mode) : Mode(mode)
    {
        if (Mode == 1)
            Timers = InitTimers();
    }

    GLTimerStats(const GLTimerStats&) = delete;
    GLTimerStats& operator=(const GLTimerStats&) = delete;

    ~GLTimerStats()
    {
        if (!Timers)
            return;
        End();
        for (QuerySet& set : Sets)
            DeleteQueries(SetQueries, set.Query);
    }

    // at the start of every RenderFrame
    void FrameStart(s64 now, int scale)
    {
        Sec.Frames++;
        if (Timers)
            Collect();

        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Mode == 2 || Timers)
            LogTimes(scale);
        Sec = {};
        LogStartNs = now;
    }

    // the queries of one RenderFrame, capture readback or Blit, from here to EndSet. one still open
    // is ended first
    void BeginSet(SetKind kind)
    {
        if (Open || SyncOpen)
            EndSet();

        if (Mode == 2)
        {
            SyncOpen = true;
            SyncKind = kind;
            SyncPass = -1;
            SyncSeen = 0;
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
        set.Kind = (u8)kind;
        Open = true;
    }

    // ends the pass before it. outside of a set it only does that
    void Begin(int pass)
    {
        if (Mode == 2)
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
        if (Mode == 2)
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
            Sec.Timed[SyncKind]++;
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
    // sets read with every query at 0, and whether that was logged (once a run)
    u64 TimedSetsRead = 0, ZeroSetsRead = 0;
    bool ZeroLogged = false;

    // mode 2: the pass being timed, -1 for none, and when the glFinish before it returned
    bool SyncOpen = false;
    int SyncKind = Set_Render;
    int SyncPass = -1;
    s64 SyncStartNs = 0;
    // the passes the open set has run, for Counters::PassSets
    u32 SyncSeen = 0;

    // waits for the GPU to finish everything so far, which ends the pass before and starts this
    // one (-1 for none). the first of a set has no pass before it, so older work isn't charged
    void SyncMark(int pass)
    {
        if (!SyncOpen || (pass < 0 && SyncPass < 0))
            return;

        glFinish();
        s64 now = GLStatsNowNs();
        if (SyncPass >= 0)
            Sec.PassNs[SyncPass] += (u64)(now - SyncStartNs);
        if (pass >= 0 && !(SyncSeen & (1u << pass)))
        {
            SyncSeen |= 1u << pass;
            Sec.PassSets[pass]++;
        }
        SyncPass = pass;
        SyncStartNs = now;
    }

    template <typename T>
    static bool Load(T& function, const char* name)
    {
        function = reinterpret_cast<T>(MELONDS_GL_GET_PROC_ADDRESS(name));
        return function != nullptr;
    }

    bool InitTimers()
    {
        const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "GL_EXT_disjoint_timer_query"))
        {
            Platform::Log(Platform::LogLevel::Warn, "GLTIME: no GL_EXT_disjoint_timer_query, staying off (try gltimers 2)\n");
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
            Platform::Log(Platform::LogLevel::Warn, "GLTIME: GL_TIME_ELAPSED_EXT can't be used (loaded=%d bits=%d), staying off (try gltimers 2)\n",
                loaded ? 1 : 0, bits);
            return false;
        }

        for (QuerySet& set : Sets)
            GenQueries(SetQueries, set.Query);

        // reading the flag clears it, so what happened before the first query doesn't count
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        Platform::Log(Platform::LogLevel::Info, "GLTIME: timer queries on, %d bits\n", bits);
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

            u64 setNs = 0;
            u32 seen = 0;
            for (int i = 0; i < set.Count; i++)
            {
                GLuint64 ns = 0;
                GetQueryObjectui64v(set.Query[i], GL_QUERY_RESULT_EXT, &ns);
                Sec.PassNs[set.Pass[i]] += ns;
                setNs += ns;
                seen |= 1u << set.Pass[i];
            }
            for (int pass = 0; pass < GLPass::Count; pass++)
                Sec.PassSets[pass] += (seen >> pass) & 1;
            Sec.Timed[set.Kind]++;
            TimedSetsRead++;
            ZeroSetsRead += setNs == 0;

            set.Pending = false;
            Tail = (Tail + 1) % SetCount;
        }

        // the Adreno 640 reads 0 for compute dispatches. if every set so far came back 0, say so once
        if (!ZeroLogged && TimedSetsRead >= 120 && ZeroSetsRead == TimedSetsRead)
        {
            ZeroLogged = true;
            Platform::Log(Platform::LogLevel::Warn, "GLTIME: all %llu timed sets read 0 ns on this driver, the times mean nothing (try gltimers 2)\n",
                (unsigned long long)TimedSetsRead);
        }
    }

    void LogTimes(int scale)
    {
        using namespace GLPass;
        double us[Count];
        double total = 0.0;
        for (int i = 0; i < Count; i++)
        {
            int kind = i >= Comp ? Set_Blit : (i == Capture ? Set_Capture : Set_Render);
            us[i] = Sec.Timed[kind] ? Sec.PassNs[i] * 1e-3 / Sec.Timed[kind] : 0.0;
            total += us[i];
        }

        // comp and compx as they were before the compositor's parts were split out. every pass is
        // timed apart from the others, so total is still the sum of them all
        double comp = us[Comp] + us[CompHires];
        double compx = 0.0;
        for (int i = CompExtra; i < Count; i++)
            compx += i == CompHires ? 0.0 : us[i];

        char share[160] = "";
        if (Mode == 2)
        {
            auto pct = [&](double value) { return total > 0.0 ? (int)(value * 100.0 / total + 0.5) : 0; };
            snprintf(share, sizeof(share), " share=upload:%d,clear:%d,opaque:%d,clear0:%d,trans:%d,edge:%d,fog:%d,capture:%d,comp:%d,compx:%d",
                pct(us[Upload]), pct(us[Clear]), pct(us[Opaque]), pct(us[Clear0]), pct(us[Trans]), pct(us[Edge]), pct(us[Fog]),
                pct(us[Capture]), pct(comp), pct(compx));
        }

        const u32* n = Sec.PassSets;
        Platform::Log(Platform::LogLevel::Info, "GLTIME mode=%s frames=%u skipped=%u timed=%u/%u/%u upload=%.0f clear=%.0f opaque=%.0f "
            "clear0=%.0f trans=%.0f edge=%.0f fog=%.0f capture=%.0f comp=%.0f compx=%.0f total=%.0f untimed=%u disjoint=%u scale=%d "
            "comp_hd=%.0f comp_hd_n=%u cx_misc=%.0f cx_sobj=%.0f cx_sobj_n=%u cx_up=%.0f cx_up_n=%u cx_filter=%.0f cx_filter_n=%u "
            "cx_shadow=%.0f cx_shadow_n=%u cx_wings=%.0f cx_wings_n=%u cx_hicap=%.0f cx_hicap_n=%u%s\n",
            Mode == 2 ? "glfinish(inflated,read_shares)" : "queries", Sec.Frames, Sec.Skipped, Sec.Timed[Set_Render],
            Sec.Timed[Set_Capture], Sec.Timed[Set_Blit], us[Upload], us[Clear], us[Opaque], us[Clear0], us[Trans], us[Edge],
            us[Fog], us[Capture], comp, compx, total, Sec.Untimed, Sec.Disjoint, scale,
            us[CompHires], n[CompHires], us[CompExtra], us[CompShadowObj], n[CompShadowObj], us[CompUpload], n[CompUpload],
            us[CompFilter], n[CompFilter], us[CompShadow], n[CompShadow], us[CompWings], n[CompWings], us[CompHiCap],
            n[CompHiCap], share);
    }
};

// a ring of small SSBOs of N counters that shaders add to, read back some frames later without
// waiting: BeginFrame zeroes one and binds it (or a scratch buffer that's never read while the ring
// is full), EndFrame puts a fence after the frame's dispatches or draws, and Collect adds the
// finished ones' counts, each over its frame's pixel count, to Sums
template <int N>
struct GLCounterRing
{
    static constexpr int Slots = 6;

    double Sums[N] {};
    u32 Counted = 0, Dropped = 0;

    GLCounterRing()
    {
        u32 zero[N] {};
        glGenBuffers(Slots + 1, Buffers);
        for (GLuint buffer : Buffers)
        {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_DYNAMIC_READ);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    GLCounterRing(const GLCounterRing&) = delete;
    GLCounterRing& operator=(const GLCounterRing&) = delete;

    ~GLCounterRing()
    {
        for (GLsync& fence : Fences)
        {
            if (fence)
                glDeleteSync(fence);
        }
        glDeleteBuffers(Slots + 1, Buffers);
    }

    void BeginFrame(GLuint binding, int pixels)
    {
        Collect();
        if (Count == Slots)
        {
            Dropped++;
            CurSlot = -1;
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, Buffers[Slots]);
            return;
        }

        static const u32 zero[N] {};
        CurSlot = (Head + Count) % Slots;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, Buffers[CurSlot]);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), zero);
        Pixels[CurSlot] = pixels;
    }

    void EndFrame()
    {
        if (CurSlot < 0)
            return;
        // the shaders' writes, for the map in Collect
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        Fences[CurSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        Count++;
        CurSlot = -1;
    }

    void Collect()
    {
        while (Count > 0)
        {
            int slot = Head;
            GLenum state = glClientWaitSync(Fences[slot], 0, 0);
            if (state == GL_TIMEOUT_EXPIRED)
                return;
            glDeleteSync(Fences[slot]);
            Fences[slot] = nullptr;

            const u32* counts = nullptr;
            if (state != GL_WAIT_FAILED)
            {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, Buffers[slot]);
                counts = (const u32*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, N * sizeof(u32), GL_MAP_READ_BIT);
            }
            if (counts)
            {
                double per = Pixels[slot] > 0 ? 1.0 / Pixels[slot] : 0.0;
                for (int i = 0; i < N; i++)
                    Sums[i] += counts[i] * per;
                Counted++;
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            }
            else
                Dropped++;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            Head = (Head + 1) % Slots;
            Count--;
        }
    }

    // after a log line
    void ResetSums()
    {
        for (double& sum : Sums)
            sum = 0.0;
        Counted = Dropped = 0;
    }

private:
    // [Slots] is the scratch buffer
    GLuint Buffers[Slots + 1] {};
    GLsync Fences[Slots] {};
    int Pixels[Slots] {};
    int Head = 0, Count = 0, CurSlot = -1;
};

// debug builds only, OpenGL renderer, GPU::SceneStats 2: the SCENEFRAGS log. the render programs are
// rebuilt with kRenderFSFragCount (see GLRenderer::SetupFragCount), which adds one to a counter of an
// SSBO for every fragment they run, before anything can discard it, so a count is every fragment
// rasterized (with discard and gl_FragDepth in these programs, early depth tests can't leave any out
// anyway). the counter is the pass's: pass 1 opaque, pass 2 the clear-alpha-0 translucent pass, pass 3
// translucent, masks and shadows. the covered pixels are counted two ways. with a clear alpha other
// than 0, after the frame, a full-screen draw with the stencil test and early fragment tests counts
// stencil low 7 bits not 0x7F, which every polygon draw leaves where it draws but a translucent one
// with polygon ID 63, and shadow masks never do (pass 2's masks, which can, only run with a clear
// alpha of 0). with a clear alpha of 0, before fog, a compute pass counts the color target's pixels
// with alpha: every polygon pixel has some then and a clear pixel none, but a clear bitmap's own
// pixels with alpha count too.
//
// SCENEFRAGS, once a second, every value per screen pixel (target width x height) and per counted frame:
//   slow           a reminder: the counting programs are slower than the real ones
//   counted        frames counted, dropped frames left out as the ring was full
//   opaque clear0 trans  fragments the pass's draws ran
//   shaded         their sum. covered: pixels a polygon drew. ratio: shaded / covered
struct GLFragCounter
{
    enum { Frag_Opaque, Frag_Clear0, Frag_Trans, Frag_Cover, Frag_Count };
    static constexpr GLuint Binding = 0;

    GLCounterRing<Frag_Count> Ring;

    // at the start of every RenderFrame
    void FrameStart(s64 now)
    {
        Ring.Collect();
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Ring.Counted > 0 || Ring.Dropped > 0)
        {
            double per = Ring.Counted ? 1.0 / Ring.Counted : 0.0;
            const double* sums = Ring.Sums;
            double shaded = sums[Frag_Opaque] + sums[Frag_Clear0] + sums[Frag_Trans];
            Platform::Log(Platform::LogLevel::Info, "SCENEFRAGS slow=debug_shader_atomics counted=%u dropped=%u opaque=%.3f clear0=%.3f "
                "trans=%.3f shaded=%.3f covered=%.3f ratio=%.3f\n",
                Ring.Counted, Ring.Dropped, sums[Frag_Opaque] * per, sums[Frag_Clear0] * per, sums[Frag_Trans] * per,
                shaded * per, sums[Frag_Cover] * per, sums[Frag_Cover] > 0.0 ? shaded / sums[Frag_Cover] : 0.0);
        }
        Ring.ResetSums();
        LogStartNs = now;
    }

private:
    s64 LogStartNs = 0;
};

// debug builds only, compute renderer, GPU::ComputeDepthStats: the COMPDEPTH log. DepthBlend is
// rebuilt with three counters (see AddDepthStats in GPU3D_Compute.cpp), summed per work group in
// shared memory and added to an SSBO once per group.
//
// COMPDEPTH, once a second, every value per screen pixel (the dispatch's tile-rounded width x height)
// and per counted frame:
//   counted dropped  frames read back, and left out as the ring was full
//   visited          polygons DepthBlend walked per pixel (every work tile binned there)
//   colored          of those, the ones with a non-zero color, for which it then reads depth
//   failed           of those, the ones that failed the first depth test (shadow masks included)
//   colored_pct failed_pct  the same as shares of visited
struct ComputeDepthCounter
{
    enum { Depth_Visited, Depth_Colored, Depth_Failed, Depth_Count };
    static constexpr GLuint Binding = 8;

    GLCounterRing<Depth_Count> Ring;

    void FrameStart(s64 now, int scale)
    {
        Ring.Collect();
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Ring.Counted > 0 || Ring.Dropped > 0)
        {
            double per = Ring.Counted ? 1.0 / Ring.Counted : 0.0;
            const double* sums = Ring.Sums;
            double visited = sums[Depth_Visited];
            Platform::Log(Platform::LogLevel::Info, "COMPDEPTH counted=%u dropped=%u visited=%.3f colored=%.3f failed=%.3f "
                "colored_pct=%.1f failed_pct=%.1f scale=%d\n",
                Ring.Counted, Ring.Dropped, visited * per, sums[Depth_Colored] * per, sums[Depth_Failed] * per,
                visited > 0.0 ? sums[Depth_Colored] * 100.0 / visited : 0.0,
                visited > 0.0 ? sums[Depth_Failed] * 100.0 / visited : 0.0, scale);
        }
        Ring.ResetSums();
        LogStartNs = now;
    }

private:
    s64 LogStartNs = 0;
};

// debug builds only, OpenGL renderer, GPU::SceneStats: the GLTRANS log, on experiment T1
// (GPU::GLTransSkip, docs/plan-gpu-discovery.md). the classes are counted whatever the switch is, the
// skips only when it's on. SCENESTATS is the CPU's view of the scene for any renderer, so this is
// its own line.
//
// GLTRANS, once a second, every value per drawn frame (renders with polygons RenderFrame didn't skip):
//   frames gltrans     those frames, and the switch
//   needopaque         translucent polygons with polygon alpha 31 (an A3I5 or A5I3 texture), drawn
//                      opaque and then translucent in each translucent pass
//   full partial mixed neither
//                      of those, the ones whose fragments can only come out at alpha 31, only at
//                      1-30, at both, or at neither (every texel alpha 0). textures that couldn't be
//                      scanned count as mixed
//   textures texels    textures scanned (once a frame each), and their texels
//   skip0              pass 3 opaque redraws bit 0 left out
//   skip1_opaque skip1_trans
//                      opaque and translucent draws bit 1 left out (both passes; not ones bit 0 did)
//   scan_us peak_scan_us
//                      CPU µs of the classification, and the most in one frame
//   clear0_frames fetch_frames
//                      counts, not per frame: frames with a clear alpha of 0 and translucent
//                      polygons (the ones that need the clear0 pass), and of those the ones bit 2
//                      drew without it, with framebuffer fetch. fetch_frames stays 0 without
//                      GL_EXT_shader_framebuffer_fetch
struct GLTransStats
{
    struct Counters
    {
        u32 Frames = 0;
        u32 Clear0Frames = 0, FetchFrames = 0;
        u64 NeedOpaque = 0, Full = 0, Partial = 0, Mixed = 0, Neither = 0, Textures = 0, Texels = 0;
        u64 Skip0 = 0, Skip1Opaque = 0, Skip1Trans = 0;
        s64 ScanNs = 0, PeakScanNs = 0;
    };
    Counters Sec;

    // at the start of every RenderFrame
    void FrameStart(s64 now, int mode)
    {
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Sec.Frames > 0)
        {
            double per = 1.0 / Sec.Frames;
            Platform::Log(Platform::LogLevel::Info, "GLTRANS frames=%u gltrans=%d needopaque=%.1f full=%.1f partial=%.1f "
                "mixed=%.1f neither=%.1f textures=%.1f texels=%.0f skip0=%.1f skip1_opaque=%.1f skip1_trans=%.1f "
                "scan_us=%.1f peak_scan_us=%.1f clear0_frames=%u fetch_frames=%u\n",
                Sec.Frames, mode, Sec.NeedOpaque * per, Sec.Full * per, Sec.Partial * per, Sec.Mixed * per,
                Sec.Neither * per, Sec.Textures * per, Sec.Texels * per, Sec.Skip0 * per, Sec.Skip1Opaque * per,
                Sec.Skip1Trans * per, Sec.ScanNs * per / 1000.0, Sec.PeakScanNs / 1000.0,
                Sec.Clear0Frames, Sec.FetchFrames);
        }
        Sec = {};
        LogStartNs = now;
    }

private:
    s64 LogStartNs = 0;
};

// debug builds only, OpenGL renderer, GPU::SceneStats: the GLOPAQUE log, a count for experiment R1
// (docs/plan-gpu-discovery.md: opaque programs without `discard`, so the GPU can use early-Z). it
// changes no drawing: GLRenderer::CountOpaqueR1 sorts the polygons the opaque shader (uRenderMode 0)
// draws by whether that shader's `discard` (col.a < 30.5/31) can ever fire for them. only Z-buffer
// frames matter for R1: the W-buffer build writes gl_FragDepth, which blocks early-Z anyway.
//
// GLOPAQUE, once a second:
//   frames zframes wframes  drawn frames with polygons, and how many used the Z / W-buffer
//                           (RenderPolygonRAM[0]->WBuffer, as RenderSceneChunk picks the shader)
//   then per Z-buffer frame, polygons and area (screen fractions: the shoelace area of the
//   polygon's 1x FinalPosition vertices over 256x192, summed, so overdraw counts again):
//   op op_area              polygons the opaque pass draws (not translucent, not shadow masks)
//   untex decal texpal texop
//                           of those, the ones that can't discard: polygon alpha 31 and texturing
//                           off (DISP3DCNT bit 0) or no texture; textured with decal or shadow
//                           blending (the fragment's alpha is the vertex's); textured, modulate or
//                           toon/highlight, with a palette texture (I2/I4/I8) whose color 0 isn't
//                           transparent (TexParam bit 29 off, nothing to scan); the same with any
//                           other texture that has no texel the shader returns with alpha under 31
//                           (full-texture scan)
//   alpha0 color0 direct0 comp0 oob
//                           the rest: polygon alpha 0 (wireframe, every fragment discards); a
//                           palette texture (I2/I4/I8) with color 0 transparent that has index-0
//                           texels; direct color with alpha-bit-0 texels; 4x4 compressed with
//                           transparent texels (index 3 in modes 0 and 1); a texture past TexMem
//   nf nm nn                need-opaque polygons (translucent with polygon alpha 31, so A3I5 or
//                           A5I3), whose opaque draw runs the same shader: nf can't discard (every
//                           texel full, or decal / untextured, T1's AlphaClass_Full only), nm has
//                           texels under 31 and needs the discard, nn has no full texel and T1
//                           (gltrans bit 1) leaves its opaque draw out
//   qual                    (untex + decal + texpal + texop + nf) area over (op + nf + nm) area: the share
//                           of opaque-shader area a no-discard program could take in Z frames
//   w_qual                  the same share in W-buffer frames, for reference only
//   textures bytes scan_us  per frame: textures scanned (GLRenderer::ScanOpaqueTexture, which keeps
//                           its results across frames until texture VRAM under them changes, so this
//                           counts the scans its cache didn't have, whoever asked: this count or
//                           GPU::GLNoDiscard bit 1), bytes read, and this count's CPU µs
//   then GPU::GLNoDiscard's (R1 steps 1 and 2) work, per Z-buffer frame:
//   nd nd_area              opaque polygons drawn with the no-discard program (0 with the switch off);
//                           with bit 1 on, texop's too
//   draws draws_base        the opaque pass's draw calls, and how many the same polygons take
//                           without the no-discard key bit (the switch's extra draws are the
//                           difference)
//   prog_switches           glUseProgram calls the switch adds: between the two opaque programs in
//                           the pass, and back to the normal one after it
//   nd_scan_us              per frame: CPU µs of bit 1's pass over the polygons (MarkScannedNoDiscard:
//                           cache lookups and the scans they make; bit 2 drops the cache every frame)
struct GLOpaqueStats
{
    enum Class
    {
        Untex, Decal, TexPal, TexOpaque,
        Alpha0, Color0, Direct0, Comp0, OutOfRange,
        NeedFull, NeedMixed, NeedNone,
        NumClasses
    };

    struct Counters
    {
        u32 Frames = 0, ZFrames = 0, WFrames = 0;
        u64 Polys[2][NumClasses] {};   // [0] Z frames, [1] W frames
        double Area[2][NumClasses] {};
        u64 Textures = 0, Bytes = 0;
        u64 NoDiscard = 0, Draws = 0, DrawsBase = 0, ProgSwitches = 0;
        double NoDiscardArea = 0;
        s64 ScanNs = 0, NdScanNs = 0;
    };
    Counters Sec;

    // at the start of every RenderFrame
    void FrameStart(s64 now)
    {
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Sec.Frames > 0)
        {
            auto share = [&](int b)
            {
                const double* a = Sec.Area[b];
                double qual = a[Untex] + a[Decal] + a[TexPal] + a[TexOpaque] + a[NeedFull];
                double all = qual + a[Alpha0] + a[Color0] + a[Direct0] + a[Comp0] + a[OutOfRange] + a[NeedMixed];
                return all > 0 ? qual / all : 0.0;
            };
            double per = Sec.ZFrames ? 1.0 / Sec.ZFrames : 0.0;
            const u64* p = Sec.Polys[0];
            const double* a = Sec.Area[0];
            u64 op = p[Untex] + p[Decal] + p[TexPal] + p[TexOpaque] + p[Alpha0] + p[Color0] + p[Direct0] + p[Comp0] + p[OutOfRange];
            double oparea = a[Untex] + a[Decal] + a[TexPal] + a[TexOpaque] + a[Alpha0] + a[Color0] + a[Direct0] + a[Comp0] + a[OutOfRange];
            Platform::Log(Platform::LogLevel::Info, "GLOPAQUE frames=%u zframes=%u wframes=%u op=%.1f op_area=%.3f "
                "untex=%.1f/%.3f decal=%.1f/%.3f texpal=%.1f/%.3f texop=%.1f/%.3f alpha0=%.1f/%.3f color0=%.1f/%.3f direct0=%.1f/%.3f "
                "comp0=%.1f/%.3f oob=%.1f/%.3f nf=%.1f/%.3f nm=%.1f/%.3f nn=%.1f/%.3f qual=%.3f w_qual=%.3f "
                "textures=%.1f bytes=%.0f scan_us=%.1f nd=%.1f nd_area=%.3f draws=%.1f draws_base=%.1f prog_switches=%.1f nd_scan_us=%.2f\n",
                Sec.Frames, Sec.ZFrames, Sec.WFrames, op * per, oparea * per,
                p[Untex] * per, a[Untex] * per, p[Decal] * per, a[Decal] * per, p[TexPal] * per, a[TexPal] * per, p[TexOpaque] * per, a[TexOpaque] * per,
                p[Alpha0] * per, a[Alpha0] * per, p[Color0] * per, a[Color0] * per, p[Direct0] * per, a[Direct0] * per,
                p[Comp0] * per, a[Comp0] * per, p[OutOfRange] * per, a[OutOfRange] * per,
                p[NeedFull] * per, a[NeedFull] * per, p[NeedMixed] * per, a[NeedMixed] * per, p[NeedNone] * per, a[NeedNone] * per,
                share(0), share(1),
                Sec.Textures / (double)Sec.Frames, Sec.Bytes / (double)Sec.Frames, Sec.ScanNs / 1000.0 / Sec.Frames,
                Sec.NoDiscard * per, Sec.NoDiscardArea * per, Sec.Draws * per, Sec.DrawsBase * per, Sec.ProgSwitches * per,
                Sec.NdScanNs / 1000.0 / Sec.Frames);
        }
        Sec = {};
        LogStartNs = now;
    }

private:
    s64 LogStartNs = 0;
};

// debug builds only, OpenGL renderer: the GLTEXCACHE log, GPU::GLTexCache's texel heap (experiment T2
// step 1, docs/plan-t2-texcache.md), with GPU::SceneStats on and the heap there. once a second, per
// frame AssignTexCache ran in (frames, the 3D frames drawn):
//   mode                    the switch's value
//   polys area              textured polygons (texturing on, a format, not shadow masks) and their area
//                           in screens (the shoelace area of the 1x vertices, overdraw counted)
//   served cover cover_area those the heap served, and their share of the polygons and of the area
//   hit kept decoded redecoded
//                           served polygons by how: already current, rehashed the same after a VRAM
//                           write, decoded now, decoded again after their bytes changed
//   range palette volatile budget full
//                           polygons/area on the old path by why: the texture reads past TexMem or the
//                           palette rows (by TexParam), a 4x4 block's palette does (by its data), it
//                           changed in 3 of the last 8 frames it was used in, the frame's decode budget
//                           is spent, no room in the heap or the entry table
//   a3i5 .. direct          per format, served area over textured area
//   entries heap_used       heap entries (textures seen, served or not) and texels in blocks, averaged
//   texels uploads upload_kb
//                           decoded texels, glTexSubImage2D calls and their KB per frame
//   decode_us upload_us hash_us hash_kb
//                           CPU µs per frame decoding, in the upload calls and hashing, and KB hashed
//   assign_us assign_peak_us
//                           CPU µs per frame of AssignTexCache, all of the above in it, and the most in
//                           one frame
//   evictions volatiles removed
//                           this second: blocks freed for room, textures that went volatile, entries
//                           dropped from a full table
//   s2_full s2_draws s2_draws_base s2_switches
//                           step 2: polygons drawn with the twins that have the old decode path (the
//                           fallback ones, 0 in step 1), the opaque and translucent polygons' batches
//                           in list order with and without that key bit (the extra draws are the
//                           difference), and the program switches between the two forms it adds
struct GLTexCacheStats
{
    struct Counters
    {
        u32 Frames = 0;
        u64 Polys[GLTexHeap::NumResults] {};
        double Area[GLTexHeap::NumResults] {};
        double FormatArea[8] {}, FormatServed[8] {};
        u64 Entries = 0, UsedTexels = 0;
        s64 AssignNs = 0, AssignPeakNs = 0;
        u64 FullPolys = 0, Draws = 0, DrawsBase = 0, Switches = 0;
        // GPU::GLTexCache 128 (step 4a): served polygons that read the color plane, and with 256
        // the served ones without one that went to the twins
        u64 ColorPolys = 0, ColorFullPolys = 0;
        double ColorArea = 0, ColorFullArea = 0;
        // step 4b: served color-format polygons that read their records because the entry is
        // palette-volatile, or its plane needed a re-resolve the budget didn't allow; and the
        // palette-volatile entries, summed over frames
        u64 ColorVolPolys = 0, ColorBudgetPolys = 0, PalVolEntries = 0;
        double ColorVolArea = 0, ColorBudgetArea = 0;
    };
    Counters Sec;

    // at the start of every RenderFrame; reads and clears the heap's counters
    void FrameStart(s64 now, GLTexHeap& heap, int mode)
    {
        if (LogStartNs == 0)
            LogStartNs = now;
        if (now - LogStartNs < 1000000000)
            return;

        if (Sec.Frames > 0)
        {
            double per = 1.0 / Sec.Frames;
            const GLTexHeap::Counters& h = heap.Sec;
            u64 polys = 0, served = 0;
            double area = 0, servedArea = 0;
            for (int r = 0; r < GLTexHeap::NumResults; r++)
            {
                polys += Sec.Polys[r];
                area += Sec.Area[r];
                if (r < GLTexHeap::NumServed)
                {
                    served += Sec.Polys[r];
                    servedArea += Sec.Area[r];
                }
            }
            auto fmt = [&](int f) { return Sec.FormatArea[f] > 0 ? Sec.FormatServed[f] / Sec.FormatArea[f] : 0.0; };
            const u64* p = Sec.Polys;
            const double* a = Sec.Area;
            Platform::Log(Platform::LogLevel::Info, "GLTEXCACHE frames=%u mode=%d polys=%.1f area=%.3f served=%.1f/%.3f "
                "cover=%.3f cover_area=%.3f hit=%.1f kept=%.1f decoded=%.2f redecoded=%.2f "
                "range=%.1f/%.3f palette=%.1f/%.3f volatile=%.1f/%.3f budget=%.1f/%.3f full=%.1f/%.3f "
                "a3i5=%.3f/%.3f i2=%.3f/%.3f i4=%.3f/%.3f i8=%.3f/%.3f 4x4=%.3f/%.3f a5i3=%.3f/%.3f direct=%.3f/%.3f "
                "entries=%.0f heap_used=%.0f texels=%.0f uploads=%.2f upload_kb=%.1f decode_us=%.1f upload_us=%.1f "
                "hash_us=%.2f hash_kb=%.1f assign_us=%.1f assign_peak_us=%.0f evictions=%llu volatiles=%llu removed=%llu "
                "s2_full=%.1f s2_draws=%.1f s2_draws_base=%.1f s2_switches=%.1f color=%.1f/%.3f color_full=%.1f/%.3f "
                "color_vol=%.1f/%.3f color_budget=%.1f/%.3f pal_rehash=%.2f pal_kept=%.2f resolves=%.2f resolve_texels=%.0f "
                "pal_volatiles=%llu pal_vol_entries=%.1f\n",
                Sec.Frames, mode, polys * per, area * per, served * per, servedArea * per,
                polys ? (double)served / polys : 0.0, area > 0 ? servedArea / area : 0.0,
                p[GLTexHeap::Hit] * per, p[GLTexHeap::Kept] * per, p[GLTexHeap::Decoded] * per, p[GLTexHeap::Redecoded] * per,
                p[GLTexHeap::FallRange] * per, a[GLTexHeap::FallRange] * per, p[GLTexHeap::FallPalette] * per, a[GLTexHeap::FallPalette] * per,
                p[GLTexHeap::FallVolatile] * per, a[GLTexHeap::FallVolatile] * per, p[GLTexHeap::FallBudget] * per, a[GLTexHeap::FallBudget] * per,
                p[GLTexHeap::FallFull] * per, a[GLTexHeap::FallFull] * per,
                fmt(1), Sec.FormatArea[1] * per, fmt(2), Sec.FormatArea[2] * per, fmt(3), Sec.FormatArea[3] * per,
                fmt(4), Sec.FormatArea[4] * per, fmt(5), Sec.FormatArea[5] * per, fmt(6), Sec.FormatArea[6] * per,
                fmt(7), Sec.FormatArea[7] * per,
                Sec.Entries * per, Sec.UsedTexels * per, h.Texels * per, h.Uploads * per, h.UploadBytes / 1024.0 * per,
                h.DecodeNs / 1000.0 * per, h.UploadNs / 1000.0 * per, h.HashNs / 1000.0 * per, h.HashBytes / 1024.0 * per,
                Sec.AssignNs / 1000.0 * per, Sec.AssignPeakNs / 1000.0,
                (unsigned long long)h.Evictions, (unsigned long long)h.Volatiles, (unsigned long long)h.Removed,
                Sec.FullPolys * per, Sec.Draws * per, Sec.DrawsBase * per, Sec.Switches * per,
                Sec.ColorPolys * per, Sec.ColorArea * per, Sec.ColorFullPolys * per, Sec.ColorFullArea * per,
                Sec.ColorVolPolys * per, Sec.ColorVolArea * per, Sec.ColorBudgetPolys * per, Sec.ColorBudgetArea * per,
                h.PalRehashes * per, h.PalKept * per, h.Resolves * per, h.ResolveTexels * per,
                (unsigned long long)h.PalVolatiles, Sec.PalVolEntries * per);
        }
        Sec = {};
        heap.Sec = {};
        LogStartNs = now;
    }

private:
    s64 LogStartNs = 0;
};

}

#endif

#endif
