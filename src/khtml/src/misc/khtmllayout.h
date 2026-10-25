/*
    This file is part of the KDE libraries

    Copyright (C) 1999 Lars Knoll (knoll@kde.org)
    Copyright (C) 2006, 2008 Apple Inc. All rights reserved.

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

    This widget holds some useful definitions needed for layouting Elements
*/
#ifndef HTML_LAYOUT_H
#define HTML_LAYOUT_H

#include <QVector>
#include <math.h>
#include <assert.h>

/*
 * this namespace contains definitions for various types needed for
 * layouting.
 */
namespace khtml
{

const int UNDEFINED = -1;
const int PERCENT_SCALE_FACTOR = 128;

// alignment
enum VAlign { VNone = 0, Bottom, VCenter, Top, Baseline };
enum HAlign { HDefault, Left, HCenter, Right, HNone = 0 };

/*
 * %multiLength and %Length
 */
enum LengthType { Auto = 0, Relative, Percent, Fixed, Static, Calc };
struct Length {
Length() : m_value(0), m_calcValue(0) {}

Length(LengthType t): m_value(t), m_calcValue(0) {}

    Length(int v, LengthType t, bool q = false)
: m_value((v * 16) | (q << 3) | t), m_calcValue(0)
    {
        assert(t != Percent);
    }

    Length(double v, LengthType t, bool q = false)
: m_value(static_cast<int>(v *PERCENT_SCALE_FACTOR) * 16 | (q << 3) | t), m_calcValue(0)
    {
        assert(t == Percent);
    }

    bool operator==(const Length &o) const
    {
        return m_value == o.m_value && m_calcValue == o.m_calcValue;
    }
    bool operator!=(const Length &o) const
    {
        return m_value != o.m_value || m_calcValue != o.m_calcValue;
    }

    // CSS calc(): a percentage part plus a fixed (px) part.
    static Length calc(double percent, int px)
    {
        Length l;
        l.m_value = static_cast<int>(percent * PERCENT_SCALE_FACTOR) * 16 | Calc;
        l.m_calcValue = px;
        return l;
    }
    bool isCalc() const
    {
        return type() == Calc;
    }
    int calcPx() const
    {
        return m_calcValue;
    }
    double calcPercent() const
    {
        return static_cast<double>(rawValue()) / PERCENT_SCALE_FACTOR;
    }
    // Resolve the calc() percentage against |basis| and add the px part.
    int calcValue(int basis) const
    {
        return basis * rawValue() / (100 * PERCENT_SCALE_FACTOR) + m_calcValue;
    }


    int value() const
    {
        assert(type() != Percent);
        if (type() == Calc)
            return m_calcValue;
        return rawValue();
    }

    int rawValue() const
    {
        return (m_value & ~0xF) / 16;
    }

    double percent() const
    {
        assert(type() == Percent);
        return static_cast<double>(rawValue()) / PERCENT_SCALE_FACTOR;
    }

    LengthType type() const
    {
        return static_cast<LengthType>(m_value & 7);
    }

    void setValue(LengthType t, int value)
    {
        assert(t != Percent);
        setRawValue(t, value);
    }

    void setRawValue(LengthType t, int value)
    {
        m_value = value * 16 | (m_value & 0x8) | t;
    }

    void setValue(int value)
    {
        assert(!value || type() != Percent);
        setRawValue(value);
    }

    void setRawValue(int value)
    {
        m_value = value * 16 | (m_value & 0xF);
    }

    void setValue(LengthType t, double value)
    {
        assert(t == Percent);
        m_value = static_cast<int>(value * PERCENT_SCALE_FACTOR) * 16 | (m_value & 0x8) | t;
    }

    void setValue(double value)
    {
        assert(type() == Percent);
        m_value = static_cast<int>(value * PERCENT_SCALE_FACTOR) * 16 | (m_value & 0xF);
    }

    /*
     * works only for certain types, returns UNDEFINED otherwise
     */
    int width(int maxValue) const
    {
        switch (type()) {
        case Fixed:
            return value();
        case Percent:
            return maxValue * rawValue() / (100 * PERCENT_SCALE_FACTOR);
        case Calc:
            return calcValue(maxValue);
        case Auto:
            return maxValue;
        default:
            return UNDEFINED;
        }
    }
    /*
     * returns the minimum width value which could work...
     */
    int minWidth(int maxValue) const
    {
        switch (type()) {
        case Fixed:
            return value();
        case Percent:
            return maxValue * rawValue() / (100 * PERCENT_SCALE_FACTOR);
        case Calc:
            return calcValue(maxValue);
        case Auto:
        default:
            return 0;
        }
    }

    int minWidthRounded(int maxValue) const
    {
        switch (type()) {
        case Fixed:
            return value();
        case Percent:
            return static_cast<int>(round(maxValue * percent() / 100.0));
        case Calc:
            return calcValue(maxValue);
        case Auto:
        default:
            return 0;
        }
    }

    bool isUndefined() const
    {
        return rawValue() == UNDEFINED;
    }
    bool isZero() const
    {
        return !(m_value & ~0xF);
    }
    bool isPositive() const
    {
        return rawValue() > 0;
    }
    bool isNegative() const
    {
        return rawValue() < 0;
    }

    bool isAuto() const
    {
        return type() == Auto;
    }
    bool isRelative() const
    {
        return type() == Relative;
    }
    bool isPercent() const
    {
        return type() == Percent;
    }
    bool isFixed() const
    {
        return type() == Fixed;
    }
    bool isQuirk() const
    {
        return (m_value >> 3) & 1;
    }

private:
    int m_value;
    int m_calcValue;
};

}

#endif
