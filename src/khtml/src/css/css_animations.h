/*
    This file is part of the KHTML library.

    Engine-native CSS animation support (practical subset).

    The host's @keyframes JS polyfill never worked for real pages (the engine
    had no getComputedStyle for it, and fast pages skipped the final rewrite
    entirely).  This header implements the smallest useful engine-native path:

      * @keyframes blocks are extracted from the stylesheet text BEFORE the
        bison parser (its grammar has no at-rule for them) and registered here
        keyed by animation name;
      * the `animation` shorthand is parsed into a spec string on the style
        ("name;durMs;delayMs;iter;direction;timing;fill");
      * during painting, RenderBlock asks khtmlAnimationLookup() for the
        interpolated opacity / transform / background-color of the element's
        animation at the current clock time.

    Supported keyframe properties: opacity, transform, background-color.
    Timing: linear (the other timing functions fall back to linear phase).
    Directions: normal, reverse, alternate, alternate-reverse.
    Iterations: infinite or a finite count (clamped at the end).
*/

#ifndef KHTML_CSS_ANIMATIONS_H
#define KHTML_CSS_ANIMATIONS_H

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace khtml
{

struct AnimationStep {
    double offset = 0.0;      // 0.0 .. 1.0
    bool hasOpacity = false;
    double opacity = 1.0;
    bool hasTransform = false;
    QString transform;
    bool hasColor = false;
    QColor color;
};

struct AnimationFrames {
    QVector<AnimationStep> steps;
};

inline QHash<QString, AnimationFrames> &animationRegistry()
{
    static QHash<QString, AnimationFrames> registry;
    return registry;
}

// Milliseconds since the first animation query.  One clock for the whole
// renderer process, so all animations stay in phase.
inline double animationClockMs()
{
    static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
    return double(clock.elapsed());
}

// Index one past the matching '}' for the '{' at |openBrace|.
inline int animationBlockEnd(const QString &text, int openBrace)
{
    int depth = 0;
    for (int i = openBrace; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return -1;
}

inline void parseAnimationDeclarations(const QString &body, AnimationStep *step)
{
    const QStringList decls = body.split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int c = d.indexOf(QLatin1Char(':'));
        if (c <= 0) {
            continue;
        }
        const QString prop = d.left(c).trimmed().toLower();
        const QString val = d.mid(c + 1).trimmed();
        if (prop == QLatin1String("opacity")) {
            bool ok = false;
            const double v = val.toDouble(&ok);
            if (ok) {
                step->hasOpacity = true;
                step->opacity = v;
            }
        } else if (prop == QLatin1String("transform") || prop == QLatin1String("-webkit-transform")) {
            step->hasTransform = true;
            step->transform = val;
        } else if (prop == QLatin1String("background-color") || prop == QLatin1String("background")) {
            const QColor col(val);
            if (col.isValid()) {
                step->hasColor = true;
                step->color = col;
            }
        }
    }
}

// Extract @keyframes / @-webkit-keyframes blocks, register their parsed steps,
// and return |css| with those blocks removed.
inline QString khtmlExtractAndRegisterKeyframes(const QString &css)
{
    QString out;
    out.reserve(css.size());
    int i = 0;
    const int n = css.size();
    while (i < n) {
        const int at = css.indexOf(QLatin1Char('@'), i);
        if (at < 0) {
            out += css.mid(i);
            break;
        }
        static const QString kWK = QStringLiteral("-webkit-keyframes");
        static const QString kPlain = QStringLiteral("keyframes");
        const QString head = css.mid(at + 1, 24).toLower();
        int nameStart = -1;
        if (head.startsWith(kWK)) {
            nameStart = at + 1 + kWK.size();
        } else if (head.startsWith(kPlain)) {
            nameStart = at + 1 + kPlain.size();
        }
        if (nameStart < 0) {
            out += css.at(i);
            ++i;
            continue;
        }
        while (nameStart < n && css.at(nameStart).isSpace()) {
            ++nameStart;
        }
        int nameEnd = nameStart;
        while (nameEnd < n) {
            const QChar c = css.at(nameEnd);
            if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) {
                ++nameEnd;
            } else {
                break;
            }
        }
        const QString name = css.mid(nameStart, nameEnd - nameStart).trimmed();
        const int open = css.indexOf(QLatin1Char('{'), nameEnd);
        if (name.isEmpty() || open < 0) {
            out += css.at(i);
            ++i;
            continue;
        }
        const int close = animationBlockEnd(css, open);
        if (close < 0) {
            // Unbalanced block (corrupt input): leave the text alone instead of
            // swallowing the rest of the stylesheet.
            out += css.at(i);
            ++i;
            continue;
        }
        const QString body = css.mid(open + 1, close - open - 2);
        AnimationFrames frames;
        int p = 0;
        while (p < body.size()) {
            const int selEnd = body.indexOf(QLatin1Char('{'), p);
            if (selEnd < 0) {
                break;
            }
            const int frameEnd = animationBlockEnd(body, selEnd);
            const QString sel = body.mid(p, selEnd - p).trimmed().toLower();
            const QString decls = body.mid(selEnd + 1, frameEnd - selEnd - 2);
            double offset = -1.0;
            if (sel == QLatin1String("from")) {
                offset = 0.0;
            } else if (sel == QLatin1String("to")) {
                offset = 1.0;
            } else if (sel.endsWith(QLatin1Char('%'))) {
                bool ok = false;
                const double v = sel.left(sel.size() - 1).toDouble(&ok);
                if (ok) {
                    offset = v / 100.0;
                }
            }
            if (offset >= 0.0) {
                AnimationStep step;
                step.offset = qBound(0.0, offset, 1.0);
                parseAnimationDeclarations(decls, &step);
                frames.steps.append(step);
            }
            p = frameEnd;
        }
        if (!name.isEmpty() && !frames.steps.isEmpty()) {
            animationRegistry().insert(name, frames);
        }
        out += css.mid(i, at - i);
        i = close;
    }
    return out;
}

// Interpolate the numeric arguments of two transform functions ("translateX(0px)"
// -> "translateX(80px)"); falls back to the discrete lower/upper value when the
// function shapes differ.
inline QString khtmlLerpTransform(const QString &a, const QString &b, double t)
{
    static const QRegularExpression reNum(QStringLiteral("-?\\d+(?:\\.\\d+)?"));
    QStringList an, bn;
    auto ita = reNum.globalMatch(a);
    while (ita.hasNext()) {
        an << ita.next().captured(0);
    }
    auto itb = reNum.globalMatch(b);
    while (itb.hasNext()) {
        bn << itb.next().captured(0);
    }
    if (an.size() != bn.size() || an.isEmpty()) {
        return t < 0.5 ? a : b;
    }
    QString outB = b;
    int pos = 0;
    for (int k = 0; k < bn.size(); ++k) {
        bool ok1 = false, ok2 = false;
        const double v1 = an.at(k).toDouble(&ok1);
        const double v2 = bn.at(k).toDouble(&ok2);
        if (!ok1 || !ok2) {
            return t < 0.5 ? a : b;
        }
        const double v = v1 + (v2 - v1) * t;
        QString num = QString::number(v, 'f', 2);
        if (num.contains(QLatin1Char('.'))) {
            while (num.endsWith(QLatin1Char('0'))) {
                num.chop(1);
            }
            if (num.endsWith(QLatin1Char('.'))) {
                num.chop(1);
            }
        }
        const int idx = outB.indexOf(bn.at(k), pos);
        if (idx < 0) {
            return t < 0.5 ? a : b;
        }
        outB.replace(idx, bn.at(k).size(), num);
        pos = idx + num.size();
    }
    return outB;
}

// Evaluate the animation spec of an element at the current clock time.
// Returns true when at least one animatable property has a value.
inline bool khtmlAnimationLookup(const QString &spec, float *opacity,
                                 QString *transform, QColor *color)
{
    if (opacity) {
        *opacity = 1.0f;
    }
    if (transform) {
        transform->clear();
    }
    if (color) {
        *color = QColor();
    }
    if (spec.isEmpty()) {
        return false;
    }
    const QStringList parts = spec.split(QLatin1Char(';'));
    if (parts.size() < 7) {
        return false;
    }
    auto it = animationRegistry().constFind(parts.at(0));
    if (it == animationRegistry().constEnd() || it.value().steps.size() < 2) {
        return false;
    }
    const QVector<AnimationStep> &steps = it.value().steps;
    const double dur = qMax(1.0, parts.at(1).toDouble());
    const double delay = parts.at(2).toDouble();
    const QString iterStr = parts.at(3).trimmed();
    const int iter = (iterStr.compare(QLatin1String("infinite"), Qt::CaseInsensitive) == 0)
                     ? -1 : qMax(1, iterStr.toInt());
    const QString dir = parts.at(4).trimmed().toLower();
    const double elapsed = animationClockMs();
    if (elapsed < delay) {
        return false;
    }
    double local = elapsed - delay;
    if (iter > 0) {
        local = qMin(local, dur * iter);
    }
    const int cycle = int(local / dur);
    double phase = (local - cycle * dur) / dur;
    if (iter > 0 && cycle >= iter) {
        phase = 1.0;
    }
    if (dir == QLatin1String("reverse")) {
        phase = 1.0 - phase;
    } else if (dir == QLatin1String("alternate") && (cycle % 2) == 1) {
        phase = 1.0 - phase;
    } else if (dir == QLatin1String("alternate-reverse") && (cycle % 2) == 0) {
        phase = 1.0 - phase;
    }
    const AnimationStep *lo = &steps.first();
    const AnimationStep *hi = &steps.last();
    for (int k = 0; k < steps.size(); ++k) {
        if (steps.at(k).offset <= phase) {
            lo = &steps.at(k);
        }
        if (steps.at(k).offset >= phase) {
            hi = &steps.at(k);
            break;
        }
    }
    double localT = 0.0;
    if (hi->offset > lo->offset) {
        localT = (phase - lo->offset) / (hi->offset - lo->offset);
    }
    bool any = false;
    if (opacity && lo->hasOpacity && hi->hasOpacity) {
        *opacity = float(lo->opacity + (hi->opacity - lo->opacity) * localT);
        any = true;
    }
    if (transform && lo->hasTransform && hi->hasTransform) {
        *transform = khtmlLerpTransform(lo->transform, hi->transform, localT);
        any = true;
    }
    if (color && lo->hasColor && hi->hasColor) {
        const QColor &c1 = lo->color;
        const QColor &c2 = hi->color;
        *color = QColor::fromRgbF(c1.redF() + (c2.redF() - c1.redF()) * localT,
                                  c1.greenF() + (c2.greenF() - c1.greenF()) * localT,
                                  c1.blueF() + (c2.blueF() - c1.blueF()) * localT);
        any = true;
    }
    return any;
}

} // namespace khtml

#endif // KHTML_CSS_ANIMATIONS_H
