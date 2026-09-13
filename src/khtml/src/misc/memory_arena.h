/*
    This file is part of the KDE libraries

    Copyright (C) 2024 KHTML Contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/
#ifndef KHTML_MEMORY_ARENA_H
#define KHTML_MEMORY_ARENA_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <utility>
#include <new>

namespace khtml
{

// Type-safe bump allocator for objects that share one lifetime, such
// as the DOM tree of a single document. Allocation is pointer-bump
// into a chunk; individual frees are not supported, the whole arena
// is released at once on document teardown.
//
// The legacy C-style ArenaPool in arena.h predates C++ templates and
// stays in place for the parser; this is the counterpart for new code
// that wants construction and alignment handled for it.
template <size_t ChunkSize = 64 * 1024>
class MemoryArena
{
public:
    MemoryArena() = default;

    ~MemoryArena()
    {
        release();
    }

    MemoryArena(const MemoryArena &) = delete;
    MemoryArena &operator=(const MemoryArena &) = delete;

    void *allocate(size_t bytes)
    {
        const size_t aligned = (bytes + kAlignment - 1) & ~(kAlignment - 1);

        if (m_chunks.empty() || m_offset + aligned > ChunkSize) {
            growChunk(aligned);
        }

        void *ptr = m_chunks.back().data + m_offset;
        m_offset += aligned;
        m_allocated += aligned;
        return ptr;
    }

    template <typename T, typename... Args>
    T *create(Args &&... args)
    {
        void *mem = allocate(sizeof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    // Release all backing memory. Destructors of live objects are not
    // called; callers that need them must walk the tree first.
    void release()
    {
        for (Chunk &c : m_chunks) {
            std::free(c.base);
        }
        m_chunks.clear();
        m_offset = 0;
        m_allocated = 0;
    }

    size_t bytesAllocated() const { return m_allocated; }
    size_t chunkCount() const { return m_chunks.size(); }

private:
    struct Chunk {
        uint8_t *base;
        uint8_t *data;
        size_t capacity;
    };

    static constexpr size_t kAlignment = alignof(std::max_align_t);

    std::vector<Chunk> m_chunks;
    size_t m_offset = 0;
    size_t m_allocated = 0;

    void growChunk(size_t needed)
    {
        size_t capacity = ChunkSize;
        if (needed > capacity) {
            // Oversized object gets its own right-sized chunk rather
            // than inflating every subsequent chunk.
            capacity = needed;
        }

        void *raw = std::malloc(capacity);
        if (!raw) {
            throw std::bad_alloc();
        }

        uint8_t *base = static_cast<uint8_t *>(raw);
        uintptr_t addr = reinterpret_cast<uintptr_t>(base);
        size_t pad = (kAlignment - (addr & (kAlignment - 1))) & (kAlignment - 1);

        m_chunks.push_back({base, base + pad, capacity});
        m_offset = 0;
    }
};

} // namespace khtml

#endif
