/*
    css_variables.h - engine-side CSS custom properties (var()) support.

    Header-only helpers used by CSSStyleSheetImpl::parseString():
      * khtmlCollectCustomProperties() removes "--name: value" declarations
        from the CSS text and reports them (they are stored in the document's
        property map, see DocumentImpl::setCustomProperty).
      * khtmlExpandCssVariables() substitutes var(--name[, fallback]) using a
        lookup callback, recursively (nested variables are resolved, with a
        depth guard).

    This is the engine-level replacement for the host's textual var()
    expansion: KHTML's parser does not support custom properties, so they are
    resolved before the grammar sees the sheet.
*/
#ifndef KHTML_CSS_VARIABLES_H
#define KHTML_CSS_VARIABLES_H

#include <QChar>
#include <QString>
#include <functional>

namespace DOM
{

// Remove custom-property declarations from |css|, reporting each one to
// |sink| as (name, value).  The scanner only accepts a declaration start
// (after '{', ';' or whitespace-only run), respects quotes and nested
// brackets, and stops at the first top-level ';' or the enclosing '}'.
inline QString khtmlCollectCustomProperties(
    const QString &css,
    const std::function<void(const QString &, const QString &)> &sink)
{
    QString out;
    out.reserve(css.size());
    const int n = css.size();
    int i = 0;
    while (i < n) {
        if (i + 1 < n && css.at(i) == QLatin1Char('-') && css.at(i + 1) == QLatin1Char('-')) {
            bool atStart = false;
            for (int j = i - 1; j >= 0; --j) {
                const QChar c = css.at(j);
                if (c == QLatin1Char('{') || c == QLatin1Char(';')) { atStart = true; break; }
                if (!c.isSpace()) break;
            }
            if (atStart) {
                int k = i + 2;
                while (k < n && (css.at(k).isLetterOrNumber() || css.at(k) == QLatin1Char('-')
                                 || css.at(k) == QLatin1Char('_')))
                    ++k;
                int colon = k;
                while (colon < n && css.at(colon).isSpace())
                    ++colon;
                if (colon < n && css.at(colon) == QLatin1Char(':')) {
                    int depth = 0;
                    int end = colon + 1;
                    QChar quote = QChar::Null;
                    while (end < n) {
                        const QChar c = css.at(end);
                        if (!quote.isNull()) {
                            if (c == quote) quote = QChar::Null;
                        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                            quote = c;
                        } else if (c == QLatin1Char('(') || c == QLatin1Char('{')
                                   || c == QLatin1Char('[')) {
                            ++depth;
                        } else if (c == QLatin1Char(')') || c == QLatin1Char(']')) {
                            if (depth > 0) --depth;
                        } else if (c == QLatin1Char('}')) {
                            if (depth > 0) --depth;
                            else break;             // end of the enclosing block
                        } else if (depth == 0 && c == QLatin1Char(';')) {
                            ++end;                  // consume the ';'
                            break;
                        }
                        ++end;
                    }
                    QString value = css.mid(colon + 1, end - colon - 1).trimmed();
                    if (value.endsWith(QLatin1Char(';')))
                        value.chop(1);
                    value = value.trimmed();
                    sink(css.mid(i, colon - i).trimmed().toLower(), value);
                    i = end;
                    continue;
                }
            }
        }
        out.append(css.at(i));
        ++i;
    }
    return out;
}

// Expand var(--name[, fallback]) using |lookup| (returns empty when unknown).
// Unresolvable references are left as-is so the declaration can still be
// dropped gracefully by the parser.
inline QString khtmlExpandCssVariables(
    const QString &css,
    const std::function<QString(const QString &)> &lookup,
    int depth = 0)
{
    if (depth > 8 || css.indexOf(QLatin1String("var(")) < 0)
        return css;
    QString out;
    out.reserve(css.size());
    const int n = css.size();
    int i = 0;
    while (i < n) {
        if (i + 4 <= n
            && css.midRef(i, 4).compare(QLatin1String("var("), Qt::CaseInsensitive) == 0) {
            int j = i + 3;   // points at '('
            int d = 0;
            QChar quote = QChar::Null;
            for (; j < n; ++j) {
                const QChar c = css.at(j);
                if (!quote.isNull()) {
                    if (c == quote) quote = QChar::Null;
                    continue;
                }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quote = c; continue; }
                if (c == QLatin1Char('(')) {
                    ++d;
                } else if (c == QLatin1Char(')')) {
                    if (--d == 0) break;
                }
            }
            if (j < n) {
                const QString inner = css.mid(i + 4, j - i - 4);
                const int comma = inner.indexOf(QLatin1Char(','));
                const QString name = (comma >= 0 ? inner.left(comma) : inner).trimmed().toLower();
                QString substitute;
                bool resolved = false;
                if (name.startsWith(QLatin1String("--"))) {
                    const QString v = lookup(name);
                    if (!v.isEmpty()) {
                        substitute = v;
                        resolved = true;
                    } else if (comma >= 0) {
                        substitute = inner.mid(comma + 1).trimmed();
                        resolved = true;
                    }
                } else if (comma >= 0) {
                    substitute = inner.mid(comma + 1).trimmed();
                    resolved = true;
                }
                if (resolved) {
                    out.append(khtmlExpandCssVariables(substitute, lookup, depth + 1));
                    i = j + 1;
                    continue;
                }
            }
        }
        out.append(css.at(i));
        ++i;
    }
    return out;
}

} // namespace DOM

#endif // KHTML_CSS_VARIABLES_H
