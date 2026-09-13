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
#ifndef KHTML_ANIMATION_CONTROLLER_H
#define KHTML_ANIMATION_CONTROLLER_H

#include "../css/css_animation.h"
#include "../css/css_transition.h"
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <vector>
#include <functional>
#include <memory>

namespace khtml
{

class RenderObject;

// Owns every running animation and transition for one document and
// drives them off a single 16 ms timer. One controller per KHTMLPart;
// the timer is suspended while nothing is animating so idle pages
// cost zero wakeups.
class AnimationController : public QObject
{
    Q_OBJECT
public:
    explicit AnimationController(QObject *parent = nullptr);
    ~AnimationController() override;

    // --- CSS Animations ---
    // Start (or replace) an animation on an element. Returns the
    // instance so the caller can wire up style resolution.
    CSSAnimation *startAnimation(RenderObject *owner,
                                 const QString &name,
                                 std::shared_ptr<KeyframeList> keyframes);
    void cancelAnimation(RenderObject *owner, const QString &name);
    void cancelAnimationsFor(RenderObject *owner);

    // --- CSS Transitions ---
    // Called by style resolution when a computed value changed and a
    // matching transition-property rule exists.
    void startTransition(RenderObject *owner,
                         int propertyId,
                         std::shared_ptr<RenderStyle> from,
                         std::shared_ptr<RenderStyle> to,
                         double durationMs,
                         const TimingFunction &timing);
    void cancelTransitionsFor(RenderObject *owner);

    // --- requestAnimationFrame ---
    // Returns an id usable with cancelAnimationFrame. Callback receives
    // the timestamp in milliseconds since document navigation start.
    int requestAnimationFrame(std::function<void(double)> callback);
    void cancelAnimationFrame(int id);

    // Pause all animation work (tab hidden, print mode).
    void suspend();
    void resume();
    bool isSuspended() const { return m_suspended; }

    // True when at least one animation, transition, or rAF callback
    // is pending. The view uses this to decide whether to keep painting.
    bool hasActiveWork() const;

Q_SIGNALS:
    // Emitted after a frame tick when one or more elements need a
    // repaint. The view connects this to its update slot.
    void needsRepaint();

private Q_SLOTS:
    void frameTick();

private:
    struct AnimationEntry {
        RenderObject *owner;
        std::unique_ptr<CSSAnimation> animation;
    };
    struct TransitionEntry {
        RenderObject *owner;
        std::unique_ptr<CSSTransition> transition;
    };
    struct FrameCallback {
        int id;
        std::function<void(double)> callback;
    };

    // std::vector (not QVector) because the entries hold unique_ptrs and
    // are therefore move-only; Qt 5's QVector still copies on grow.
    std::vector<AnimationEntry> m_animations;
    std::vector<TransitionEntry> m_transitions;
    std::vector<FrameCallback> m_frameCallbacks;

    QTimer m_frameTimer;
    QElapsedTimer m_clock;
    qint64 m_lastFrameMs = 0;
    int m_nextCallbackId = 1;
    bool m_suspended = false;

    void ensureTimerRunning();
    void stopTimerIfIdle();
};

} // namespace khtml

#endif
