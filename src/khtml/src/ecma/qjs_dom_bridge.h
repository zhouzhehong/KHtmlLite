/* qjs_dom_bridge.h - QuickJS to KHTML DOM bridge (engine-internal).
 *
 * Minimal-but-sufficient DOM surface so page scripts can run on the
 * embedded QuickJS engine (ES2020) instead of the 2010-era KJS.
 * The KJSProxy::evaluate() entry prefers this engine and falls back to
 * KJS when a script references a DOM API the bridge does not expose.
 *
 * This header deliberately does NOT include quickjs.h: KJS also exports
 * a type named JSValue, so pulling QuickJS's typedef into translation
 * units that use KJS would be ambiguous. All QuickJS types are hidden
 * behind the PIMPL pointer below.
 */
#ifndef KHTML_QJS_DOM_BRIDGE_H
#define KHTML_QJS_DOM_BRIDGE_H

#include <QString>
#include <QVariant>

namespace DOM
{
class Document;
class Node;
class NodeImpl;
class Event;
class EventImpl;
}

namespace khtml
{

class QJsDomBridge
{
public:
    QJsDomBridge();
    ~QJsDomBridge();

    // Evaluate one script. DOM is reachable through the global `document`
    // object. Returns the JS result serialized as a variant when possible.
    QVariant evaluate(const QString &script, const QString &filename,
                      const DOM::Document &doc);

    // Re-seed `document` for a new page load.
    void setDocument(const DOM::Document &doc);

    // True when the last evaluate() hit a script that referenced an API
    // we do not support (caller should fall back to KJS).
    bool needsFallback() const { return m_fallback; }

    // Dispatch a DOM event to a QuickJS handler. Returns true if QuickJS
    // handled the event (KJS must NOT re-run it). False means caller should
    // fall back to KJS event execution.
    bool dispatchEvent(DOM::EventImpl *event, DOM::NodeImpl *target,
                       const QString &handlerSource);

    // Register a QuickJS-side event listener for a node. Used by the
    // addEventListener bridge so JS callbacks live in QuickJS, not KJS.
    void addEventListener(DOM::NodeImpl *node, const QString &type,
                          const QString &handlerSource);

    // Check if QuickJS has a listener for this node+type.
    bool hasListener(DOM::NodeImpl *node, const QString &type) const;

private:
    struct Impl;
    Impl *m_d = nullptr;
    bool m_fallback = false;
};

} // namespace khtml

#endif
