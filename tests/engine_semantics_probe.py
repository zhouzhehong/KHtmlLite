# engine_semantics_probe.py — pixel-level check of the engine-level features
# added 2026-10-24:
#   1) @supports evaluation  (true condition applies, false is dropped)
#   2) -webkit- prefix support (property + display value)
#   3) HTML5 semantic elements default to block-level
#
# Loads tests/engine_semantics.html in the real browser (own instance) and
# samples the painted pixels; kills its own process tree in finally.
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
EXE = os.path.join(ROOT, "build", "host", "khtml_browser.exe")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics.html"
OUT = os.path.join(ROOT, "tests", "engine_semantics.png")
ERR = os.path.join(ROOT, "tests", "engine_semantics_err.txt")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402


def load_px(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im, im.load()


def near(px, want, tol=40):
    return all(abs(px[i] - want[i]) <= tol for i in range(3))


def main_cluster_bbox(pixels, w, h, want, tol=45, min_row=20):
    """bbox of the dominant cluster: rows with many matching pixels.
    Filters out stray anti-aliasing pixels from other elements."""
    rows = {}
    for y in range(h):
        xs = [x for x in range(w) if near(pixels[x, y], want, tol)]
        if len(xs) >= min_row:
            rows[y] = xs
    if not rows:
        return None
    xs = [x for v in rows.values() for x in v]
    ys = list(rows.keys())
    return (min(xs), min(ys), max(xs), max(ys))

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


def main():
    if not os.path.exists(EXE):
        print("FAIL: browser exe missing:", EXE)
        return 1
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    errf = open(ERR, "wb")
    proc = subprocess.Popen([EXE, PAGE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.DEVNULL, stderr=errf)
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
        # Wait for the page to paint: poll until the blue box shows up.
        pixels = None
        for _ in range(40):
            grab(hwnd, OUT)
            im, pixels = load_px(OUT)
            w, h = im.size
            if bbox(pixels, w, h, (0, 0, 255)) is not None:
                break
            time.sleep(0.5)
        im, pixels = load_px(OUT)
        w, h = im.size
        print("shot: %dx%d" % (w, h))

        ok = True

        def check(name, good, detail=""):
            nonlocal ok
            print("  [%s] %s %s" % ("OK" if good else "FAIL", name, detail))
            ok = ok and good

        # 1) @supports true applied: green glyph pixels present.
        green = bbox(pixels, w, h, (0, 255, 0), tol=60)
        check("@supports (display:flex) applied", green is not None,
              "(green bbox %s)" % (green,))

        # 2) @supports false dropped: no red pixels anywhere.
        red = bbox(pixels, w, h, (255, 0, 0), tol=40)
        check("@supports (display:grid) dropped", red is None,
              "(red bbox %s)" % (red,))

        # 3) -webkit-border-radius: blue box exists, corner is rounded
        #    (top-left pixel of its bbox is not blue).
        blue = bbox(pixels, w, h, (0, 0, 255), tol=40)
        if blue:
            x0, y0, x1, y1 = blue
            corner = pixels[x0, y0]
            check("-webkit-border-radius rounded corner", not near(corner, (0, 0, 255), 40),
                  "(corner %s, bbox %s)" % (corner, blue))
            check("-webkit-border-radius box size", (x1 - x0) > 150 and (y1 - y0) > 30,
                  "(w=%d h=%d)" % (x1 - x0, y1 - y0))
        else:
            check("-webkit-border-radius box exists", False)

        # 4) display:-webkit-flex engages flex: three 40px squares in ONE row.
        mag = bbox(pixels, w, h, (255, 0, 255), tol=40)
        if mag:
            x0, y0, x1, y1 = mag
            check("display:-webkit-flex row layout",
                  (x1 - x0) > 100 and (y1 - y0) < 60,
                  "(bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
        else:
            check("display:-webkit-flex boxes exist", False)

        # 5) HTML5 <section> is block: yellow background spans most of the width.
        yellow = bbox(pixels, w, h, (255, 255, 0), tol=40)
        if yellow:
            x0, y0, x1, y1 = yellow
            check("HTML5 <section> block-level", (x1 - x0) > 0.5 * w,
                  "(yellow bbox w=%d of %d)" % (x1 - x0, w))
        else:
            check("HTML5 <section> renders", False)

        # 6) flex-wrap: 5 x 100px cards in a 300px container wrap into rows
        #    (3 per row => bbox ~450 x 120 physical at dpr 1.5).
        cyan = bbox(pixels, w, h, (0, 255, 255), tol=40)
        if cyan:
            x0, y0, x1, y1 = cyan
            check("flex-wrap wraps into rows",
                  (x1 - x0) > 300 and (y1 - y0) > 90,
                  "(cyan bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
        else:
            check("flex-wrap cards render", False)

        # 7) logical property margin-inline-start (LTR) offsets the box.
        orange = main_cluster_bbox(pixels, w, h, (255, 128, 0), tol=45)
        if orange:
            x0, y0, x1, y1 = orange
            check("margin-inline-start maps to left", x0 > 150 and 60 < (x1 - x0) < 140,
                  "(orange bbox x0=%d w=%d)" % (x0, x1 - x0))
        else:
            check("logical-margin box renders", False)

        print("\nRESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()


if __name__ == "__main__":
    sys.exit(main())
