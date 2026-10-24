/*
    render_grid.h - minimal CSS Grid container.

    A deliberately small subset of CSS Grid, enough for the common card/nav
    layouts:
      - display: grid / inline-grid (inline-grid uses the inline-block path)
      - grid-template-columns: <length>+ | repeat(<n>, <track>) |
        repeat(auto-fit|auto-fill, minmax(<length>, 1fr)) | none
        where <track> = px | % | fr | auto | minmax(px, 1fr)
      - gap (both axes)
      - row-major auto-placement of in-flow children (no spans / named lines /
        template areas / explicit row definitions yet)

    Rows are sized by their tallest item, columns by the template.  Items are
    stretched to their column width when their own width is auto.
*/

#ifndef KHTML_RENDER_GRID_H
#define KHTML_RENDER_GRID_H

#include "render_block.h"

namespace khtml
{

class RenderGrid : public RenderBlock
{
public:
    RenderGrid(DOM::NodeImpl *node);
    ~RenderGrid() override;

    const char *renderName() const override;
    bool isGrid() const
    {
        return true;
    }

    void layoutBlockChildren(bool relayoutChildren) override;
};

} // namespace khtml

#endif
