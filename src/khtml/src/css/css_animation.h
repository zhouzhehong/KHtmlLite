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
#ifndef CSS_ANIMATION_H
#define CSS_ANIMATION_H

#include <QString>
#include <QHash>
#include <QVector>
#include <QElapsedTimer>
#include <memory>

#include "timing_function.h"

namespace DOM
{
class CSSProperty;
}

namespace khtml
{

class RenderStyle;

// Linearly interpolate the animatable numeric properties of two computed
// styles into target. Properties without a scalar accessor fall back to the
// lower keyframe, the CSS-defined discrete behaviour for values that cannot
// be interpolated. 'from' may be null, in which case 'to' is the base.
void blendRenderStyle(RenderStyle *target, const RenderStyle *from,
                      const RenderStyle *to, double t);

// One resolved keyframe. Properties are stored as raw CSS declarations
// and applied through the normal style cascade so animations inherit
// specificity handling for free.
struct Keyframe {
    double offset;            // 0.0 .. 1.0
    RenderStyle *style;       // resolved style at this offset, owned by the animation
    TimingFunction easing;    // per-keyframe easing override

    Keyframe(double off, RenderStyle *s) : offset(off), style(s) {}
};

// Parsed @keyframes rule. Shared across all elements that reference
// the same animation name; refcounted through Shared.
class KeyframeList
{
public:
    explicit KeyframeList(const QString &name) : m_name(name) {}
    ~KeyframeList();

    const QString &name() const { return m_name; }

    // Insert a keyframe, keeping the vector sorted by offset. Duplicate
    // offsets merge property-by-property (later wins on conflict).
    void insert(double offset, RenderStyle *style);

    const QVector<Keyframe> &frames() const { return m_frames; }

    // Find the surrounding keyframes for a given progress value.
    // Returns false when the list has fewer than two entries.
    bool surroundingFrames(double progress, const Keyframe *&from, const Keyframe *&to) const;

private:
    QString m_name;
    QVector<Keyframe> m_frames;
};

enum AnimationFillMode {
    FillNone,
    FillForwards,
    FillBackwards,
    FillBoth
};

enum AnimationDirection {
    DirectionNormal,
    DirectionReverse,
    DirectionAlternate,
    DirectionAlternateReverse
};

enum AnimationPlayState {
    StateIdle,
    StateDelayed,
    StateRunning,
    StatePaused,
    StateFinished
};

// A running animation attached to one element. The controller ticks
// these every frame; the animation interpolates its keyframes and
// writes the result into a mutable RenderStyle.
class CSSAnimation
{
public:
    CSSAnimation(const QString &name, std::shared_ptr<KeyframeList> frames);

    void setDuration(double ms) { m_duration = qMax(0.0, ms); }
    void setDelay(double ms) { m_delay = ms; }
    void setIterationCount(int count) { m_iterations = count; } // -1 = infinite
    void setDirection(AnimationDirection d) { m_direction = d; }
    void setFillMode(AnimationFillMode m) { m_fill = m; }
    void setTimingFunction(const TimingFunction &tf) { m_timing = tf; }

    // Advance the animation by elapsed milliseconds. Returns true when
    // the active style changed and the element needs repaint.
    bool tick(double elapsedMs);

    void pause();
    void resume();

    // Current iteration, zero-based.
    int currentIteration() const { return m_iteration; }

    // Overall progress within one iteration, already adjusted for
    // direction. Range 0..1.
    double iterationProgress() const { return m_progress; }

    AnimationPlayState state() const { return m_state; }
    bool isActive() const { return m_state == StateRunning || m_state == StateDelayed; }

    // Blend two keyframe styles and write into target. Called by the
    // style resolver; exposed here so tests can drive it directly.
    void applyInterpolatedStyle(RenderStyle *target) const;

    const QString &name() const { return m_name; }

private:
    double activeDuration() const;
    double adjustForDirection(double raw) const;

    QString m_name;
    std::shared_ptr<KeyframeList> m_frames;

    double m_duration = 0;
    double m_delay = 0;
    int m_iterations = 1;
    AnimationDirection m_direction = DirectionNormal;
    AnimationFillMode m_fill = FillNone;
    TimingFunction m_timing;

    double m_elapsed = 0;       // total elapsed including delay
    double m_progress = 0;      // 0..1 within current iteration
    int m_iteration = 0;
    AnimationPlayState m_state = StateIdle;
};

} // namespace khtml

#endif
