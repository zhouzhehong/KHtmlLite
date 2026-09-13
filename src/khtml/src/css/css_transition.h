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
#ifndef CSS_TRANSITION_H
#define CSS_TRANSITION_H

#include "css_animation.h"
#include <QHash>
#include <QString>

namespace khtml
{

class RenderObject;
class RenderStyle;

// One in-flight property transition. Unlike CSSAnimation, a transition
// has no keyframes: it interpolates a single property from the previous
// computed value to the new one over a fixed duration.
class CSSTransition
{
public:
    CSSTransition(RenderObject *owner, int propertyId,
                  std::shared_ptr<RenderStyle> from,
                  std::shared_ptr<RenderStyle> to,
                  double durationMs,
                  const TimingFunction &timing);

    // Returns true while the transition is still running. Once it
    // returns false the controller removes it.
    bool tick(double elapsedMs);

    // Called when the target value changes mid-transition. Instead of
    // restarting from scratch we reverse from the current position,
    // which is what makes hovering back and forth feel smooth.
    void retarget(std::shared_ptr<RenderStyle> newTo, double durationMs);

    bool finished() const { return m_finished; }
    double progress() const { return m_progress; }
    int propertyId() const { return m_property; }
    RenderObject *owner() const { return m_owner; }

    void writeInterpolatedStyle(RenderStyle *target) const;

private:
    RenderObject *m_owner;
    int m_property;
    std::shared_ptr<RenderStyle> m_from;
    std::shared_ptr<RenderStyle> m_to;
    double m_duration;
    double m_elapsed = 0;
    double m_progress = 0;
    TimingFunction m_timing;
    bool m_finished = false;
};

} // namespace khtml

#endif
