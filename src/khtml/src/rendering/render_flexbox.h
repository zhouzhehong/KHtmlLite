/*
    render_flexbox.h — minimal CSS flex container.

    A deliberately small subset of the Flexbox spec, enough to make common
    layouts (nav bars, button rows, card grids) render correctly:
      - flex-direction: row | column
      - justify-content: flex-start | center | flex-end | space-between | space-around
      - align-items: stretch | flex-start | center | flex-end
      - gap
      - flex-grow / flex: N  (proportional filling of free space)
    Not implemented (yet): wrap, order, align-self, flex-basis percentages.
    Properties are read from the element's inline style attribute; external
    stylesheet support would require real CSS property plumbing and can be
    added incrementally.
*/

#ifndef KHTML_RENDER_FLEXBOX_H
#define KHTML_RENDER_FLEXBOX_H

#include "render_block.h"

namespace khtml
{

enum FlexDir { FlexDirRow, FlexDirRowReverse, FlexDirColumn, FlexDirColumnReverse };
enum FlexJustify { JustifyFlexStart, JustifyCenter, JustifyFlexEnd,
                   JustifySpaceBetween, JustifySpaceAround };
enum FlexAlign { AlignStretch, AlignFlexStart, AlignCenter, AlignFlexEnd };

class RenderFlexBox : public RenderBlock
{
public:
    RenderFlexBox(DOM::NodeImpl *node);
    ~RenderFlexBox() override;

    const char *renderName() const override;
    bool isFlexibleBox() const { return true; }

    void layoutBlockChildren(bool relayoutChildren) override;

private:
    struct FlexConfig {
        FlexDir dir = FlexDirRow;
        FlexJustify justify = JustifyFlexStart;
        FlexAlign align = AlignStretch;
        int gap = 0;
    };

    FlexConfig config() const;
};

} // namespace khtml

#endif
