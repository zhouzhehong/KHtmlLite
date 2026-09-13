/*
 * DOMTokenList (classList) and DOMStringMap (dataset) for KHtmlLite.
 */
#include "kjs_dom_extras.h"
#include "kjs_dom.h"
#include "kjs_dom_extras.lut.h"
#include <kjs/identifier.h>
#include <kjs/value.h>
#include <kjs/operations.h>
#include <QStringList>

namespace KJS {

const ClassInfo DOMTokenList::info = { "DOMTokenList", nullptr, nullptr, nullptr };

static QStringList tokenClasses(DOM::ElementImpl *e)
{
    return e->getAttribute(DOM::DOMString(QStringLiteral("class"))).string().split(
        QRegExp(QStringLiteral("\\s+")), QString::SkipEmptyParts);
}

static void setTokenClasses(DOM::ElementImpl *e, const QStringList &list)
{
    int ec = 0;
    e->setAttribute(DOM::DOMString(QStringLiteral("class")),
                    DOM::DOMString(list.join(QStringLiteral(" "))), ec);
}

DOMTokenList::DOMTokenList(ExecState *exec, DOM::ElementImpl *e)
    : DOMObject(exec->lexicalInterpreter()->builtinObjectPrototype()), m_element(e) {}

bool DOMTokenList::getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot)
{
    if (getStaticOwnValueSlot(&DOMTokenListTable, this, propertyName, slot))
        return true;
    bool ok = false;
    unsigned idx = propertyName.toStrictUInt32(&ok);
    if (ok) {
        QStringList classes = tokenClasses(m_element);
        if (idx < (unsigned)classes.size()) {
            slot.setValue(this, jsString(classes[idx]));
            return true;
        }
        return false;
    }
    return DOMObject::getOwnPropertySlot(exec, propertyName, slot);
}

JSValue *DOMTokenList::getValueProperty(ExecState *, int token) const
{
    if (token == Length)
        return jsNumber((unsigned int)tokenClasses(m_element).size());
    return jsUndefined();
}

/* Source for DOMTokenListProtoTable.
@begin DOMTokenListProtoTable 6
  item      DOMTokenList::Item      DontDelete|Function 1
  contains  DOMTokenList::Contains  DontDelete|Function 1
  add       DOMTokenList::Add       DontDelete|Function 1
  remove    DOMTokenList::Remove    DontDelete|Function 1
  toggle    DOMTokenList::Toggle    DontDelete|Function 1
  toString  DOMTokenList::ToString  DontDelete|Function 0
@end
*/
/* Source for DOMTokenListTable.
@begin DOMTokenListTable 1
  length    DOMTokenList::Length    DontDelete|ReadOnly
@end
*/
KJS_DEFINE_PROTOTYPE(DOMTokenListProto)
KJS_IMPLEMENT_PROTOFUNC(DOMTokenListProtoFunc)
KJS_IMPLEMENT_PROTOTYPE("DOMTokenList", DOMTokenListProto, DOMTokenListProtoFunc, ObjectPrototype)

JSValue *DOMTokenListProtoFunc::callAsFunction(ExecState *exec, JSObject *thisObj, const List &args)
{
    DOMTokenList *tl = static_cast<DOMTokenList *>(thisObj);
    DOM::ElementImpl *e = tl->element();
    QStringList classes = tokenClasses(e);

    switch (id) {
    case DOMTokenList::Item: {
        if (args.isEmpty()) return jsNull();
        bool ok = false;
        unsigned idx = args[0]->toUInt32(exec, ok);
        if (ok && idx < (unsigned)classes.size()) return jsString(classes[idx]);
        return jsNull();
    }
    case DOMTokenList::Contains:
        if (args.isEmpty()) return jsBoolean(false);
        return jsBoolean(classes.contains(args[0]->toString(exec).qstring()));
    case DOMTokenList::Add:
        for (int i = 0; i < args.size(); i++) {
            QString tok = args[i]->toString(exec).qstring();
            if (!tok.isEmpty() && !classes.contains(tok)) classes.append(tok);
        }
        setTokenClasses(e, classes);
        return jsUndefined();
    case DOMTokenList::Remove:
        for (int i = 0; i < args.size(); i++)
            classes.removeAll(args[i]->toString(exec).qstring());
        setTokenClasses(e, classes);
        return jsUndefined();
    case DOMTokenList::Toggle: {
        if (args.isEmpty()) return jsUndefined();
        QString tok = args[0]->toString(exec).qstring();
        if (classes.contains(tok)) {
            classes.removeAll(tok);
            setTokenClasses(e, classes);
            return jsBoolean(false);
        }
        classes.append(tok);
        setTokenClasses(e, classes);
        return jsBoolean(true);
    }
    case DOMTokenList::ToString:
        return jsString(classes.join(QStringLiteral(" ")));
    }
    return jsUndefined();
}

JSValue *getDOMTokenList(ExecState *exec, DOM::ElementImpl *e)
{
    return new DOMTokenList(exec, e);
}

// --- DOMStringMap (dataset) ---

const ClassInfo DOMStringMap::info = { "DOMStringMap", nullptr, nullptr, nullptr };

QString DOMStringMap::camelToDash(const QString &camel)
{
    QString result;
    for (int i = 0; i < camel.length(); i++) {
        QChar ch = camel.at(i);
        if (ch.isUpper()) {
            result += QLatin1Char('-');
            result += ch.toLower();
        } else {
            result += ch;
        }
    }
    return result;
}

DOMStringMap::DOMStringMap(ExecState *exec, DOM::ElementImpl *e)
    : DOMObject(exec->lexicalInterpreter()->builtinObjectPrototype()), m_element(e) {}

bool DOMStringMap::getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot)
{
    QString attr = QStringLiteral("data-") + camelToDash(propertyName.qstring());
    if (m_element->hasAttribute(DOM::DOMString(attr))) {
        slot.setValue(this, jsString(m_element->getAttribute(DOM::DOMString(attr))));
        return true;
    }
    return DOMObject::getOwnPropertySlot(exec, propertyName, slot);
}

void DOMStringMap::put(ExecState *exec, const Identifier &propertyName, JSValue *value, int)
{
    QString attr = QStringLiteral("data-") + camelToDash(propertyName.qstring());
    int ec = 0;
    m_element->setAttribute(DOM::DOMString(attr), value->toString(exec).domString(), ec);
}

bool DOMStringMap::deleteProperty(ExecState *exec, const Identifier &propertyName)
{
    QString attr = QStringLiteral("data-") + camelToDash(propertyName.qstring());
    if (m_element->hasAttribute(DOM::DOMString(attr))) {
        int ec = 0;
        m_element->removeAttribute(DOM::DOMString(attr), ec);
        return true;
    }
    return false;
}

JSValue *getDOMStringMap(ExecState *exec, DOM::ElementImpl *e)
{
    return new DOMStringMap(exec, e);
}

} // namespace KJS
