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

#include "kjs_proxy.h"

#include "qjs_dom_bridge.h"
#include <cstdio>
static void kjlog(const char *m) { FILE *f = fopen("C:/Users/zhouzhehong/khtml_qjs.log", "a"); if (f) { fprintf(f, "%s\n", m); fclose(f); } }

#include "dom/dom2_events.h"
#include <xml/dom_nodeimpl.h>
#include <xml/dom_docimpl.h>
#include <khtmlpart_p.h>
#include <khtml_part.h>
#include "khtml_debug.h"

#ifndef NDEBUG
int KJSProxy::s_count = 0;
#endif

namespace {

// A KJS-free replacement for the old KJS::JSLazyEventListener. It wraps an
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
        // Reach the KJSProxy through the event target's document -> part.
        // This deliberately does NOT touch any JS window object: that would
        // force an interpreter to be constructed, which we have removed.
        KJSProxy *proxy = nullptr;
        DOM::Node ct = evt.currentTarget();
        DOM::NodeImpl *target = nullptr;
        if (!ct.isNull()) {
            target = ct.handle();
            if (target && target->document()) {
                KHTMLPart *part = qobject_cast<KHTMLPart *>(target->document()->part());
                if (part) {
                    proxy = KJSProxy::proxy(part);
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

KJSProxy::KJSProxy(khtml::ChildFrame *frame)
{
    m_frame = frame;
    m_debugEnabled = false;
    m_running = 0;
    m_handlerLineno = 0;
    m_qjs = nullptr;
#ifndef NDEBUG
    s_count++;
#endif
}

KJSProxy::~KJSProxy()
{
    delete m_qjs;
    m_qjs = nullptr;
#ifndef NDEBUG
    s_count--;
#endif
}

bool KJSProxy::evaluateQuickJS(const QString &script, const QString &filename,
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


bool KJSProxy::dispatchEventToQuickJS(DOM::EventImpl *event, DOM::NodeImpl *target,
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

QVariant KJSProxy::evaluate(const QString &filename, int baseLine,
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

bool KJSProxy::isRunningScript()
{
    return m_running != 0;
}

void KJSProxy::clear()
{
    // KF5JS interpreter removed. The QuickJS bridge keeps its own document
    // state; dropping it here would force a runtime rebuild on next page load.
}

DOM::EventListener *KJSProxy::createHTMLEventHandler(const QString &sourceUrl, const QString &name, const QString &code, DOM::NodeImpl *node, bool svg)
{
    // Hand back a self-contained inline listener that retains only the raw
    // handler source. It parses nothing with KJS; at dispatch time it feeds
    // the source to QuickJS.
    Q_UNUSED(node);
    Q_UNUSED(svg);
    return new InlineQJSListener(code, sourceUrl, m_handlerLineno, name);
}

void KJSProxy::finishedWithEvent(const DOM::Event &event)
{
    // Used to tell the KJS interpreter to release its wrapper for this event.
    // QuickJS owns no such cache, so this is a no-op.
    Q_UNUSED(event);
}

void KJSProxy::setDebugEnabled(bool enabled)
{
    m_debugEnabled = enabled;
}

bool KJSProxy::debugEnabled() const
{
    return m_debugEnabled;
}

void KJSProxy::showDebugWindow(bool /*show*/)
{
    // KJS debugger removed.
}

bool KJSProxy::paused() const
{
    return false;
}

// Helper method, so that all classes which need jScript() don't need to be added
// as friend to KHTMLPart
KJSProxy *KJSProxy::proxy(KHTMLPart *part)
{
    return part->jScript();
}
