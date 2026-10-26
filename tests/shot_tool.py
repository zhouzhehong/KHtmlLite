# shot_tool.py — DPI-aware client-area screenshot of a khtml_browser window.
#
# Usage: python tests\shot_tool.py <out.png> [url] [seconds]
# Launches the browser at the given URL (default: the resize probe page over
# local HTTP so the production loader path is exercised), waits for the page,
# then BitBlts the client area in TRUE device pixels and writes a PNG.
#
# DPI awareness is essential: without SetProcessDpiAwareness(2) the capture
# comes back virtualized and DWM-downscaled, which hides exactly the
# high-DPI rendering this tool exists to inspect.
import ctypes
import http.server
import os
import socketserver
import struct
import subprocess
import sys
import threading
import time

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PER_MONITOR_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

# DWM-composited page content is not in the window's GDI surface: capture
# from the desktop DC at the client origin (screen capture) instead.
# KHTML_SCREEN_CAPTURE
try:
    from win_shot import force_foreground as _force_foreground
except Exception:
    _force_foreground = None
USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32
ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8199


def start_server():
    os.chdir(os.path.join(ROOT, "tests"))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def find_window(pid):
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, _):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    USER32.EnumWindows(CB(cb), 0)
    return found[0] if found else None


def grab(hwnd, out_path):
    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    l, t, r, b = struct.unpack("4i", buf.raw)
    w, h = r - l, b - t
    if _force_foreground:
        _force_foreground(hwnd)
    time.sleep(0.04)
    _pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", _pt, 0, 0, 0)
    USER32.ClientToScreen(hwnd, _pt)
    ox, oy = struct.unpack("2i", _pt.raw)
    hdc = USER32.GetDC(0)
    mem = GDI32.CreateCompatibleDC(hdc)
    bmp = GDI32.CreateCompatibleBitmap(hdc, w, h)
    GDI32.SelectObject(mem, bmp)
    GDI32.BitBlt(mem, 0, 0, w, h, hdc, ox, oy, 0x00CC0020)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]
    bih = BIH()
    bih.biSize = ctypes.sizeof(BIH)
    bih.biWidth, bih.biHeight, bih.biPlanes, bih.biBitCount = w, -h, 1, 32
    raw = ctypes.create_string_buffer(w * 4 * h)
    GDI32.GetDIBits(mem, bmp, 0, h, raw, ctypes.byref(bih), 0)
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(0, hdc)

    # BGRX -> RGB PNG (PIL is available in this environment).
    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), raw.raw, "raw", "BGRA", 0, 1)
    img.convert("RGB").save(out_path)
    return w, h


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "shot.png")
    # start_server() chdirs into tests/ for SimpleHTTPRequestHandler, so the
    # output path must be absolute before anything moves the working dir.
    out = os.path.abspath(out)
    url = sys.argv[2] if len(sys.argv) > 2 else f"http://127.0.0.1:{PORT}/resize_probe.html"
    wait_s = float(sys.argv[3]) if len(sys.argv) > 3 else 7.0
    # Optional 4th arg "WxH" (device px): shrink the window first, mirroring
    # resize_probe.py's geometry setup, so the steady state of a small
    # viewport can be captured.
    size = sys.argv[4] if len(sys.argv) > 4 else None
    httpd = None
    if url.startswith(f"http://127.0.0.1:{PORT}"):
        httpd = start_server()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    err = open(os.path.join(ROOT, "tests", "shot_stderr.txt"), "wb")
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), url],
                            cwd=BUILD, env=env, stderr=err)
    try:
        hwnd = None
        for _ in range(40):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no window")
            return 1
        if size:
            try:
                sw, sh = size.lower().split("x")
                USER32.SetWindowPos(hwnd, 0, 8, 8, int(sw), int(sh),
                                    0x0004 | 0x0010)   # SWP_NOZORDER | SWP_NOACTIVATE
            except Exception as e:
                print("bad size:", e)
                return 1
        dpi = USER32.GetDpiForWindow(hwnd) if hasattr(USER32, "GetDpiForWindow") else 0
        time.sleep(wait_s)
        w, h = grab(hwnd, out)
        print(f"shot {out} {w}x{h} dpi={dpi} scale={dpi/96.0 if dpi else '?'}")
        return 0
    finally:
        proc.kill()
        err.close()
        if httpd:
            httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
