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
#include "css_transition.h"
#include "css_animation.h"
#include "rendering/render_object.h"
#include "rendering/render_style.h"
#include <QtGlobal>

namespace khtml
{

CSSTransition::CSSTransition(RenderObject *owner, int propertyId,
                             std::shared_ptr<RenderStyle> from,
                             std::shared_ptr<RenderStyle> to,
                             double durationMs,
                             const TimingFunction &timing)
    : m_owner(owner)
    , m_property(propertyId)
    , m_from(std::move(from))
    , m_to(std::move(to))
    , m_duration(qMax(0.0, durationMs))
    , m_timing(timing)
{
}

bool CSSTransition::tick(double elapsedMs)
{
    if (m_finished) {
        return false;
    }
    if (m_duration <= 0) {
        m_progress = 1;
        m_finished = true;
        return true;
    }

    m_elapsed += elapsedMs;
    double raw = qBound(0.0, m_elapsed / m_duration, 1.0);
    m_progress = m_timing.evaluate(raw);

    if (raw >= 1.0) {
        m_progress = 1.0;
        m_finished = true;
    }
    return true;
}

void CSSTransition::retarget(std::shared_ptr<RenderStyle> newTo, double durationMs)
{
    // Snapshot the current visual state as the new starting point so
    // reversing does not jump.
    if (m_from && m_to) {
        // Snapshot the current visual value: start from a full copy of the
        // lower endpoint so non-interpolated properties stay stable, then
        // overwrite the interpolatable scalars at the current progress.
        auto current = std::make_shared<RenderStyle>(*m_from);
        blendRenderStyle(current.get(), m_from.get(), m_to.get(), m_progress);
        m_from = std::move(current);
    }
    m_to = std::move(newTo);
    m_elapsed = 0;
    m_duration = qMax(0.0, durationMs);
    m_finished = false;
}

void CSSTransition::writeInterpolatedStyle(RenderStyle *target) const
{
    if (!target || !m_from || !m_to) {
        return;
    }
    blendRenderStyle(target, m_from.get(), m_to.get(), m_progress);
}

} // namespace khtml
