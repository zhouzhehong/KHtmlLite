// qjs_dom_bridge.cpp 鈥?QuickJS to KHTML DOM bridge.
//
// Replaces the KJS interpreter for page scripts. KHTML still does the
// rendering (HTML/CSS/layout); QuickJS (ES2020) executes the page's JS and
// manipulates the live DOM through the DOM:: API. The bridge is deliberately
// small: enough surface for modern-but-not-exotic pages, with graceful
// degradation (missing APIs read as undefined) instead of crashing.
#include "qjs_dom_bridge.h"
#include <QTimer>
#include <QDateTime>
#include <QDebug>
#include <cstring>
#include <windows.h>

#include <dom/dom_node.h>
#include <dom/dom_core.h>
#include <dom/dom_doc.h>
#include <dom/dom_element.h>
#include <dom/css_value.h>
#include <dom/html_element.h>
#include <dom/html_document.h>
#include <dom/html_form.h>
#include <dom/dom_string.h>

// QuickJS C API
extern "C" {
#include "quickjs.h"
}

using namespace DOM;

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

static JSValue jsStr(JSContext *ctx, const QString &s)
{
    const QByteArray b = s.toUtf8();
    return JS_NewStringLen(ctx, b.constData(), b.size());
}

static QString jsValStr(JSContext *ctx, JSValueConst v)
{
    size_t len = 0;
    const char *p = JS_ToCStringLen(ctx, &len, v);
    if (!p) return QString();
    QString s = QString::fromUtf8(p, int(len));
    JS_FreeCString(ctx, p);
    return s;
}

// ---------------------------------------------------------------------------
// wrapper: JS object <-> DOM::Node
// ---------------------------------------------------------------------------
static JSClassID nodeClassId;

struct NodeWrapper {
    Node node;
};

static JSClassDef nodeClassDef = {
    "KHTMLNode",
    [](JSRuntime *rt, JSValue val) {
        NodeWrapper *w = static_cast<NodeWrapper *>(JS_GetOpaque(val, nodeClassId));
        if (w) delete w;
    },
};

static JSValue wrapNode(JSContext *ctx, const Node &n)
{
    if (n.isNull())
        return JS_NULL;
    JSValue proto = JS_GetClassProto(ctx, nodeClassId);
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, nodeClassId);
    JS_FreeValue(ctx, proto);
    NodeWrapper *w = new NodeWrapper;
    w->node = n;
    JS_SetOpaque(obj, w);
    return obj;
}

static NodeWrapper *unwrap(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v)) return nullptr;
    return static_cast<NodeWrapper *>(JS_GetOpaque(v, nodeClassId));
}

// style object wrapper: element.style.<prop>
struct StyleWrap {
    CSSStyleDeclaration style;
};
static JSClassID styleClassId;
static JSClassDef styleClassDef = {
    "KHTMLStyle",
    [](JSRuntime *rt, JSValue val) {
        StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(val, styleClassId));
        if (w) delete w;
    },
};

static JSValue wrapStyle(JSContext *ctx, const CSSStyleDeclaration &s)
{
    JSValue proto = JS_GetClassProto(ctx, styleClassId);
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, styleClassId);
    JS_FreeValue(ctx, proto);
    StyleWrap *w = new StyleWrap;
    w->style = s;
    JS_SetOpaque(obj, w);
    return obj;
}

// ---------------------------------------------------------------------------
// property / method tables on the node prototype
// ---------------------------------------------------------------------------
static JSValue nodeGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    const Node n = w->node;
    switch (magic) {
    case 0: return jsStr(ctx, dstr(n.nodeName()));        // nodeName
    case 1: return jsStr(ctx, dstr(n.nodeValue()));       // nodeValue
    case 2: return wrapNode(ctx, n.parentNode());         // parentNode
    case 3: return wrapNode(ctx, n.firstChild());         // firstChild
    case 4: return wrapNode(ctx, n.lastChild());          // lastChild
    case 5: return wrapNode(ctx, n.nextSibling());        // nextSibling
    case 6: return wrapNode(ctx, n.previousSibling());    // previousSibling
    case 7: return jsStr(ctx, dstr(n.nodeName()));        // tagName
    case 8: { // childNodes
        const NodeList list = n.childNodes();
        const unsigned long cnt = list.length();
        JSValue arr = JS_NewArray(ctx);
        for (unsigned long i = 0; i < cnt; i++) {
            JSValue child = wrapNode(ctx, list.item(i));
            if (JS_SetPropertyUint32(ctx, arr, i, child) < 0)
                JS_FreeValue(ctx, child);
        }
        return arr;
    }
    case 9: return JS_NewInt32(ctx, (int)n.nodeType());   // nodeType
    case 10: { // ownerDocument
        Document d = n.ownerDocument();
        if (d.isNull()) return JS_NULL;
        return wrapNode(ctx, d);
    }
    }
    return JS_UNDEFINED;
}

static JSValue nodeSetProp(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    switch (magic) {
    case 1: w->node.setNodeValue(mk(jsValStr(ctx, val))); break; // nodeValue
    }
    return JS_UNDEFINED;
}

static JSValue nodeAppendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *self = unwrap(ctx, this_val);
    NodeWrapper *child = unwrap(ctx, argv[0]);
    if (!self || !child) return JS_EXCEPTION;
    Node r = self->node.appendChild(child->node);
    return wrapNode(ctx, r);
}

static JSValue nodeRemoveChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *self = unwrap(ctx, this_val);
    NodeWrapper *child = unwrap(ctx, argv[0]);
    if (!self || !child) return JS_EXCEPTION;
    Node r = self->node.removeChild(child->node);
    return wrapNode(ctx, r);
}

static JSValue nodeInsertBefore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *self = unwrap(ctx, this_val);
    NodeWrapper *child = unwrap(ctx, argv[0]);
    if (!self || !child) return JS_EXCEPTION;
    Node r = self->node.insertBefore(child->node, Node());
    return wrapNode(ctx, r);
}

// element-specific ----------------------------------------------------------
static bool nodeIsElement(const Node &n)
{
    return n.nodeType() == 1; // ELEMENT_NODE
}

static JSValue elGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w || !nodeIsElement(w->node)) return JS_UNDEFINED;
    Element el(w->node);
    switch (magic) {
    case 0: return jsStr(ctx, dstr(el.getAttribute(mk("id"))));      // id
    case 1: return jsStr(ctx, dstr(el.getAttribute(mk("class"))));   // className
    case 2: return wrapStyle(ctx, el.style());                        // style
    case 3: return jsStr(ctx, dstr(el.textContent()));                // textContent
    case 4: { // innerHTML (HTML elements only)
        HTMLElement he(w->node);
        if (!he.isNull()) return jsStr(ctx, dstr(he.innerHTML()));
        return jsStr(ctx, QString());
    }
    case 5: return jsStr(ctx, dstr(el.getAttribute(mk("value"))));    // value
    case 6: return jsStr(ctx, dstr(el.tagName()));                    // tagName
    }
    return JS_UNDEFINED;
}

static JSValue elSetProp(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w || !nodeIsElement(w->node)) return JS_UNDEFINED;
    Element el(w->node);
    const QString v = jsValStr(ctx, val);
    switch (magic) {
    case 0: el.setAttribute(mk("id"), mk(v)); break;                       // id
    case 1: el.setAttribute(mk("class"), mk(v)); break;                     // className
    case 3: el.setTextContent(mk(v)); break;                                // textContent
    case 4: { // innerHTML (HTML elements only)
        HTMLElement he(w->node);
        if (!he.isNull()) he.setInnerHTML(mk(v));
        break;
    }
    case 5: el.setAttribute(mk("value"), mk(v)); break;                     // value
    }
    return JS_UNDEFINED;
}

static JSValue elSetAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    Element el(w->node);
    el.setAttribute(mk(jsValStr(ctx, argv[0])), mk(jsValStr(ctx, argv[1])));
    return JS_UNDEFINED;
}

static JSValue elGetAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    Element el(w->node);
    DOM::DOMString v = el.getAttribute(mk(jsValStr(ctx, argv[0])));
    if (v.isNull()) return JS_NULL;
    return jsStr(ctx, dstr(v));
}

static JSValue elRemoveAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    Element el(w->node);
    el.removeAttribute(mk(jsValStr(ctx, argv[0])));
    return JS_UNDEFINED;
}

static JSValue elAddEventListener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    // No-op for now: we record nothing. Prevents crashes on sites that bind
    // handlers early; DOM events are wired later.
    return JS_UNDEFINED;
}

// document ------------------------------------------------------------------
static JSValue docGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    HTMLDocument doc(w->node);
    switch (magic) {
    case 0: return wrapNode(ctx, doc.documentElement());   // documentElement
    case 1: return wrapNode(ctx, doc.body());              // body
    case 2: return jsStr(ctx, dstr(doc.title()));          // title
    }
    return JS_UNDEFINED;
}

static JSValue docSetProp(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    HTMLDocument doc(w->node);
    if (magic == 2)
        doc.setTitle(mk(jsValStr(ctx, val)));              // title
    return JS_UNDEFINED;
}

static JSValue docGetElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_NULL;
    Document doc(w->node);
    return wrapNode(ctx, doc.getElementById(mk(jsValStr(ctx, argv[0]))));
}

static JSValue docQuerySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_NULL;
    Document doc(w->node);
    return wrapNode(ctx, doc.querySelector(mk(jsValStr(ctx, argv[0]))));
}

static JSValue docQuerySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_UNDEFINED;
    Document doc(w->node);
    NodeList list = doc.querySelectorAll(mk(jsValStr(ctx, argv[0])));
    const unsigned long cnt = list.length();
    JSValue arr = JS_NewArray(ctx);
    for (unsigned long i = 0; i < cnt; i++) {
        JSValue child = wrapNode(ctx, list.item(i));
        if (JS_SetPropertyUint32(ctx, arr, i, child) < 0)
            JS_FreeValue(ctx, child);
    }
    return arr;
}

static JSValue docCreateElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_NULL;
    Document doc(w->node);
    return wrapNode(ctx, doc.createElement(mk(jsValStr(ctx, argv[0]))));
}

static JSValue docCreateTextNode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    NodeWrapper *w = unwrap(ctx, this_val);
    if (!w) return JS_NULL;
    Document doc(w->node);
    return wrapNode(ctx, doc.createTextNode(mk(jsValStr(ctx, argv[0]))));
}

// style object ---------------------------------------------------------------
static JSValue styleGetProp(JSContext *ctx, JSValueConst this_val, JSAtom prop)
{
    StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(this_val, styleClassId));
    if (!w || w->style.isNull()) return JS_UNDEFINED;
    const char *pname = JS_AtomToCString(ctx, prop);
    if (!pname) return JS_UNDEFINED;
    QString name(pname);
    JS_FreeCString(ctx, pname);
    // camelCase -> kebab-case for CSS property lookup
    QString kebab;
    for (int i = 0; i < name.size(); i++) {
        const QChar c = name.at(i);
        if (c.isUpper()) { kebab += QLatin1Char('-'); kebab += c.toLower(); }
        else kebab += c;
    }
    DOM::DOMString v = w->style.getPropertyValue(mk(kebab));
    return jsStr(ctx, dstr(v));
}

static JSValue styleSetProp(JSContext *ctx, JSValueConst this_val, JSAtom prop, JSValueConst val)
{
    StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(this_val, styleClassId));
    if (!w || w->style.isNull()) return JS_UNDEFINED;
    const char *pname = JS_AtomToCString(ctx, prop);
    if (!pname) return JS_UNDEFINED;
    QString name(pname);
    JS_FreeCString(ctx, pname);
    QString kebab;
    for (int i = 0; i < name.size(); i++) {
        const QChar c = name.at(i);
        if (c.isUpper()) { kebab += QLatin1Char('-'); kebab += c.toLower(); }
        else kebab += c;
    }
    w->style.setProperty(mk(kebab), mk(jsValStr(ctx, val)), DOM::DOMString());
    return JS_UNDEFINED;
}

static JSValue styleSetProperty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(this_val, styleClassId));
    if (!w || w->style.isNull()) return JS_UNDEFINED;
    w->style.setProperty(mk(jsValStr(ctx, argv[0])), mk(jsValStr(ctx, argv[1])), DOM::DOMString());
    return JS_UNDEFINED;
}

static JSValue styleGetPropertyValue(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StyleWrap *w = static_cast<StyleWrap *>(JS_GetOpaque(this_val, styleClassId));
    if (!w || w->style.isNull()) return jsStr(ctx, QString());
    return jsStr(ctx, dstr(w->style.getPropertyValue(mk(jsValStr(ctx, argv[0])))));
}

// ---------------------------------------------------------------------------
// module init
// ---------------------------------------------------------------------------
static const JSCFunctionListEntry nodeProtoFuncs[] = {
    JS_CFUNC_DEF("appendChild", 1, nodeAppendChild),
    JS_CFUNC_DEF("removeChild", 1, nodeRemoveChild),
    JS_CFUNC_DEF("insertBefore", 2, nodeInsertBefore),
    JS_CFUNC_DEF("setAttribute", 2, elSetAttribute),
    JS_CFUNC_DEF("getAttribute", 1, elGetAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, elRemoveAttribute),
    JS_CFUNC_DEF("addEventListener", 2, elAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, elAddEventListener),
    JS_PROP_STRING_DEF("nodeName", "", 0),
    JS_PROP_STRING_DEF("tagName", "", 0),
};

// property getters/setters via magic
static const JSCFunctionListEntry nodeGetProps[] = {
    JS_CGETSET_MAGIC_DEF("nodeName", nodeGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("nodeValue", nodeGetProp, nodeSetProp, 1),
    JS_CGETSET_MAGIC_DEF("parentNode", nodeGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("firstChild", nodeGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("lastChild", nodeGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("nextSibling", nodeGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("previousSibling", nodeGetProp, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("tagName", nodeGetProp, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("childNodes", nodeGetProp, nullptr, 8),
    JS_CGETSET_MAGIC_DEF("nodeType", nodeGetProp, nullptr, 9),
    JS_CGETSET_MAGIC_DEF("ownerDocument", nodeGetProp, nullptr, 10),
};

// element getters (extend node class)
static const JSCFunctionListEntry elGetProps[] = {
    JS_CGETSET_MAGIC_DEF("id", elGetProp, elSetProp, 0),
    JS_CGETSET_MAGIC_DEF("className", elGetProp, elSetProp, 1),
    JS_CGETSET_MAGIC_DEF("style", elGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("textContent", elGetProp, elSetProp, 3),
    JS_CGETSET_MAGIC_DEF("innerHTML", elGetProp, elSetProp, 4),
    JS_CGETSET_MAGIC_DEF("value", elGetProp, elSetProp, 5),
    JS_CGETSET_MAGIC_DEF("tagName", elGetProp, nullptr, 6),
};

static const JSCFunctionListEntry docFuncs[] = {
    JS_CFUNC_DEF("getElementById", 1, docGetElementById),
    JS_CFUNC_DEF("querySelector", 1, docQuerySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, docQuerySelectorAll),
    JS_CFUNC_DEF("createElement", 1, docCreateElement),
    JS_CFUNC_DEF("createTextNode", 1, docCreateTextNode),
    JS_CGETSET_MAGIC_DEF("documentElement", docGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("body", docGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("title", docGetProp, docSetProp, 2),
};

static const JSCFunctionListEntry styleFuncs[] = {
    JS_CFUNC_DEF("setProperty", 2, styleSetProperty),
    JS_CFUNC_DEF("getPropertyValue", 1, styleGetPropertyValue),
};

// ---------------------------------------------------------------------------
// JsEngine
// ---------------------------------------------------------------------------
namespace QJSBridge
{

JsEngine::JsEngine()
{
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    // register node class
    JS_NewClassID(&nodeClassId);
    JS_NewClass(rt, nodeClassId, &nodeClassDef);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, nodeGetProps, _countof(nodeGetProps));
    JS_SetPropertyFunctionList(ctx, proto, nodeProtoFuncs, _countof(nodeProtoFuncs));
    JS_SetClassProto(ctx, nodeClassId, proto);
    JS_FreeValue(ctx, proto);

    // style class
    JS_NewClassID(&styleClassId);
    JS_NewClass(rt, styleClassId, &styleClassDef);
    JSValue sproto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sproto, styleFuncs, _countof(styleFuncs));
    JS_SetClassProto(ctx, styleClassId, sproto);
    JS_FreeValue(ctx, sproto);
}

JsEngine::~JsEngine()
{
    clearDocument();

    if (ctx) {
        JS_FreeContext(ctx);
        ctx = nullptr;
    }

    if (rt) {
        JS_FreeRuntime(rt);
        rt = nullptr;
    }

    qDeleteAll(timers);
    timers.clear();
}

void JsEngine::clearDocument()
{
    if (JS_IsObject(m_domRoot))
        JS_FreeValue(ctx, m_domRoot);

    m_domRoot = JS_UNDEFINED;
    m_doc = DOM::Document();
}

void JsEngine::setDocument(const DOM::Document &doc)
{
    clearDocument();
    m_doc = doc;

    if (!m_doc.isNull())
        m_domRoot = wrapNode(ctx, m_doc);
}

JSValue JsEngine::domRoot(const DOM::Document &doc)
{
    if (!m_doc.isNull() && !doc.isNull() &&
        m_doc.handle() == doc.handle() && JS_IsObject(m_domRoot))
        return JS_DupValue(ctx, m_domRoot);

    if (!doc.isNull())
        setDocument(doc);

    if (!JS_IsObject(m_domRoot))
        return JS_NULL;

    return JS_DupValue(ctx, m_domRoot);
}

bool JsEngine::eval(const QString &script, QString *errorOut)
{
    JSValue r = JS_Eval(ctx, script.toUtf8().constData(), script.toUtf8().size(),
                        "<page>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        if (errorOut) *errorOut = jsValStr(ctx, exc);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, r);
        return false;
    }
    JS_FreeValue(ctx, r);
    return true;
}

} // namespace QJSBridge
