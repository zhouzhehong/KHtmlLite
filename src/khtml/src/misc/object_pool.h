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
#ifndef KHTML_OBJECT_POOL_H
#define KHTML_OBJECT_POOL_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <new>

namespace khtml
{

// Free-list pool for fixed-size objects that churn during layout:
// line boxes, floating objects, break opportunities. Acquire/release
// avoid malloc after the initial ramp-up, and the freed slots reuse
// their own storage for the linked list so there is no side allocation.
template <typename T, size_t BlockSize = 256>
class ObjectPool
{
public:
    ObjectPool() = default;

    ~ObjectPool()
    {
        for (void *block : m_blocks) {
            std::free(block);
        }
    }

    ObjectPool(const ObjectPool &) = delete;
    ObjectPool &operator=(const ObjectPool &) = delete;

    template <typename... Args>
    T *acquire(Args &&... args)
    {
        if (!m_freeList) {
            grow();
        }
        Node *node = m_freeList;
        m_freeList = node->next;
        ++m_live;
        return new (node) T(std::forward<Args>(args)...);
    }

    void release(T *obj)
    {
        if (!obj) {
            return;
        }
        obj->~T();
        Node *node = reinterpret_cast<Node *>(obj);
        node->next = m_freeList;
        m_freeList = node;
        --m_live;
    }

    size_t liveCount() const { return m_live; }
    size_t capacity() const { return m_blocks.size() * BlockSize; }

    // Return unused blocks to the system. Keeps the currently-populated
    // block so steady-state allocation does not immediately re-malloc.
    void shrink()
    {
        // Rebuild a compact free list, then release fully-free blocks.
        // In practice layout calls this between documents, not frames.
        if (m_live != 0) {
            return;
        }
        for (size_t i = 1; i < m_blocks.size(); ++i) {
            std::free(m_blocks[i]);
        }
        if (m_blocks.size() > 1) {
            m_blocks.resize(1);
        }
        m_freeList = nullptr;
        if (!m_blocks.empty()) {
            chainBlock(m_blocks.front(), 0);
        }
    }

private:
    union Node {
        Node *next;
        alignas(T) unsigned char storage[sizeof(T)];
    };

    static_assert(sizeof(T) >= sizeof(void *),
                  "pooled objects must be at least pointer-sized");

    Node *m_freeList = nullptr;
    std::vector<void *> m_blocks;
    size_t m_live = 0;

    void grow()
    {
        void *block = std::malloc(sizeof(Node) * BlockSize);
        if (!block) {
            throw std::bad_alloc();
        }
        m_blocks.push_back(block);
        chainBlock(block, m_blocks.size() - 1);
    }

    void chainBlock(void *block, size_t /*index*/)
    {
        Node *nodes = static_cast<Node *>(block);
        for (size_t i = 0; i < BlockSize; ++i) {
            nodes[i].next = m_freeList;
            m_freeList = &nodes[i];
        }
    }
};

} // namespace khtml

#endif
