/*
    render_flexbox.cpp 鈥?minimal flex container layout.

    Algorithm: parse flex settings from the element's inline style, lay out
    each in-flow child to learn its size, distribute any free space to
    flex-grow children, then place items along the main axis honouring
    justify-content / align-items / gap.
*/

#include "render_flexbox.h"
#include "rendering/render_style.h"
#include "xml/dom_elementimpl.h"
#include <QVector>
#include <QRegularExpression>

using namespace DOM;

namespace khtml
{

RenderFlexBox::RenderFlexBox(DOM::NodeImpl *node)
    : RenderBlock(node)
{
    // Flex containers establish a block-formatting context for their
    // children; we never inline them.
    setChildrenInline(false);
}

RenderFlexBox::~RenderFlexBox()
{
}

const char *RenderFlexBox::renderName() const
{
    return "RenderFlexBox";
}

static QString inlineStyleOf(const RenderObject *obj)
{
    if (!obj->node() || !obj->node()->isElementNode())
        return QString();
    ElementImpl *el = static_cast<ElementImpl *>(obj->node());
    return el->getAttribute(QString::fromLatin1("style")).string();
}

RenderFlexBox::FlexConfig RenderFlexBox::config() const
{
    FlexConfig cfg;
    if (style()) {
        switch (style()->flexDirection()) {
        case 1: cfg.dir = FlexDirRowReverse; break;
        case 2: cfg.dir = FlexDirColumn; break;
        case 3: cfg.dir = FlexDirColumnReverse; break;
        default: cfg.dir = FlexDirRow;
        }
        cfg.justify = static_cast<FlexJustify>(style()->justifyContent());
        cfg.align = static_cast<FlexAlign>(style()->alignItems());
        cfg.gap = style()->gap();
    }

    // Elements styled via inline attributes bypass the stylesheet pipeline for
    // these non-standard properties; fall back to parsing the attribute.
    const QString style = inlineStyleOf(this);
    if (style.isEmpty())
        return cfg;

    if (style.contains(QStringLiteral("column-reverse"), Qt::CaseInsensitive)) {
        cfg.dir = FlexDirColumnReverse;
    } else if (style.contains(QStringLiteral("row-reverse"), Qt::CaseInsensitive)) {
        cfg.dir = FlexDirRowReverse;
    } else if (style.contains(QStringLiteral("column"), Qt::CaseInsensitive)) {
        cfg.dir = FlexDirColumn;
    }

    const QRegularExpression jc(QStringLiteral("justify-content\\s*:\\s*(\\w+(?:-\\w+)?)"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch jm = jc.match(style);
    if (jm.hasMatch()) {
        const QString v = jm.captured(1).toLower();
        if (v == QLatin1String("center"))
            cfg.justify = JustifyCenter;
        else if (v == QLatin1String("flex-end"))
            cfg.justify = JustifyFlexEnd;
        else if (v == QLatin1String("space-between"))
            cfg.justify = JustifySpaceBetween;
        else if (v == QLatin1String("space-around"))
            cfg.justify = JustifySpaceAround;
    }

    const QRegularExpression ai(QStringLiteral("align-items\\s*:\\s*(\\w+(?:-\\w+)?)"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch am = ai.match(style);
    if (am.hasMatch()) {
        const QString v = am.captured(1).toLower();
        if (v == QLatin1String("center"))
            cfg.align = AlignCenter;
        else if (v == QLatin1String("flex-end"))
            cfg.align = AlignFlexEnd;
        else if (v == QLatin1String("flex-start"))
            cfg.align = AlignFlexStart;
    }

    const QRegularExpression gap(QStringLiteral("(?:gap|column-gap)\\s*:\\s*(\\d+(?:\\.\\d+)?)px"),
                                 QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch gm = gap.match(style);
    if (gm.hasMatch())
        cfg.gap = qMax(0, qRound(gm.captured(1).toDouble()));
    return cfg;
}

// Read flex-grow / "flex: N" from a child (stylesheet first, inline attr second).
static int childGrow(const RenderObject *child)
{
    if (child->style() && child->style()->flexGrow() > 0)
        return qRound(child->style()->flexGrow());
    const QString style = inlineStyleOf(child);
    if (style.isEmpty())
        return 0;
    const QRegularExpression re(QStringLiteral("(?:flex-grow|flex)\\s*:\\s*(\\d+)"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(style);
    if (!m.hasMatch())
        return 0;
    return qMax(0, m.captured(1).toInt());
}

void RenderFlexBox::layoutBlockChildren(bool relayoutChildren)
{
    const FlexConfig cfg = config();
    const bool row = (cfg.dir == FlexDirRow || cfg.dir == FlexDirRowReverse);

    const int top = borderTop() + paddingTop();
    const int bottom = borderBottom() + paddingBottom();
    const int left = borderLeft() + paddingLeft();
    const int right = borderRight() + paddingRight();
    const int cw = contentWidth();
    const int ch = contentHeight();

    struct Item {
        RenderBox *box;
        int baseW;
        int baseH;
        int grow;
        int autoMainBefore;
        int autoMainAfter;
    };
    QVector<Item> items;

    // First pass: lay out every in-flow child, collect its natural size.
    for (RenderObject *child = firstChild(); child; child = child->nextSibling()) {
        if (child->isPositioned() || child->isFloating())
            continue;
        if (!child->isBox())
            continue; // anonymous inlines inside flex are wrapped as boxes already

        RenderBox *box = static_cast<RenderBox *>(child);
        if (relayoutChildren || child->needsLayout()) {
            child->setChildNeedsLayout(true);
            box->layout();
        }

        // setChildrenInline(false) wraps inline-level children (e.g. inline-block)
        // in anonymous blocks whose width stretches to the container.  For those
        // we must use the content max-width as the shrink-to-fit size, otherwise
        // every flex item ends up container-wide and overflows the viewport.
        int itemW = box->width();
        if (itemW >= cw && cw > 0) {
            int contentMax = box->maxWidth();
            int contentMin = box->minWidth();
            if (contentMax > 0 && contentMax < cw) {
                itemW = qMax(contentMin, contentMax);
                // Pin the width so calcWidth() does not stretch it back to cw
                // during layout; children must be laid out at the flex base size.
                box->setOverrideWidth(true);
                box->setWidth(itemW);
                child->setChildNeedsLayout(true);
                box->layout();
                box->setOverrideWidth(false);
            }
        }

        // Detect auto margins on the main axis.  -1 means "not auto"; >=0 means
        // "auto, to be filled with distributed free space".
        int autoBefore = -1, autoAfter = -1;
        if (box->style()) {
            if (row) {
                if (box->style()->marginLeft().isAuto())  autoBefore = 0;
                if (box->style()->marginRight().isAuto()) autoAfter  = 0;
            } else {
                if (box->style()->marginTop().isAuto())    autoBefore = 0;
                if (box->style()->marginBottom().isAuto()) autoAfter  = 0;
            }
        }

        items.append({box, itemW, box->height(), childGrow(child), autoBefore, autoAfter});
    }

    if (items.isEmpty()) {
        m_height = m_overflowHeight = top + bottom;
        m_overflowWidth = left + right;
        layoutPositionedObjects(relayoutChildren);
        return;
    }

    int used = 0;
    for (const Item &it : items)
        used += row ? it.baseW : it.baseH;
    used += cfg.gap * (items.size() - 1);
    const int free = row ? (cw - used) : (ch - used);

    // Second pass: grow children share any free space.
    int growSum = 0;
    for (const Item &it : items)
        growSum += it.grow;
    if (free > 0 && growSum > 0) {
        for (Item &it : items) {
            if (it.grow <= 0)
                continue;
            const int share = (free * it.grow) / growSum;
            if (row)
                it.box->setWidth(it.baseW + share);
            else
                it.box->setHeight(it.baseH + share);
            it.baseW = it.box->width();
            it.baseH = it.box->height();
        }
    }

    // Recompute free space after flex-grow; auto margins absorb what remains.
    int usedAfterGrow = 0;
    for (const Item &it : items)
        usedAfterGrow += row ? it.baseW : it.baseH;
    usedAfterGrow += cfg.gap * (items.size() - 1);
    int freeAfterGrow = (row ? cw : ch) - usedAfterGrow;

    int autoCount = 0;
    for (const Item &it : items) {
        if (it.autoMainBefore >= 0) ++autoCount;
        if (it.autoMainAfter  >= 0) ++autoCount;
    }
    if (freeAfterGrow > 0 && autoCount > 0) {
        const int share = freeAfterGrow / autoCount;
        for (Item &it : items) {
            if (it.autoMainBefore >= 0) it.autoMainBefore = share;
            if (it.autoMainAfter  >= 0) it.autoMainAfter  = share;
        }
        freeAfterGrow = 0;
    }

    // Third pass: place children along the main axis.
    const int totalMain = usedAfterGrow;
    int mainPos = 0;
    if (autoCount == 0) {
        if (cfg.justify == JustifyCenter) {
            mainPos = (row ? cw : ch) / 2 - totalMain / 2;
        } else if (cfg.justify == JustifyFlexEnd) {
            mainPos = (row ? cw : ch) - totalMain;
        }
    }

    int crossMax = 0;
    for (const Item &it : items)
        crossMax = qMax(crossMax, row ? it.baseH : it.baseW);

    int cursor = qMax(0, mainPos);
    for (int i = 0; i < items.size(); ++i) {
        Item &it = items[i];
        int spacing = cfg.gap;
        if (autoCount == 0) {
            if (cfg.justify == JustifySpaceBetween && items.size() > 1) {
                spacing = qMax(0, freeAfterGrow) / (items.size() - 1);
            } else if (cfg.justify == JustifySpaceAround) {
                spacing = qMax(0, freeAfterGrow) / items.size();
            }
        }

        cursor += qMax(0, it.autoMainBefore);

        if (row) {
            int itemY = top;
            if (cfg.align == AlignCenter)
                itemY = top + qMax(0, (crossMax - it.baseH) / 2);
            else if (cfg.align == AlignFlexEnd)
                itemY = top + qMax(0, crossMax - it.baseH);
            it.box->setPos(left + cursor, itemY);
        } else {
            int itemX = left;
            if (cfg.align == AlignCenter)
                itemX = left + qMax(0, (crossMax - it.baseW) / 2);
            else if (cfg.align == AlignFlexEnd)
                itemX = left + qMax(0, crossMax - it.baseW);
            it.box->setPos(itemX, top + cursor);
        }
        cursor += (row ? it.baseW : it.baseH) + qMax(0, it.autoMainAfter) + spacing;
    }

    m_height = m_overflowHeight = top + (row ? crossMax : qMax(0, cursor - cfg.gap)) + bottom;
    m_overflowWidth = left + (row ? qMax(0, cursor - cfg.gap) : crossMax) + right;

    layoutPositionedObjects(relayoutChildren);
}

} // namespace khtml
