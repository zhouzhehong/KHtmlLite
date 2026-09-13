/* qjs_dom_bridge.cpp - QuickJS to KHTML DOM bridge (engine-internal).
 *
 * Binds a compact DOM surface onto the embedded QuickJS engine:
 *   document.getElementById / querySelector / createElement / body / title
 *   element.style.<prop>  (read+write via CSSStyleDeclaration)
 *   element.setAttribute / getAttribute / appendChild / className / id
 *   element.addEventListener (no-op: DOM events are wired by KHTML itself)
 *
 * Scripts that touch anything else return undefined instead of throwing,
 * so page code degrades gracefully; needsFallback() flags the truly
 * unsupported case for the caller to fall back to KJS.
 *
 * All QuickJS types live inside Impl so this translation unit never
 * pollutes KJS-using TUs with the global JSValue typedef.
 */
#include "qjs_dom_bridge.h"

#include <QUrl>
#include <QByteArray>
#include <QDateTime>

#include <quickjs.h>

#include <dom/dom_doc.h>
#include <dom/dom_element.h>
#include <dom/dom_node.h>
#include <dom/html_document.h>
#include <dom/html_element.h>
#include <dom/css_value.h>
#include <dom/dom_string.h>
#include <xml/dom2_eventsimpl.h>
#include <cstdio>
#include <QHash>
#include <QVector>

static void qjslog(const char *msg)
{
    FILE *f = fopen("C:/Users/zhouzhehong/khtml_qjs.log", "a");
    if (f) { fprintf(f, "[%lld] %s\n", (long long)QDateTime::currentMSecsSinceEpoch(), msg); fclose(f); }
}

static void qjslog2(const char *msg, const void *p1, const void *p2)
{
    FILE *f = fopen("C:/Users/zhouzhehong/khtml_qjs.log", "a");
    if (f) { fprintf(f, "[%lld] %s p1=%p p2=%p\n", (long long)QDateTime::currentMSecsSinceEpoch(), msg, p1, p2); fclose(f); }
}

using namespace DOM;

namespace khtml
{

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static QString dstr(const DOM::DOMString &s)
{
    return QString::fromUtf16(reinterpret_cast<const ushort *>(s.unicode()), s.length());
}

static DOM::DOMString mk(const QString &s)
{
    return DOM::DOMString(s);
}

static ::JSValue jsStr(::JSContext *ctx, const QString &s)
{
    const QByteArray b = s.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), b.size());
}

static QString jsValStr(::JSContext *ctx, ::JSValueConst v)
{
    size_t len = 0;
    const char *p = JS_ToCStringLen(ctx, &len, v);
    if (!p) {
        return QString();
    }
    QString s = QString::fromUtf8(p, int(len));
    JS_FreeCString(ctx, p);
    return s;
}

// ---------------------------------------------------------------------------
// node wrapper: JS object <-> DOM::Node
// ---------------------------------------------------------------------------
static ::JSClassID nodeClassId;

struct NodeWrap {
    Node node;
};

static ::JSClassDef nodeClassDef = {
    "KHTMLNode",
    [](JSRuntime *rt, ::JSValue val) {
        NodeWrap *w = static_cast<NodeWrap *>(JS_GetOpaque(val, nodeClassId));
        if (w) {
            qjslog2("NODE_FINALIZE", w->node.handle(), (void*)JS_VALUE_GET_PTR(val));
            delete w;
        }
    },
};

static ::JSValue wrapNode(::JSContext *ctx, const Node &n)
{
    if (n.isNull()) {
        return JS_NULL;
    }
    ::JSValue proto = JS_GetClassProto(ctx, nodeClassId);
    ::JSValue obj = JS_NewObjectProtoClass(ctx, proto, nodeClassId);
    JS_FreeValue(ctx, proto);
    NodeWrap *w = new NodeWrap;
    w->node = n;
    JS_SetOpaque(obj, w);
    qjslog2("NODE_WRAP", n.handle(), (void*)JS_VALUE_GET_PTR(obj));
    return obj;
}

static NodeWrap *unwrapNode(::JSContext *ctx, ::JSValueConst v)
{
    if (!JS_IsObject(v)) {
        return nullptr;
    }
    return static_cast<NodeWrap *>(JS_GetOpaque(v, nodeClassId));
}

// ---------------------------------------------------------------------------
// QuickJS event listener table
//
// addEventListener() callbacks registered from page JS live here so that
// DOM events can be dispatched through QuickJS instead of falling through
// to the legacy KJS event path. Keyed by NodeImpl* identity + event type.
// ---------------------------------------------------------------------------
struct QjsListenerEntry {
    QString type;
    ::JSValue func;  // owned, DupValue'd on insert
};

static QHash<quintptr, QVector<QjsListenerEntry>> g_qjsListeners;
static ::JSRuntime *g_qjsListenerRt = nullptr;

static const char *kSupportedEvents[] = {
    "click", "mousedown", "mouseup", "input", "change", "keydown", "keyup"
};

static bool isSupportedEventType(const QString &type)
{
    for (const char *e : kSupportedEvents) {
        if (type == QLatin1String(e)) return true;
    }
    return false;
}

// Build a minimal event object for QuickJS handlers.
static ::JSValue wrapEvent(::JSContext *ctx, DOM::EventImpl *evt)
{
    if (!evt) return JS_NULL;
    ::JSValue obj = JS_NewObject(ctx);
    DOM::Event ev(evt);
    JS_SetPropertyStr(ctx, obj, "type", jsStr(ctx, dstr(ev.type())));
    JS_SetPropertyStr(ctx, obj, "bubbles", JS_NewBool(ctx, evt->bubbles()));
    JS_SetPropertyStr(ctx, obj, "cancelable", JS_NewBool(ctx, evt->cancelable()));
    return obj;
}

// style wrapper
static ::JSClassID styleClassId;

struct StyleWrap {
    Node node;  // element; style() fetched on demand to avoid stale refs
};

static ::JSClassDef styleClassDef = {
    "KHTMLStyle",
    [](JSRuntime *rt, ::JSValue val) {
        StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(val, styleClassId));
        if (w) {
            qjslog2("STYLE_FINALIZE", w->node.handle(), (void*)JS_VALUE_GET_PTR(val));
            delete w;
        }
    },
};

static ::JSValue wrapStyle(::JSContext *ctx, const Node &n)
{
    ::JSValue proto = JS_GetClassProto(ctx, styleClassId);
    ::JSValue obj = JS_NewObjectProtoClass(ctx, proto, styleClassId);
    JS_FreeValue(ctx, proto);
    StyleWrap *w = new StyleWrap;
    w->node = n;
    JS_SetOpaque(obj, w);
    qjslog2("STYLE_WRAP", n.handle(), (void*)JS_VALUE_GET_PTR(obj));
    return obj;
}

static StyleWrap *unwrapStyle(::JSContext *ctx, ::JSValueConst v)
{
    if (!JS_IsObject(v)) {
        return nullptr;
    }
    return static_cast<StyleWrap *>(JS_GetOpaque(v, styleClassId));
}

// camelCase -> kebab-case
static QString cssKebab(const QString &name)
{
    QString out;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (c.isUpper()) {
            out += QLatin1Char('-');
            out += c.toLower();
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// node prototype: properties
// ---------------------------------------------------------------------------
static ::JSValue nodeGetProp(::JSContext *ctx, ::JSValueConst this_val, int magic)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_UNDEFINED;
    }
    const Node n = w->node;
    switch (magic) {
    case 0: return jsStr(ctx, dstr(n.nodeName()));      // nodeName
    case 1: return wrapNode(ctx, n.parentNode());        // parentNode
    case 2: return wrapNode(ctx, n.firstChild());        // firstChild
    case 3: return wrapNode(ctx, n.lastChild());         // lastChild
    case 4: return wrapNode(ctx, n.nextSibling());       // nextSibling
    case 5: return wrapNode(ctx, n.previousSibling());   // previousSibling
    case 6: { // childNodes -> array
        const NodeList list = n.childNodes();
        const unsigned long cnt = list.length();
        ::JSValue arr = JS_NewArray(ctx);
        for (unsigned long i = 0; i < cnt; ++i) {
            JS_SetPropertyUint32(ctx, arr, i, wrapNode(ctx, list.item(i)));
        }
        return arr;
    }
    case 7: return JS_NewInt32(ctx, (int)n.nodeType());  // nodeType
    }
    return JS_UNDEFINED;
}

// element properties (magic shifted by 100 to avoid node collisions)
static ::JSValue elGetProp(::JSContext *ctx, ::JSValueConst this_val, int magic)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w || w->node.nodeType() != 1) {
        return JS_UNDEFINED;
    }
    Element el(w->node);
    switch (magic) {
    case 100: return jsStr(ctx, dstr(el.getAttribute(mk("id"))));      // id
    case 101: return jsStr(ctx, dstr(el.getAttribute(mk("class"))));   // className
    case 102: return wrapStyle(ctx, w->node);                     // style
    case 103: return jsStr(ctx, dstr(el.textContent()));                // textContent
    case 104: return jsStr(ctx, dstr(el.tagName()));                    // tagName
    case 105: { // innerHTML (HTML elements only)
        HTMLElement he(w->node);
        return he.isNull() ? jsStr(ctx, QString()) : jsStr(ctx, dstr(he.innerHTML()));
    }
    }
    return JS_UNDEFINED;
}

static ::JSValue elSetProp(::JSContext *ctx, ::JSValueConst this_val,
                           ::JSValueConst val, int magic)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w || w->node.nodeType() != 1) {
        return JS_UNDEFINED;
    }
    Element el(w->node);
    const QString v = jsValStr(ctx, val);
    switch (magic) {
    case 100: el.setAttribute(mk("id"), mk(v)); break;
    case 101: el.setAttribute(mk("class"), mk(v)); break;
    case 103: el.setTextContent(mk(v)); break;
    case 105: {
        HTMLElement he(w->node);
        if (!he.isNull()) {
            he.setInnerHTML(mk(v));
        }
        break;
    }
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// element methods
// ---------------------------------------------------------------------------
static ::JSValue elSetAttribute(::JSContext *ctx, ::JSValueConst this_val,
                                int argc, ::JSValueConst *argv)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_UNDEFINED;
    }
    Element el(w->node);
    el.setAttribute(mk(jsValStr(ctx, argv[0])), mk(jsValStr(ctx, argv[1])));
    return JS_UNDEFINED;
}

static ::JSValue elGetAttribute(::JSContext *ctx, ::JSValueConst this_val,
                                int argc, ::JSValueConst *argv)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_NULL;
    }
    Element el(w->node);
    DOM::DOMString v = el.getAttribute(mk(jsValStr(ctx, argv[0])));
    return v.isNull() ? JS_NULL : jsStr(ctx, dstr(v));
}

static ::JSValue elAppendChild(::JSContext *ctx, ::JSValueConst this_val,
                               int argc, ::JSValueConst *argv)
{
    NodeWrap *self = unwrapNode(ctx, this_val);
    NodeWrap *child = unwrapNode(ctx, argv[0]);
    if (!self || !child) {
        return JS_EXCEPTION;
    }
    Node r = self->node.appendChild(child->node);
    return wrapNode(ctx, r);
}

static ::JSValue elAddEventListener(::JSContext *ctx, ::JSValueConst this_val,
                                    int argc, ::JSValueConst *argv)
{
    // Temporarily no-op: listener table storage is being debugged.
    // Event dispatch for inline handlers goes through JSLazyEventListener.
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// style object
// ---------------------------------------------------------------------------
// Magic index -> CSS property name table (must match kProps in buildProtos).
static const char *const kStyleProps[] = {
    "display", "visibility", "color", "background", "backgroundImage",
    "backgroundSize", "backgroundPosition", "width", "height",
    "padding", "paddingTop", "paddingBottom", "paddingLeft", "paddingRight",
    "margin", "marginTop", "marginBottom", "marginLeft", "marginRight",
    "position", "top", "left", "right", "bottom", "zIndex", "opacity",
    "fontSize", "fontFamily", "fontWeight", "textAlign", "lineHeight",
    "cursor", "overflow", "border", "borderRadius", "boxShadow",
    "transform", "transition", "flex", "flexDirection", "justifyContent",
    "alignItems", "flexWrap", "gap", "whiteSpace", "textDecoration",
    "float", "verticalAlign", "minWidth", "maxWidth", "minHeight", "maxHeight",
};

static ::JSValue styleGetProp(::JSContext *ctx, ::JSValueConst this_val, int magic)
{
    StyleWrap *w = unwrapStyle(ctx, this_val);
    if (!w || magic < 0 || magic >= (int)_countof(kStyleProps)) {
        return JS_UNDEFINED;
    }
    Element el(w->node);
    if (el.isNull()) return JS_UNDEFINED;
    CSSStyleDeclaration style = el.style();
    QString name = QString::fromLatin1(kStyleProps[magic]);
    QString v = dstr(style.getPropertyValue(mk(cssKebab(name))));
    return jsStr(ctx, v);
}

static ::JSValue styleSetProp(::JSContext *ctx, ::JSValueConst this_val,
                              ::JSValueConst val, int magic)
{
    StyleWrap *w = unwrapStyle(ctx, this_val);
    if (!w || magic < 0 || magic >= (int)_countof(kStyleProps)) {
        return JS_UNDEFINED;
    }
    Element el(w->node);
    if (el.isNull()) return JS_UNDEFINED;
    CSSStyleDeclaration style = el.style();
    QString name = QString::fromLatin1(kStyleProps[magic]);
    style.setProperty(mk(cssKebab(name)), mk(jsValStr(ctx, val)), DOM::DOMString());
    return JS_UNDEFINED;
}

static ::JSValue styleSetPropertyFn(::JSContext *ctx, ::JSValueConst this_val,
                                    int argc, ::JSValueConst *argv)
{
    StyleWrap *w = unwrapStyle(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    Element el(w->node);
    if (el.isNull()) return JS_UNDEFINED;
    CSSStyleDeclaration style = el.style();
    style.setProperty(mk(jsValStr(ctx, argv[0])), mk(jsValStr(ctx, argv[1])), DOM::DOMString());
    return JS_UNDEFINED;
}

static ::JSValue styleGetPropertyValueFn(::JSContext *ctx, ::JSValueConst this_val,
                                         int argc, ::JSValueConst *argv)
{
    StyleWrap *w = unwrapStyle(ctx, this_val);
    if (!w) return jsStr(ctx, QString());
    Element el(w->node);
    if (el.isNull()) return jsStr(ctx, QString());
    CSSStyleDeclaration style = el.style();
    return jsStr(ctx, dstr(style.getPropertyValue(mk(jsValStr(ctx, argv[0])))));
}

// ---------------------------------------------------------------------------
// document
// ---------------------------------------------------------------------------
static ::JSValue docGetElementById(::JSContext *ctx, ::JSValueConst this_val,
                                   int argc, ::JSValueConst *argv)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_NULL;
    }
    Document doc(w->node);
    return wrapNode(ctx, doc.getElementById(mk(jsValStr(ctx, argv[0]))));
}

static ::JSValue docQuerySelector(::JSContext *ctx, ::JSValueConst this_val,
                                  int argc, ::JSValueConst *argv)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_NULL;
    }
    Document doc(w->node);
    return wrapNode(ctx, doc.querySelector(mk(jsValStr(ctx, argv[0]))));
}

static ::JSValue docCreateElement(::JSContext *ctx, ::JSValueConst this_val,
                                  int argc, ::JSValueConst *argv)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_NULL;
    }
    Document doc(w->node);
    return wrapNode(ctx, doc.createElement(mk(jsValStr(ctx, argv[0]))));
}

static ::JSValue docGetProp(::JSContext *ctx, ::JSValueConst this_val, int magic)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_UNDEFINED;
    }
    HTMLDocument doc(w->node);
    switch (magic) {
    case 0: return wrapNode(ctx, doc.documentElement());   // documentElement
    case 1: return wrapNode(ctx, doc.body());              // body
    case 2: return jsStr(ctx, dstr(doc.title()));          // title
    }
    return JS_UNDEFINED;
}

static ::JSValue docSetProp(::JSContext *ctx, ::JSValueConst this_val,
                            ::JSValueConst val, int magic)
{
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        return JS_UNDEFINED;
    }
    HTMLDocument doc(w->node);
    if (magic == 2) {
        doc.setTitle(mk(jsValStr(ctx, val)));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Impl (PIMPL body)
// ---------------------------------------------------------------------------
struct QJsDomBridge::Impl
{
    ::JSRuntime *rt = nullptr;
    ::JSContext *ctx = nullptr;
    ::JSValue documentObj = JS_UNDEFINED;

    // Module kill-switches (env vars: KHTML_QJS_NODE=0 etc.)
    bool enableNode = true, enableElement = true, enableDocument = true;
    bool enableStyle = true, enableEvent = true;
    static bool flagOn(const char *name) {
        const char *v = getenv(name);
        return !v || v[0] != '0';
    }
    void *lastDocHandle = nullptr;

    void resetRuntime() {
        qjslog2("RUNTIME_RESET_BEGIN", rt, ctx);
        // Free all listener JSValues before tearing down the runtime they
        // belong to.
        for (auto it = g_qjsListeners.begin(); it != g_qjsListeners.end(); ++it) {
            for (const QjsListenerEntry &e : it.value()) {
                JS_FreeValue(ctx, e.func);
            }
        }
        g_qjsListeners.clear();

        if (ctx) {
            if (!JS_IsUndefined(documentObj)) {
                qjslog2("RUNTIME_RESET_FREE_DOCUMENTOBJ", (void*)JS_VALUE_GET_PTR(documentObj), nullptr);
                JS_FreeValue(ctx, documentObj);
                documentObj = JS_UNDEFINED;
            }
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt) { JS_FreeRuntime(rt); rt = nullptr; }
        rt = JS_NewRuntime();
        ctx = JS_NewContext(rt);
        if (rt) JS_SetMemoryLimit(rt, 64u * 1024 * 1024);
        qjslog2("RUNTIME_RESET_END", rt, ctx);
        buildProtos();
    }

    // Dispatch an event through QuickJS. First checks the listener table
    // (addEventListener callbacks); if handlerSource is non-empty, also
    // evaluates the inline handler code. Returns true if QuickJS handled
    // the event (KJS must NOT re-run it).
    bool dispatchEvent(DOM::EventImpl *event, DOM::NodeImpl *target,
                       const QString &handlerSource)
    {
        if (!ctx || !target) {
            qjslog2("DISPATCH_EVENT: null ctx or target", ctx, target);
            return false;
        }

        DOM::Node targetNode(target);
        QString evtType = event ? dstr(DOM::Event(event).type()) : QString();
        qjslog2("DISPATCH_EVENT_BEGIN", target, (void*)handlerSource.size());

        // Build event + target objects for the JS environment.
        ::JSValue eventObj = wrapEvent(ctx, event);
        ::JSValue targetObj = wrapNode(ctx, targetNode);

        ::JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__khtml_event", JS_DupValue(ctx, eventObj));
        if (JS_IsObject(targetObj)) {
            JS_SetPropertyStr(ctx, global, "__khtml_target", JS_DupValue(ctx, targetObj));
        }

        bool handled = false;

        // 1) Inline handler (onclick="...", oninput="...")
        if (!handlerSource.isEmpty()) {
            // Wrap inline code as a function called with (event) and `this`=target.
            QString wrapped = QStringLiteral(
                "(function(event){\n"
                "  %1\n"
                "})").arg(handlerSource);
            QByteArray code = wrapped.toUtf8();
            ::JSValue fnVal = JS_Eval(ctx, code.constData(), code.size(),
                                      "<event>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(fnVal) && JS_IsFunction(ctx, fnVal)) {
                ::JSValue argv[1] = { eventObj };
                ::JSValue thisVal = JS_IsObject(targetObj) ? targetObj : JS_UNDEFINED;
                ::JSValue r = JS_Call(ctx, fnVal, thisVal, 1, argv);
                if (!JS_IsException(r)) {
                    handled = true;
                } else {
                    qjslog("DISPATCH_EVENT: inline handler JS_Call threw exception");
                }
                JS_FreeValue(ctx, r);
            } else {
                qjslog("DISPATCH_EVENT: inline handler JS_Eval failed or not function");
            }
            if (!JS_IsException(fnVal)) JS_FreeValue(ctx, fnVal);
        }

        // 2) addEventListener callbacks from the listener table
        if (!evtType.isEmpty()) {
            quintptr key = reinterpret_cast<quintptr>(target);
            auto it = g_qjsListeners.find(key);
            if (it != g_qjsListeners.end()) {
                for (const QjsListenerEntry &e : it.value()) {
                    if (e.type != evtType) continue;
                    ::JSValue argv[1] = { eventObj };
                    ::JSValue thisVal = JS_IsObject(targetObj) ? targetObj : JS_UNDEFINED;
                    ::JSValue r = JS_Call(ctx, e.func, thisVal, 1, argv);
                    if (!JS_IsException(r)) {
                        handled = true;
                    }
                    JS_FreeValue(ctx, r);
                }
            }
        }

        JS_FreeValue(ctx, eventObj);
        if (JS_IsObject(targetObj)) JS_FreeValue(ctx, targetObj);
        JS_FreeValue(ctx, global);
        qjslog2("DISPATCH_EVENT_END", target, (void*)handled);
        return handled;
    }

    void addEventListener(DOM::NodeImpl *node, const QString &type,
                          const QString &handlerSource)
    {
        // Inline-handler registration: store source for later dispatchEvent.
        if (!node || type.isEmpty() || handlerSource.isEmpty()) return;
        if (!isSupportedEventType(type)) return;
        // Inline handlers are handled via handlerSource in dispatchEvent;
        // this entry point is for programmatic registration with source.
        Q_UNUSED(node);
    }

    bool hasListener(DOM::NodeImpl *node, const QString &type) const
    {
        if (!node) return false;
        quintptr key = reinterpret_cast<quintptr>(node);
        auto it = g_qjsListeners.find(key);
        if (it == g_qjsListeners.end()) return false;
        for (const QjsListenerEntry &e : it.value()) {
            if (e.type == type) return true;
        }
        return false;
    }

    // Register one getter/setter property with magic.
    void defCgetset(const char *name,
                    JSValue (*getter)(JSContext *, JSValueConst, int),
                    JSValue (*setter)(JSContext *, JSValueConst, JSValueConst, int),
                    int magic, ::JSValue proto)
    {
        ::JSValue getFn = JS_NewCFunction2(ctx, reinterpret_cast<::JSCFunction *>(getter),
                                           name, 0, ::JS_CFUNC_getter_magic, magic);
        ::JSValue setFn = JS_UNDEFINED;
        if (setter) {
            setFn = JS_NewCFunction2(ctx, reinterpret_cast<::JSCFunction *>(setter),
                                     name, 0, ::JS_CFUNC_setter_magic, magic);
        }
        ::JSAtom atom = JS_NewAtom(ctx, name);
        // JS_DefinePropertyGetSet consumes both getter and setter (they are
        // JSValue, not JSValueConst), so we must NOT free them here.
        JS_DefinePropertyGetSet(ctx, proto, atom, getFn, setFn, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    }

    // Register a plain method.
    void defFunc(const char *name, ::JSCFunction *fn, int argc, ::JSValue proto)
    {
        ::JSValue f = JS_NewCFunction(ctx, fn, name, argc);
        // JS_SetPropertyStr consumes val (JSValue, not JSValueConst).
        JS_SetPropertyStr(ctx, proto, name, f);
    }

    void buildProtos()
    {
        qjslog("bp: start");
        JS_NewClassID(&nodeClassId);
        JS_NewClass(rt, nodeClassId, &nodeClassDef);
        qjslog("bp: nodeclass");
        ::JSValue proto = JS_NewObject(ctx);
        if (enableNode) {
            defCgetset("nodeName", nodeGetProp, nullptr, 0, proto);
            defCgetset("parentNode", nodeGetProp, nullptr, 1, proto);
            defCgetset("firstChild", nodeGetProp, nullptr, 2, proto);
            defCgetset("lastChild", nodeGetProp, nullptr, 3, proto);
            defCgetset("nextSibling", nodeGetProp, nullptr, 4, proto);
            defCgetset("previousSibling", nodeGetProp, nullptr, 5, proto);
            defCgetset("childNodes", nodeGetProp, nullptr, 6, proto);
            defCgetset("nodeType", nodeGetProp, nullptr, 7, proto);
        }
        if (enableElement) {
            defCgetset("id", elGetProp, elSetProp, 100, proto);
        defCgetset("className", elGetProp, elSetProp, 101, proto);
        defCgetset("style", elGetProp, nullptr, 102, proto);
        defCgetset("textContent", elGetProp, elSetProp, 103, proto);
        defCgetset("tagName", elGetProp, nullptr, 104, proto);
        defCgetset("innerHTML", elGetProp, elSetProp, 105, proto);
        defFunc("setAttribute", elSetAttribute, 2, proto);
        defFunc("getAttribute", elGetAttribute, 1, proto);
        defFunc("appendChild", elAppendChild, 1, proto);
        }
        if (enableEvent) {
            defFunc("addEventListener", elAddEventListener, 2, proto);
            defFunc("removeEventListener", elAddEventListener, 2, proto);
        }
        if (enableDocument) {
            // document-level methods (available on every node; document is a node)
            defFunc("getElementById", docGetElementById, 1, proto);
            defFunc("querySelector", docQuerySelector, 1, proto);
            defFunc("createElement", docCreateElement, 1, proto);
            defCgetset("documentElement", docGetProp, nullptr, 0, proto);
            defCgetset("body", docGetProp, nullptr, 1, proto);
            defCgetset("title", docGetProp, docSetProp, 2, proto);
        }
        // JS_SetClassProto consumes proto (JSValue, not JSValueConst).
        JS_SetClassProto(ctx, nodeClassId, proto);
        qjslog("bp: nodeproto");

        JS_NewClassID(&styleClassId);
        JS_NewClass(rt, styleClassId, &styleClassDef);
        qjslog("bp: styleclass");
        ::JSValue sproto = JS_NewObject(ctx);
        if (enableStyle) {
            defFunc("setProperty", styleSetPropertyFn, 2, sproto);
            defFunc("getPropertyValue", styleGetPropertyValueFn, 1, sproto);
            for (size_t i = 0; i < _countof(kStyleProps); ++i) {
                defCgetset(kStyleProps[i], styleGetProp, styleSetProp, (int)i, sproto);
            }
        }
        JS_SetClassProto(ctx, styleClassId, sproto);
        qjslog("bp: styleproto");
    }

    Impl()
    {
        enableNode = flagOn("KHTML_QJS_NODE");
        enableElement = flagOn("KHTML_QJS_ELEMENT");
        enableDocument = flagOn("KHTML_QJS_DOCUMENT");
        enableStyle = flagOn("KHTML_QJS_STYLE");
        enableEvent = flagOn("KHTML_QJS_EVENT");
        qjslog("impl: rt");
        rt = JS_NewRuntime();
        qjslog2("RUNTIME_CREATE", rt, nullptr);
        qjslog("impl: ctx");
        ctx = JS_NewContext(rt);
        qjslog2("CONTEXT_CREATE", rt, ctx);
        qjslog("impl: limit");
        if (rt) {
            JS_SetMemoryLimit(rt, 64u * 1024 * 1024);
        }
        qjslog("impl: protos");
        buildProtos();
        qjslog("impl: done");
    }

    ~Impl()
    {
        qjslog2("IMPL_DESTRUCTOR_BEGIN", rt, ctx);
        if (ctx) {
            if (!JS_IsUndefined(documentObj)) {
                qjslog2("IMPL_DESTRUCTOR_FREE_DOCUMENTOBJ", (void*)JS_VALUE_GET_PTR(documentObj), nullptr);
                JS_FreeValue(ctx, documentObj);
                documentObj = JS_UNDEFINED;
            }
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt) {
            JS_FreeRuntime(rt);
            rt = nullptr;
        }
        qjslog("IMPL_DESTRUCTOR_END");
    }

    void setDocument(const DOM::Document &doc)
    {
        if (!ctx || doc.isNull())
            return;

        void *h = doc.handle();
        qjslog2("DOCUMENT_SET_BEGIN", h, lastDocHandle);

        // 同一 Document：直接复用当前 documentObj。
        if (h && h == lastDocHandle && JS_IsObject(documentObj)) {
            qjslog("DOCUMENT_SET_REUSE");
            return;
        }

        // 新 Document：销毁旧 QuickJS World。
        if (h && lastDocHandle && h != lastDocHandle) {
            lastDocHandle = h;
            resetRuntime();
        } else if (h) {
            // 第一次设置 Document。
            lastDocHandle = h;
        }

        // 当前 runtime 中只创建一次 document wrapper。
        documentObj = wrapNode(ctx, doc);
        qjslog2("DOCUMENT_SET_NEW", (void*)JS_VALUE_GET_PTR(documentObj), h);

        ::JSValue global = JS_GetGlobalObject(ctx);
        // documentObj 自己持有 1 个 reference。
        // global.document / global.window 各持有 1 个 duplicate reference。
        JS_SetPropertyStr(
            ctx,
            global,
            "document",
            JS_DupValue(ctx, documentObj)
        );
        JS_SetPropertyStr(
            ctx,
            global,
            "window",
            JS_DupValue(ctx, documentObj)
        );
        qjslog("GLOBAL_DOCUMENT_WINDOW_SET");
        JS_FreeValue(ctx, global);
        qjslog("DOCUMENT_SET_END");
    }

    QVariant evaluate(const QString &script, const QString &filename)
    {
        const QByteArray utf8 = script.toUtf8();
        const QByteArray fname = filename.toUtf8();
        qjslog2("EVAL_BEGIN", (void*)utf8.size(), (void*)fname.size());
        ::JSValue r = JS_Eval(ctx, utf8.constData(), utf8.size(),
                              fname.isEmpty() ? "<page>" : fname.constData(),
                              JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            ::JSValue exc = JS_GetException(ctx);
            qjslog("EVAL_EXCEPTION");
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, r);
            m_threw = true;
            return QVariant();
        }
        m_threw = false;
        QVariant result;
        if (JS_IsString(r)) {
            result = jsValStr(ctx, r);
        } else if (JS_IsNumber(r)) {
            double d = 0;
            JS_ToFloat64(ctx, &d, r);
            result = d;
        } else if (JS_IsBool(r)) {
            result = JS_ToBool(ctx, r);
        }
        JS_FreeValue(ctx, r);
        qjslog("EVAL_END");
        return result;
    }

    bool m_threw = false;
};

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
QJsDomBridge::QJsDomBridge()
    : m_d(new Impl())
{
    qjslog("QJsDomBridge ctor");
}

QJsDomBridge::~QJsDomBridge()
{
    delete m_d;
}

void QJsDomBridge::setDocument(const DOM::Document &doc)
{
    if (m_d) {
        m_d->setDocument(doc);
    }
}

QVariant QJsDomBridge::evaluate(const QString &script, const QString &filename,
                                const DOM::Document &doc)
{
    m_fallback = false;
    if (!m_d) {
        m_fallback = true;
        return QVariant();
    }
    m_d->setDocument(doc);
    QVariant r = m_d->evaluate(script, filename);
    if (m_d->m_threw) {
        m_fallback = true;
    }
    return r;
}

bool QJsDomBridge::dispatchEvent(DOM::EventImpl *event, DOM::NodeImpl *target,
                                 const QString &handlerSource)
{
    if (!m_d) return false;
    return m_d->dispatchEvent(event, target, handlerSource);
}

void QJsDomBridge::addEventListener(DOM::NodeImpl *node, const QString &type,
                                    const QString &handlerSource)
{
    if (m_d) m_d->addEventListener(node, type, handlerSource);
}

bool QJsDomBridge::hasListener(DOM::NodeImpl *node, const QString &type) const
{
    if (!m_d) return false;
    return m_d->hasListener(node, type);
}

} // namespace khtml
