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
 * unsupported case so the caller (ScriptProxy) can degrade gracefully too.
 *
 * All QuickJS types live inside Impl so this translation unit never leaks
 * the global JSValue typedef into unrelated headers.
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
#include <xml/dom_nodeimpl.h>
#include <xml/dom_docimpl.h>
#include <khtmlview.h>
#include "js_proxy.h"
#include <khtml_part.h>
#include <cstdio>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>

// Trace log for the QuickJS DOM bridge. OFF by default: the old
// unconditional fopen wrote khtml_qjs.log on EVERY evaluate/world reset —
// per-eval latency plus an ever-growing file on user machines.
// KHTML_QJS_TRACE=1 turns it back on for debugging.
static bool qjsTraceEnabled()
{
    static const bool on = [] { const char *v = getenv("KHTML_QJS_TRACE"); return v && v[0] == '1'; }();
    return on;
}

static void qjslog(const char *msg)
{
    if (!qjsTraceEnabled()) return;
    FILE *f = fopen("C:/Users/zhouzhehong/khtml_qjs.log", "a");
    if (f) { fprintf(f, "[%lld] %s\n", (long long)QDateTime::currentMSecsSinceEpoch(), msg); fclose(f); }
}

static void qjslog2(const char *msg, const void *p1, const void *p2)
{
    if (!qjsTraceEnabled()) return;
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
// to the legacy event path. Keyed by NodeImpl* identity + event type.
// ---------------------------------------------------------------------------
struct QjsListenerEntry {
    QString type;
    ::JSValue func;  // owned, DupValue'd on insert
};

static QHash<quintptr, QVector<QjsListenerEntry>> g_qjsListeners;
static ::JSRuntime *g_qjsListenerRt = nullptr;

// Tracks which (NodeImpl*, event-type) pairs already have a native C++
// EventListener shim installed, so we register the shim exactly once per pair.
// The shim bridges KHTML's native dispatch (handleLocalEvents) into the
// QuickJS listener table; without it, programmatic addEventListener('click',fn)
// callbacks never fire because g_qjsListeners is otherwise only walked from the
// inline onXXX attribute path.
static QHash<quintptr, QSet<QString>> g_qjsShimmed;

class QJSNodeListener : public DOM::EventListener
{
public:
    // The node captured at REGISTRATION is the lookup key for
    // g_qjsListeners.  For window-level events the shim lives on
    // WindowEventTargetImpl, which is not a NodeImpl: Event::currentTarget()
    // guards on eventTargetType() == DOM_NODE and hands back an EMPTY Node
    // there, so the previous implementation (which read currentTarget()) did
    // not crash — it silently returned without ever running the callback.
    // The registration node (the document, since window === the document
    // wrapper here) is the key the JS listener table uses in both cases.
    explicit QJSNodeListener(DOM::NodeImpl *lookupNode) : m_lookup(lookupNode) {}

    // Invoked by KHTML during native event dispatch on the target the shim
    // is registered on. Routes through ScriptProxy so the QuickJS runtime is
    // lazily brought up if needed, then walks g_qjsListeners for
    // m_lookup + type.
    void handleEvent(DOM::Event &evt) override
    {
        DOM::NodeImpl *target = m_lookup;
        if (!target || !target->document()) {
            return;
        }
        KHTMLPart *part = qobject_cast<KHTMLPart *>(target->document()->part());
        if (!part) {
            return;
        }
        ScriptProxy *proxy = ScriptProxy::proxy(part);
        if (proxy) {
            // Empty handlerSource: run ONLY the programmatic listener table.
            // The inline onXXX attribute path uses a non-empty handlerSource.
            proxy->dispatchEventToQuickJS(evt.handle(), target, QString());
        }
    }
    DOM::DOMString eventListenerType() override { return "_khtml_QJSNodeListener"; }
private:
    DOM::NodeImpl *m_lookup;
};

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

    // target: wrap the DOM node the event targeted (if it is a node target).
    if (EventTargetImpl *t = evt->target()) {
        if (t->eventTargetType() == EventTargetImpl::DOM_NODE) {
            DOM::Node targetNode(static_cast<NodeImpl *>(t));
            if (!targetNode.isNull()) {
                JS_SetPropertyStr(ctx, obj, "target", wrapNode(ctx, targetNode));
            }
        }
    }

    // Mouse geometry + button.
    if (evt->isMouseEvent()) {
        DOM::MouseEventImpl *me = static_cast<DOM::MouseEventImpl *>(evt);
        JS_SetPropertyStr(ctx, obj, "clientX", JS_NewInt32(ctx, (int)me->clientX()));
        JS_SetPropertyStr(ctx, obj, "clientY", JS_NewInt32(ctx, (int)me->clientY()));
        JS_SetPropertyStr(ctx, obj, "button", JS_NewInt32(ctx, (int)me->button()));
        JS_SetPropertyStr(ctx, obj, "which", JS_NewInt32(ctx, (int)me->which()));
    }

    // Keyboard: keyCode + human-readable key identifier.
    if (evt->isKeyboardEvent()) {
        DOM::KeyboardEventImpl *ke = static_cast<DOM::KeyboardEventImpl *>(evt);
        JS_SetPropertyStr(ctx, obj, "keyCode", JS_NewInt32(ctx, ke->keyCode()));
        JS_SetPropertyStr(ctx, obj, "key", jsStr(ctx, dstr(ke->keyIdentifier())));
    } else if (evt->isUIEvent()) {
        DOM::UIEventImpl *ue = static_cast<DOM::UIEventImpl *>(evt);
        JS_SetPropertyStr(ctx, obj, "keyCode", JS_NewInt32(ctx, ue->keyCode()));
    }
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
    // Store the JS callback in g_qjsListeners keyed by the node impl handle.
    // DOM event dispatch later walks this table and JS_Call()s the function
    // through QuickJS (see Impl::dispatchEvent), never via legacy dispatch.
    if (argc < 2) {
        return JS_UNDEFINED;
    }
    const QString type = jsValStr(ctx, argv[0]);
    if (type.isEmpty() || !JS_IsFunction(ctx, argv[1])) {
        return JS_UNDEFINED;
    }
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w || !w->node.handle()) {
        return JS_UNDEFINED;
    }
    const quintptr key = reinterpret_cast<quintptr>(w->node.handle());
    QjsListenerEntry e;
    e.type = type;
    e.func = JS_DupValue(ctx, argv[1]);
    g_qjsListeners[key].append(e);

    // Install a native C++ EventListener shim on this node exactly once per
    // (node, type) so KHTML's real dispatch path (handleLocalEvents) invokes
    // this table. Without it the programmatic callback only fires when an
    // inline onXXX attribute on the same node happens to dispatch.
    auto &shimmed = g_qjsShimmed[key];
    if (!shimmed.contains(type)) {
        shimmed.insert(type);
        NodeImpl *nodeImpl = w->node.handle();
        EventName evName = EventName::fromString(mk(type));
        // Events the engine dispatches with target = WindowEventTargetImpl
        // (NodeImpl::dispatchWindowEvent).  The dispatch chain then contains
        // ONLY that target (NodeImpl::dispatchGenericEvent prepends it and
        // never joins the ancestor chain for non-DOM_NODE targets), so a shim
        // registered on the DOCUMENT node never fires for them — pages'
        // window.addEventListener('resize', ...) silently did nothing.
        // Register the shim on the window event target instead;
        // QJSNodeListener still looks the callback up by the document node,
        // which is the key used above.
        //
        // This list must only contain events that really are window-targeted
        // in THIS tree: resize (khtmlview.cpp resizeEvent), load
        // (html_documentimpl.cpp / dom_docimpl.cpp), unload (khtml_part.cpp),
        // hashchange (khtml_part.cpp).  Names that have no static mapping in
        // EventName would be allocated a dynamic id that the engine never
        // dispatches, i.e. a listener that can never fire; do not add them.
        // Document-targeted bubbling events (DOMContentLoaded) must NOT be
        // listed: they reach window through the ancestor chain anyway and
        // would then fire twice.
        static const QSet<QString> kWindowOnlyEvents = {
            QStringLiteral("resize"), QStringLiteral("hashchange"),
            QStringLiteral("unload"), QStringLiteral("load"),
        };
        if (nodeImpl->nodeType() == Node::DOCUMENT_NODE
                && kWindowOnlyEvents.contains(type)) {
            DocumentImpl *di = static_cast<DocumentImpl *>(nodeImpl);
            di->windowEventTarget()->addEventListener(
                evName, new QJSNodeListener(nodeImpl), false);
        } else {
            nodeImpl->addEventListener(evName, new QJSNodeListener(nodeImpl), false);
        }
    }
    qjslog2("ADD_EVENT_LISTENER", (void *)key, nullptr);
    return JS_UNDEFINED;
}

static ::JSValue elRemoveEventListener(::JSContext *ctx, ::JSValueConst this_val,
                                      int argc, ::JSValueConst *argv)
{
    if (argc < 2) {
        return JS_UNDEFINED;
    }
    const QString type = jsValStr(ctx, argv[0]);
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w || !w->node.handle()) {
        return JS_UNDEFINED;
    }
    const quintptr key = reinterpret_cast<quintptr>(w->node.handle());
    auto it = g_qjsListeners.find(key);
    if (it == g_qjsListeners.end()) {
        return JS_UNDEFINED;
    }
    QVector<QjsListenerEntry> &v = it.value();
    for (int i = v.size() - 1; i >= 0; --i) {
        if (v[i].type != type) {
            continue;
        }
        if (JS_SameValue(ctx, v[i].func, argv[1])) {
            JS_FreeValue(ctx, v[i].func);
            v.removeAt(i);
        }
    }
    if (v.isEmpty()) {
        g_qjsListeners.erase(it);
    }
    qjslog2("REMOVE_EVENT_LISTENER", (void *)key, nullptr);
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
    // Damage tracing: does a "el.style.left = ..." write reach the bridge at
    // all, and does it resolve to a live element?  Qt-gated (the CRT getenv()
    // does not see the environment in these binaries).
    static const bool s_trace = qEnvironmentVariableIsSet("KHTML_DAMAGE_TRACE");
    StyleWrap *w = unwrapStyle(ctx, this_val);
    if (!w || magic < 0 || magic >= (int)_countof(kStyleProps)) {
        if (s_trace) {
            fprintf(stderr, "[css] styleSetProp REJECTED wrap=%p magic=%d\n",
                    (void *)w, magic);
            fflush(stderr);
        }
        return JS_UNDEFINED;
    }
    Element el(w->node);
    if (el.isNull()) {
        if (s_trace) {
            fprintf(stderr, "[css] styleSetProp NULL element magic=%d\n", magic);
            fflush(stderr);
        }
        return JS_UNDEFINED;
    }
    CSSStyleDeclaration style = el.style();
    QString name = QString::fromLatin1(kStyleProps[magic]);
    QString v = jsValStr(ctx, val);
    style.setProperty(mk(cssKebab(name)), mk(v), DOM::DOMString());
    if (s_trace) {
        fprintf(stderr, "[css] styleSetProp %s=%s\n",
                kStyleProps[magic], v.toUtf8().constData());
        fflush(stderr);
    }
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

static qint64 qjsTraceMs()
{
    static QElapsedTimer et;
    static bool started = [] { et.start(); return true; }();
    Q_UNUSED(started);
    return et.elapsed();
}

// ---------------------------------------------------------------------------
// document
// ---------------------------------------------------------------------------
// window.innerWidth / window.innerHeight / window.devicePixelRatio — the live
// viewport, exactly the WebCore semantics (visibleWidth/visibleHeight exclude
// the scrollbar area and track resize; they are CSS pixels because the
// renderer lays out at the logical size and rasters at dpr). window IS the
// document wrapper in this bridge, so these are getter properties on the
// document object. magic 0 = width, 1 = height, 2 = device pixel ratio.
static ::JSValue windowViewportGetter(::JSContext *ctx, ::JSValueConst this_val, int magic)
{
    // this_val is the wrapper when called as window.innerWidth, or the
    // global object for bare `innerWidth` (both are defined).  Primitives
    // (someone stole the receiver with .call(...)) have no wrapper at all.
    if (!JS_IsObject(this_val))
        return JS_UNDEFINED;
    NodeWrap *w = unwrapNode(ctx, this_val);
    if (!w) {
        // With the global as `this` there is no wrapper to unwrap: fetch
        // global.window, which is the document wrapper this bridge installs.
        // A page can redefine `window` as a throwing getter; QuickJS
        // requires such an exception to propagate, not to be swallowed.
        ::JSValue win = ::JS_GetPropertyStr(ctx, this_val, "window");
        if (JS_IsException(win))
            return JS_EXCEPTION;
        w = unwrapNode(ctx, win);
        ::JS_FreeValue(ctx, win);
        if (!w) return JS_UNDEFINED;
    }
    Document doc(w->node);
    KHTMLView *v = doc.view();
    if (!v) return JS_UNDEFINED;
    if (magic == 2) {
        KHTMLPart *part = v->part();
        return ::JS_NewFloat64(ctx, part ? part->devicePixelRatio() : 1.0);
    }
    return ::JS_NewInt32(ctx, magic == 0 ? v->visibleWidth() : v->visibleHeight());
}

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
        if (qjsTraceEnabled()) {
            KHTMLView *v = doc.view();
            fprintf(stderr, "[qjs title +%lld] set '%s' doc=%p view=%p parent=%d\n",
                    (long long)qjsTraceMs(),
                    jsValStr(ctx, val).left(24).toUtf8().constData(),
                    (void *)w->node.handle(), (void *)v,
                    (v && v->part() && v->part()->parentPart()) ? 1 : 0);
        }
        doc.setTitle(mk(jsValStr(ctx, val)));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// window timers: setTimeout / setInterval / clearTimeout / clearInterval /
// requestAnimationFrame / cancelAnimationFrame
// ---------------------------------------------------------------------------
// WebCore semantics on the Qt event loop of the host process.  Timer
// records hold duplicated JSValue references; the registry is wiped on
// every world teardown (resetRuntime / destructor), so a callback can
// never fire into a dead context.  requestAnimationFrame is a ~60 Hz
// single-shot alias (16 ms), exactly what the CSS animation polyfill
// needs; it was previously missing entirely, silently disabling every
// @keyframes polyfill injection.
struct QjsTimerRec {
    int id = 0;
    QTimer *t = nullptr;
    ::JSValue fn = JS_UNDEFINED;   // function form
    QString code;                  // setTimeout("code") string form
    bool isString = false;
    QVector<::JSValue> args;       // trailing args passed back to fn
    bool repeat = false;
};
static QHash<int, QjsTimerRec *> g_qjsTimers;    // id -> record
static QHash<QTimer *, int> g_qjsTimerIds;       // QObject -> id
static int g_qjsNextTimerId = 1;
static ::JSContext *g_qjsTimerCtx = nullptr;     // the world the timers live in

static void qjsTimerFreeRec(::JSContext *ctx, QjsTimerRec *rec)
{
    if (ctx && !rec->isString) {
        JS_FreeValue(ctx, rec->fn);
        for (::JSValue &a : rec->args)
            JS_FreeValue(ctx, a);
    }
    delete rec;
}

// Kill every pending timer.  Must run BEFORE the owning JSContext dies so
// the JSValue references can be freed on a live context.  Idempotent.
static void qjsTimerKillAll()
{
    if (qjsTraceEnabled())
        fprintf(stderr, "[qjs timer +%lld] killall n=%d\n", (long long)qjsTraceMs(), g_qjsTimers.size());
    for (auto it = g_qjsTimers.begin(); it != g_qjsTimers.end(); ++it) {
        QjsTimerRec *rec = it.value();
        if (rec->t) {
            rec->t->stop();
            delete rec->t;   // outside any timeout handler: direct delete is safe
        }
        qjsTimerFreeRec(g_qjsTimerCtx, rec);
    }
    g_qjsTimers.clear();
    g_qjsTimerIds.clear();
    g_qjsTimerCtx = nullptr;
}

// magic: 0 = setTimeout, 1 = setInterval, 2 = requestAnimationFrame
static ::JSValue jsWindowTimer(::JSContext *ctx, ::JSValueConst, int argc,
                               ::JSValueConst *argv, int magic)
{
    if (qjsTraceEnabled())
        fprintf(stderr, "[qjs timer +%lld] create magic=%d argc=%d ctx=%p live=%p\n",
                (long long)qjsTraceMs(), magic, argc, (void *)ctx, (void *)g_qjsTimerCtx);
    if (argc < 1 || !g_qjsTimerCtx || ctx != g_qjsTimerCtx)
        return ::JS_NewInt32(ctx, 0);
    QjsTimerRec *rec = new QjsTimerRec;
    rec->id = g_qjsNextTimerId++;
    rec->repeat = magic == 1;
    if (JS_IsString(argv[0])) {
        rec->isString = true;
        rec->code = jsValStr(ctx, argv[0]);
    } else {
        rec->fn = JS_DupValue(ctx, argv[0]);
    }
    for (int i = 2; i < argc; ++i)
        rec->args.append(JS_DupValue(ctx, argv[i]));
    int delay = 0;
    if (magic == 2) {
        delay = 16;                      // rAF: one frame cadence
    } else if (argc >= 2) {
        int32_t d = 0;
        if (JS_ToInt32(ctx, &d, argv[1])) {
            // Non-numeric delay: swallow the pending exception, default 0.
            ::JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
        delay = d > 0 ? d : 0;
    }
    QTimer *t = new QTimer;
    t->setSingleShot(!rec->repeat);
    t->setInterval(delay);
    rec->t = t;
    g_qjsTimers.insert(rec->id, rec);
    g_qjsTimerIds.insert(t, rec->id);
    // Context object = the timer itself: destroying it disconnects, so the
    // lambda can never run against a dead registry entry.
    QObject::connect(t, &QTimer::timeout, t, [t]() {
        const int id = g_qjsTimerIds.value(t, 0);
        QjsTimerRec *rec = g_qjsTimers.value(id, nullptr);
        if (qjsTraceEnabled())
            fprintf(stderr, "[qjs timer +%lld] fire id=%d rec=%p live=%p\n",
                    (long long)qjsTraceMs(), id, (void *)rec, (void *)g_qjsTimerCtx);
        if (!rec || !g_qjsTimerCtx)
            return;
        ::JSContext *ctx2 = g_qjsTimerCtx;
        if (!rec->repeat) {
            // Detach first: clearTimeout() / world reset during the callback
            // must not be able to dangle the record we are about to call.
            g_qjsTimers.remove(id);
            g_qjsTimerIds.remove(t);
        }
        ::JSValue r;
        if (rec->isString) {
            const QByteArray b = rec->code.toUtf8();
            r = JS_Eval(ctx2, b.constData(), b.size(), "<timer>", JS_EVAL_TYPE_GLOBAL);
        } else {
            r = JS_Call(ctx2, rec->fn, JS_UNDEFINED,
                        rec->args.size(), rec->args.data());
        }
        if (JS_IsException(r)) {
            ::JSValue e = JS_GetException(ctx2);
            if (qjsTraceEnabled()) {
                const char *s = JS_ToCString(ctx2, e);
                fprintf(stderr, "[qjs timer] callback exception: %s\n",
                        s ? s : "(null)");
                if (s) ::JS_FreeCString(ctx2, s);
            }
            JS_FreeValue(ctx2, e);
        } else if (qjsTraceEnabled()) {
            fprintf(stderr, "[qjs timer] callback ok id=%d\n", id);
        }
        JS_FreeValue(ctx2, r);

        // RENDERING UPDATE after a script task.
        //
        // A style write from a timer callback (el.style.left = ...) only marks
        // the document dirty: CSSStyleDeclarationImpl::setChanged() ->
        // NodeImpl::setChanged() -> DocumentImpl::setDocumentChanged() pushes
        // the document onto the static s_changedDocuments() queue.  That queue
        // is drained by DocumentImpl::updateDocumentsRendering(), whose only
        // KHTML call site is the END OF EVENT DISPATCH
        // (xml/dom_nodeimpl.cpp:527) plus script evaluation
        // (html/html_headimpl.cpp:592).  A setTimeout/setInterval/rAF callback
        // is neither, so the drain never ran for it.
        //
        // What the drain does (and therefore what was missing): recalcStyle()
        // for the dirty subtree and — when anything actually changed —
        // m_view->layout() (xml/dom_docimpl.cpp:1486-1494).  It does NOT
        // paint.  The missing piece was the LAYOUT: the render tree kept the
        // element at its old coordinates, so the next paint (and the damage
        // reported for it) described the old position.  Measured with
        // KHTML_DAMAGE_TRACE=1 (tests/rg_diag4.py): "[css] styleSetProp
        // left=600px" arrived from this bridge with NO "[setStyle]" after it,
        // while the identical write made from a resize event handler did
        // produce one — and once this call was added, the same timer write
        // produced "[setStyle] diff=2" plus damage for both the old and the
        // new rectangle, and the moved element painted correctly with no
        // window resize anywhere.
        //
        // HTML semantics: the "update the rendering" step runs at the end of
        // every task, not only after events.  This is the minimal equivalent;
        // the DOM API is untouched and an event-dispatched script still gets
        // its single update at the end of dispatch.  Drained with
        // takeFirst(), so a second drain (event dispatch) finds the queue
        // empty; recalcStyle() is additionally guarded by m_inStyleRecalc.
        DocumentImpl::updateDocumentsRendering();

        if (!rec->repeat) {
            qjsTimerFreeRec(ctx2, rec);
            t->deleteLater();            // we are inside its own handler
        }
    });
    t->start();
    if (qjsTraceEnabled())
        fprintf(stderr, "[qjs timer +%lld] started id=%d interval=%d active=%d\n",
                (long long)qjsTraceMs(), rec->id, t->interval(), (int)t->isActive());
    return ::JS_NewInt32(ctx, rec->id);
}

// clearTimeout() / clearInterval() / cancelAnimationFrame() share one body.
static ::JSValue jsWindowClearTimer(::JSContext *ctx, ::JSValueConst,
                                    int argc, ::JSValueConst *argv, int)
{
    int32_t id = 0;
    if (argc >= 1) {
        if (JS_ToInt32(ctx, &id, argv[0])) {
            // Non-numeric id: swallow the pending exception, id stays 0.
            ::JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
    }
    QjsTimerRec *rec = g_qjsTimers.value(id, nullptr);
    if (rec) {
        if (rec->t) {
            g_qjsTimerIds.remove(rec->t);
            rec->t->stop();
            rec->t->deleteLater();
        }
        g_qjsTimers.remove(id);
        qjsTimerFreeRec(g_qjsTimerCtx, rec);
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
        // Pending timers reference JSValues of the outgoing world — kill
        // them (and their JSValue refs) on the still-live context first.
        qjsTimerKillAll();
        // Free all listener JSValues before tearing down the runtime they
        // belong to.
        for (auto it = g_qjsListeners.begin(); it != g_qjsListeners.end(); ++it) {
            for (const QjsListenerEntry &e : it.value()) {
                JS_FreeValue(ctx, e.func);
            }
        }
        g_qjsListeners.clear();
        g_qjsShimmed.clear();

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
        g_qjsListenerRt = rt;
        ctx = JS_NewContext(rt);
        if (rt) JS_SetMemoryLimit(rt, 64u * 1024 * 1024);
        qjslog2("RUNTIME_RESET_END", rt, ctx);
        buildProtos();
    }

    // Dispatch an event through QuickJS. First checks the listener table
    // (addEventListener callbacks); if handlerSource is non-empty, also
    // evaluates the inline handler code. Returns true if QuickJS handled
    // the event (legacy dispatch must NOT re-run it).
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

        // 2) addEventListener callbacks from the listener table.
        //    This is walked ONLY on the QJSNodeListener shim path (empty
        //    handlerSource). The inline onXXX path (non-empty handlerSource)
        //    relies on the shim to dispatch programmatic callbacks, so the
        //    table must not be iterated here too — that would double-fire.
        if (handlerSource.isEmpty() && !evtType.isEmpty()) {
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
            defFunc("removeEventListener", elRemoveEventListener, 2, proto);
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
        if (qjsTraceEnabled()) fprintf(stderr, "[qjs +%lld] IMPL_CTOR\n", (long long)qjsTraceMs());
        enableNode = flagOn("KHTML_QJS_NODE");
        enableElement = flagOn("KHTML_QJS_ELEMENT");
        enableDocument = flagOn("KHTML_QJS_DOCUMENT");
        enableStyle = flagOn("KHTML_QJS_STYLE");
        enableEvent = flagOn("KHTML_QJS_EVENT");
        qjslog("impl: rt");
        rt = JS_NewRuntime();
        g_qjsListenerRt = rt;
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
        if (qjsTraceEnabled()) fprintf(stderr, "[qjs +%lld] IMPL_DTOR\n", (long long)qjsTraceMs());
        qjslog2("IMPL_DESTRUCTOR_BEGIN", rt, ctx);
        // Pending timers reference JSValues of this world — they must die
        // with it, never after.
        qjsTimerKillAll();
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
        // window.document / window.window: same duplicates on the WRAPPER
        // itself.  Browsers have window === globalThis, so both spellings
        // exist on the window object; real pages write
        // window.document.getElementById(...) far more often than the bare
        // form, and a missing own property there returned `undefined` ->
        // TypeError, killing the rest of the <script>.
        // window.window === window is a self-reference, exactly as in the
        // DOM.  Both die with the world (resetRuntime frees the context),
        // so the extra two references are not a leak.
        JS_SetPropertyStr(
            ctx,
            documentObj,
            "document",
            JS_DupValue(ctx, documentObj)
        );
        JS_SetPropertyStr(
            ctx,
            documentObj,
            "window",
            JS_DupValue(ctx, documentObj)
        );
        // window.innerWidth / innerHeight / devicePixelRatio (magic 0/1/2)
        // — live getters so resize is reflected without re-instantiation.
        // CSS-px viewport: innerWidth is the logical size and
        // devicePixelRatio the raster scale (Safari semantics for a web
        // process asked to lay out at CSS pixels).
        // Installed on BOTH the window wrapper and the global object: browsers
        // have `window === globalThis`, so pages legitimately read bare
        // `innerWidth` / `devicePixelRatio` (e.g. inside a setTimeout
        // callback).  Only defining them on the wrapper made those throw
        // "ReferenceError: 'innerWidth' is not defined".
        const ::JSValue viewportTargets[2] = { documentObj, global };
        for (const ::JSValue &target : viewportTargets) {
            defCgetset("innerWidth", windowViewportGetter, nullptr, 0, target);
            defCgetset("innerHeight", windowViewportGetter, nullptr, 1, target);
            defCgetset("devicePixelRatio", windowViewportGetter, nullptr, 2, target);
        }

        // window timers (setTimeout / setInterval / clearTimeout /
        // clearInterval / requestAnimationFrame / cancelAnimationFrame):
        // installed on BOTH the global object and the window wrapper, so
        // bare setTimeout(...) and window.setTimeout(...) both resolve.
        // This is what makes @keyframes CSS animation polyfills (which
        // hard-require requestAnimationFrame) actually run.
        {
            static const bool s_timersOn = [] {
                const char *v = getenv("KHTML_QJS_TIMERS");
                return !v || v[0] != '0';
            }();
            if (s_timersOn) {
                g_qjsTimerCtx = ctx;
                auto defTimer = [&](const char *name, int magic, ::JSValue target) {
                    ::JSValue f = JS_NewCFunction2(ctx,
                        reinterpret_cast<::JSCFunction *>(&jsWindowTimer),
                        name, 3, ::JS_CFUNC_generic_magic, magic);
                    JS_SetPropertyStr(ctx, target, name, f);
                };
                const ::JSValue targets[2] = { global, documentObj };
                for (const ::JSValue &target : targets) {
                    defTimer("setTimeout", 0, target);
                    defTimer("setInterval", 1, target);
                    defTimer("requestAnimationFrame", 2, target);
                    ::JSValue cf = JS_NewCFunction2(ctx,
                        reinterpret_cast<::JSCFunction *>(&jsWindowClearTimer),
                        "clearTimeout", 1, ::JS_CFUNC_generic_magic, 0);
                    JS_SetPropertyStr(ctx, target, "clearTimeout", cf);
                    cf = JS_NewCFunction2(ctx,
                        reinterpret_cast<::JSCFunction *>(&jsWindowClearTimer),
                        "clearInterval", 1, ::JS_CFUNC_generic_magic, 0);
                    JS_SetPropertyStr(ctx, target, "clearInterval", cf);
                    cf = JS_NewCFunction2(ctx,
                        reinterpret_cast<::JSCFunction *>(&jsWindowClearTimer),
                        "cancelAnimationFrame", 1, ::JS_CFUNC_generic_magic, 0);
                    JS_SetPropertyStr(ctx, target, "cancelAnimationFrame", cf);
                }
            }
        }
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
