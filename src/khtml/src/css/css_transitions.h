/*
    This file is part of the KHTML library.

    Engine-native CSS transition support (paint-only practical subset).

    The transition shorthand is parsed into a spec string on the style
    ("property;durationMs;delayMs;timingFunction").  When a style change
    replaces a RenderObject's style, RenderObject::setStyle() calls
    khtmlStartTransition() with the old and the new style; the captured
    from/to values live in a registry keyed by the render object and the paint
    callback (RenderBlock::paint) interpolates them with the animation clock.

    Supported transition properties: all / opacity / transform /
    background-color / background / color (all mapped onto the three paint-time
    values).  Layout properties are NOT animated (the box would have to
    relayout every frame); they change instantly.
*/

#ifndef KHTML_CSS_TRANSITIONS_H
#define KHTML_CSS_TRANSITIONS_H

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "css_animations.h"
#include "../rendering/render_style.h"

namespace khtml
{

struct TransitionState {
    QString property;      // transitioned property (as written)
    double startMs = 0.0;  // animationClockMs() at the style change
    double durationMs = 0.0;
    double delayMs = 0.0;
    QString timing;
    bool hasOpacity = false;
    float fromOpacity = 1.0f;
    float toOpacity = 1.0f;
    bool hasTransform = false;
    QString fromTransform;
    QString toTransform;
    bool hasColor = false;
    QColor fromColor;
    QColor toColor;
};

inline QHash<const void *, TransitionState> &transitionRegistry()
{
    static QHash<const void *, TransitionState> registry;
    return registry;
}

inline bool khtmlTransitionPropertyMatches(const QString &prop, const QString &want)
{
    return prop == QLatin1String("all") || prop == want
        || (want == QLatin1String("background-color")
            && (prop == QLatin1String("background") || prop == QLatin1String("all")))
        || (want == QLatin1String("color") && prop == QLatin1String("color"));
}

// Capture the animatable value difference between |from| and |to|.  Called
// from RenderObject::setStyle() while the old style is still current.
inline void khtmlStartTransition(const void *obj, const RenderStyle *from, const RenderStyle *to)
{
    if (!obj || !from || !to) {
        return;
    }
    const QString spec = to->transition();
    if (spec.isEmpty()) {
        return;
    }
    const QStringList parts = spec.split(QLatin1Char(';'));
    if (parts.size() < 2) {
        return;
    }
    const QString prop = parts.at(0).trimmed().toLower();
    const double durMs = qMax(1.0, parts.at(1).toDouble());
    const double delayMs = parts.size() > 2 ? parts.at(2).toDouble() : 0.0;
    const QString timing = parts.size() > 3 ? parts.at(3).trimmed().toLower()
                                           : QStringLiteral("ease");
    TransitionState st;
    st.property = prop;
    st.durationMs = durMs;
    st.delayMs = delayMs;
    st.timing = timing;

    if (khtmlTransitionPropertyMatches(prop, QStringLiteral("opacity"))) {
        const float fo = from->opacity();
        const float ft = to->opacity();
        if (!qFuzzyCompare(fo + 1.0f, ft + 1.0f)) {
            st.hasOpacity = true;
            st.fromOpacity = fo;
            st.toOpacity = ft;
        }
    }
    if (khtmlTransitionPropertyMatches(prop, QStringLiteral("transform"))) {
        const QString ft = from->transform();
        const QString tt = to->transform();
        if (ft != tt) {
            st.hasTransform = true;
            st.fromTransform = ft;
            st.toTransform = tt;
        }
    }
    if (khtmlTransitionPropertyMatches(prop, QStringLiteral("background-color"))
        || khtmlTransitionPropertyMatches(prop, QStringLiteral("color"))) {
        const QColor fc = from->backgroundColor();
        const QColor tc = to->backgroundColor();
        if (fc != tc) {
            st.hasColor = true;
            st.fromColor = fc;
            st.toColor = tc;
        }
    }
    if (!st.hasOpacity && !st.hasTransform && !st.hasColor) {
        return;
    }
    st.startMs = animationClockMs();
    transitionRegistry().insert(obj, st);
}

// Interpolate the captured transition at the current clock time.  Returns true
// when this object still has a transition; the entry is dropped once finished.
inline bool khtmlTransitionLookup(const void *obj, float *opacity,
                                  QString *transform, QColor *color)
{
    auto it = transitionRegistry().find(obj);
    if (it == transitionRegistry().end()) {
        return false;
    }
    const TransitionState st = it.value();
    const double elapsed = animationClockMs() - st.startMs;
    double t = (elapsed - st.delayMs) / st.durationMs;
    if (t < 0.0) {
        t = 0.0;
    }
    const bool done = (t >= 1.0);
    if (t > 1.0) {
        t = 1.0;
    }
    // Timing functions other than linear fall back to linear ("够用即止").
    bool any = false;
    if (st.hasOpacity && opacity) {
        *opacity = st.fromOpacity + (st.toOpacity - st.fromOpacity) * float(t);
        any = true;
    }
    if (st.hasTransform && transform) {
        *transform = done ? st.toTransform
                          : khtmlLerpTransform(st.fromTransform, st.toTransform, t);
        any = true;
    }
    if (st.hasColor && color) {
        *color = QColor::fromRgbF(st.fromColor.redF() + (st.toColor.redF() - st.fromColor.redF()) * t,
                                  st.fromColor.greenF() + (st.toColor.greenF() - st.fromColor.greenF()) * t,
                                  st.fromColor.blueF() + (st.toColor.blueF() - st.fromColor.blueF()) * t);
        any = true;
    }
    if (done) {
        transitionRegistry().erase(it);
    }
    return any;
}

// Expire finished transitions and report whether any is still running.
inline bool khtmlTransitionsActive()
{
    const double now = animationClockMs();
    for (auto it = transitionRegistry().begin(); it != transitionRegistry().end();) {
        const TransitionState &st = it.value();
        if (now - st.startMs >= st.delayMs + st.durationMs) {
            it = transitionRegistry().erase(it);
        } else {
            ++it;
        }
    }
    return !transitionRegistry().isEmpty();
}

} // namespace khtml

#endif // KHTML_CSS_TRANSITIONS_H
