/*
    DWM-composited page canvas implementation (DirectComposition + DXGI).
    See dwm_canvas.h for the design and failure policy.
*/

#include "khtml_compositor.h"
#include <QtGlobal>

#ifdef Q_OS_WIN

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef WS_EX_NOREDIRECTIONBITMAP
// Not exposed by older MinGW winuser.h; stable Win8+ value.
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>

#include <cstdio>
#include <cstring>
#include <vector>

static void logHr(const char *step, HRESULT hr)
{
    if (FAILED(hr)) {
        fprintf(stderr, "[canvas] %s failed hr=0x%08lx\n", step, (unsigned long)hr);
        fflush(stderr);
    }
}

namespace khtml
{

namespace
{

const wchar_t kCanvasClass[] = L"KHtmlKHTMLCompositorWindow";

// Local GUIDs (avoid depending on dxguid/dcomp import-library GUID objects,
// whose availability differs between MinGW/LLVM toolchains).
const GUID kIID_IDCompositionDevice =
    {0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};
const GUID kIID_IDXGIFactory2 =
    {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
const GUID kIID_IDXGIDevice =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
const GUID kIID_IDXGIAdapter =
    {0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};

void logCanvas(const char *backend, const char *detail)
{
    fprintf(stderr, "[canvas] backend=%s%s%s\n", backend,
            (detail && *detail) ? " " : "", detail ? detail : "");
    fflush(stderr);
}

LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;    // input falls through to the Qt widget below
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        return TRUE;             // the Qt PageSurface owns the cursor
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool ensureWindowClass()
{
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CanvasWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kCanvasClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    registered = true;
    return true;
}

} // namespace

struct KHTMLCompositor::Impl
{
    HWND hwnd = nullptr;
    HWND parent = nullptr;

    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain1 *swapChain = nullptr;
    ID3D11Texture2D *backBuffer = nullptr;
    ID3D11Texture2D *upload = nullptr;
    IDCompositionDevice *dcomp = nullptr;
    IDCompositionTarget *target = nullptr;
    IDCompositionVisual *visual = nullptr;

    int width = 0;
    int height = 0;
    bool active = false;
    bool hasFrame = false;
    bool warp = false;

    char backendName[32] = "none";

    void releaseDeviceChain();
    void releaseComposition();
    void destroyWindow();
    void destroyAll();

    bool createWindow(HWND parentHwnd, int x, int y, int w, int h);
    bool createDevice(bool forceWarp);
    bool createSwapChain(int w, int h);
    bool createComposition();
    bool uploadFrame(const unsigned char *bgra, int fw, int fh, int stride);
    bool presentOnce(const unsigned char *bgra, int fw, int fh, int stride);
};

void KHTMLCompositor::Impl::releaseDeviceChain()
{
    if (backBuffer) {
        backBuffer->Release();
        backBuffer = nullptr;
    }
    if (upload) {
        upload->Release();
        upload = nullptr;
    }
    if (swapChain) {
        swapChain->Release();
        swapChain = nullptr;
    }
    if (context) {
        context->Release();
        context = nullptr;
    }
    if (device) {
        device->Release();
        device = nullptr;
    }
}

void KHTMLCompositor::Impl::releaseComposition()
{
    if (visual) {
        visual->Release();
        visual = nullptr;
    }
    if (target) {
        target->Release();
        target = nullptr;
    }
    if (dcomp) {
        dcomp->Release();
        dcomp = nullptr;
    }
}

void KHTMLCompositor::Impl::destroyWindow()
{
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    parent = nullptr;
}

void KHTMLCompositor::Impl::destroyAll()
{
    releaseComposition();
    releaseDeviceChain();
    destroyWindow();
    active = false;
    hasFrame = false;
    strcpy(backendName, "none");
}

bool KHTMLCompositor::Impl::createWindow(HWND parentHwnd, int x, int y, int w, int h)
{
    if (!ensureWindowClass()) {
        return false;
    }
    parent = parentHwnd;
    hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kCanvasClass, L"", WS_CHILD | WS_VISIBLE,
        x, y, w, h, parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    return hwnd != nullptr;
}

bool KHTMLCompositor::Impl::createDevice(bool forceWarp)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = E_FAIL;
    if (!forceWarp) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, &obtained, &context);
        if (FAILED(hr)) {
            logCanvas("d3d11-hw", "device create failed, trying WARP");
        }
    }
    if (forceWarp || FAILED(hr)) {
        warp = true;
        obtained = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, &obtained, &context);
        if (FAILED(hr)) {
            logCanvas("none", "WARP device create failed");
            return false;
        }
    }
    strcpy(backendName, warp ? "warp" : "d3d11-hw");
    return true;
}

bool KHTMLCompositor::Impl::createSwapChain(int w, int h)
{
    if (!device || w <= 0 || h <= 0) {
        return false;
    }
    if (backBuffer) {
        backBuffer->Release();
        backBuffer = nullptr;
    }
    if (upload) {
        upload->Release();
        upload = nullptr;
    }
    if (swapChain) {
        swapChain->Release();
        swapChain = nullptr;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    HRESULT hr = device->QueryInterface(kIID_IDXGIDevice, (void **)&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) {
        return false;
    }
    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory2 *factory = nullptr;
    if (SUCCEEDED(hr) && adapter) {
        hr = adapter->GetParent(kIID_IDXGIFactory2, (void **)&factory);
    }
    if (dxgiDevice) {
        dxgiDevice->Release();
    }
    if (adapter) {
        adapter->Release();
    }
    if (FAILED(hr) || !factory) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;
    hr = factory->CreateSwapChainForComposition(device, &desc, nullptr, &swapChain);
    logHr("CreateSwapChainForComposition", hr);
    factory->Release();
    if (FAILED(hr) || !swapChain) {
        return false;
    }

    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer);
    logHr("GetBuffer", hr);
    if (FAILED(hr) || !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC tex = {};
    tex.Width = UINT(w);
    tex.Height = UINT(h);
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = 0;
    tex.CPUAccessFlags = 0;
    hr = device->CreateTexture2D(&tex, nullptr, &upload);
    logHr("CreateTexture2D(upload)", hr);
    if (FAILED(hr) || !upload) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool KHTMLCompositor::Impl::createComposition()
{
    if (!hwnd || !device) {
        return false;
    }
    IDXGIDevice *dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(kIID_IDXGIDevice, (void **)&dxgiDevice)) || !dxgiDevice) {
        return false;
    }
    HRESULT hr = DCompositionCreateDevice(dxgiDevice, kIID_IDCompositionDevice,
                                          (void **)&dcomp);
    logHr("DCompositionCreateDevice", hr);
    dxgiDevice->Release();
    if (FAILED(hr) || !dcomp) {
        return false;
    }
    hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
    logHr("CreateTargetForHwnd", hr);
    if (FAILED(hr) || !target) {
        return false;
    }
    hr = dcomp->CreateVisual(&visual);
    logHr("CreateVisual", hr);
    if (FAILED(hr) || !visual) {
        return false;
    }
    hr = visual->SetContent(swapChain);
    logHr("Visual::SetContent", hr);
    if (FAILED(hr)) {
        return false;
    }
    hr = target->SetRoot(visual);
    logHr("Target::SetRoot", hr);
    if (FAILED(hr)) {
        return false;
    }
    hr = dcomp->Commit();
    return SUCCEEDED(hr);
}

bool KHTMLCompositor::Impl::uploadFrame(const unsigned char *bgra, int fw, int fh, int stride)
{
    if (!bgra || fw <= 0 || fh <= 0) {
        return false;
    }
    if (fw != width || fh != height) {
        if (!createSwapChain(fw, fh)) {
            return false;
        }
        if (visual) {
            visual->SetContent(swapChain);
            if (dcomp) {
                dcomp->Commit();
            }
        }
    }
    // UpdateSubresource needs tightly packed rows.
    std::vector<unsigned char> packed;
    const int srcStride = stride > 0 ? stride : fw * 4;
    const unsigned char *src = bgra;
    if (srcStride != fw * 4) {
        packed.resize(size_t(fw) * 4 * size_t(fh));
        for (int y = 0; y < fh; ++y) {
            memcpy(packed.data() + size_t(y) * fw * 4,
                   bgra + size_t(y) * srcStride, size_t(fw) * 4);
        }
        src = packed.data();
    }
    context->UpdateSubresource(upload, 0, nullptr, src, UINT(fw * 4), 0);
    context->CopyResource(backBuffer, upload);
    return true;
}

bool KHTMLCompositor::Impl::presentOnce(const unsigned char *bgra, int fw, int fh, int stride)
{
    if (!uploadFrame(bgra, fw, fh, stride)) {
        return false;
    }
    const HRESULT hr = swapChain->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
        || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        return false;
    }
    hasFrame = SUCCEEDED(hr);
    return hasFrame;
}

KHTMLCompositor::KHTMLCompositor() : d(new Impl) {}

KHTMLCompositor::~KHTMLCompositor()
{
    destroy();
    delete d;
    d = nullptr;
}

bool KHTMLCompositor::create(void *parentHwnd, int x, int y, int width, int height,
                     bool forceWarp, bool forceD3DFail)
{
    if (!d || d->active) {
        return d && d->active;
    }
    HWND parent = reinterpret_cast<HWND>(parentHwnd);
    if (!parent || width <= 0 || height <= 0) {
        return false;
    }
    if (!d->createWindow(parent, x, y, width, height)) {
        logCanvas("none", "child window create failed");
        return false;
    }
    // forceD3DFail: pretend the hardware device is unavailable and start on
    // WARP (the same code path a real GPU failure takes).
    if (!d->createDevice(forceWarp || forceD3DFail)) {
        d->destroyAll();
        return false;
    }
    if (!d->createSwapChain(width, height) || !d->createComposition()) {
        logCanvas(d->backendName, "composition setup failed");
        d->destroyAll();
        return false;
    }
    d->active = true;
    logCanvas(d->backendName, "dwm composition active");
    return true;
}

void KHTMLCompositor::destroy()
{
    if (d) {
        d->destroyAll();
    }
}

bool KHTMLCompositor::isActive() const
{
    return d && d->active;
}

bool KHTMLCompositor::hasFrame() const
{
    return d && d->hasFrame;
}

const char *KHTMLCompositor::backend() const
{
    return d ? d->backendName : "none";
}

void KHTMLCompositor::setGeometry(int x, int y, int width, int height)
{
    if (!d || !d->hwnd || width <= 0 || height <= 0) {
        return;
    }
    SetWindowPos(d->hwnd, nullptr, x, y, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (d->active && (width != d->width || height != d->height)) {
        // Geometry change needs a new swap chain of the new pixel size.
        if (d->createSwapChain(width, height)) {
            if (d->visual) {
                d->visual->SetContent(d->swapChain);
            }
            if (d->dcomp) {
                d->dcomp->Commit();
            }
            d->hasFrame = false;
        } else {
            logCanvas(d->backendName, "swap-chain resize failed");
            d->destroyAll();
        }
    }
}

void KHTMLCompositor::setVisible(bool visible)
{
    if (d && d->hwnd) {
        ShowWindow(d->hwnd, visible ? SW_SHOWNA : SW_HIDE);
    }
}

bool KHTMLCompositor::present(const unsigned char *bgra, int width, int height, int stride)
{
    if (!d || !d->active) {
        return false;
    }
    if (d->presentOnce(bgra, width, height, stride)) {
        return true;
    }
    // Device removed/reset (or a transient present failure): recover on the
    // WARP software rasterizer once, then let the caller fall back to GDI.
    logCanvas(d->backendName, "device failure, retrying on WARP");
    d->releaseComposition();
    d->releaseDeviceChain();
    d->hasFrame = false;
    if (d->createDevice(true) && d->width > 0 && d->height > 0) {
        if (d->createSwapChain(d->width, d->height) && d->createComposition()) {
            d->active = true;
            logCanvas(d->backendName, "recovered on WARP");
            if (d->presentOnce(bgra, width, height, stride)) {
                return true;
            }
        }
    }
    logCanvas("none", "DWM canvas failed, falling back to GDI");
    d->destroyAll();
    return false;
}

} // namespace khtml

#else // !Q_OS_WIN

namespace khtml
{

struct KHTMLCompositor::Impl {};

KHTMLCompositor::KHTMLCompositor() : d(nullptr) {}
KHTMLCompositor::~KHTMLCompositor() {}

bool KHTMLCompositor::create(void *, int, int, int, int, bool, bool) { return false; }
void KHTMLCompositor::destroy() {}
bool KHTMLCompositor::isActive() const { return false; }
bool KHTMLCompositor::hasFrame() const { return false; }
const char *KHTMLCompositor::backend() const { return "none"; }
void KHTMLCompositor::setGeometry(int, int, int, int) {}
void KHTMLCompositor::setVisible(bool) {}
bool KHTMLCompositor::present(const unsigned char *, int, int, int) { return false; }

} // namespace khtml

#endif // Q_OS_WIN
