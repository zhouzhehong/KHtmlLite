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
#ifndef KHTML_JSENGINE_H
#define KHTML_JSENGINE_H

#include <QString>
#include <QVariant>
#include <QUrl>
#include <functional>
#include <memory>

namespace khtml
{

class JSEngine;
class JSContext;

// Opaque handle to a JS value. Concrete engines wrap their own value
// type inside; callers should not assume anything about the layout.
class JSValueHandle
{
public:
    JSValueHandle() = default;
    virtual ~JSValueHandle() = default;

    virtual bool isUndefined() const = 0;
    virtual bool isNull() const = 0;
    virtual bool isObject() const = 0;
    virtual bool isFunction() const = 0;
    virtual bool isString() const = 0;
    virtual bool isNumber() const = 0;
    virtual bool isBoolean() const = 0;

    virtual QString toString() const = 0;
    virtual double toNumber() const = 0;
    virtual bool toBoolean() const = 0;
};

// Native C++ function exposed to script. Receives the context and arguments,
// returns a value handle owned by the context.
using JSNativeFn = std::function<std::shared_ptr<JSValueHandle>(
    JSContext *ctx,
    const std::vector<std::shared_ptr<JSValueHandle>> &args)>;

// One execution context. Roughly maps to a realm / window global.
// A document owns one context for its lifetime.
class JSContext
{
public:
    virtual ~JSContext() = default;

    // Evaluate a script. sourceUrl is used for stack traces and error
    // reporting; it does not trigger a network load.
    virtual std::shared_ptr<JSValueHandle> evaluate(
        const QString &source, const QUrl &sourceUrl = QUrl()) = 0;

    // Drain pending microtasks (Promise reactions). Called by the event
    // loop after each task boundary, per HTML spec.
    virtual void runMicrotasks() = 0;

    // Bind a native function on the global object.
    virtual void defineGlobalFunction(const QString &name, JSNativeFn fn) = 0;

    // Fetch a property off the global object.
    virtual std::shared_ptr<JSValueHandle> globalProperty(const QString &name) = 0;

    // Throw a JS Error with the given message.
    virtual void throwError(const QString &message) = 0;

    // Last uncaught exception, empty if none. Cleared on next evaluate.
    virtual QString lastException() const = 0;

    // Force a garbage collection pass. The engine also collects on its
    // own heuristics; this is mainly for tests and memory pressure.
    virtual void collectGarbage() = 0;
};

// Factory + lifecycle owner. A JSEngine instance can serve multiple
// contexts (frames), sharing compiled code and heap where the backend
// supports it.
class JSEngine
{
public:
    enum Backend {
        QuickJS,    // default: small footprint, ES2020
        KJS         // legacy KF5JS, kept for regression comparison
    };

    virtual ~JSEngine() = default;

    static std::unique_ptr<JSEngine> create(Backend backend = QuickJS);

    virtual std::unique_ptr<JSContext> createContext() = 0;

    // Engine-wide memory limit in bytes. The backend should trigger GC
    // before exceeding it, and throw on allocation failure rather than
    // grow unbounded. 0 means no explicit cap.
    virtual void setMemoryLimit(size_t bytes) = 0;
    virtual size_t memoryUsage() const = 0;

    // Rough heap object count. Intended for the about:memory style
    // diagnostics, not for programmatic decisions.
    virtual size_t objectCount() const = 0;

    virtual Backend backend() const = 0;
};

} // namespace khtml

#endif
