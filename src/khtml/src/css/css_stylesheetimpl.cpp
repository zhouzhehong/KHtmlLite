/**
 * This file is part of the DOM implementation for KDE.
 *
 * Copyright (C) 1999-2003 Lars Knoll (knoll@kde.org)
 *           (C) 2004 Apple Computer, Inc.
 *           (C) 2008 Germain Garand <germain@ebooksfrance.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

//#define CSS_STYLESHEET_DEBUG

#include "css_stylesheetimpl.h"
#include "css_ruleimpl.h"
#include "css_valueimpl.h"
#include "cssparser.h"
#include "css_variables.h"
#include "xml/dom_docimpl.h"
#include "css_mediaquery.h"

#include <dom/dom_string.h>
#include <dom/dom_exception.h>
#include <dom/css_stylesheet.h>
#include <dom/css_rule.h>
#include <dom/dom_exception.h>

#include <xml/dom_nodeimpl.h>
#include <html/html_documentimpl.h>
#include <misc/loader.h>

#include "khtml_debug.h"

#include <QRegularExpression>
#include "cssproperties.h"
#include <QStringList>
#include <functional>

using namespace DOM;
using namespace khtml;
// --------------------------------------------------------------------------------

StyleSheetImpl::StyleSheetImpl(StyleSheetImpl *parentSheet, DOMString href)
    : StyleListImpl(parentSheet)
{
    m_disabled = false;
    m_media = nullptr;
    m_parentNode = nullptr;
    m_strHref = href;
}

StyleSheetImpl::StyleSheetImpl(DOM::NodeImpl *parentNode, DOMString href)
    : StyleListImpl()
{
    m_parentNode = parentNode;
    m_disabled = false;
    m_media = nullptr;
    m_strHref = href;
}

StyleSheetImpl::StyleSheetImpl(StyleBaseImpl *owner, DOMString href)
    : StyleListImpl(owner)
{
    m_disabled = false;
    m_media = nullptr;
    m_parentNode = nullptr;
    m_strHref = href;
}

StyleSheetImpl::~StyleSheetImpl()
{
    if (m_media) {
        m_media->setParent(nullptr);
        m_media->deref();
    }
}

StyleSheetImpl *StyleSheetImpl::parentStyleSheet() const
{
    if (!m_parent) {
        return nullptr;
    }
    if (m_parent->isStyleSheet()) {
        return static_cast<StyleSheetImpl *>(m_parent);
    }
    if (m_parent->isRule()) {
        return m_parent->stylesheet();
    }
    return nullptr;
}

void StyleSheetImpl::setMedia(MediaListImpl *media)
{
    if (media) {
        media->ref();
    }
    if (m_media) {
        m_media->setParent(nullptr);
        m_media->deref();
    }
    m_media = media;
    if (m_media) {
        m_media->setParent(this);
    }
}

void StyleSheetImpl::setDisabled(bool disabled)
{
    bool updateStyle = isCSSStyleSheet() && m_parentNode && disabled != m_disabled;
    m_disabled = disabled;
    if (updateStyle) {
        m_parentNode->document()->updateStyleSelector();
    }
}

// -----------------------------------------------------------------------

CSSStyleSheetImpl::CSSStyleSheetImpl(CSSStyleSheetImpl *parentSheet, DOMString href)
    : StyleSheetImpl(parentSheet, href)
{
    m_lstChildren = new QList<StyleBaseImpl *>;
    m_doc = parentSheet ? parentSheet->doc() : nullptr;
    m_implicit = false;
    m_namespaces = nullptr;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);
    m_loadedHint = false;
}

CSSStyleSheetImpl::CSSStyleSheetImpl(DOM::NodeImpl *parentNode, DOMString href, bool _implicit)
    : StyleSheetImpl(parentNode, href)
{
    m_lstChildren = new QList<StyleBaseImpl *>;
    m_doc = parentNode->document();
    m_implicit = _implicit;
    m_namespaces = nullptr;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);
    m_loadedHint = false;
}

CSSStyleSheetImpl::CSSStyleSheetImpl(CSSRuleImpl *ownerRule, DOMString href)
    : StyleSheetImpl(ownerRule, href)
{
    m_lstChildren = new QList<StyleBaseImpl *>;
    m_doc = static_cast<CSSStyleSheetImpl *>(ownerRule->stylesheet())->doc();
    m_implicit = false;
    m_namespaces = nullptr;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);
    m_loadedHint = false;
}

CSSStyleSheetImpl::CSSStyleSheetImpl(DOM::NodeImpl *parentNode, CSSStyleSheetImpl *orig)
    : StyleSheetImpl(parentNode, orig->m_strHref)
{
    m_lstChildren = new QList<StyleBaseImpl *>;
    StyleBaseImpl *rule;
    QListIterator<StyleBaseImpl *> it(*orig->m_lstChildren);
    while (it.hasNext()) {
        rule = it.next();
        m_lstChildren->append(rule);
        rule->setParent(this);
    }
    m_doc = parentNode->document();
    m_implicit = false;
    m_namespaces = nullptr;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);
    m_loadedHint = false;

    recomputeNamespaceInfo(); // as we cloned kids
}

CSSStyleSheetImpl::CSSStyleSheetImpl(CSSRuleImpl *ownerRule, CSSStyleSheetImpl *orig)
    : StyleSheetImpl(ownerRule, orig->m_strHref)
{
    // m_lstChildren is deleted in StyleListImpl
    m_lstChildren = new QList<StyleBaseImpl *>;
    StyleBaseImpl *rule;
    QListIterator<StyleBaseImpl *> it(*orig->m_lstChildren);
    while (it.hasNext()) {
        rule = it.next();
        m_lstChildren->append(rule);
        rule->setParent(this);
    }
    m_doc = static_cast<CSSStyleSheetImpl *>(ownerRule->stylesheet())->doc();
    m_implicit = false;
    m_namespaces = nullptr;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);
    m_loadedHint = false;

    recomputeNamespaceInfo(); // as we cloned kids
}

CSSRuleImpl *CSSStyleSheetImpl::ownerRule() const
{
    if (!m_parent) {
        return nullptr;
    }
    if (m_parent->isRule()) {
        return static_cast<CSSRuleImpl *>(m_parent);
    }
    return nullptr;
}

unsigned long CSSStyleSheetImpl::insertRule(const DOMString &rule, unsigned long index, int &exceptioncode)
{
    exceptioncode = 0;
    if (index > (unsigned) m_lstChildren->count()) {
        exceptioncode = DOMException::INDEX_SIZE_ERR;
        return 0;
    }
    CSSParser p(strictParsing);
    CSSRuleImpl *r = p.parseRule(this, rule);

    if (!r) {
        exceptioncode = CSSException::SYNTAX_ERR + CSSException::_EXCEPTION_OFFSET;
        return 0;
    }
    // ###
    // HIERARCHY_REQUEST_ERR: Raised if the rule cannot be inserted at the specified index e.g. if an
    //@import rule is inserted after a standard rule set or other at-rule.
    m_lstChildren->insert(index, r);
    if (m_doc) {
        m_doc->updateStyleSelector(true /*shallow*/);
    }

    if (r->type() == DOM::CSSRule::NAMESPACE_RULE) {
        dirtyNamespaceInfo();
        if (static_cast<CSSNamespaceRuleImpl *>(r)->isDefault()) {
            recomputeNamespaceInfo();    // default may have changed
        }
        // ### too late for some rules?
    }

    return index;
}

void CSSStyleSheetImpl::appendNamespaceRule(CSSNamespaceRuleImpl *ns)
{
    append(ns);
    dirtyNamespaceInfo();
    if (ns->isDefault()) {
        recomputeNamespaceInfo();
    }
}

CSSRuleListImpl *CSSStyleSheetImpl::cssRules(bool omitCharsetRules)
{
    return new CSSRuleListImpl(this, omitCharsetRules);
}

void CSSStyleSheetImpl::deleteRule(unsigned long index, int &exceptioncode)
{
    exceptioncode = 0;
    if (index + 1 > (unsigned) m_lstChildren->count()) {
        exceptioncode = DOMException::INDEX_SIZE_ERR;
        return;
    }
    StyleBaseImpl *b = m_lstChildren->takeAt(index);

    if (b->isRule() && static_cast<CSSRuleImpl *>(b)->type() == DOM::CSSRule::NAMESPACE_RULE) {
        dirtyNamespaceInfo();
        if (static_cast<CSSNamespaceRuleImpl *>(b)->isDefault()) {
            recomputeNamespaceInfo();    // default may have changed
        }
        // ### too late for some rules?
    }

    // TreeShared requires delete not deref when removed from tree
    b->setParent(nullptr);
    if (!b->refCount()) {
        delete b;
    }
    if (m_doc) {
        m_doc->updateStyleSelector(true /*shallow*/);
    }
}

void CSSStyleSheetImpl::recomputeNamespaceInfo()
{
    assert(!m_namespaces);

    m_namespaces = new QList<CSSNamespaceRuleImpl *>;
    m_defaultNamespace = NamespaceName::fromId(anyNamespace);

    // Compute list of all the @namespace nodes, as well as the default one.
    for (int i = 0; i < m_lstChildren->count(); ++i) {
        StyleBaseImpl *b = m_lstChildren->at(i);
        if (b->isRule() && static_cast<CSSRuleImpl *>(b)->type() == DOM::CSSRule::NAMESPACE_RULE) {
            CSSNamespaceRuleImpl *nr = static_cast<CSSNamespaceRuleImpl *>(b);
            DOM::DOMString prefix = nr->prefix();
            DOM::DOMString uri    = nr->namespaceURI();

            if (uri.isNull()) {
                continue;
            }

            if (nr->isDefault()) {
                m_defaultNamespace = NamespaceName::fromString(uri);
            }

            m_namespaces->append(nr);
        }
    }
}

void CSSStyleSheetImpl::determineNamespace(NamespaceName &namespacename, const DOM::DOMString &prefix)
{
    if (prefix.isEmpty()) {
        namespacename = NamespaceName::fromId(emptyNamespace);    // No namespace. If an element/attribute has a namespace, we won't match it.
    } else if (prefix == "*") {
        namespacename = NamespaceName::fromId(anyNamespace);    // We'll match any namespace.
    } else {
        if (!m_namespaces) {
            recomputeNamespaceInfo();
        }

        // To lookup the name, we go backwards, so the latest one wins
        for (int p = m_namespaces->count() - 1; p >= 0; --p) {
            CSSNamespaceRuleImpl *ns = m_namespaces->at(p);
            if (ns->prefix() == prefix) {
                namespacename = NamespaceName::fromString(ns->namespaceURI());
                return;
            }
        }
    }
}

bool CSSStyleSheetImpl::parseString(const DOMString &string, bool strict)
{
#ifdef CSS_STYLESHEET_DEBUG
    qCDebug(KHTML_LOG) << "parsing sheet, len=" << string.length() << ", sheet is " << string.string();
#endif

    QString css = string.string();

    // ---- @supports condition evaluation (engine-side, pre-parse) ----------
    // The bison grammar has no @supports token, so the block is expanded (or
    // dropped) here before parsing: a true condition inlines its rules, a
    // false condition removes the block entirely.  This is the behaviour
    // modern sites rely on for progressive enhancement; previously the host
    // deleted the whole block textually, which also killed rules the engine
    // could actually render.
    //
    // Declarations are considered supported when the property id is known
    // (after -webkit-/-moz-/-ms-/-o- prefix stripping); a handful of
    // known-property-but-unsupported values (display:grid, position:sticky)
    // are answered false.  Selector()/font-tech() queries are answered false
    // (conservative).  Nested blocks recurse.
    {
        // Split |s| on |kw| at paren/quote depth 0 (case-insensitive).
        auto splitTop = [](const QString &s, const QString &kw) {
            QStringList parts;
            int depth = 0, start = 0;
            QChar quote = QChar::Null;
            for (int i = 0; i < s.size(); ++i) {
                const QChar c = s.at(i);
                if (!quote.isNull()) {
                    if (c == quote) quote = QChar::Null;
                    continue;
                }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
                if (c == QLatin1Char('(')) { ++depth; continue; }
                if (c == QLatin1Char(')')) { if (depth > 0) --depth; continue; }
                if (depth == 0 && i + kw.size() <= s.size()
                    && s.midRef(i, kw.size()).compare(kw, Qt::CaseInsensitive) == 0) {
                    parts.append(s.mid(start, i - start));
                    i += kw.size() - 1;
                    start = i + 1;
                }
            }
            parts.append(s.mid(start));
            return parts;
        };
        // A single "(property: value)" query (or selector()/font-tech()).
        std::function<bool(const QString &)> condTrue;
        condTrue = [&](const QString &raw) -> bool {
            QString s = raw;
            // Strip comments from the condition before evaluating it.
            while (true) {
                const int cs = s.indexOf(QLatin1String("/*"));
                if (cs < 0) break;
                const int ce = s.indexOf(QLatin1String("*/"), cs + 2);
                if (ce < 0) { s.truncate(cs); break; }
                s.remove(cs, ce + 2 - cs);
            }
            s = s.trimmed();
            if (s.isEmpty())
                return false;
            if (s.startsWith(QLatin1String("not "), Qt::CaseInsensitive))
                return !condTrue(s.mid(4));
            // OR has lower precedence than AND.
            const QStringList ors = splitTop(s, QStringLiteral(" or "));
            if (ors.size() > 1) {
                for (const QString &p : ors)
                    if (condTrue(p)) return true;
                return false;
            }
            const QStringList ands = splitTop(s, QStringLiteral(" and "));
            if (ands.size() > 1) {
                for (const QString &p : ands)
                    if (!condTrue(p)) return false;
                return true;
            }
            // Strip balanced outer parentheses.
            while (s.startsWith(QLatin1Char('(')) && s.endsWith(QLatin1Char(')'))) {
                int d = 0;
                bool whole = true;
                for (int i = 0; i < s.size(); ++i) {
                    if (s.at(i) == QLatin1Char('(')) ++d;
                    else if (s.at(i) == QLatin1Char(')')) {
                        --d;
                        if (d == 0 && i != s.size() - 1) { whole = false; break; }
                    }
                }
                if (!whole) break;
                s = s.mid(1, s.size() - 2).trimmed();
            }
            if (s.startsWith(QLatin1String("selector("), Qt::CaseInsensitive)
                || s.startsWith(QLatin1String("font-tech("), Qt::CaseInsensitive)
                || s.startsWith(QLatin1String("font-format("), Qt::CaseInsensitive))
                return false;
            const int colon = s.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                return false;
            const QString prop = s.left(colon).trimmed().toLower();
            const QString val = s.mid(colon + 1).trimmed().toLower();
            const QByteArray pb = prop.toLatin1();
            const int pid = DOM::getPropertyID(pb.constData(), pb.size());
            if (pid == 0)
                return false;
            return true;
        };
        std::function<QString(const QString &)> expand;
        expand = [&](const QString &css) -> QString {
            QString out;
            out.reserve(css.size());
            int i = 0;
            const int n = css.size();
            while (i < n) {
                // Skip comments: a literal "@supports" inside a comment must
                // not be treated as an at-rule start.
                if (css.at(i) == QLatin1Char('/') && i + 1 < n && css.at(i + 1) == QLatin1Char('*')) {
                    const int ce = css.indexOf(QLatin1String("*/"), i + 2);
                    if (ce < 0) { out.append(css.mid(i)); break; }
                    out.append(css.mid(i, ce + 2 - i));
                    i = ce + 2;
                    continue;
                }
                // An at-rule only starts at a rule boundary.
                bool atBoundary = (i == 0);
                if (!atBoundary) {
                    const QChar prev = css.at(i - 1);
                    atBoundary = prev.isSpace() || prev == QLatin1Char('{')
                                 || prev == QLatin1Char('}') || prev == QLatin1Char(';');
                }
                if (atBoundary && css.at(i) == QLatin1Char('@')
                    && i + 9 <= n
                    && css.midRef(i, 9).compare(QLatin1String("@supports"), Qt::CaseInsensitive) == 0
                    && (i + 9 >= n || !css.at(i + 9).isLetterOrNumber())) {
                    int j = i + 9;
                    int depth = 0;
                    QChar quote = QChar::Null;
                    int brace = -1;
                    for (; j < n; ++j) {
                        const QChar c = css.at(j);
                        if (c == QLatin1Char('/') && j + 1 < n && css.at(j + 1) == QLatin1Char('*')) {
                            const int ce = css.indexOf(QLatin1String("*/"), j + 2);
                            j = ce < 0 ? n : ce + 1;
                            continue;
                        }
                        if (!quote.isNull()) { if (c == quote) quote = QChar::Null; continue; }
                        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
                        if (c == QLatin1Char('(')) { ++depth; continue; }
                        if (c == QLatin1Char(')')) { if (depth > 0) --depth; continue; }
                        if (c == QLatin1Char('{') && depth == 0) { brace = j; break; }
                        if (c == QLatin1Char(';') && depth == 0) break;
                    }
                    if (brace < 0) { out.append(css.at(i)); ++i; continue; }
                    int d = 0, end = -1;
                    quote = QChar::Null;
                    for (int k = brace; k < n; ++k) {
                        const QChar c = css.at(k);
                        if (c == QLatin1Char('/') && k + 1 < n && css.at(k + 1) == QLatin1Char('*')) {
                            const int ce = css.indexOf(QLatin1String("*/"), k + 2);
                            k = ce < 0 ? n : ce + 1;
                            continue;
                        }
                        if (!quote.isNull()) { if (c == quote) quote = QChar::Null; continue; }
                        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
                        if (c == QLatin1Char('{')) ++d;
                        else if (c == QLatin1Char('}')) { --d; if (d == 0) { end = k; break; } }
                    }
                    if (end < 0) { out.append(css.at(i)); ++i; continue; }
                    const QString cond = css.mid(i + 9, brace - (i + 9));
                    const QString inner = css.mid(brace + 1, end - brace - 1);
                    if (condTrue(cond))
                        out.append(expand(inner));   // inline the nested rules
                    // else: drop the unsupported block entirely
                    i = end + 1;
                    continue;
                }
                out.append(css.at(i));
                ++i;
            }
            return out;
        };
        css = expand(css);
    }

    // CSS custom properties: collect "--name: value" declarations into the
    // document's property map and expand all var() references against it.
    // This is the engine-level replacement for the host's textual expansion,
    // and it also lets a LATER stylesheet (e.g. an external .css) resolve
    // variables defined by an earlier one (the inline <style> in <head>).
    {
        DOM::DocumentImpl *doc = this->doc();
        QHash<QString, QString> local;
        css = DOM::khtmlCollectCustomProperties(css,
            [doc, &local](const QString &name, const QString &value) {
                if (doc)
                    doc->setCustomProperty(name, value);
                local.insert(name, value);
            });
        auto lookup = [doc, &local](const QString &name) -> QString {
            const QString v = doc ? doc->customProperty(name) : QString();
            return v.isEmpty() ? local.value(name) : v;
        };
        css = DOM::khtmlExpandCssVariables(css, lookup);
    }

    // Remove empty rule blocks (selector { }) left after stripping custom
    // properties. KHTML parser also fails on empty declaration blocks.
    {
        QRegularExpression reEmpty(
            QStringLiteral("[^{};\\s]+\\s*\\{\\s*\\}"),
            QRegularExpression::CaseInsensitiveOption);
        css = css.remove(reEmpty);
    }

    // Fix malformed CSS where ';' is used instead of '{' at rule-block start.
    // Only when at brace depth 0 (i.e. between rules, not inside a declaration
    // block) and followed by "property:". This avoids touching normal ";".
    {
        QString out;
        out.reserve(css.size());
        int depth = 0;
        QChar quote = QChar::Null;
        for (int i = 0; i < css.size(); ++i) {
            const QChar c = css.at(i);
            if (!quote.isNull()) {
                out.append(c);
                if (c == quote) quote = QChar::Null;
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
                out.append(c);
                continue;
            }
            if (c == QLatin1Char('{')) { ++depth; out.append(c); continue; }
            if (c == QLatin1Char('}')) { if (depth > 0) --depth; out.append(c); continue; }
            if (depth == 0 && c == QLatin1Char(';')) {
                // Look ahead: whitespace + property-name + ':'
                int j = i + 1;
                while (j < css.size() && css.at(j).isSpace()) ++j;
                int nameStart = j;
                while (j < css.size() && (css.at(j).isLetter() || css.at(j) == QLatin1Char('-'))) ++j;
                if (j > nameStart && j < css.size() && css.at(j) == QLatin1Char(':')) {
                    out.append(QLatin1Char('{'));
                    continue;
                }
            }
            out.append(c);
        }
        css = out;
    }

    // calc(): the bison expression grammar cannot parse '+'/'-' between terms,
    // so rewrite "calc(A - B)" as "calc(A,-B)" (comma separated, signed).
    // parseCalcSpec() then sums the signed terms.
    {
        QString out;
        out.reserve(css.size());
        const int n = css.size();
        int i = 0;
        while (i < n) {
            if (i + 5 <= n
                && css.midRef(i, 5).compare(QLatin1String("calc("), Qt::CaseInsensitive) == 0) {
                int j = i + 4;
                int depth = 0;
                QChar quote = QChar::Null;
                for (; j < n; ++j) {
                    const QChar c = css.at(j);
                    if (!quote.isNull()) {
                        if (c == quote) quote = QChar::Null;
                        continue;
                    }
                    if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
                    if (c == QLatin1Char('(')) { ++depth; continue; }
                    if (c == QLatin1Char(')')) { if (--depth == 0) break; }
                }
                if (j < n) {
                    const QString inner = css.mid(i + 5, j - i - 5).trimmed();
                    QString rew;
                    bool first = true;
                    int k = 0;
                    const int m = inner.size();
                    while (k < m) {
                        int sign = 1;
                        if (inner.at(k) == QLatin1Char('+') || inner.at(k) == QLatin1Char('-')) {
                            sign = inner.at(k) == QLatin1Char('-') ? -1 : 1;
                            ++k;
                            while (k < m && inner.at(k).isSpace()) ++k;
                        }
                        int e = k;
                        int d2 = 0;
                        while (e < m) {
                            const QChar c = inner.at(e);
                            if (c == QLatin1Char('(')) ++d2;
                            else if (c == QLatin1Char(')')) --d2;
                            else if (d2 == 0 && (c == QLatin1Char('+') || c == QLatin1Char('-')) && e > k)
                                break;
                            ++e;
                        }
                        QString term = inner.mid(k, e - k).trimmed();
                        if (!term.isEmpty()) {
                            if (!first)
                                rew += QLatin1Char(',');
                            if (sign < 0 && !term.startsWith(QLatin1Char('-')))
                                rew += QLatin1Char('-');
                            rew += term;
                            first = false;
                        }
                        k = e;
                    }
                    out += QLatin1String("calc(") + rew + QLatin1Char(')');
                    i = j + 1;
                    continue;
                }
            }
            out += css.at(i);
            ++i;
        }
        css = out;
    }

    strictParsing = strict;
    CSSParser p(strict);
    p.parseSheet(this, DOMString(css));
    return true;
}

bool CSSStyleSheetImpl::isLoading() const
{
    StyleBaseImpl *rule;
    QListIterator<StyleBaseImpl *> it(*m_lstChildren);
    while (it.hasNext()) {
        rule = it.next();
        if (rule->isImportRule()) {
            CSSImportRuleImpl *import = static_cast<CSSImportRuleImpl *>(rule);
#ifdef CSS_STYLESHEET_DEBUG
            qCDebug(KHTML_LOG) << "found import";
#endif
            if (import->isLoading()) {
#ifdef CSS_STYLESHEET_DEBUG
                qCDebug(KHTML_LOG) << "--> not loaded";
#endif
                m_loadedHint = false;
                return true;
            }
        }
    }
    m_loadedHint = true;
    return false;
}

void CSSStyleSheetImpl::checkLoaded() const
{
    if (isLoading()) {
        return;
    }
    if (m_parent) {
        m_parent->checkLoaded();
    }
    if (m_parentNode) {
        m_loadedHint = m_parentNode->checkRemovePendingSheet();
    } else if (parentStyleSheet() && parentStyleSheet()->isCSSStyleSheet()) {
        m_loadedHint = static_cast<CSSStyleSheetImpl *>(parentStyleSheet())->loadedHint();
    } else {
        m_loadedHint = true;
    }
}

void CSSStyleSheetImpl::checkPending() const
{
    if (!m_loadedHint) {
        return;
    }
    if (m_parent) {
        m_parent->checkPending();
    } else if (m_parentNode) {
        m_parentNode->checkAddPendingSheet();
    }
}

// ---------------------------------------------------------------------------

StyleSheetListImpl::~StyleSheetListImpl()
{
    for (StyleSheetImpl *sh : styleSheets) {
        sh->deref();
    }
}

void StyleSheetListImpl::add(StyleSheetImpl *s)
{
    if (managerDocument) {
        managerDocument->ensureStyleSheetListUpToDate();
    }

    // ### in cases this is document.styleSheets, maybe
    // we should route to DocumentImpl::addStyleSheets?

    if (!styleSheets.contains(s)) {
        s->ref();
        styleSheets.append(s);
    }
}

void StyleSheetListImpl::remove(StyleSheetImpl *s)
{
    if (managerDocument) {
        managerDocument->ensureStyleSheetListUpToDate();
    }

    if (styleSheets.removeAll(s)) {
        s->deref();
    }
}

unsigned long StyleSheetListImpl::length() const
{
    if (managerDocument) {
        managerDocument->ensureStyleSheetListUpToDate();
    }

    // hack so implicit BODY stylesheets don't get counted here
    unsigned long l = 0;
    for (StyleSheetImpl *sh : styleSheets) {
        if (!sh->isCSSStyleSheet() || !static_cast<CSSStyleSheetImpl *>(sh)->implicit()) {
            ++l;
        }
    }
    return l;
}

StyleSheetImpl *StyleSheetListImpl::item(unsigned long index)
{
    if (managerDocument) {
        managerDocument->ensureStyleSheetListUpToDate();
    }

    unsigned long l = 0;
    for (StyleSheetImpl *sh : styleSheets) {
        if (!sh->isCSSStyleSheet() || !static_cast<CSSStyleSheetImpl *>(sh)->implicit()) {
            if (l == index) {
                return sh;
            }
            ++l;
        }
    }
    return nullptr;
}

// --------------------------------------------------------------------------------------------

/* MediaList is used to store 3 types of media related entities which mean the same:
 * Media Queries, Media Types and Media Descriptors.
 * Currently MediaList always tries to parse media queries and if parsing fails,
 * tries to fallback to Media Descriptors if m_fallback flag is set.
 * Slight problem with syntax error handling:
 * CSS 2.1 Spec (https://www.w3.org/TR/CSS21/media.html)
 * specifies that failing media type parsing is a syntax error
 * CSS 3 Media Queries Spec (https://www.w3.org/TR/css3-mediaqueries/)
 * specifies that failing media query is a syntax error
 * HTML 4.01 spec (https://www.w3.org/TR/REC-html40/present/styles.html#adef-media)
 * specifies that Media Descriptors should be parsed with forward-compatible syntax
 * DOM Level 2 Style Sheet spec (https://www.w3.org/TR/DOM-Level-2-Style/)
 * talks about MediaList.mediaText and refers
 *   -  to Media Descriptors of HTML 4.0 in context of StyleSheet
 *   -  to Media Types of CSS 2.0 in context of CSSMediaRule and CSSImportRule
 *
 * These facts create situation where same (illegal) media specification may result in
 * different parses depending on whether it is media attr of style element or part of
 * css @media rule.
 * <style media="screen and resolution > 40dpi"> ..</style> will be enabled on screen devices where as
 * @media screen and resolution > 40dpi {..} will not.
 * This gets more counter-intuitive in JavaScript:
 * document.styleSheets[0].media.mediaText = "screen and resolution > 40dpi" will be ok and
 * enabled, while
 * document.styleSheets[0].cssRules[0].media.mediaText = "screen and resolution > 40dpi" will
 * throw SYNTAX_ERR exception.
 */

MediaListImpl::MediaListImpl(CSSStyleSheetImpl *parentSheet,
                             const DOMString &media, bool fallbackToDescriptor)
    : StyleBaseImpl(parentSheet)
    , m_fallback(fallbackToDescriptor)
{
    int ec = 0;
    setMediaText(media, ec);
    // FIXME: parsing can fail. The problem with failing constructor is that
    // we would need additional flag saying MediaList is not valid
    // Parse can fail only when fallbackToDescriptor == false, i.e when HTML4 media descriptor
    // forward-compatible syntax is not in use.
    // DOMImplementationCSS seems to mandate that media descriptors are used
    // for both html and svg, even though svg:style doesn't use media descriptors
    // Currently the only places where parsing can fail are
    // creating <svg:style>, creating css media / import rules from js
    if (ec) {
        setMediaText("invalid", ec);
    }
}

MediaListImpl::MediaListImpl(CSSRuleImpl *parentRule, const DOMString &media, bool fallbackToDescriptor)
    : StyleBaseImpl(parentRule)
    , m_fallback(fallbackToDescriptor)
{
    int ec = 0;
    setMediaText(media, ec);
    // FIXME: parsing can fail. The problem with failing constructor is that
    // we would need additional flag saying MediaList is not valid
    // Parse can fail only when fallbackToDescriptor == false, i.e when HTML4 media descriptor
    // forward-compatible syntax is not in use.
    // DOMImplementationCSS seems to mandate that media descriptors are used
    // for both html and svg, even though svg:style doesn't use media descriptors
    // Currently the only places where parsing can fail are
    // creating <svg:style>, creating css media / import rules from js
    if (ec) {
        setMediaText("invalid", ec);
    }
}

MediaListImpl::~MediaListImpl()
{
    qDeleteAll(m_queries);
}

CSSStyleSheetImpl *MediaListImpl::parentStyleSheet() const
{
    if (m_parent->isCSSStyleSheet()) {
        return static_cast<CSSStyleSheetImpl *>(m_parent);
    }
    return nullptr;
}

CSSRuleImpl *MediaListImpl::parentRule() const
{
    if (m_parent->isRule()) {
        return static_cast<CSSRuleImpl *>(m_parent);
    }
    return nullptr;
}

static DOMString parseMediaDescriptor(const DOMString &s)
{
    int len = s.length();

    // https://www.w3.org/TR/REC-html40/types.html#type-media-descriptors
    // "Each entry is truncated just before the first character that isn't a
    // US ASCII letter [a-zA-Z] (ISO 10646 hex 41-5a, 61-7a), digit [0-9] (hex 30-39),
    // or hyphen (hex 2d)."
    int i;
    unsigned short c;
    for (i = 0; i < len; ++i) {
        c = s[i].unicode();
        if (!((c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '1' && c <= '9')
                || (c == '-'))) {
            break;
        }
    }
    return s.implementation()->substring(0, i);
}

void MediaListImpl::deleteMedium(const DOMString &oldMedium, int &ec)
{
    MediaListImpl tempMediaList;
    CSSParser p(true);

    MediaQuery *oldQuery = nullptr;
    bool deleteOldQuery = false;

    if (p.parseMediaQuery(&tempMediaList, oldMedium)) {
        if (tempMediaList.m_queries.size() > 0) {
            oldQuery = tempMediaList.m_queries[0];
        }
    } else if (m_fallback) {
        DOMString medium = parseMediaDescriptor(oldMedium);
        if (!medium.isNull()) {
            oldQuery = new MediaQuery(MediaQuery::None, medium, nullptr);
            deleteOldQuery = true;
        }
    }

    // DOM Style Sheets spec doesn't allow SYNTAX_ERR to be thrown in deleteMedium
    ec = DOMException::NOT_FOUND_ERR;

    if (oldQuery) {
        for (int i = 0; i < m_queries.size(); ++i) {
            MediaQuery *a = m_queries[i];
            if (*a == *oldQuery) {
                m_queries.removeAt(i);
                delete a;
                ec = 0;
                break;
            }
        }
        if (deleteOldQuery) {
            delete oldQuery;
        }
    }
}

DOM::DOMString MediaListImpl::mediaText() const
{
    DOMString text;
    bool first = true;
    const QList<MediaQuery *>::ConstIterator itEnd = m_queries.end();

    for (QList<MediaQuery *>::ConstIterator it = m_queries.begin(); it != itEnd; ++it) {
        if (!first) {
            text += ", ";
        }
        text += (*it)->cssText();
        first = false;
    }
    return text;
}

void MediaListImpl::setMediaText(const DOM::DOMString &value, int &ec)
{
    MediaListImpl tempMediaList;
    CSSParser p(true);

    const QString val = value.string();
    const QStringList list = val.split(',');

    const QStringList::ConstIterator itEnd = list.end();

    for (QStringList::ConstIterator it = list.begin(); it != itEnd; ++it) {
        const DOMString medium = (*it).trimmed();
        if (!medium.isEmpty()) {
            if (!p.parseMediaQuery(&tempMediaList, medium)) {
                if (m_fallback) {
                    DOMString mediaDescriptor = parseMediaDescriptor(medium);
                    if (!mediaDescriptor.isNull()) {
                        tempMediaList.m_queries.append(new MediaQuery(MediaQuery::None, mediaDescriptor, nullptr));
                    }
                } else {
                    ec = CSSException::SYNTAX_ERR;
                    return;
                }
            }
        } else if (!m_fallback) {
            ec = CSSException::SYNTAX_ERR;
            return;
        }
    }
    // ",,,," falls straight through, but is not valid unless fallback
    if (!m_fallback && list.begin() == list.end()) {
        DOMString s = value.string().trimmed();
        if (!s.isEmpty()) {
            ec = CSSException::SYNTAX_ERR;
            return;
        }
    }

    ec = 0;
    qDeleteAll(m_queries);
    m_queries = tempMediaList.m_queries;
    tempMediaList.m_queries.clear();
}

DOMString MediaListImpl::item(unsigned long index) const
{
    if (index < (unsigned)m_queries.size()) {
        MediaQuery *query = m_queries[index];
        return query->cssText();
    }

    return DOMString();
}

void MediaListImpl::appendMedium(const DOMString &newMedium, int &ec)
{
    ec = DOMException::INVALID_CHARACTER_ERR;
    CSSParser p(true);
    if (p.parseMediaQuery(this, newMedium)) {
        ec = 0;
    } else if (m_fallback) {
        DOMString medium = parseMediaDescriptor(newMedium);
        if (!medium.isNull()) {
            m_queries.append(new MediaQuery(MediaQuery::None, medium, nullptr));
            ec = 0;
        }
    }
}

void MediaListImpl::appendMediaQuery(MediaQuery *mediaQuery)
{
    m_queries.append(mediaQuery);
}

