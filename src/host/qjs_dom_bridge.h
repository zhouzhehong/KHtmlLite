// qjs_dom_bridge.h — QuickJS to KHTML DOM bridge.
//
// KHTML's own JS engine (KJS) is a 2010-era interpreter that crashes on
// modern scripts. This bridge lets QuickJS (a 2020-era engine, supports
// ES2020) drive KHTML's DOM tree directly via the DOM:: API. Architecture
// mirrors WebKit's WebCore+JSC split: KHTML renders, QuickJS runs JS.
#ifndef QJS_DOM_BRIDGE_H
#define QJS_DOM_BRIDGE_H

#include <quickjs.h>
#include <QObject>
#include <QHash>
#include <QList>

#include <dom/dom_node.h>
#include <dom/dom_doc.h>
#include <dom/dom_element.h>
#include <dom/css_value.h>

class QTimer;

namespace QJSBridge
{

// Owning runtime + context for one tab. Also holds the QTimer objects
// backing setTimeout/setInterval so they live as long as the runtime.
class JsEngine
{
public:
    JsEngine();
    ~JsEngine();

    bool eval(const QString &script, QString *errorOut = nullptr);
    // Run all queued microtasks/pending timers? No-op placeholder.
    void tick() {}

    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    // DOM root document wrapper created on demand.
    JSValue domRoot(const DOM::Document &doc);
    // Clear document wrapper on navigation (does not destroy runtime).
    void clearDocument();
    // Set current document; clears previous wrapper first.
    void setDocument(const DOM::Document &doc);
    // Timers we own (kept alive by this object).
    QList<QTimer *> timers;

private:
    JSValue m_domRoot = JS_UNDEFINED;
    DOM::Document m_doc;
};

// Convert DOM::DOMString <-> JS string helpers.
JSValue toJsString(JSContext *ctx, const DOM::DOMString &s);
QString fromJsString(JSContext *ctx, JSValue v);

} // namespace QJSBridge

#endif
