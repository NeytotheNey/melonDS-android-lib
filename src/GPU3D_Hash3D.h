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

#ifndef GPU3D_HASH3D_H
#define GPU3D_HASH3D_H

#include "types.h"
#include "Platform.h"

namespace melonDS
{

// debug only (GPU::Hash3DLogging, Renderer3D::QueueHash3D): the HASH3D line's checksum, a 64-bit
// FNV-1a over the 256x192 words of a 3D image in GetLine's format (r, g and b in 6 bits in bytes 0
// to 2, alpha in 5 bits in byte 3), the same as HASH2D's. every renderer hashes that format, so
// their lines can be compared with each other as well as with another build's. HASH3DF's is the
// same over the compute renderer's full-resolution RGBA8 words
inline u64 Hash3DImage(const u32* pixels, int stride, int width = 256, int height = 192) noexcept
{
    u64 h = 0xCBF29CE484222325ULL;
    for (int y = 0; y < height; y++)
    {
        const u32* row = &pixels[y * stride];
        for (int x = 0; x < width; x++)
            h = (h ^ row[x]) * 0x100000001B3ULL;
    }
    return h;
}

// the OpenGL renderer's color target as read back (bytes b, g, r, a in 8 bits) in GetLine's format,
// the conversion GLRenderer::GetLine does
inline u32 Hash3DFromGLColor(u32 p) noexcept
{
    u32 rb = ((p & 0x000000FC) << 16) | ((p & 0x00FC0000) >> 16);
    u32 rgb = (p & 0x0000FC00) | rb;
    return (rgb >> 2) | ((p & 0xF8000000) >> 3);
}

// logs a frame's line. two call sites, taking turns: the Android frontend lets 50 lines a second
// through from one, and a game at 60 FPS needs a line every frame. the format strings differ (the
// %.0s prints nothing), so the linker can't fold them into one
inline void LogHash3D(u32 frame, u64 hash) noexcept
{
    if (frame & 1)
        Platform::Log(Platform::LogLevel::Info, "HASH3D frame=%u hash=%016llX%.0s\n", frame, (unsigned long long)hash, "");
    else
        Platform::Log(Platform::LogLevel::Info, "HASH3D frame=%u hash=%016llX\n", frame, (unsigned long long)hash);
}

// and HASH3DF's, with the image's size
inline void LogHash3DFull(u32 frame, u64 hash, int width, int height) noexcept
{
    if (frame & 1)
        Platform::Log(Platform::LogLevel::Info, "HASH3DF frame=%u hash=%016llX size=%dx%d%.0s\n", frame, (unsigned long long)hash, width, height, "");
    else
        Platform::Log(Platform::LogLevel::Info, "HASH3DF frame=%u hash=%016llX size=%dx%d\n", frame, (unsigned long long)hash, width, height);
}

// a frame whose image couldn't be read back without waiting for the GPU
inline void LogHash3DDropped(u32 frame, const char* why, bool full = false) noexcept
{
    Platform::Log(Platform::LogLevel::Info, "%s frame=%u dropped=%s\n", full ? "HASH3DF" : "HASH3D", frame, why);
}

}

#if defined(MELONADE_DEBUG_TOOLS) && defined(OGLRENDERER_ENABLED)

#include "OpenGLSupport.h"

namespace melonDS
{

// the OpenGL and compute renderers' HASH3D readbacks: Queue reads the 256x192 image bound for reading
// into a pixel buffer of a ring, with a fence after it, and a later Queue (or Collect) hashes and logs
// the ones the GPU has finished, oldest first, so frames stay in order. nothing waits for the GPU: a
// frame that finds the ring full is logged as dropped. emulator thread only, GL context current
struct Hash3DReadback
{
    // HASH3DF's slots are whole full-resolution images (12 MB each at 8x), so its ring is shorter
    static constexpr int MaxSlots = 6;
    static constexpr int FullSlots = 3;

    // GL_RGBA / GL_UNSIGNED_BYTE from the OpenGL renderer's downscale target (converted with
    // Hash3DFromGLColor), or GL_RGBA_INTEGER / GL_UNSIGNED_BYTE from the compute renderer's low res
    // image, which is GetLine's format already. with a width, HASH3DF's: width x height of GL_RGBA /
    // GL_UNSIGNED_BYTE (the compute renderer's full-resolution image), hashed as they are
    explicit Hash3DReadback(bool glColor, int width = 0, int height = 0)
        : GLColor(glColor), Full(width > 0), Width(Full ? width : 256), Height(Full ? height : 192),
          Slots(Full ? FullSlots : MaxSlots)
    {
        glGenBuffers(Slots, Buffers);
        GLint previous = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
        // HASH3DF's are checked. errors from before aren't the allocations'
        for (int i = 0; Full && i < 16 && glGetError() != GL_NO_ERROR; i++) {}
        GLsizeiptr size = (GLsizeiptr)Width * Height * 4;
        for (int i = 0; i < Slots; i++)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, Buffers[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
            if (!Full)
                continue;
            GLint64 real = -1;
            glGetBufferParameteri64v(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &real);
            Allocated = Allocated && glGetError() == GL_NO_ERROR && real == (GLint64)size;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous);
    }

    Hash3DReadback(const Hash3DReadback&) = delete;
    Hash3DReadback& operator=(const Hash3DReadback&) = delete;

    // the readbacks still pending (a renderer change, a game stop) are logged as dropped
    ~Hash3DReadback()
    {
        DropPending("rendererclosed");
        glDeleteBuffers(Slots, Buffers);
    }

    // logs the readbacks still pending as dropped, with why, and forgets them
    void DropPending(const char* why)
    {
        for (; Count > 0; Count--, Head = (Head + 1) % Slots)
        {
            LogHash3DDropped(Frames[Head], why, Full);
            glDeleteSync(Fences[Head]);
            Fences[Head] = nullptr;
        }
    }

    // with the image to hash bound to GL_READ_FRAMEBUFFER and its read buffer set. leaves every
    // binding it changes as it found it
    void Queue(u32 frame)
    {
        Collect();
        if (Count == Slots)
        {
            LogHash3DDropped(frame, "ringfull", Full);
            return;
        }

        int slot = (Head + Count) % Slots;
        GLint previous = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, Buffers[slot]);
        glReadPixels(0, 0, Width, Height, GLColor || Full ? GL_RGBA : GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous);
        Fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        Frames[slot] = frame;
        Count++;
    }

    // hashes and logs every readback the GPU has finished, in order
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

            if (state == GL_WAIT_FAILED)
                LogHash3DDropped(Frames[slot], "waitfailed", Full);
            else
            {
                GLint previous = 0;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, Buffers[slot]);
                const u32* data = (const u32*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)Width * Height * 4, GL_MAP_READ_BIT);
                if (data && Full)
                {
                    LogHash3DFull(Frames[slot], Hash3DImage(data, Width, Width, Height), Width, Height);
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
                else if (data)
                {
                    u64 hash;
                    if (GLColor)
                    {
                        for (int i = 0; i < 256 * 192; i++)
                            Converted[i] = Hash3DFromGLColor(data[i]);
                        hash = Hash3DImage(Converted, 256);
                    }
                    else
                        hash = Hash3DImage(data, 256);
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                    LogHash3D(Frames[slot], hash);
                }
                else
                    LogHash3DDropped(Frames[slot], "mapfailed", Full);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous);
            }

            Head = (Head + 1) % Slots;
            Count--;
        }
    }

    const bool GLColor, Full;
    const int Width, Height, Slots;
    // every slot got its storage (checked at construction)
    bool Allocated = true;

private:
    GLuint Buffers[MaxSlots] {};
    GLsync Fences[MaxSlots] {};
    u32 Frames[MaxSlots] {};
    int Head = 0, Count = 0;
    u32 Converted[256 * 192] {};
};

}

#endif

#endif
