# scroll_smoke.py — wheel/scroll smoke test for KHtmlLite.
#
# Launches khtml_browser.exe on a tall test page, finds the browser's
# top-level window, posts synthetic mouse wheel input into its client area,
# then verifies:
#   1. the page surface pixels change between before/after scroll samples
#      (frame actually scrolled / repainted),
#   2. browser and renderer processes stay alive,
#   3. no new entries in khtml_renderer_crash.log.
#
# IMPORTANT: requires a real desktop + the C++ build.  Run in MSYS2/Windows
# after building khtml_browser.exe.  NOT runnable in the agent sandbox
# (no compiler, GUI processes).

import ctypes
import http.server
import os
import socketserver
import struct
import subprocess
import sys
import threading
import time

# DWM-composited page content is not in the window's GDI surface: capture
# from the desktop DC at the client origin (screen capture) instead.
# KHTML_SCREEN_CAPTURE
try:
    from win_shot import force_foreground as _force_foreground
except Exception:
    _force_foreground = None
USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32

BUILD = r"C:\Users\zhouzhehong\Desktop\KHtmlLite\build\host"
CRASHLOG = os.path.join(BUILD, "khtml_renderer_crash.log")
# Served over local HTTP: the PageLoader http path is the production load
# path.  file:// TOP-LEVEL loads are broken in KHtmlLite (missing KIO
# plumbing — subresources got a QFile fast path, top-level navigation did
# not), so a file:// URL here would render a blank page and the smoke would
# measure "scrolling" on an empty surface.
PORT = 8198
URL = f"http://127.0.0.1:{PORT}/scroll_smoke.html"

env = dict(os.environ)
env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]


def crashlog_size():
    return os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1


def find_browser_window(pid):
    found = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def enum_cb(hwnd, lparam):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            length = USER32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(length + 1)
            USER32.GetWindowTextW(hwnd, buf, length + 1)
            if buf.value:
                found.append(hwnd)
        return True

    USER32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return found[0] if found else None


def client_center(hwnd):
    rect = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, rect)
    # RECT: left, top, right, bottom (ints)
    _, _, r, b = struct.unpack("4i", rect.raw)
    pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", pt, 0, r // 2, b // 2)
    USER32.ClientToScreen(hwnd, pt)
    return struct.unpack("2i", pt.raw)


def send_wheel(hwnd, delta, count=1):
    """Post WM_MOUSEWHEEL at the client center. delta: +120 up / -120 down."""
    x, y = client_center(hwnd)
    lparam = (y << 16) | (x & 0xFFFF)
    wparam = (delta << 16)
    WM_MOUSEWHEEL = 0x020A
    for _ in range(count):
        ok = USER32.PostMessageW(hwnd, WM_MOUSEWHEEL, wparam, lparam)
        if not ok:
            return False
        time.sleep(0.05)
    return True


def capture_window_grayscale_sig(hwnd):
    """Sample a diagnostic signature of the window's client area using
    BitBlt into a DIB, computing a coarse checksum of downsampled luminance.
    Returns None on failure."""
    rect = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, rect)
    _, _, w, h = struct.unpack("4i", rect.raw)
    if w <= 0 or h <= 0:
        return None
    if _force_foreground:
        _force_foreground(hwnd)
    time.sleep(0.04)
    _pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", _pt, 0, 0, 0)
    USER32.ClientToScreen(hwnd, _pt)
    ox, oy = struct.unpack("2i", _pt.raw)
    hdc = USER32.GetDC(0)
    if not hdc:
        return None
    mem = GDI32.CreateCompatibleDC(hdc)
    bmp = GDI32.CreateCompatibleBitmap(hdc, w, h)
    if not mem or not bmp:
        if mem:
            GDI32.DeleteDC(mem)
        USER32.ReleaseDC(0, hdc)
        return None
    GDI32.SelectObject(mem, bmp)
    SRCCOPY = 0x00CC0020
    GDI32.BitBlt(mem, 0, 0, w, h, hdc, ox, oy, SRCCOPY)

    try:
        from PIL import Image
        import ctypes as ct

        class BITMAPINFOHEADER(ct.Structure):
            _fields_ = [
                ("biSize", ct.c_uint32), ("biWidth", ct.c_int32),
                ("biHeight", ct.c_int32), ("biPlanes", ct.c_uint16),
                ("biBitCount", ct.c_uint16), ("biCompression", ct.c_uint32),
                ("biSizeImage", ct.c_uint32), ("biXPelsPerMeter", ct.c_int32),
                ("biYPelsPerMeter", ct.c_int32), ("biClrUsed", ct.c_uint32),
                ("biClrImportant", ct.c_uint32),
            ]

        bmi = BITMAPINFOHEADER()
        bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.biWidth = w
        bmi.biHeight = -h  # top-down
        bmi.biPlanes = 1
        bmi.biBitCount = 32
        bmi.biCompression = 0
        stride = w * 4
        buf = ctypes.create_string_buffer(stride * h)
        GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
        img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", stride, 1)
        # Coarse luminance checksum over a sparse grid.
        px = img.load()
        sig = 0
        for sy in range(0, h, max(1, h // 60)):
            for sx in range(0, w, max(1, w // 60)):
                r, g, b, _ = px[sx, sy]
                sig = (sig * 131 + r + g + b) & 0xFFFFFFFFFFFFFFFF
        return sig
    finally:
        GDI32.DeleteObject(bmp)
        GDI32.DeleteDC(mem)
        USER32.ReleaseDC(0, hdc)


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__))))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                    http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        return run(httpd)
    finally:
        httpd.shutdown()


def run(httpd):
    before = crashlog_size()
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env,
                            stderr=open(r"C:\Users\zhouzhehong\AppData\Local\Temp\kb_scroll_err.txt", "wb"))
    time.sleep(8)

    hwnd = find_browser_window(proc.pid)
    if not hwnd:
        print("FAIL: no browser window found")
        proc.kill()
        return 1
    print(f"hwnd={hwnd}")

    sig_before = capture_window_grayscale_sig(hwnd)
    ok_wheel = send_wheel(hwnd, -120, count=20)  # 20 downward wheel notches
    time.sleep(3)
    sig_after = capture_window_grayscale_sig(hwnd)

    # Scroll back up and confirm content changes back.
    send_wheel(hwnd, 120, count=20)
    time.sleep(3)
    sig_restored = capture_window_grayscale_sig(hwnd)

    alive = proc.poll() is None
    rend_alive = False
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq khtml_renderer.exe"],
            text=True, errors="replace")
        rend_alive = "khtml_renderer.exe" in out
    except Exception:
        pass
    after = crashlog_size()

    print(f"wheel posted: {ok_wheel}")
    print(f"pixels: before={sig_before} after={sig_after} restored={sig_restored}")
    print(f"browser alive: {alive}, renderer alive: {rend_alive}")
    print(f"crashlog before={before} after={after} -> "
          f"{'CLEAN' if after == before else 'NEW CRASH ENTRIES'}")

    proc.kill()
    time.sleep(1)

    scrolled = (sig_before is not None and sig_after is not None
                and sig_before != sig_after)
    ok = (alive and rend_alive and ok_wheel and scrolled
          and after == before)
    print("SCROLL SMOKE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
