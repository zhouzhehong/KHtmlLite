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
#ifndef CSS_TIMING_FUNCTION_H
#define CSS_TIMING_FUNCTION_H

#include <algorithm>
#include <cmath>

namespace khtml
{

// Animation easing curves. Kept free of Qt dependencies: the math is
// pure and shared by keyframe animations, transitions and the timing
// code that wants to unit-test it without pulling in RenderStyle.
enum TimingFunctionType {
    TimingLinear,
    TimingEase,
    TimingEaseIn,
    TimingEaseOut,
    TimingEaseInOut,
    TimingStepStart,
    TimingStepEnd,
    TimingCubicBezier,
    TimingSteps
};

struct TimingFunction {
    TimingFunctionType type = TimingEase;

    // cubic-bezier(x1, y1, x2, y2), endpoints are implicitly (0,0)/(1,1)
    double x1 = 0, y1 = 0, x2 = 1, y2 = 1;

    // steps(n, jump-term)
    int steps = 1;
    bool stepStart = false;

    double evaluate(double t) const;
};

// Cubic bezier with implicit endpoints P0=(0,0), P3=(1,1). We solve for
// the parameter given x, then return y(u). Newton-Raphson with a
// bisection fallback stays both fast and robust near steep curves.
class CubicBezier
{
public:
    CubicBezier(double x1, double y1, double x2, double y2)
    {
        cx = 3.0 * x1;
        bx = 3.0 * (x2 - x1) - cx;
        ax = 1.0 - cx - bx;
        cy = 3.0 * y1;
        by = 3.0 * (y2 - y1) - cy;
        ay = 1.0 - cy - by;
    }

    double solve(double x) const
    {
        if (x <= 0.0) return 0.0;
        if (x >= 1.0) return 1.0;
        return sampleY(solveParameterX(x));
    }

private:
    double ax, bx, cx;
    double ay, by, cy;

    double sampleX(double u) const { return ((ax * u + bx) * u + cx) * u; }
    double sampleY(double u) const { return ((ay * u + by) * u + cy) * u; }
    double sampleDX(double u) const { return (3.0 * ax * u + 2.0 * bx) * u + cx; }

    double solveParameterX(double x) const
    {
        double u = x;
        for (int i = 0; i < 8; ++i) {
            double xu = sampleX(u) - x;
            if (std::fabs(xu) < 1e-6) {
                return u;
            }
            double d = sampleDX(u);
            if (std::fabs(d) < 1e-6) {
                break;
            }
            u -= xu / d;
        }
        double lo = 0.0, hi = 1.0;
        u = x;
        while (lo < hi) {
            double xu = sampleX(u);
            if (std::fabs(xu - x) < 1e-6) {
                return u;
            }
            if (x > xu) lo = u; else hi = u;
            u = (lo + hi) * 0.5;
        }
        return u;
    }
};

inline double TimingFunction::evaluate(double t) const
{
    switch (type) {
    case TimingLinear:
        return t;
    case TimingEase: {
        static const CubicBezier c(0.25, 0.1, 0.25, 1.0);
        return c.solve(t);
    }
    case TimingEaseIn: {
        static const CubicBezier c(0.42, 0.0, 1.0, 1.0);
        return c.solve(t);
    }
    case TimingEaseOut: {
        static const CubicBezier c(0.0, 0.0, 0.58, 1.0);
        return c.solve(t);
    }
    case TimingEaseInOut: {
        static const CubicBezier c(0.42, 0.0, 0.58, 1.0);
        return c.solve(t);
    }
    case TimingCubicBezier: {
        CubicBezier c(x1, y1, x2, y2);
        return c.solve(t);
    }
    case TimingStepStart:
        return t >= 0.0 ? 1.0 : 0.0;
    case TimingStepEnd:
        return t >= 1.0 ? 1.0 : 0.0;
    case TimingSteps: {
        if (steps <= 1) {
            return stepStart ? 1.0 : (t >= 1.0 ? 1.0 : 0.0);
        }
        double n = std::floor(t * steps);
        if (stepStart && t > 0.0 && t < 1.0) {
            n += 1.0;
        }
        return std::min(n / steps, 1.0);
    }
    }
    return t;
}

} // namespace khtml

#endif
