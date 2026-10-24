# engine_semantics_probe.py — pixel-level checks for the engine-level CSS
# features added on 2026-10-24.  Two fixture pages (each fits the browser
# window; kept separate so neither page is taller than the capture area):
#
#   page 1 (engine_semantics.html):
#     @supports evaluation, -webkit- prefixes, HTML5 block elements,
#     flex-wrap, logical properties
#   page 2 (engine_semantics2.html):
#     flex order, align-self, flex-basis
#
# Launches the real browser (own instance), captures the client area and
# samples pixels; kills its own process tree in finally.
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
EXE = os.path.join(ROOT, "build", "host", "khtml_browser.exe")
PAGE1 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics.html"
PAGE2 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics2.html"
OUT = os.path.join(ROOT, "tests", "engine_semantics.png")
ERR = os.path.join(ROOT, "tests", "engine_semantics_err.txt")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402


def near(px, want, tol=40):
    return all(abs(px[i] - want[i]) <= tol for i in range(3))


def bbox(pixels, w, h, want, tol=40):
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if near(pixels[x, y], want, tol):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def main_cluster_bbox(pixels, w, h, want, tol=45, min_row=10, max_gap=2):
    """bbox of the DOMINANT BAND: the largest run of consecutive rows that
    contain many matching pixels.  Filters out stray pixels from other
    elements (browser-chrome antialiasing, other colored blocks)."""
    rows = {}
    for y in range(h):
        xs = [x for x in range(w) if near(pixels[x, y], want, tol)]
        if len(xs) >= min_row:
            rows[y] = xs
    if not rows:
        return None
    ys_sorted = sorted(rows)
    bands = []
    cur = [ys_sorted[0]]
    for y in ys_sorted[1:]:
        if y - cur[-1] <= max_gap:
            cur.append(y)
        else:
            bands.append(cur)
            cur = [y]
    bands.append(cur)
    best = max(bands, key=lambda b: sum(len(rows[y]) for y in b))
    xs = [x for y in best for x in rows[y]]
    return (min(xs), min(best), max(xs), max(best))


def load_pixels(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.size, im.load()


def launch(page, wait_color, timeout=40):
    """Start the browser on |page|, wait until |wait_color| appears."""
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    errf = open(ERR, "wb")
    proc = subprocess.Popen([EXE, page], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.DEVNULL, stderr=errf)
    hwnd = None
    for _ in range(40):
        hwnd = find_window(proc.pid)
        if hwnd:
            break
        time.sleep(0.5)
    if not hwnd:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        return None
    size = None
    pixels = None
    for _ in range(40):
        grab(hwnd, OUT)
        size, pixels = load_pixels(OUT)
        w, h = size
        if bbox(pixels, w, h, wait_color, tol=45) is not None:
            break
        time.sleep(0.5)
    return proc, errf, hwnd, size, pixels


def finish(proc, errf):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                   capture_output=True)
    errf.close()


def page1_checks(pixels, w, h):
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    green = bbox(pixels, w, h, (0, 255, 0), tol=60)
    add("@supports (display:flex) applied", green is not None, "(green bbox %s)" % (green,))
    red = bbox(pixels, w, h, (255, 0, 0), tol=40)
    add("@supports (display:grid) dropped", red is None, "(red bbox %s)" % (red,))

    blue = bbox(pixels, w, h, (0, 0, 255), tol=40)
    if blue:
        x0, y0, x1, y1 = blue
        add("-webkit-border-radius rounded corner", not near(pixels[x0, y0], (0, 0, 255), 40),
            "(corner %s)" % (pixels[x0, y0],))
        add("-webkit-border-radius box size", (x1 - x0) > 150 and (y1 - y0) > 30,
            "(w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("-webkit-border-radius box exists", False)

    mag = bbox(pixels, w, h, (255, 0, 255), tol=40)
    if mag:
        x0, y0, x1, y1 = mag
        add("display:-webkit-flex row layout", (x1 - x0) > 100 and (y1 - y0) < 60,
            "(bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("display:-webkit-flex boxes exist", False)

    yellow = bbox(pixels, w, h, (255, 255, 0), tol=40)
    if yellow:
        x0, y0, x1, y1 = yellow
        add("HTML5 <section> block-level", (x1 - x0) > 0.5 * w,
            "(yellow bbox w=%d of %d)" % (x1 - x0, w))
    else:
        add("HTML5 <section> renders", False)

    cyan = bbox(pixels, w, h, (0, 255, 255), tol=40)
    if cyan:
        x0, y0, x1, y1 = cyan
        add("flex-wrap wraps into rows", (x1 - x0) > 300 and (y1 - y0) > 90,
            "(cyan bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("flex-wrap cards render", False)

    orange = main_cluster_bbox(pixels, w, h, (255, 128, 0), tol=45)
    if orange:
        x0, y0, x1, y1 = orange
        add("margin-inline-start maps to left", x0 > 150 and 60 < (x1 - x0) < 140,
            "(orange bbox x0=%d w=%d)" % (x0, x1 - x0))
    else:
        add("logical-margin box renders", False)
    return out


def page2_checks(pixels, w, h):
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    olive = main_cluster_bbox(pixels, w, h, (128, 128, 0), tol=40)
    navy = main_cluster_bbox(pixels, w, h, (0, 0, 128), tol=30)
    red = main_cluster_bbox(pixels, w, h, (255, 0, 0), tol=30)
    if olive and navy and red:
        add("flex order reorders items", olive[0] < navy[0] < red[0],
            "(x0 olive=%d navy=%d red=%d)" % (olive[0], navy[0], red[0]))
    else:
        add("flex order boxes render", False,
            "(olive=%s navy=%s red=%s)" % (olive, navy, red))

    teal = main_cluster_bbox(pixels, w, h, (0, 128, 128), tol=40)
    maroon = main_cluster_bbox(pixels, w, h, (128, 0, 0), tol=40)
    if teal and maroon:
        add("align-self moves one item", maroon[1] > teal[1] + 40,
            "(y0 teal=%d maroon=%d)" % (teal[1], maroon[1]))
    else:
        add("align-self boxes render", False, "(teal=%s maroon=%s)" % (teal, maroon))

    basis = main_cluster_bbox(pixels, w, h, (0, 170, 0), tol=40)
    if basis:
        x0, y0, x1, y1 = basis
        add("flex-basis sets item width", 260 < (x1 - x0) < 340,
            "(basis bbox w=%d)" % (x1 - x0))
    else:
        add("flex-basis box renders", False)
    return out


def main():
    if not os.path.exists(EXE):
        print("FAIL: browser exe missing:", EXE)
        return 1
    ok = True
    for page, name, fn, wait in ((PAGE1, "page1 (batch1)", page1_checks, (0, 0, 255)),
                                 (PAGE2, "page2 (flex)", page2_checks, (255, 0, 0))):
        print("\n=== %s ===" % name)
        started = launch(page, wait)
        if not started:
            print("FAIL: no window for", page)
            return 1
        proc, errf, hwnd, size, pixels = started
        try:
            w, h = size
            print("shot: %dx%d" % (w, h))
            for cname, good, detail in fn(pixels, w, h):
                print("  [%s] %s %s" % ("OK" if good else "FAIL", cname, detail))
                ok = ok and good
        finally:
            finish(proc, errf)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
