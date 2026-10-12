/*
    Copyright (C) 2004, 2005 Nikolas Zimmermann <wildfox@kde.org>
                  2004, 2005, 2006, 2007 Rob Buis <buis@kde.org>
    Copyright (C) 2006 Apple Computer, Inc.

    This file is part of the KDE project

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "wtf/Platform.h"
#if ENABLE(SVG)
#include "SVGStyleElement.h"

//#include "CSSStyleSheet.h"
#include "Document.h"
#include "ExceptionCode.h"
#include "css/css_mediaquery.h"
//#include "HTMLNames.h"
//#include "XMLNames.h"

namespace WebCore
{

//using namespace HTMLNames;

SVGStyleElement::SVGStyleElement(const QualifiedName &tagName, Document *doc)
    : SVGElement(tagName, doc)
    , m_createdByParser(false)
    , m_sheet(nullptr)
{
}

DOMString SVGStyleElement::xmlspace() const
{
    // ### shouldn't this provide default?
    return getAttribute(ATTR_XML_SPACE);
}

void SVGStyleElement::setXmlspace(const DOMString &, ExceptionCode &ec)
{
    ec = DOMException::NO_MODIFICATION_ALLOWED_ERR;
}

const DOMString SVGStyleElement::type() const
{
    static const DOMString defaultValue("text/css");
    DOMString n = getAttribute(ATTR_TYPE);
    return n.isNull() ? defaultValue : n;
}

void SVGStyleElement::setType(const DOMString &, ExceptionCode &ec)
{
    ec = DOMException::NO_MODIFICATION_ALLOWED_ERR;
}

const DOMString SVGStyleElement::media() const
{
    static const DOMString defaultValue("all");
    DOMString n = getAttribute(ATTR_MEDIA);
    return n.isNull() ? defaultValue : n;
}

void SVGStyleElement::setMedia(const DOMString &, ExceptionCode &ec)
{
    ec = DOMException::NO_MODIFICATION_ALLOWED_ERR;
}

String SVGStyleElement::title() const
{
    return getAttribute(ATTR_TITLE);
}

void SVGStyleElement::setTitle(const DOMString &, ExceptionCode &ec)
{
    ec = DOMException::NO_MODIFICATION_ALLOWED_ERR;
}

void SVGStyleElement::parseMappedAttribute(MappedAttribute *attr)
{
    // qCDebug(KHTML_LOG) << "parse: " << attr->id() << attr->localName() << attr->value();
    if (/*attr->name() == titleAttr*/attr->id() == ATTR_TITLE && m_sheet)
        ;//FIXME m_sheet->setTitle(attr->value());
    else {
        SVGElement::parseMappedAttribute(attr);
    }
}

void SVGStyleElement::finishParsingChildren()
{
    m_createdByParser = false;
    SVGElement::finishParsingChildren();
    parseText();
}

void SVGStyleElement::insertedIntoDocument()
{
    SVGElement::insertedIntoDocument();

    // If we're empty, we have to call parseText here, since we won't get
    // childrenChanged(); we still want a CSSOM object (same as HTML).
    if (!firstChild()) {
        parseText();
    }
}

void SVGStyleElement::removedFromDocument()
{
    SVGElement::removedFromDocument();
    if (m_sheet) {
        document()->updateStyleSelector();
    }
}

void SVGStyleElement::childrenChanged(bool changedByParser, Node *beforeChange, Node *afterChange, int childCountDelta)
{
    Q_UNUSED(changedByParser);
    Q_UNUSED(beforeChange);
    Q_UNUSED(afterChange);
    Q_UNUSED(childCountDelta);
    SVGElement::childrenChanged();
    if (m_createdByParser) {
        // incremental parser: children arrive in batches; parse at
        // finishParsingChildren() instead
        return;
    }
    parseText();
}

void SVGStyleElement::parseText()
{
    if (!document()) {
        return;
    }
    DOMString text("");
    for (DOM::NodeImpl *c = firstChild(); c != nullptr; c = c->nextSibling()) {
        if (c->nodeType() == DOM::Node::TEXT_NODE ||
                c->nodeType() == DOM::Node::CDATA_SECTION_NODE) {
            text += c->nodeValue();
        }
    }

    if (m_sheet) {
        m_sheet->deref();
        m_sheet = nullptr;
    }

    if (type().string() == QLatin1String("text/css")) {
        DOM::MediaListImpl *mediaList =
            new DOM::MediaListImpl((DOM::CSSStyleSheetImpl *)nullptr, media().string(), true);
        mediaList->ref();
        khtml::MediaQueryEvaluator screenEval("screen", true);
        khtml::MediaQueryEvaluator printEval("print", true);
        if (screenEval.eval(mediaList) || printEval.eval(mediaList)) {
            DOM::CSSStyleSheetImpl *css = new DOM::CSSStyleSheetImpl(this);
            css->ref();
            css->parseString(text, !document()->inCompatMode());
            css->setMedia(mediaList);
            css->setTitle(title());
            m_sheet = css;
        }
        mediaList->deref();
    }

    if (m_sheet) {
        document()->updateStyleSelector();
    }
}

StyleSheet *SVGStyleElement::sheet()
{
    //return StyleElement::sheet(this);
    return m_sheet;
}

bool SVGStyleElement::sheetLoaded()
{
    //document()->removePendingSheet();
    document()->styleSheetLoaded();
    return true;
}

// khtml compatibility methods:
quint32 SVGStyleElement::id() const
{
    return SVGNames::styleTag.id();
}

}

#endif // ENABLE(SVG)
