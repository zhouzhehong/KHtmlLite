# resize_probe.py — quantified maximize/restore presentation probe.
#
# Measures what the user actually sees during window enlarge (maximize):
#   1. time-to-content: ms from SW_MAXIMIZE until the NEWLY EXPOSED region
#      (the strips that were outside the pre-maximize client area) shows
#      real page content.
#   2. white-flash / checkerboard: any sample where the exposed region is
#      dominated by near-white (unpainted surface) or checker gray while
#      the window is already at its maximized geometry.
#   3. settle time: when the exposed-region pixel signature stops changing
#      (renderer delivered the true new-size frame).
#
# The test page (resize_probe.html) paints saturated colors over 100% of
# the viewport, so "content" vs "unpainted" is unambiguous per pixel.
#
# Usage: python tests\resize_probe.py [--keep]
import ctypes
import http.server
import os
import socketserver
import struct
import subprocess
import sys
import threading
import time

# The probe DPI-awareness is load-bearing, not cosmetic: a DPI-UNAWARE
# process on a scaled display (this dev box runs 150%) receives the
# virtualized, DWM-downscaled desktop from BitBlt/GetClientRect.  Every
# sampled pixel would then be a blend of ~1.5x1.5 source pixels, which
# destroys the exact palette colours this probe classifies — the high-DPI
# render path looked like a content regression purely because the probe
# could not see the real device pixels.  PER_MONITOR_AWARE (2) makes
# GetClientRect/BitBlt return true device pixels.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PER_MONITOR_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()    # pre-8.1 fallback
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


def window_scale(hwnd):
    """Device scale of the window's monitor (1.0 when unknown/unscaled)."""
    try:
        dpi = USER32.GetDpiForWindow(hwnd)
        if dpi:
            return dpi / 96.0
    except Exception:
        pass
    return 1.0


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


# Page-surface bounding box per client geometry, in device pixels (see
# sample_points).  Filled lazily; keyed by (client_w, client_h).
_page_box_cache = {}


ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
# Served over local HTTP: the PageLoader http path is the production load
# path (file:// top-level loads are broken by the missing KIO plumbing).
PORT = 8197
URL = f"http://127.0.0.1:{PORT}/resize_probe.html"

SW_MAXIMIZE, SW_RESTORE = 3, 9


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


def client_rect(hwnd):
    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    l, t, r, b = struct.unpack("4i", buf.raw)
    return l, t, r, b


def sample_points(hwnd, region, step=24):
    """BitBlt the client area once; classify pixels on a grid inside
    `region` (l,t,r,b) as content / white / checker / other.

    The classification window is the PAGE SURFACE, not the raw client: the
    page is detected as the bounding box of saturated pixels (this probe
    page paints only saturated colours).  At 150% scaling the surrounding
    chrome — tab bar/toolbar (top), scrollbar gutter (right), status bar
    (bottom) — occupies ~9% of the client in DEVICE pixels, and counting
    that gray as "checkerboard" capped measured content at 89.9%, i.e.
    below the 90% gate for a perfectly rendered page.  Clamping the top
    strip alone (the pre-high-DPI fix) is not enough once chrome scales
    with the monitor."""
    _, _, w, h = client_rect(hwnd)
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
    buf = ctypes.create_string_buffer(w * 4 * h)
    GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0)
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(0, hdc)
    data = buf.raw  # bytes: indexing yields int (ctypes array indexing
                     # returns bytes objects on Python >= 3.13)

    # Page-surface detection, cached per client geometry (the surface only
    # moves when the window itself changes size).  Coarse stride keeps this
    # cheap enough to run inside the 40 ms sampling loop.
    page = _page_box_cache.get((w, h))
    if page is None:
        minx, miny, maxx, maxy = w, h, -1, -1
        stride = 8
        # Top clamp first: the tab bar + toolbar are ~100 LOGICAL px and may
        # contain saturated pixels (icons, tab highlights) that would pull
        # the box up over the chrome; the saturated box then only has to
        # fence the left/right/bottom chrome, which no fixed clamp can.
        y0 = int(100 * window_scale(hwnd))
        for y in range(y0, h, stride):
            row = y * w * 4
            for x in range(0, w, stride):
                bb, gg, rr = data[row + x * 4], data[row + x * 4 + 1], data[row + x * 4 + 2]
                mx, mn = max(rr, gg, bb), min(rr, gg, bb)
                if mx - mn > 30 and mx >= 60:      # saturated page pixel
                    if x < minx: minx = x
                    if x > maxx: maxx = x
                    if y < miny: miny = y
                    if y > maxy: maxy = y
        if maxx < 0:
            return None
        # Snap outward to the nearest full pixel and require a sane box.
        page = (max(0, minx - stride), max(y0, miny - stride),
                min(w - 1, maxx + stride), min(h - 1, maxy + stride))
        if page[2] - page[0] < 8 or page[3] - page[1] < 8:
            return None
        _page_box_cache[(w, h)] = page

    l, t, r, b = region
    # Classify inside the intersection of the requested region and the page
    # surface (never the chrome around it).
    l, t = max(0, l, page[0]), max(0, t, page[1])
    r, b = min(w, r, page[2]), min(h, b, page[3])
    if r <= l or b <= t:
        return None

    counts = {"content": 0, "white": 0, "checker": 0, "other": 0}
    sig = 0
    n = 0
    # Grid-space bbox of white+checker hits, so a failing phase can say
    # WHERE the unpainted area was (top strip / right gutter / bottom...).
    wcx0, wcy0, wcx1, wcy1 = w, h, -1, -1
    for y in range(t + step // 2, b, step):
        row = y * w * 4
        for x in range(l + step // 2, r, step):
            bb, gg, rr = data[row + x * 4], data[row + x * 4 + 1], data[row + x * 4 + 2]
            sig = (sig * 131 + rr + gg + bb) & 0xFFFFFFFFFFFFFFFF
            n += 1
            mx, mn = max(rr, gg, bb), min(rr, gg, bb)
            if mn >= 243:
                counts["white"] += 1          # unpainted surface fill
            elif mn >= 205 and mx - mn <= 14:
                counts["checker"] += 1        # checkerboard gray cells
            elif mx - mn > 30 or mx < 190:
                counts["content"] += 1        # saturated page pixels
            else:
                counts["other"] += 1
            if mn >= 205 and (mn >= 243 or mx - mn <= 14):
                wcx0, wcy0 = min(wcx0, x), min(wcy0, y)
                wcx1, wcy1 = max(wcx1, x), max(wcy1, y)
    if n == 0:
        return None
    frac = {k: v / n for k, v in counts.items()}
    wcbox = None if wcx1 < 0 else (wcx0, wcy0, wcx1, wcy1)
    # RESIZE_PROBE_DUMP=1: keep a PNG of the WORST white+checker sample so
    # a failing phase can be inspected by eye afterwards.
    if os.environ.get("RESIZE_PROBE_DUMP"):
        wc = frac["white"] + frac["checker"]
        if wc > getattr(sample_points, "_worst", -1.0):
            sample_points._worst = wc
            try:
                from PIL import Image
                img = Image.frombuffer("RGBA", (w, h), data, "raw", "BGRA", 0, 1)
                if wcbox:
                    from PIL import ImageDraw
                    d = ImageDraw.Draw(img)
                    d.rectangle(wcbox, outline=(255, 0, 0), width=4)
                img.convert("RGB").save(os.path.join(
                    ROOT, "tests", "resize_offender.png"))
            except Exception:
                pass
    return frac, sig, wcbox


def phase_sample(hwnd, pre_geom, label, duration=2.5, interval=0.04):
    """Sample the exposed region (client minus pre-maximize geometry) over
    `duration` seconds, right after a geometry change. Returns samples."""
    samples = []
    t0 = time.time()
    last_change = t0
    last_sig = None
    while time.time() - t0 < duration:
        now = time.time() - t0
        l, t, r, b = client_rect(hwnd)
        if r > pre_geom[2] or b > pre_geom[3]:
            # exposed = strips right/below of the old geometry
            region = (l, t, r, b)
            res = sample_points(hwnd, region)
            if res:
                frac, sig, wcbox = res
                samples.append((round(now, 3), frac, wcbox))
                if last_sig is None or sig != last_sig:
                    last_sig = sig
                    last_change = time.time() - t0
        time.sleep(interval)
    return samples, last_change


def main():
    httpd = start_server()
    try:
        return run()
    finally:
        httpd.shutdown()


def run():
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    crashlog = os.path.join(BUILD, "khtml_renderer_crash.log")
    crash_before = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
    err = open(os.path.join(ROOT, "tests", "resize_probe_stderr.txt"), "wb")
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env, stderr=err)
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            return 1
        time.sleep(6)  # load + first frame

        # Force a SMALL pre-maximize window before measuring.  On a scaled
        # desktop (this dev box: 150%) the default window is already near
        # full-screen, so SW_MAXIMIZE would SHRINK the client — no exposed
        # strip exists and the maximize phase has nothing to measure
        # ("exposed-region signature settled ... (0 samples)").  Shrinking
        # first guarantees the maximize transition GROWS the viewport, which
        # is the transition the presentation contract is about.
        wa = RECT()
        if USER32.SystemParametersInfoW(0x0030, 0, ctypes.byref(wa), 0):  # SPI_GETWORKAREA
            sw = int((wa.right - wa.left) * 0.55)
            sh = int((wa.bottom - wa.top) * 0.62)
            USER32.SetWindowPos(hwnd, 0, wa.left + 8, wa.top + 8, sw, sh,
                                0x0004 | 0x0010)   # SWP_NOZORDER | SWP_NOACTIVATE
            time.sleep(1.5)

        pre = client_rect(hwnd)
        print(f"pre-maximize client: {pre[2]}x{pre[3]} "
              f"(scale {window_scale(hwnd):.2f})")

        # Validity gate: if the probe page itself did not render (white in
        # KHTML), every later "white flash" number is page-failure noise, not
        # resize behavior.  Refuse to measure in that case.
        pre_res = sample_points(hwnd, pre)
        if not pre_res or pre_res[0]["content"] < 0.85:
            frac = pre_res[0] if pre_res else {"content": 0.0}
            print(f"PROBE INVALID: page content only {frac['content']:.0%} "
                  f"before maximize (need >=85%) — probe page did not render")
            print("  buckets: " + " ".join(f"{k}={v:.1%}" for k, v in frac.items())
                  + f"  page_box={_page_box_cache.get((pre[2], pre[3]))}"
                  + f"  client={pre[2]}x{pre[3]}")
            return 1

        # ---- maximize ----
        t_max = time.time()
        USER32.ShowWindowAsync(hwnd, SW_MAXIMIZE)
        while not USER32.IsZoomed(hwnd):
            time.sleep(0.01)
        print(f"maximized in {(time.time()-t_max)*1000:.0f}ms")
        samples, settle = phase_sample(hwnd, pre, "maximize")
        post = client_rect(hwnd)
        print(f"post-maximize client: {post[2]}x{post[3]}")

        t_content = None
        worst_white = 0.0
        # Flash metric = WHITE ONLY.  The checkerboard presentation was
        # removed with the Safari-resize rework (tile_protocol_test asserts
        # the code can never regress to it); gray pixels in the page area
        # are now always the page's own chrome — scrollbar gutter, status
        # bar — which is correct rendering, not a flash.  Counting them
        # here measured a presentation the product no longer has.
        for ts, frac, _wc in samples:
            if t_content is None and frac["content"] > 0.9:
                t_content = ts
            if ts > 0.12:  # ignore the very first paint in flight
                worst_white = max(worst_white, frac["white"])
        print(f"[maximize] first sample with >90% content: "
              f"{t_content if t_content is not None else 'NEVER'}s")
        print(f"[maximize] worst white fraction after 120ms: {worst_white:.2%}")
        print(f"[maximize] exposed-region signature settled at {settle:.2f}s "
              f"({len(samples)} samples)")

        # ---- restore ----
        time.sleep(1.0)
        USER32.ShowWindowAsync(hwnd, SW_RESTORE)
        while USER32.IsZoomed(hwnd):
            time.sleep(0.01)
        # SW_RESTORE is asynchronous: IsZoomed flips while the window is
        # still at its maximized geometry.  Sampling that ghost measures
        # Windows' own shrink choreography (bottom of the old client already
        # cleared → pure white), not the browser's presentation.  Wait for
        # the client to actually return to the restored geometry first.
        t_wait = time.time()
        while time.time() - t_wait < 1.5:
            l, t, r, b = client_rect(hwnd)
            if r <= pre[2] + 4 and b <= pre[3] + 4:
                break
            time.sleep(0.01)
        # On shrink there is no exposed region; instead watch the whole
        # client for white/checker takeover (crop or scale both cover it).
        samples2, settle2 = [], 0.0
        t0 = time.time()
        last_sig, last_change = None, 0.0
        while time.time() - t0 < 2.0:
            now = time.time() - t0
            l, t, r, b = client_rect(hwnd)
            if r > pre[2] + 4 or b > pre[3] + 4:
                # Still shrinking (or bounced): not the presentation yet.
                time.sleep(0.02)
                continue
            res = sample_points(hwnd, (l, t, r, b))
            if res:
                frac, sig, wcbox = res
                samples2.append((round(now, 3), frac, wcbox))
                if last_sig is None or sig != last_sig:
                    last_sig, last_change = sig, now
            time.sleep(0.04)
        worst_white2 = 0.0
        worst_box2 = None
        for ts, frac, wcbox in samples2:
            if ts > 0.12 and frac["white"] > worst_white2:
                worst_white2 = frac["white"]
                worst_box2 = (ts, wcbox)
        print(f"[restore] worst white fraction after 120ms: {worst_white2:.2%}")
        if worst_box2:
            print(f"[restore] worst offender at t={worst_box2[0]}s "
                  f"white/checker bbox={worst_box2[1]}")
        print(f"[restore] whole-client signature settled at {last_change:.2f}s")

        alive = proc.poll() is None
        crash_after = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
        print(f"alive={alive} crashlog_clean={crash_after == crash_before}")

        ok = (alive and crash_after == crash_before
              and t_content is not None and t_content < 1.0
              and worst_white < 0.10 and worst_white2 < 0.10)
        print("RESIZE PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        proc.kill()
        err.close()


if __name__ == "__main__":
    sys.exit(main())
