/*
 *  This file is part of the KDE libraries
 *  Copyright (C) 1999 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
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

#ifndef _KHTML_SCRIPT_PROXY_H_
#define _KHTML_SCRIPT_PROXY_H_

#include <QVariant>
#include <QString>

class KHTMLPart;

namespace DOM
{
class Node;
class NodeImpl;
class EventListener;
class Event;
class EventImpl;
}

namespace khtml
{
class ChildFrame;
class QJsDomBridge;
}

/**
 * @internal
 *
 * @short Proxy class routing script evaluation and inline event dispatch.
 *
 * QuickJS is the only script engine in this tree; this class owns the
 * per-frame QJsDomBridge and the handler-line bookkeeping — it holds no
 * interpreter state of its own.
 */
class ScriptProxy
{
public:
    ScriptProxy(khtml::ChildFrame *frame);
    ~ScriptProxy();

    QVariant evaluate(const QString &filename, int baseLine, const QString &, const DOM::Node &n);
    void clear();

    DOM::EventListener *createHTMLEventHandler(const QString &sourceUrl, const QString &name, const QString &code, DOM::NodeImpl *node, bool svg = false);
    void finishedWithEvent(const DOM::Event &event);

    bool isRunningScript();

    void setEventHandlerLineno(int lineno)
    {
        m_handlerLineno = lineno;
    }

    // Helper method, to access the private KHTMLPart::jScript()
    static ScriptProxy *proxy(KHTMLPart *part);

    bool quickJSAvailable() const { return m_qjs != nullptr; }
    bool dispatchEventToQuickJS(DOM::EventImpl *event, DOM::NodeImpl *target,
                                const QString &handlerSource);
private:
    bool evaluateQuickJS(const QString &script, const QString &filename,
                         const DOM::Node &n, QVariant *result);

    khtml::ChildFrame *m_frame;
    int m_handlerLineno;

    khtml::QJsDomBridge *m_qjs;
    int m_running;
#ifndef NDEBUG
    static int s_count;
#endif
};

#endif
