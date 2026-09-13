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
#ifndef KHTML_QUICKJS_ENGINE_H
#define KHTML_QUICKJS_ENGINE_H

#include "jsengine.h"

struct JSRuntime;
struct JSContext;
struct JSValue;

namespace khtml
{

// Wraps a QuickJS runtime. One per KHTMLPart root; child frames share
// the same runtime so that structured clone and cross-frame lookups
// stay cheap.
class QuickJSEngine : public JSEngine
{
public:
    QuickJSEngine();
    ~QuickJSEngine() override;

    std::unique_ptr<JSContext> createContext() override;

    void setMemoryLimit(size_t bytes) override;
    size_t memoryUsage() const override;
    size_t objectCount() const override;
    Backend backend() const override { return QuickJS; }

    JSRuntime *runtime() const { return m_rt; }

private:
    JSRuntime *m_rt;
    size_t m_memLimit = 0;
};

} // namespace khtml

#endif
