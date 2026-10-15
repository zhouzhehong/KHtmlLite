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

USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32
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
    The top CHROME_H px of the client is the tab bar + toolbar (flat
    #f0f0f0 gray) — sampling it would read as a permanent ~14% checker
    baseline, so the probe clamps it out and watches the page area only."""
    CHROME_H = 100
    _, _, w, h = client_rect(hwnd)
    if w <= 0 or h <= 0:
        return None
    l, t, r, b = region
    l, t = max(0, l), max(CHROME_H, t)
    r, b = min(w, r), min(h, b)
    if r <= l or b <= t:
        return None
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
    buf = ctypes.create_string_buffer(w * 4 * h)
    GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0)
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(hwnd, hdc)

    # l,t,r,b were already clamped (incl. CHROME_H) before the BitBlt.
    counts = {"content": 0, "white": 0, "checker": 0, "other": 0}
    sig = 0
    n = 0
    data = buf.raw  # bytes: indexing yields int (ctypes array indexing
                     # returns bytes objects on Python >= 3.13)
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
    if n == 0:
        return None
    frac = {k: v / n for k, v in counts.items()}
    return frac, sig


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
                frac, sig = res
                samples.append((round(now, 3), frac))
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
        pre = client_rect(hwnd)
        print(f"pre-maximize client: {pre[2]}x{pre[3]}")

        # Validity gate: if the probe page itself did not render (white in
        # KHTML), every later "white flash" number is page-failure noise, not
        # resize behavior.  Refuse to measure in that case.
        pre_res = sample_points(hwnd, pre)
        if not pre_res or pre_res[0]["content"] < 0.85:
            frac = pre_res[0] if pre_res else {"content": 0.0}
            print(f"PROBE INVALID: page content only {frac['content']:.0%} "
                  f"before maximize (need >=85%) — probe page did not render")
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
        for ts, frac in samples:
            if t_content is None and frac["content"] > 0.9:
                t_content = ts
            if ts > 0.12:  # ignore the very first paint in flight
                worst_white = max(worst_white, frac["white"] + frac["checker"])
        print(f"[maximize] first sample with >90% content: "
              f"{t_content if t_content is not None else 'NEVER'}s")
        print(f"[maximize] worst white+checker fraction after 120ms: {worst_white:.2%}")
        print(f"[maximize] exposed-region signature settled at {settle:.2f}s "
              f"({len(samples)} samples)")

        # ---- restore ----
        time.sleep(1.0)
        USER32.ShowWindowAsync(hwnd, SW_RESTORE)
        while USER32.IsZoomed(hwnd):
            time.sleep(0.01)
        # On shrink there is no exposed region; instead watch the whole
        # client for white/checker takeover (crop or scale both cover it).
        samples2, settle2 = [], 0.0
        t0 = time.time()
        last_sig, last_change = None, 0.0
        while time.time() - t0 < 2.0:
            now = time.time() - t0
            res = sample_points(hwnd, client_rect(hwnd))
            if res:
                frac, sig = res
                samples2.append((round(now, 3), frac))
                if last_sig is None or sig != last_sig:
                    last_sig, last_change = sig, now
            time.sleep(0.04)
        worst_white2 = 0.0
        for ts, frac in samples2:
            if ts > 0.12:
                worst_white2 = max(worst_white2, frac["white"] + frac["checker"])
        print(f"[restore] worst white+checker fraction after 120ms: {worst_white2:.2%}")
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
