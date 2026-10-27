/*
    CSS declarations for properties KHTML cannot render.

    Shared by PageLoader (host) and the css_trim_probe unit test so both run
    exactly the same code.  The remover must never destroy brace balance:
    it deletes only the declaration text (plus its separating ';'), keeping
    the enclosing '{' / '}'.

    A small scanner protects ';' and '}' inside parentheses and quoted
    strings before the regex runs, so values such as
        mask-image: url(data:image/png;base64,AAA)
    are removed as a whole instead of being truncated at the first ';'.
*/
#ifndef KHTML_CSS_UNSUPPORTED_H
#define KHTML_CSS_UNSUPPORTED_H

#include <QRegularExpression>
#include <QString>
#include <QStringList>

inline QString simplifyUnsupportedCss(const QString &text)
{
    // Properties genuinely absent from KHTML 5.116 (or known-crashy) get their
    // declaration dropped.  Supported ones (border-radius, box-shadow,
    // opacity, box-sizing, text-shadow, transform, sticky, calc, ...) are
    // deliberately NOT listed.
    const QStringList props = {
        QStringLiteral("backdrop-filter"), QStringLiteral("filter"),
        QStringLiteral("clip-path"),
        QStringLiteral("mask"), QStringLiteral("mask-image"), QStringLiteral("will-change"),
        QStringLiteral("contain"), QStringLiteral("scroll-behavior"),
        QStringLiteral("backface-visibility"), QStringLiteral("perspective"),
        QStringLiteral("transform-style"), QStringLiteral("object-fit"),
        QStringLiteral("object-position"), QStringLiteral("overflow-anchor"),
        QStringLiteral("overscroll-behavior"), QStringLiteral("touch-action"),
        QStringLiteral("scroll-snap-type"), QStringLiteral("scroll-snap-align"),
        QStringLiteral("scroll-margin"), QStringLiteral("scroll-padding"),
        QStringLiteral("inline-size"), QStringLiteral("min-inline-size"),
        QStringLiteral("max-inline-size"), QStringLiteral("block-size"),
        QStringLiteral("min-block-size"), QStringLiteral("max-block-size"),
        QStringLiteral("margin-inline"), QStringLiteral("padding-inline"),
        QStringLiteral("border-inline"), QStringLiteral("inset"),
        QStringLiteral("inset-block"), QStringLiteral("inset-inline"),
        QStringLiteral("text-decoration-thickness"), QStringLiteral("text-underline-offset"),
        QStringLiteral("text-underline-position"), QStringLiteral("font-variant-numeric"),
        QStringLiteral("font-feature-settings"), QStringLiteral("font-optical-sizing"),
        QStringLiteral("font-variation-settings"),
        QStringLiteral("text-wrap"), QStringLiteral("white-space-collapse"),
        QStringLiteral("box-decoration-break"),
        QStringLiteral("break-inside"), QStringLiteral("break-before"),
        QStringLiteral("break-after"), QStringLiteral("column-count"),
        QStringLiteral("column-gap"), QStringLiteral("column-rule"),
        QStringLiteral("column-width"), QStringLiteral("columns"),
        QStringLiteral("mix-blend-mode"), QStringLiteral("isolation"),
        QStringLiteral("shape-outside"), QStringLiteral("shape-margin"),
    };

    QString out = text;

    // Scanner pass: ';' / '}' inside ()/[] or quotes must not be seen by the
    // declaration regex as value terminators.  Two private-use sentinels are
    // restored after all removals.
    const QChar semiSentinel(0xE000);
    const QChar braceSentinel(0xE001);
    {
        bool inStr = false;
        bool escaped = false;
        QChar quote;
        int depth = 0;
        for (int i = 0; i < out.size(); ++i) {
            const QChar c = out.at(i);
            if (inStr) {
                if (escaped) {
                    escaped = false;
                } else if (c == QLatin1Char('\\')) {
                    escaped = true;
                } else if (c == quote) {
                    inStr = false;
                } else if (c == QLatin1Char(';')) {
                    out[i] = semiSentinel;
                } else if (c == QLatin1Char('}')) {
                    out[i] = braceSentinel;
                }
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                inStr = true;
                quote = c;
                continue;
            }
            if (c == QLatin1Char('(') || c == QLatin1Char('[')) {
                ++depth;
                continue;
            }
            if ((c == QLatin1Char(')') || c == QLatin1Char(']')) && depth > 0) {
                --depth;
                continue;
            }
            if (depth > 0) {
                if (c == QLatin1Char(';')) {
                    out[i] = semiSentinel;
                } else if (c == QLatin1Char('}')) {
                    out[i] = braceSentinel;
                }
            }
        }
    }

    // Group 1 is the separator before the property ('{', ';', whitespace or
    // start of text).  Replacing the whole match with "\1" removes exactly the
    // declaration + its ';' while keeping the brace that opened the rule.
    for (const QString &p : props) {
        const QRegularExpression re(
            QStringLiteral("((?:^|[;{}\\s]))") + QRegularExpression::escape(p) +
            QStringLiteral("\\s*:[^;}]*(?:;|(?=\\}))"),
            QRegularExpression::CaseInsensitiveOption);
        out.replace(re, QStringLiteral("\\1"));
    }

    out.replace(semiSentinel, QLatin1Char(';'));
    out.replace(braceSentinel, QLatin1Char('}'));
    return out;
}

#endif // KHTML_CSS_UNSUPPORTED_H
