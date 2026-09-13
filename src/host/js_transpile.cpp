// Embedded JS transpiler: QuickJS runtime + Buble ES2015->ES3.
// Used by the page loader so modern page scripts can run on the KJS engine.
#include "js_transpile.h"

#include <QRegularExpression>
#include <QByteArray>

extern "C" {
#include "quickjs.h"
}

#include "buble_embed.h"

namespace {

JSRuntime *s_rt = nullptr;
JSContext *s_ctx = nullptr;
JSValue s_transformFn = JS_UNDEFINED;

bool ensureEngine()
{
    if (s_ctx)
        return true;

    s_rt = JS_NewRuntime();
    if (!s_rt)
        return false;
    // Keep the transpiler sandbox bounded so it can never balloon
    // memory on low-end devices; an oversized transform simply falls
    // back to the original script.
    JS_SetMemoryLimit(s_rt, 96 * 1024 * 1024);
    JS_SetGCThreshold(s_rt, 8 * 1024 * 1024);
    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx)
        return false;

    // Evaluate the bundled Buble (IIFE, exposes __bubleTransform globally).
    JSValue v = JS_Eval(s_ctx, reinterpret_cast<const char *>(g_bubleBundle),
                        static_cast<size_t>(g_bubleBundleLen), "buble.js",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JS_FreeValue(s_ctx, v);
        return false;
    }
    JS_FreeValue(s_ctx, v);

    s_transformFn = JS_GetPropertyStr(s_ctx, JS_GetGlobalObject(s_ctx),
                                      "__bubleTransform");
    if (JS_IsException(s_transformFn) || !JS_IsFunction(s_ctx, s_transformFn)) {
        JS_FreeValue(s_ctx, s_transformFn);
        s_transformFn = JS_UNDEFINED;
        return false;
    }
    return true;
}

// Strip syntax Buble refuses to transform (async/await/generators) so the
// remaining ES2015 code can still be lowered. Only run when modern syntax was
// detected, so plain ES3 scripts are never touched.
QString stripUnsupportedSyntax(const QString &src)
{
    QString out = src;
    out.replace(QRegularExpression(QStringLiteral("\\basync\\s+(?=function\\b)")), QString());
    out.replace(QRegularExpression(QStringLiteral("\\basync\\s*(?=\\()")), QString());
    out.replace(QRegularExpression(QStringLiteral("\\bawait\\s+")), QString());
    out.replace(QRegularExpression(QStringLiteral("function\\s*\\*")), QStringLiteral("function"));
    out.replace(QRegularExpression(QStringLiteral("\\byield\\s+")), QString());
    out.replace(QRegularExpression(QStringLiteral("\\byield\\b")), QString());
    return out;
}

} // namespace

bool looksLikeEs6(const QString &source)
{
    static const QRegularExpression es6Markers(
        QStringLiteral("=>|\\bconst\\s|\\blet\\s|`|\\bclass\\s|\\.\\.\\.|"
                       "\\bfor\\s*\\([^)]*\\bof\\b|function\\s*\\*|\\basync\\b|"
                       "\\bawait\\b|\\bimport\\b|\\bexport\\b"));
    return es6Markers.match(source).hasMatch();
}

QString transpileJs(const QString &source)
{
    if (source.isEmpty() || !looksLikeEs6(source))
        return source;
    if (!ensureEngine())
        return source;

    QString pre = stripUnsupportedSyntax(source);
    QByteArray utf8 = pre.toUtf8();

    JSValue arg = JS_NewStringLen(s_ctx, utf8.constData(),
                                  static_cast<size_t>(utf8.size()));
    JSValue ret = JS_Call(s_ctx, s_transformFn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(s_ctx, arg);

    if (JS_IsException(ret) || JS_IsNull(ret)) {
        // Buble reported an error; keep the original source. A parse error in
        // one script must never break the page.
        if (JS_IsException(ret))
            JS_FreeValue(s_ctx, ret);
        return source;
    }

    const char *cstr = JS_ToCString(s_ctx, ret);
    QString result;
    if (cstr) {
        result = QString::fromUtf8(cstr);
        JS_FreeCString(s_ctx, cstr);
    } else {
        result = source;
    }
    JS_FreeValue(s_ctx, ret);
    JS_RunGC(s_rt);
    return result;
}
