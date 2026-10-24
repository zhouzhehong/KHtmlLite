/*
    render_grid.cpp - minimal CSS Grid container layout.

    See render_grid.h for the supported subset.  The template spec comes from
    CSSParser::parseGridTemplateColumns() in the compact form
        "px:100,pct:25,fr:1,auto"   or   "repeat:3,fr:1"   or   "autofit:200"
*/

#include "render_grid.h"

#include "rendering/render_style.h"
#include "khtml_debug.h"

#include <QStringList>
#include <QVector>

namespace khtml
{

RenderGrid::RenderGrid(DOM::NodeImpl *node)
    : RenderBlock(node)
{
}

RenderGrid::~RenderGrid() = default;

const char *RenderGrid::renderName() const
{
    return "RenderGrid";
}

void RenderGrid::layoutBlockChildren(bool relayoutChildren)
{
    const int top = borderTop() + paddingTop();
    const int bottom = borderBottom() + paddingBottom();
    const int left = borderLeft() + paddingLeft();
    const int right = borderRight() + paddingRight();
    const int cw = contentWidth();
    const int gap = style() ? style()->gap() : 0;

    // ------------------------------------------------------------------
    // 1. Collect the in-flow children (positioned/floating are handled by
    //    layoutPositionedObjects() at the end, like the flex container does).
    // ------------------------------------------------------------------
    QVector<RenderBox *> boxes;
    for (RenderObject *child = firstChild(); child; child = child->nextSibling()) {
        if (child->isPositioned() || child->isFloating())
            continue;
        if (!child->isBox())
            continue;
        boxes.append(static_cast<RenderBox *>(child));
    }
    if (boxes.isEmpty()) {
        m_height = m_overflowHeight = top + bottom;
        m_overflowWidth = left + cw + right;
        layoutPositionedObjects(relayoutChildren);
        return;
    }

    // ------------------------------------------------------------------
    // 2. Column template -> column count + widths.
    // ------------------------------------------------------------------
    QStringList tracks;
    QString spec = style() ? style()->gridTemplateColumns() : QString();
    int autoFitMin = 0;
    if (spec.isEmpty() || spec == QLatin1String("none")) {
        tracks.append(QStringLiteral("fr:1"));
    } else if (spec.startsWith(QLatin1String("autofit:"))) {
        autoFitMin = qMax(1, spec.mid(8).toInt());
        const int usable = qMax(0, cw + gap);
        int n = autoFitMin > 0 ? usable / (autoFitMin + gap) : 1;
        n = qBound(1, n, 24);
        for (int i = 0; i < n; ++i)
            tracks.append(QStringLiteral("fr:1"));
    } else if (spec.startsWith(QLatin1String("repeat:"))) {
        const QString rest = spec.mid(7);
        const int comma = rest.indexOf(QLatin1Char(','));
        if (comma <= 0)
            tracks.append(QStringLiteral("fr:1"));
        else {
            const int n = qBound(1, rest.left(comma).toInt(), 64);
            const QString track = rest.mid(comma + 1);
            for (int i = 0; i < n; ++i)
                tracks.append(track);
        }
    } else {
        tracks = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    if (tracks.isEmpty())
        tracks.append(QStringLiteral("fr:1"));

    const int n = tracks.size();
    QVector<int> widths(n, 0);
    int fixedTotal = 0;
    double frTotal = 0.0;
    for (int i = 0; i < n; ++i) {
        const QString t = tracks.at(i);
        if (t.startsWith(QLatin1String("px:"))) {
            widths[i] = qRound(t.mid(3).toDouble());
            fixedTotal += widths[i];
        } else if (t.startsWith(QLatin1String("pct:"))) {
            widths[i] = qRound(cw * t.mid(4).toDouble() / 100.0);
            fixedTotal += widths[i];
        } else if (t.startsWith(QLatin1String("fr:"))) {
            frTotal += qMax(0.0, t.mid(3).toDouble());
        } else if (t == QLatin1String("auto")) {
            frTotal += 1.0;
        } else if (t.startsWith(QLatin1String("minmax:"))) {
            const QStringList mm = t.mid(7).split(QLatin1Char(','));
            int minW = 0;
            if (!mm.isEmpty() && mm[0].startsWith(QLatin1String("px:")))
                minW = qRound(mm[0].mid(3).toDouble());
            if (mm.size() > 1 && mm[1].startsWith(QLatin1String("fr:"))) {
                widths[i] = minW;
                fixedTotal += minW;
                frTotal += qMax(0.0, mm[1].mid(3).toDouble());
            } else {
                widths[i] = minW;
                fixedTotal += minW;
            }
        } else {
            frTotal += 1.0;
        }
    }
    const int gapsTotal = gap * (n - 1);
    const int remaining = qMax(0, cw - fixedTotal - gapsTotal);
    if (frTotal > 0.0) {
        for (int i = 0; i < n; ++i) {
            const QString t = tracks.at(i);
            if (t.startsWith(QLatin1String("fr:")))
                widths[i] += qRound(remaining * qMax(0.0, t.mid(3).toDouble()) / frTotal);
            else if (t == QLatin1String("auto"))
                widths[i] += qRound(remaining * 1.0 / frTotal);
        }
    }

    // ------------------------------------------------------------------
    // 3. Lay the items out at their column width and place them row-major.
    // ------------------------------------------------------------------
    int x = left;
    int y = top;
    int col = 0;
    int rowHeight = 0;
    int totalWidth = 0;
    int maxRight = left;
    for (int i = 0; i < boxes.size(); ++i) {
        RenderBox *box = boxes.at(i);
        const int w = widths.at(col);

        // Stretch auto-width items to the column width (like the flex code).
        const bool autoWidth = box->style() && box->style()->width().isAuto();
        if (autoWidth && w > 0) {
            box->setOverrideWidth(true);
            box->setWidth(w);
            box->setChildNeedsLayout(true);
        }
        if (relayoutChildren || box->needsLayout() || autoWidth) {
            box->setChildNeedsLayout(true);
            box->layout();
        }
        if (autoWidth && w > 0)
            box->setOverrideWidth(false);

        box->setPos(x, y);
        rowHeight = qMax(rowHeight, box->height());
        maxRight = qMax(maxRight, x + qMax<int>(box->width(), w));
        totalWidth = qMax(totalWidth, maxRight - left);

        ++col;
        if (col == n) {
            col = 0;
            x = left;
            y += rowHeight + gap;
            rowHeight = 0;
        } else {
            x += w + gap;
        }
    }
    if (col != 0)
        y += rowHeight;   // last, partially filled row
    else if (y > top)
        y -= gap;         // no trailing gap after the final full row

    m_height = m_overflowHeight = top + qMax(0, y - top) + bottom;
    m_overflowWidth = left + qMax<int>(cw, totalWidth) + right;

    layoutPositionedObjects(relayoutChildren);
}

} // namespace khtml
