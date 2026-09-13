/*
    This file is part of the KDE libraries

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
#include "animation_controller.h"
#include "render_object.h"

namespace khtml
{

// Target ~60 fps. The timer is coarse on purpose: sub-millisecond
// precision is wasted on a display that refreshes at 16.7 ms, and a
// slightly slower tick saves meaningful battery on laptops.
static constexpr int FRAME_INTERVAL_MS = 16;

AnimationController::AnimationController(QObject *parent)
    : QObject(parent)
{
    m_frameTimer.setInterval(FRAME_INTERVAL_MS);
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer, &QTimer::timeout, this, &AnimationController::frameTick);
    m_clock.start();
}

AnimationController::~AnimationController() = default;

// --- animations ---

CSSAnimation *AnimationController::startAnimation(RenderObject *owner,
                                                  const QString &name,
                                                  std::shared_ptr<KeyframeList> keyframes)
{
    // Replacing a running animation with the same name restarts it,
    // per CSS Animations spec section on animation-name changes.
    cancelAnimation(owner, name);

    auto anim = std::make_unique<CSSAnimation>(name, std::move(keyframes));
    CSSAnimation *ptr = anim.get();
    m_animations.push_back(AnimationEntry{owner, std::move(anim)});
    ensureTimerRunning();
    return ptr;
}

void AnimationController::cancelAnimation(RenderObject *owner, const QString &name)
{
    m_animations.erase(
        std::remove_if(m_animations.begin(), m_animations.end(),
                       [owner, &name](const AnimationEntry &e) {
                           return e.owner == owner && e.animation->name() == name;
                       }),
        m_animations.end());
    stopTimerIfIdle();
}

void AnimationController::cancelAnimationsFor(RenderObject *owner)
{
    m_animations.erase(
        std::remove_if(m_animations.begin(), m_animations.end(),
                       [owner](const AnimationEntry &e) { return e.owner == owner; }),
        m_animations.end());
    cancelTransitionsFor(owner);
}

// --- transitions ---

void AnimationController::startTransition(RenderObject *owner,
                                          int propertyId,
                                          std::shared_ptr<RenderStyle> from,
                                          std::shared_ptr<RenderStyle> to,
                                          double durationMs,
                                          const TimingFunction &timing)
{
    // If a transition on the same property is already running, retarget
    // it from its current visual position instead of stacking another.
    for (auto &entry : m_transitions) {
        if (entry.owner == owner && entry.transition->propertyId() == propertyId) {
            entry.transition->retarget(std::move(to), durationMs);
            ensureTimerRunning();
            return;
        }
    }

    auto tr = std::make_unique<CSSTransition>(owner, propertyId,
                                              std::move(from), std::move(to),
                                              durationMs, timing);
    m_transitions.push_back(TransitionEntry{owner, std::move(tr)});
    ensureTimerRunning();
}

void AnimationController::cancelTransitionsFor(RenderObject *owner)
{
    m_transitions.erase(
        std::remove_if(m_transitions.begin(), m_transitions.end(),
                       [owner](const TransitionEntry &e) { return e.owner == owner; }),
        m_transitions.end());
    stopTimerIfIdle();
}

// --- requestAnimationFrame ---

int AnimationController::requestAnimationFrame(std::function<void(double)> callback)
{
    int id = m_nextCallbackId++;
    m_frameCallbacks.push_back(FrameCallback{id, std::move(callback)});
    ensureTimerRunning();
    return id;
}

void AnimationController::cancelAnimationFrame(int id)
{
    m_frameCallbacks.erase(
        std::remove_if(m_frameCallbacks.begin(), m_frameCallbacks.end(),
                       [id](const FrameCallback &c) { return c.id == id; }),
        m_frameCallbacks.end());
}

// --- suspend / resume ---

void AnimationController::suspend()
{
    m_suspended = true;
    m_frameTimer.stop();
}

void AnimationController::resume()
{
    m_suspended = false;
    m_lastFrameMs = m_clock.elapsed();
    ensureTimerRunning();
}

bool AnimationController::hasActiveWork() const
{
    return !m_animations.empty() || !m_transitions.empty() || !m_frameCallbacks.empty();
}

// --- frame loop ---

void AnimationController::ensureTimerRunning()
{
    if (!m_suspended && !m_frameTimer.isActive() && hasActiveWork()) {
        m_lastFrameMs = m_clock.elapsed();
        m_frameTimer.start();
    }
}

void AnimationController::stopTimerIfIdle()
{
    if (!hasActiveWork() && m_frameTimer.isActive()) {
        m_frameTimer.stop();
    }
}

void AnimationController::frameTick()
{
    qint64 now = m_clock.elapsed();
    double delta = static_cast<double>(now - m_lastFrameMs);
    m_lastFrameMs = now;

    // Clamp delta so a background tab that wakes up after minutes does
    // not try to catch up on thousands of animation frames at once.
    if (delta > 250.0) {
        delta = FRAME_INTERVAL_MS;
    }

    bool needsPaint = false;

    // rAF callbacks run before style updates so script-driven changes
    // land in the same frame they were requested.
    std::vector<FrameCallback> callbacks;
    callbacks.swap(m_frameCallbacks);
    double timestamp = static_cast<double>(now);
    for (auto &cb : callbacks) {
        cb.callback(timestamp);
    }

    for (auto &entry : m_animations) {
        if (entry.animation->tick(delta)) {
            needsPaint = true;
        }
    }

    for (auto &entry : m_transitions) {
        if (entry.transition->tick(delta)) {
            needsPaint = true;
        }
    }

    // Drop finished transitions. Finished animations with a fill mode
    // stay around until explicitly cancelled; the rest are removed.
    m_transitions.erase(
        std::remove_if(m_transitions.begin(), m_transitions.end(),
                       [](const TransitionEntry &e) { return e.transition->finished(); }),
        m_transitions.end());

    m_animations.erase(
        std::remove_if(m_animations.begin(), m_animations.end(),
                       [](const AnimationEntry &e) {
                           return e.animation->state() == StateFinished
                               && e.animation->isActive() == false;
                       }),
        m_animations.end());

    if (needsPaint) {
        emit needsRepaint();
    }

    stopTimerIfIdle();
}

} // namespace khtml

#include "moc_animation_controller.cpp"
