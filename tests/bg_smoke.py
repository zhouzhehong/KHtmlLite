# bg_smoke.py — background-image / subresource race regression test.
#
# Serves tests/bg_smoke/ over local HTTP, opens it in khtml_browser.exe,
# waits, then samples the browser window's client area via BitBlt and
# verifies:
#   1. a significant number of RED pixels   (CSS background image painted —
#      this fails before the PageLoader deliver-refresh race fix: the
#      initial document references file:// paths that do not exist yet, the
#      CachedImage errors are sticky, and the page stays white forever).
#   2. a significant number of GREEN pixels (<img> tag likewise healed).
#   3. both processes alive, no new crash-log entries.
#
# Requires the current build (build/host). GUI will briefly appear (one
# test window, ~20 s).
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

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
CRASHLOG = os.path.join(BUILD, "khtml_renderer_crash.log")
SERVE_DIR = os.path.join(ROOT, "tests", "bg_smoke")
PORT = 8123
URL = f"http://127.0.0.1:{PORT}/bg_smoke.html"


REQS = []


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        REQS.append(self.path)


def start_server():
    os.chdir(SERVE_DIR)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def crashlog_size():
    return os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1


def find_browser_window(pid):
    found = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def enum_cb(hwnd, lparam):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    USER32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return found[0] if found else None


def count_colors(hwnd):
    """BitBlt the window client area; return (red_px, green_px)."""
    from PIL import Image
    rect = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, rect)
    _, _, w, h = struct.unpack("4i", rect.raw)
    if w <= 0 or h <= 0:
        return (0, 0)
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

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [(k, v) for k, v in [
            ("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
            ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
            ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
            ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
            ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
            ("biClrImportant", ctypes.c_uint32)]]

    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    stride = w * 4
    buf = ctypes.create_string_buffer(stride * h)
    GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", stride, 1)
    if "--dump" in sys.argv:
        path = os.path.join(ROOT, "tests", f"bg_smoke_dump_{int(time.time()*10)%10**7}.png")
        img.save(path)
        print(f"dumped {path}")
    red = green = 0
    px = img.load()
    for sy in range(0, h, 4):
        for sx in range(0, w, 4):
            r, g, b, _ = px[sx, sy]
            if r > 200 and g < 90 and b < 90:
                red += 1
            elif g > 150 and r < 90 and b < 90:
                green += 1
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(0, hdc)
    return (red, green)


def main():
    httpd = start_server()
    before = crashlog_size()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env,
                            stderr=open(os.path.join(ROOT, "tests", "bg_smoke_err.txt"), "wb"))
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_browser_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            return 1
        # Initial paint happens fast; the refreshed document lands after the
        # subresource downloads. Poll until heal or ~18 s cap.
        red = green = 0
        t0 = time.time()
        while time.time() - t0 < 18:
            red, green = count_colors(hwnd)
            if red > 150 and green > 40:
                break
            time.sleep(1.0)
        elapsed = time.time() - t0
        alive = proc.poll() is None
        after = crashlog_size()
        print(f"elapsed={elapsed:.1f}s red_px={red} green_px={green}")
        print(f"browser alive: {alive}, crashlog clean: {after == before}")
        print(f"server requests: {REQS}")
        errp = os.path.join(ROOT, "tests", "bg_smoke_err.txt")
        try:
            tail = open(errp, "r", errors="replace").read()[-2000:]
            if tail.strip():
                print("--- renderer/browser stderr tail ---")
                print(tail)
        except OSError:
            pass
        ok = alive and red > 150 and green > 40 and after == before
        print("BG SMOKE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        proc.kill()
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
