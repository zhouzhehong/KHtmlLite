/*
    Kernel-side adapter for the Windows DWM presentation path.

    DWM/DirectComposition is a Windows component; the engine does not implement
    a compositor, it ADAPTS the platform interface:

        browser-provided HWND
          -> DirectComposition target/visual (DWM)
          -> DXGI composition swap chain
          -> D3D11 device: hardware, or the WARP software rasterizer when the
             GPU is unavailable

    This class lives in the KHTML kernel library (KF5KHtml) and is called by
    the browser host, which owns the window and forwards the page frames.
    There is deliberately no GDI/QPainter fallback: if neither D3D11 hardware
    nor WARP composition is available, the graphics stack is considered broken
    (policy: hardware -> WARP only).
*/

#ifndef KHTML_COMPOSITOR_H
#define KHTML_COMPOSITOR_H

#include <khtml_export.h>

namespace khtml
{

class KHTML_EXPORT KHTMLCompositor
{
public:
    KHTMLCompositor();
    ~KHTMLCompositor();
    KHTMLCompositor(const KHTMLCompositor &) = delete;
    KHTMLCompositor &operator=(const KHTMLCompositor &) = delete;

    // windowHandle: HWND that owns the presentation (browser window).
    // Coordinates are PHYSICAL client pixels.  forceWarp starts on the WARP
    // rasterizer; forceD3DFail simulates "GPU unavailable" (same WARP path).
    bool create(void *windowHandle, int x, int y, int width, int height,
                bool forceWarp, bool forceD3DFail);
    void destroy();

    bool isActive() const;
    bool hasFrame() const;
    const char *backend() const;   // "d3d11-hw", "warp", "none"

    void setGeometry(int x, int y, int width, int height);
    void setVisible(bool visible);

    // BGRA rows (B8G8R8A8, matching QImage::Format_RGB32), top-down.
    bool present(const unsigned char *bgra, int width, int height, int stride);

private:
    struct Impl;
    Impl *d;
};

} // namespace khtml

#endif // KHTML_COMPOSITOR_H
