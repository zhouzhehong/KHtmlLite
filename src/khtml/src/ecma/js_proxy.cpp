/*
 *  This file is part of the KDE libraries
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001,2003 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2001-2003 David Faure (faure@kde.org)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "js_proxy.h"

#include "qjs_dom_bridge.h"
#include <cstdio>
// Trace log for the QuickJS execution path. OFF by default: the old
// unconditional fopen here wrote khtml_qjs.log on EVERY script evaluation —
// measurable per-eval latency plus a file that grows forever on user
// machines. KHTML_QJS_TRACE=1 turns it back on for debugging.
static void kjlog(const char *m) {
    static const bool s_trace = [] {
        const char *v = getenv("KHTML_QJS_TRACE");
        return v && v[0] == '1';
    }();
    if (!s_trace) return;
    FILE *f = fopen("C:/Users/zhouzhehong/khtml_qjs.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

#include "dom/dom2_events.h"
#include <xml/dom_nodeimpl.h>
#include <xml/dom_docimpl.h>
#include <khtmlpart_p.h>
#include <khtml_part.h>
#include "khtml_debug.h"

#ifndef NDEBUG
int ScriptProxy::s_count = 0;
#endif

namespace {

// A self-contained replacement for the historic lazy event listener. It wraps an
// inline event-handler body (e.g. the value of an onclick attribute). At
// dispatch time it resolves the KHTMLPart from the event target and feeds the
// retained source to QuickJS. It holds no interpreter state.
class InlineQJSListener : public DOM::EventListener
{
public:
    InlineQJSListener(const QString &code, const QString &sourceUrl, int lineNo, const QString &name)
        : m_code(code), m_sourceUrl(sourceUrl), m_lineNo(lineNo), m_name(name) {}

    void handleEvent(DOM::Event &evt) override
    {
        // Reach the ScriptProxy through the event target's document -> part.
        // This deliberately does NOT touch any JS window object: that would
        // force an interpreter to be constructed, which we have removed.
        ScriptProxy *proxy = nullptr;
        DOM::Node ct = evt.currentTarget();
        DOM::NodeImpl *target = nullptr;
        if (!ct.isNull()) {
            target = ct.handle();
            if (target && target->document()) {
                KHTMLPart *part = qobject_cast<KHTMLPart *>(target->document()->part());
                if (part) {
                    proxy = ScriptProxy::proxy(part);
                }
            }
        }
        if (proxy) {
            proxy->dispatchEventToQuickJS(evt.handle(), target, m_code);
        }
    }

    DOM::DOMString eventListenerType() override { return "_khtml_HTMLEventListener"; }

private:
    QString m_code;
    QString m_sourceUrl;
    int m_lineNo;
    QString m_name;
};

} // namespace

ScriptProxy::ScriptProxy(khtml::ChildFrame *frame)
{
    m_frame = frame;
    m_running = 0;
    m_handlerLineno = 0;
    m_qjs = nullptr;
#ifndef NDEBUG
    s_count++;
#endif
}

ScriptProxy::~ScriptProxy()
{
    delete m_qjs;
    m_qjs = nullptr;
#ifndef NDEBUG
    s_count--;
#endif
}

bool ScriptProxy::evaluateQuickJS(const QString &script, const QString &filename,
                               const DOM::Node &n, QVariant *result)
{
    // Global kill-switch: KHTML_QJS_RUNTIME=0 disables the QuickJS path entirely.
    kjlog("evaluateQuickJS: entry");
    if (script.size() > 1048576) {
        return false;
    }
    {
        const char *v = getenv("KHTML_QJS_RUNTIME");
        if (v && v[0] == '0') return false;
    }

    if (!m_qjs) {
        m_qjs = new khtml::QJsDomBridge();
    }

    // The DOM document comes from the part's current document.
    DOM::Document doc;
    if (!n.isNull()) {
        doc = n.ownerDocument();
    } else if (m_frame && m_frame->m_part) {
        if (KHTMLPart *kp = qobject_cast<KHTMLPart *>(m_frame->m_part.data())) {
            doc = kp->document();
        }
    }
    if (doc.isNull()) {
        return false;
    }

    QVariant r = m_qjs->evaluate(script, filename, doc);
    if (m_qjs->needsFallback()) {
        return false;
    }
    if (result) {
        *result = r;
    }
    return true;
}


bool ScriptProxy::dispatchEventToQuickJS(DOM::EventImpl *event, DOM::NodeImpl *target,
                                      const QString &handlerSource)
{
    // QuickJS is the only engine: lazily create the runtime on first event,
    // mirroring evaluateQuickJS(). Inline handlers can fire before any
    // <script> has run, so the runtime must be brought up here too.
    if (!m_qjs) {
        m_qjs = new khtml::QJsDomBridge();
        if (m_frame && m_frame->m_part) {
            if (KHTMLPart *kp = qobject_cast<KHTMLPart *>(m_frame->m_part.data())) {
                m_qjs->setDocument(kp->document());
            }
        }
    }
    if (!m_qjs) {
        return false;
    }
    return m_qjs->dispatchEvent(event, target, handlerSource);
}

QVariant ScriptProxy::evaluate(const QString &filename, int baseLine,
                            const QString &str, const DOM::Node &n)
{
    ++m_running;
    // evaluate code. Returns the JS return value or an invalid QVariant
    // if there was none, an error occurred or the type couldn't be converted.

    // QuickJS is the only JavaScript engine. Run the script through it.
    QVariant qjsResult;
    if (evaluateQuickJS(str, filename, n, &qjsResult)) {
        --m_running;
        return qjsResult;
    }

    // On any failure (unsupported API, syntax error, oversized script) return
    // an empty QVariant. There is no other engine to fall back to.
    Q_UNUSED(baseLine);
    --m_running;
    return QVariant();
}

bool ScriptProxy::isRunningScript()
{
    return m_running != 0;
}

void ScriptProxy::clear()
{
    // The QuickJS bridge keeps its own document state; dropping it here would
    // force a runtime rebuild on the next page load.
}

DOM::EventListener *ScriptProxy::createHTMLEventHandler(const QString &sourceUrl, const QString &name, const QString &code, DOM::NodeImpl *node, bool svg)
{
    // Hand back a self-contained inline listener that retains only the raw
    // handler source. Nothing is parsed here; at dispatch time the source is
    // fed to QuickJS.
    Q_UNUSED(node);
    Q_UNUSED(svg);
    return new InlineQJSListener(code, sourceUrl, m_handlerLineno, name);
}

void ScriptProxy::finishedWithEvent(const DOM::Event &event)
{
    // QuickJS owns no per-event listener wrapper cache, so there is nothing
    // to release.
    Q_UNUSED(event);
}

// Helper method, so that all classes which need jScript() don't need to be added
// as friend to KHTMLPart
ScriptProxy *ScriptProxy::proxy(KHTMLPart *part)
{
    return part->jScript();
}
