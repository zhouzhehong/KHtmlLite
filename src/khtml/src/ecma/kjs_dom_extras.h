/*
 * DOMTokenList and DOMStringMap for KHtmlLite.
 * Provides element.classList and element.dataset.
 */
#ifndef KJS_DOM_EXTRAS_H
#define KJS_DOM_EXTRAS_H

#include "ecma/kjs_dom.h"
#include <kjs/object.h>

namespace KJS {

class DOMTokenList : public DOMObject {
public:
    DOMTokenList(ExecState *exec, DOM::ElementImpl *e);
    JSValue *getValueProperty(ExecState *exec, int token) const;
    bool getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot);
    const ClassInfo *classInfo() const override { return &info; }
    static const ClassInfo info;
    enum { Length, Item, Contains, Add, Remove, Toggle, ToString };
    DOM::ElementImpl *element() const { return m_element; }
private:
    DOM::ElementImpl *m_element;
};

class DOMStringMap : public DOMObject {
public:
    DOMStringMap(ExecState *exec, DOM::ElementImpl *e);
    bool getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot) override;
    void put(ExecState *exec, const Identifier &propertyName, JSValue *value, int slot = 0) override;
    bool deleteProperty(ExecState *exec, const Identifier &propertyName) override;
    const ClassInfo *classInfo() const override { return &info; }
    static const ClassInfo info;
private:
    DOM::ElementImpl *m_element;
    static QString camelToDash(const QString &camel);
};

JSValue *getDOMTokenList(ExecState *exec, DOM::ElementImpl *e);
JSValue *getDOMStringMap(ExecState *exec, DOM::ElementImpl *e);

} // namespace KJS

#endif
