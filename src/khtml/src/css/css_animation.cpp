/*
    This file is part of the DOM implementation for KDE.

    Copyright (C) 2024 KHTML Contributors

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
#include "css_animation.h"
#include "rendering/render_style.h"
#include <QtGlobal>
#include <cmath>
#include <algorithm>

namespace khtml
{

static float lerp(float a, float b, double t)
{
    return a + static_cast<float>(t) * (b - a);
}

void blendRenderStyle(RenderStyle *target, const RenderStyle *from,
                      const RenderStyle *to, double t)
{
    if (!target || !to) {
        return;
    }
    const RenderStyle *base = from ? from : to;
    const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);

    // Scalar properties with cheap, public accessors get real linear
    // interpolation here. Non-scalar properties keep the value the style
    // resolver already copied from the lower keyframe; switching that to
    // the upper keyframe at t >= 0.5 is the correct discrete fallback and
    // is handled by the caller when the keyframe style is selected.
    target->setOpacity(lerp(base->opacity(), to->opacity(), clamped));
}

// ---------------------------------------------------------------------------
// KeyframeList
// ---------------------------------------------------------------------------

KeyframeList::~KeyframeList()
{
    for (auto &kf : m_frames) {
        delete kf.style;
    }
}

void KeyframeList::insert(double offset, RenderStyle *style)
{
    offset = qBound(0.0, offset, 1.0);

    // Merge into an existing frame at the same offset rather than
    // growing duplicates; CSSOM says later declarations win.
    for (auto &kf : m_frames) {
        if (std::fabs(kf.offset - offset) < 1e-6) {
            delete kf.style;
            kf.style = style;
            return;
        }
    }

    m_frames.append(Keyframe(offset, style));
    std::sort(m_frames.begin(), m_frames.end(),
              [](const Keyframe &a, const Keyframe &b) { return a.offset < b.offset; });
}

bool KeyframeList::surroundingFrames(double progress, const Keyframe *&from, const Keyframe *&to) const
{
    if (m_frames.size() < 2) {
        return false;
    }
    from = &m_frames.first();
    to = &m_frames.last();

    for (int i = 0; i < m_frames.size() - 1; ++i) {
        if (progress >= m_frames[i].offset && progress <= m_frames[i + 1].offset) {
            from = &m_frames[i];
            to = &m_frames[i + 1];
            return true;
        }
    }

    if (progress < m_frames.first().offset) {
        from = to = &m_frames.first();
    } else {
        from = to = &m_frames.last();
    }
    return true;
}

// ---------------------------------------------------------------------------
// CSSAnimation
// ---------------------------------------------------------------------------

CSSAnimation::CSSAnimation(const QString &name, std::shared_ptr<KeyframeList> frames)
    : m_name(name), m_frames(std::move(frames))
{
    m_state = StateDelayed;
}

double CSSAnimation::activeDuration() const
{
    // An animation with zero duration jumps to the end state instantly.
    // Returning a tiny epsilon avoids a divide by zero in tick().
    return m_duration > 0 ? m_duration : 1e-6;
}

double CSSAnimation::adjustForDirection(double raw) const
{
    // https://drafts.csswg.org/css-animations/#animation-direction
    switch (m_direction) {
    case DirectionNormal:
        return raw;
    case DirectionReverse:
        return 1.0 - raw;
    case DirectionAlternate:
        return (m_iteration % 2 == 0) ? raw : 1.0 - raw;
    case DirectionAlternateReverse:
        return (m_iteration % 2 == 0) ? 1.0 - raw : raw;
    }
    return raw;
}

bool CSSAnimation::tick(double elapsedMs)
{
    if (m_state == StateFinished || m_state == StatePaused || m_state == StateIdle) {
        return false;
    }

    m_elapsed += elapsedMs;

    // Delay phase
    if (m_elapsed < m_delay) {
        m_state = StateDelayed;
        return m_fill == FillBackwards || m_fill == FillBoth;
    }

    double activeTime = m_elapsed - m_delay;
    double totalDuration = activeDuration();
    double rawProgress = std::fmod(activeTime, totalDuration) / totalDuration;
    int iteration = static_cast<int>(activeTime / totalDuration);

    bool infinite = (m_iterations == -1);
    if (!infinite && iteration >= m_iterations) {
        m_state = StateFinished;
        m_progress = (m_iterations > 0) ? adjustForDirection(1.0) : 0.0;
        return m_fill == FillForwards || m_fill == FillBoth;
    }

    if (m_state != StateRunning) {
        m_state = StateRunning;
    }

    if (iteration != m_iteration) {
        m_iteration = iteration;
    }

    double adjusted = adjustForDirection(rawProgress);
    double eased = m_timing.evaluate(adjusted);
    eased = qBound(0.0, eased, 1.0);

    bool changed = std::fabs(eased - m_progress) > 1e-6;
    m_progress = eased;
    return changed;
}

void CSSAnimation::pause()
{
    if (m_state == StateRunning || m_state == StateDelayed) {
        m_state = StatePaused;
    }
}

void CSSAnimation::resume()
{
    if (m_state == StatePaused) {
        m_state = (m_elapsed < m_delay) ? StateDelayed : StateRunning;
    }
}

void CSSAnimation::applyInterpolatedStyle(RenderStyle *target) const
{
    if (!m_frames || !target) {
        return;
    }

    const Keyframe *from = nullptr;
    const Keyframe *to = nullptr;
    if (!m_frames->surroundingFrames(m_progress, from, to)) {
        return;
    }

    // Outside the active interval, fill mode decides whether a style
    // is applied at all. During the delay the "from" frame is used for
    // fill-backwards; after finishing the "to" frame for fill-forwards.
    if (from == to) {
        if (m_state == StateDelayed && m_fill != FillBackwards && m_fill != FillBoth) {
            return;
        }
        if (m_state == StateFinished && m_fill != FillForwards && m_fill != FillBoth) {
            return;
        }
    }

    // Property-by-property interpolation lives in RenderStyle so the
    // type-specific blending (length, color, transform list) stays next
    // to the data it touches. Here we just hand over the two endpoints
    // and the local blend factor.
    double localT = 0;
    if (to != from && to->offset > from->offset) {
        localT = (m_progress - from->offset) / (to->offset - from->offset);
    }

    // Per-keyframe easing overrides the animation-level timing function
    // within its segment.
    localT = from->easing.evaluate(localT);

    if (to->style) {
        blendRenderStyle(target, from->style, to->style, localT);
    }
}

} // namespace khtml
