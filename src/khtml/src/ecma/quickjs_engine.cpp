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
#include "quickjs_engine.h"

#include <quickjs.h>
#include <QDebug>
#include <cstring>
#include <unordered_map>

namespace khtml
{

// ---------------------------------------------------------------------------
// Value wrapper
//
// QuickJS uses manual refcounting. We dup on construction and free on
// destruction so handles can be passed around safely.
// ---------------------------------------------------------------------------

class QuickJSValue : public JSValueHandle
{
public:
    QuickJSValue(::JSContext *ctx, JSValue val)
        : m_ctx(ctx), m_val(val) {}

    ~QuickJSValue() override;

    bool isUndefined() const override;
    bool isNull() const override;
    bool isObject() const override;
    bool isFunction() const override;
    bool isString() const override;
    bool isNumber() const override;
    bool isBoolean() const override;

    QString toString() const override;
    double toNumber() const override;
    bool toBoolean() const override;

    JSValue raw() const { return m_val; }

private:
    // Global ::JSContext is the QuickJS type; the unqualified name would
    // resolve to this file's abstract khtml::JSContext base.
    ::JSContext *m_ctx;
    JSValue m_val;
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

class QuickJSContext : public JSContext
{
public:
    explicit QuickJSContext(JSRuntime *rt);
    ~QuickJSContext() override;

    std::shared_ptr<JSValueHandle> evaluate(const QString &source, const QUrl &sourceUrl) override;
    void runMicrotasks() override;
    void defineGlobalFunction(const QString &name, JSNativeFn fn) override;
    std::shared_ptr<JSValueHandle> globalProperty(const QString &name) override;
    void throwError(const QString &message) override;
    QString lastException() const override;
    void collectGarbage() override;

    ::JSContext *raw() const { return m_ctx; }

private:
    ::JSContext *m_ctx;
    QString m_lastException;

    // Native callbacks are kept alive here for as long as the context
    // exists. QuickJS only stores a function pointer, so the std::function
    // has to live somewhere stable.
    std::unordered_map<uint32_t, JSNativeFn> m_natives;
    uint32_t m_nextNativeId = 1;

    static JSValue nativeTrampoline(::JSContext *ctx, JSValueConst thisVal,
                                    int argc, JSValueConst *argv,
                                    int magic);
    QString extractException();
};

// ---------------------------------------------------------------------------
// QuickJSValue
// ---------------------------------------------------------------------------

QuickJSValue::~QuickJSValue()
{
    JS_FreeValue(m_ctx, m_val);
}

bool QuickJSValue::isUndefined() const { return JS_IsUndefined(m_val); }
bool QuickJSValue::isNull() const { return JS_IsNull(m_val); }
bool QuickJSValue::isObject() const { return JS_IsObject(m_val); }
bool QuickJSValue::isFunction() const { return JS_IsFunction(m_ctx, m_val); }
bool QuickJSValue::isString() const { return JS_IsString(m_val); }
bool QuickJSValue::isNumber() const { return JS_IsNumber(m_val); }
bool QuickJSValue::isBoolean() const { return JS_IsBool(m_val); }

QString QuickJSValue::toString() const
{
    const char *cstr = JS_ToCString(m_ctx, m_val);
    if (!cstr) {
        return QString();
    }
    QString result = QString::fromUtf8(cstr);
    JS_FreeCString(m_ctx, cstr);
    return result;
}

double QuickJSValue::toNumber() const
{
    double d = 0;
    JS_ToFloat64(m_ctx, &d, m_val);
    return d;
}

bool QuickJSValue::toBoolean() const
{
    return JS_ToBool(m_ctx, m_val);
}

// ---------------------------------------------------------------------------
// QuickJSContext
// ---------------------------------------------------------------------------

QuickJSContext::QuickJSContext(JSRuntime *rt)
{
    m_ctx = JS_NewContext(rt);
    if (!m_ctx) {
        qFatal("QuickJS: failed to allocate context");
    }
}

QuickJSContext::~QuickJSContext()
{
    m_natives.clear();
    if (m_ctx) {
        JS_FreeContext(m_ctx);
    }
}

QString QuickJSContext::extractException()
{
    JSValue exc = JS_GetException(m_ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        JS_FreeValue(m_ctx, exc);
        return QString();
    }

    const char *msg = JS_ToCString(m_ctx, exc);
    QString result = msg ? QString::fromUtf8(msg) : QStringLiteral("unknown error");
    if (msg) {
        JS_FreeCString(m_ctx, msg);
    }

    // Append the first stack frame if available, useful for console output.
    JSValue stack = JS_GetPropertyStr(m_ctx, exc, "stack");
    if (JS_IsString(stack)) {
        const char *s = JS_ToCString(m_ctx, stack);
        if (s) {
            result = QString::fromUtf8(s);
            JS_FreeCString(m_ctx, s);
        }
    }
    JS_FreeValue(m_ctx, stack);
    JS_FreeValue(m_ctx, exc);
    return result;
}

std::shared_ptr<JSValueHandle> QuickJSContext::evaluate(const QString &source, const QUrl &sourceUrl)
{
    m_lastException.clear();

    QByteArray utf8 = source.toUtf8();
    QByteArray url = sourceUrl.toString().toUtf8();

    JSValue result = JS_Eval(m_ctx, utf8.constData(), utf8.size(),
                             url.isEmpty() ? "eval" : url.constData(),
                             JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        m_lastException = extractException();
        JS_FreeValue(m_ctx, result);
        return std::make_shared<QuickJSValue>(m_ctx, JS_UNDEFINED);
    }

    auto handle = std::make_shared<QuickJSValue>(m_ctx, result);
    return handle;
}

void QuickJSContext::runMicrotasks()
{
    // QuickJS queues promise reactions as jobs on the runtime. Execute
    // them until the queue drains; newly scheduled jobs run in the same
    // drain pass, matching HTML's microtask checkpoint.
    JSRuntime *rt = JS_GetRuntime(m_ctx);
    ::JSContext *ctx1;
    int err;
    size_t jobCount = 0;
    while ((err = JS_ExecutePendingJob(rt, &ctx1)) > 0) {
        if (++jobCount > 100000) {
            // Guard against runaway promise chains.
            qWarning() << "QuickJS: microtask queue exceeded 100k jobs, breaking";
            break;
        }
    }
    if (err < 0) {
        m_lastException = extractException();
    }
}

JSValue QuickJSContext::nativeTrampoline(::JSContext *ctx, JSValueConst /*thisVal*/,
                                         int argc, JSValueConst *argv,
                                         int magic)
{
    // The magic value carries the id used to look up the real callback.
    auto *self = static_cast<QuickJSContext *>(JS_GetContextOpaque(ctx));
    if (!self) {
        return JS_ThrowTypeError(ctx, "context detached");
    }

    auto it = self->m_natives.find(static_cast<uint32_t>(magic));
    if (it == self->m_natives.end()) {
        return JS_ThrowInternalError(ctx, "native callback missing");
    }

    std::vector<std::shared_ptr<JSValueHandle>> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args.push_back(std::make_shared<QuickJSValue>(ctx, JS_DupValue(ctx, argv[i])));
    }

    auto result = it->second(self, args);
    auto *qv = dynamic_cast<QuickJSValue *>(result.get());
    if (qv) {
        return JS_DupValue(ctx, qv->raw());
    }
    return JS_UNDEFINED;
}

void QuickJSContext::defineGlobalFunction(const QString &name, JSNativeFn fn)
{
    uint32_t id = m_nextNativeId++;
    m_natives.emplace(id, std::move(fn));

    QByteArray nameUtf8 = name.toUtf8();
    JSValue func = JS_NewCFunctionMagic(m_ctx, &QuickJSContext::nativeTrampoline,
                                        nameUtf8.constData(), 0,
                                        ::JS_CFUNC_generic,
                                        static_cast<int>(id));

    JSValue global = JS_GetGlobalObject(m_ctx);
    JS_SetPropertyStr(m_ctx, global, nameUtf8.constData(), func);
    JS_FreeValue(m_ctx, global);
}

std::shared_ptr<JSValueHandle> QuickJSContext::globalProperty(const QString &name)
{
    JSValue global = JS_GetGlobalObject(m_ctx);
    QByteArray nameUtf8 = name.toUtf8();
    JSValue prop = JS_GetPropertyStr(m_ctx, global, nameUtf8.constData());
    JS_FreeValue(m_ctx, global);
    return std::make_shared<QuickJSValue>(m_ctx, prop);
}

void QuickJSContext::throwError(const QString &message)
{
    QByteArray utf8 = message.toUtf8();
    JS_ThrowTypeError(m_ctx, "%s", utf8.constData());
}

QString QuickJSContext::lastException() const
{
    return m_lastException;
}

void QuickJSContext::collectGarbage()
{
    JS_RunGC(JS_GetRuntime(m_ctx));
}

// ---------------------------------------------------------------------------
// QuickJSEngine
// ---------------------------------------------------------------------------

QuickJSEngine::QuickJSEngine()
{
    m_rt = JS_NewRuntime();
    if (!m_rt) {
        qFatal("QuickJS: failed to allocate runtime");
    }
    // Default to a 64 MiB heap. Pages that legitimately need more can
    // raise it via setMemoryLimit, but an unbounded heap is how browser
    // tabs end up eating gigabytes.
    m_memLimit = 64 * 1024 * 1024;
    JS_SetMemoryLimit(m_rt, m_memLimit);

    // Interrupt long-running scripts after 10 seconds of wall time.
    // The interrupt callback is set per-context in real integration;
    // here we just establish the policy.
}

QuickJSEngine::~QuickJSEngine()
{
    if (m_rt) {
        JS_FreeRuntime(m_rt);
    }
}

std::unique_ptr<JSContext> QuickJSEngine::createContext()
{
    auto ctx = std::make_unique<QuickJSContext>(m_rt);
    JS_SetContextOpaque(ctx->raw(), ctx.get());
    return ctx;
}

void QuickJSEngine::setMemoryLimit(size_t bytes)
{
    m_memLimit = bytes;
    JS_SetMemoryLimit(m_rt, bytes);
}

size_t QuickJSEngine::memoryUsage() const
{
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(m_rt, &usage);
    return usage.memory_used_size;
}

size_t QuickJSEngine::objectCount() const
{
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(m_rt, &usage);
    return usage.obj_count + usage.js_func_count + usage.c_func_count + usage.atom_count;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<JSEngine> JSEngine::create(Backend backend)
{
    switch (backend) {
    case QuickJS:
        return std::make_unique<QuickJSEngine>();
    case KJS:
        // KJS backend lives in the legacy ecma/ binding. Returning null
        // here keeps the abstraction honest; the caller falls back.
        qWarning() << "KJS backend requested but not linked in this build";
        return nullptr;
    }
    return nullptr;
}

} // namespace khtml
