# browser_click_probe.py — end-to-end click mapping through the REAL browser.
#
# The deviceScaleFactor rework made the renderer's content widget a CSS-px
# viewport, so the browser must forward mouse positions as CSS pixels.  This
# probe clicks a SMALL target placed far from the origin (a stale /dpr mapping
# would miss it entirely) and verifies the page reacted by reading the pixel
# colour from a DPI-aware screenshot.
#
# Usage: python tests\browser_click_probe.py
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
    pass

USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32
ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8201

PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>click-probe</title>
<style>
  body { margin:0; background:#ffffff }
  #t { position:absolute; left:500px; top:250px; width:60px; height:40px;
       background:#cc0000 }
</style></head><body>
<div id="t" onclick="document.title='click-ok';this.style.backgroundColor='#00aa00'"></div>
<script>document.title='r'+window.devicePixelRatio</script>
</body></html>
"""


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


def grab(hwnd):
    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    l, t, r, b = struct.unpack("4i", buf.raw)
    w, h = r - l, b - t
    hdc = USER32.GetDC(hwnd)
    mem = GDI32.CreateCompatibleDC(hdc)
    bmp = GDI32.CreateCompatibleBitmap(hdc, w, h)
    GDI32.SelectObject(mem, bmp)
    GDI32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)

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
    USER32.ReleaseDC(hwnd, hdc)
    return w, h, raw.raw


def px(data, w, x, y):
    o = (y * w + x) * 4
    return (data[o + 2], data[o + 1], data[o])     # BGRA -> RGB


def find_page_top(data, w, h):
    """First row (scanning down at x=20) that is pure white: page background."""
    for y in range(h):
        if px(data, w, 20, y) == (255, 255, 255):
            return y
    return -1


def force_foreground(hwnd):
    """Bring the browser window to the foreground even when another app owns
    the foreground (Windows focus-stealing protection).  AttachThreadInput to
    the current foreground thread is the usual way around it."""
    if USER32.GetForegroundWindow() == hwnd:
        return True
    fg = USER32.GetForegroundWindow()
    tid_fg = USER32.GetWindowThreadProcessId(fg, None) if fg else 0
    tid_me = USER32.GetWindowThreadProcessId(hwnd, None)
    attached = False
    if tid_fg and tid_me and tid_fg != tid_me:
        attached = USER32.AttachThreadInput(tid_me, tid_fg, True) != 0
    try:
        USER32.ShowWindow(hwnd, 9)          # SW_RESTORE
        USER32.BringWindowToTop(hwnd)
    finally:
        if attached:
            USER32.AttachThreadInput(tid_me, tid_fg, False)
    for _ in range(10):
        if USER32.GetForegroundWindow() == hwnd:
            return True
        time.sleep(0.1)
    return False


def click(hwnd, x, y):
    if not force_foreground(hwnd):
        print("WARN: could not bring the browser window to the foreground")
    time.sleep(0.2)
    wr = ctypes.create_string_buffer(16)
    USER32.GetWindowRect(hwnd, wr)
    # client origin in screen coords
    pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", pt, 0, 0, 0)
    USER32.ClientToScreen(hwnd, pt)
    ox, oy = struct.unpack("2i", pt.raw)
    USER32.SetCursorPos(ox + x, oy + y)
    time.sleep(0.15)
    USER32.mouse_event(0x0002, 0, 0, 0, 0)   # left down
    time.sleep(0.05)
    USER32.mouse_event(0x0004, 0, 0, 0, 0)   # left up


def window_text(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    USER32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def main():
    os.makedirs(os.path.join(ROOT, "tests", "dpr"), exist_ok=True)
    page_path = os.path.join(ROOT, "tests", "dpr", "click.html")
    with open(page_path, "w") as f:
        f.write(PAGE)
    os.chdir(os.path.join(ROOT, "tests", "dpr"))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    err = open(os.path.join(ROOT, "tests", "click_probe_stderr.txt"), "wb")
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"),
         f"http://127.0.0.1:{PORT}/click.html"],
        cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=err)
    ok = False
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
        # Let the page load and the first tiles arrive.
        time.sleep(6)
        # The page reports window.devicePixelRatio through its own title: this
        # is the ratio production actually used (the probe supplies none).
        pre_title = window_text(hwnd)
        print(f"window title before click: {pre_title!r}")
        w, h, data = grab(hwnd)
        dpi = USER32.GetDpiForWindow(hwnd)
        dpr = dpi / 96.0 if dpi else 1.0
        # Locate the red target by colour instead of deriving the page origin
        # from the chrome layout: the colour box is unambiguous.
        xs, ys = [], []
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                # exact page colour (#cc0000): tiles are copied 1:1, so the
                # div's fill is byte-exact; chrome icons (anti-aliased) are not.
                if px(data, w, x, y) == (204, 0, 0):
                    xs.append(x)
                    ys.append(y)
        if not xs:
            print(f"FAIL: red target not found (client {w}x{h})")
            return 1
        cx = (min(xs) + max(xs)) // 2
        cy = (min(ys) + max(ys)) // 2
        bbox = (min(xs), min(ys), max(xs), max(ys))
        before = px(data, w, cx, cy)
        print(f"client={w}x{h} dpi={dpi} dpr={dpr} target bbox={bbox} "
              f"centre=({cx},{cy}) before={before}")
        click(hwnd, cx, cy)
        time.sleep(1.2)
        # The page's onclick sets document.title -> the browser's window title
        # is the authoritative signal that the click reached the RIGHT DOM
        # element.  (The div's style change is CPU-only evidence: KHTML's
        # CSSOM currently drops style.backgroundColor writes, so colour is
        # informational, not part of the verdict.)
        title = window_text(hwnd)
        w2, h2, data2 = grab(hwnd)
        after = px(data2, w2, cx, cy)
        print(f"window title after click: {title!r}")
        print(f"after={after}")
        # Force a full repaint (1px resize round-trip) to tell a style failure
        # apart from a damage/repaint miss: if the colour appears only after
        # the forced full frame, the click's invalidate region was wrong.
        wr2 = ctypes.create_string_buffer(16)
        USER32.GetWindowRect(hwnd, wr2)
        x0, y0, x1, y1 = struct.unpack("4i", wr2.raw)
        USER32.SetWindowPos(hwnd, 0, x0, y0, (x1 - x0) + 2, (y1 - y0) + 2,
                            0x0004 | 0x0010)
        time.sleep(1.0)
        w3, h3, data3 = grab(hwnd)
        forced = px(data3, w3, cx, cy)
        print(f"after forced repaint={forced}")
        # Debug dumps for offline inspection (DPI-aware BitBlt, no PIL needed).
        for tag, (ww, hh, dd) in (("before", (w, h, data)),
                                  ("after", (w2, h2, data2))):
            path = os.path.join(ROOT, "tests", f"click_{tag}.raw")
            with open(path, "wb") as f:
                f.write(struct.pack("2i", ww, hh) + dd)
        # #cc0000 -> #00aa00 when the click lands.
        ok = "click-ok" in title
        print(f"click landed: {ok}")
    finally:
        proc.kill()
        err.close()
        httpd.shutdown()
    print("BROWSER CLICK PROBE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
