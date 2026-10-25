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
import struct
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
EXE = os.path.join(ROOT, "build", "host", "khtml_browser.exe")
PAGE1 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics.html"
PAGE2 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics2.html"
PAGE3 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics3.html"
PAGE4 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics4.html"
PAGE5 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics5.html"
PAGE6 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics6.html"
PAGE7 = "file:///" + ROOT.replace("\\", "/") + "/tests/engine_semantics7.html"
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
    try:
        ctypes.windll.user32.SetForegroundWindow(hwnd)
    except Exception:
        pass
    for _ in range(40):
        grab(hwnd, OUT)
        size, pixels = load_pixels(OUT)
        w, h = size
        if bbox(pixels, w, h, wait_color, tol=45) is not None:
            break
        time.sleep(0.5)
    # Settle: wait until two consecutive grabs are identical, so the checks
    # read the finished paint instead of a partially uploaded tile frame.
    import hashlib
    prev = None
    for _ in range(8):
        time.sleep(0.25)
        grab(hwnd, OUT)
        try:
            with open(OUT, "rb") as fh:
                cur = hashlib.md5(fh.read()).hexdigest()
        except OSError:
            cur = None
        if cur is not None and cur == prev:
            break
        prev = cur
    size, pixels = load_pixels(OUT)
    return proc, errf, hwnd, size, pixels


def finish(proc, errf):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                   capture_output=True)
    errf.close()
    # Let the window manager settle: grabbing the next window immediately after
    # killing the previous browser can return a stale white frame (DWM race).
    time.sleep(1.0)


def page1_checks(pixels, w, h):
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    green = bbox(pixels, w, h, (0, 255, 0), tol=60)
    add("@supports (display:flex) applied", green is not None, "(green bbox %s)" % (green,))
    red = bbox(pixels, w, h, (255, 0, 0), tol=40)
    add("@supports (display:grid) applied", red is not None, "(red bbox %s)" % (red,))
    unsupported_px = sum(1 for y in range(h) for x in range(w)
                        if near(pixels[x, y], (0, 255, 136), 25))
    add("@supports (selector(:has)) dropped", unsupported_px < 50,
        "(matching px %d)" % unsupported_px)

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

    # CSS custom properties: var(--brand-bg) defined on :root must paint the
    # box; var(--missing, fallback) must use the fallback colour.
    v1 = main_cluster_bbox(pixels, w, h, (0, 153, 153), tol=40)
    if v1:
        x0, y0, x1, y1 = v1
        add("var() custom property applied", (x1 - x0) > 80 and (y1 - y0) > 20,
            "(vars1 bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("var() custom property applied", False)

    v2 = main_cluster_bbox(pixels, w, h, (204, 204, 0), tol=40)
    if v2:
        x0, y0, x1, y1 = v2
        add("var() fallback applied", (x1 - x0) > 80 and (y1 - y0) > 20,
            "(vars2 bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("var() fallback applied", False)
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

    ratio = main_cluster_bbox(pixels, w, h, (204, 136, 0), tol=40)
    if ratio:
        x0, y0, x1, y1 = ratio
        add("aspect-ratio derives height from width",
            430 < (x1 - x0) < 470 and 290 < (y1 - y0) < 310,
            "(ratio bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("aspect-ratio box renders", False)

    ifbox = main_cluster_bbox(pixels, w, h, (102, 68, 187), tol=40)
    if ifbox:
        x0, y0, x1, y1 = ifbox
        add("inline-flex containers share one line",
            180 < (x1 - x0) < 230 and 45 < (y1 - y0) < 80,
            "(ifbox bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("inline-flex containers render", False)

    ifrow = main_cluster_bbox(pixels, w, h, (0, 136, 187), tol=40)
    if ifrow:
        x0, y0, x1, y1 = ifrow
        add("inline-flex lays children out in a row",
            55 < (x1 - x0) < 90 and 20 < (y1 - y0) < 45,
            "(if1 children bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("inline-flex children render", False)

    ifone = main_cluster_bbox(pixels, w, h, (68, 187, 102), tol=40)
    if ifone:
        x0, y0, x1, y1 = ifone
        add("second inline-flex renders inline", 20 < (x1 - x0) < 45 and 20 < (y1 - y0) < 45,
            "(if2 child bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("second inline-flex renders", False)

    shrink = main_cluster_bbox(pixels, w, h, (51, 102, 255), tol=40)
    if shrink:
        x0, y0, x1, y1 = shrink
        add("flex shorthand + shrink fits the row",
            280 < (x1 - x0) < 320 and (y1 - y0) < 40,
            "(shrink bbox w=%d h=%d)" % (x1 - x0, y1 - y0))
    else:
        add("flex-shrink boxes render", False)
    return out


def page3_checks(pixels, w, h):
    """Engine-painted CSS gradients: geometry + direction checks."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    def dist(a, b):
        return sum(abs(a[i] - b[i]) for i in range(3))

    # The two gradient boxes are the only saturated pixels on this page.
    # A gradient row spans the 450px-wide box; browser-chrome accent rows are
    # either much wider (spread over the whole window) or much narrower, so
    # keep only rows whose saturated-pixel span is ~450px.
    rows = []
    for y in range(h):
        row = [x for x in range(w)
               if max(pixels[x, y]) - min(pixels[x, y]) > 40 and max(pixels[x, y]) > 60]
        if not row:
            continue
        if 400 <= (max(row) - min(row)) <= 480:
            rows.append((y, row))
    if not rows:
        add("gradient boxes render", False)
        return out
    y_top = rows[0][0]
    y_bot = rows[-1][0]
    x0 = min(min(r[1]) for r in rows)
    x1 = max(max(r[1]) for r in rows)
    bw, bh = x1 - x0, y_bot - y_top
    add("gradient boxes sized", 430 < bw < 470 and 180 < bh < 200,
        "(bbox %d,%d %dx%d)" % (x0, y_top, bw, bh))

    cx = (x0 + x1) // 2
    # g1: vertical gradient, 300x60 CSS -> 450x90 physical.
    top = pixels[cx, y_top + 5]
    bottom = pixels[cx, y_top + 80]
    add("vertical gradient top = start colour",
        dist(top, (170, 0, 170)) < dist(top, (0, 170, 136)), "(top %s)" % (top,))
    add("vertical gradient bottom = end colour",
        dist(bottom, (0, 170, 136)) < dist(bottom, (170, 0, 170)),
        "(bottom %s)" % (bottom,))

    # g2: horizontal gradient; sample near the bottom box's vertical centre.
    g2y = y_bot - 44
    left = pixels[x0 + 5, g2y]
    right = pixels[x1 - 5, g2y]
    add("horizontal gradient left = start colour",
        dist(left, (255, 0, 170)) < dist(left, (0, 170, 255)), "(left %s)" % (left,))
    add("horizontal gradient right = end colour",
        dist(right, (0, 170, 255)) < dist(right, (255, 0, 170)), "(right %s)" % (right,))
    return out


def page4_checks(pixels, w, h):
    """Engine-painted CSS transforms: translate / rotate / scale."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    t1 = main_cluster_bbox(pixels, w, h, (136, 68, 204), tol=40)
    if t1:
        x0, y0, x1, y1 = t1
        add("transform translateX shifts the box",
            90 < x0 < 130 and 140 < (x1 - x0) < 170,
            "(t1 bbox x0=%d w=%d)" % (x0, x1 - x0))
    else:
        add("translateX box renders", False)

    t2 = main_cluster_bbox(pixels, w, h, (68, 136, 204), tol=40)
    if t2:
        x0, y0, x1, y1 = t2
        add("transform rotate grows the bbox to the diagonal",
            195 < (x1 - x0) < 235 and 150 < (y1 - y0) < 235,
            "(t2 bbox %dx%d)" % (x1 - x0, y1 - y0))
    else:
        add("rotate box renders", False)

    t3 = main_cluster_bbox(pixels, w, h, (204, 68, 136), tol=40)
    if t3:
        x0, y0, x1, y1 = t3
        add("transform scale enlarges the box",
            205 < (x1 - x0) < 245 and 205 < (y1 - y0) < 245,
            "(t3 bbox %dx%d)" % (x1 - x0, y1 - y0))
    else:
        add("scale box renders", False)
    return out


def page5_checks(pixels, w, h):
    """Engine-painted CSS grid: 3 fixed columns, row-major auto placement."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    colors = {
        "c1": (204, 20, 20), "c2": (20, 204, 20), "c3": (20, 20, 204),
        "c4": (204, 204, 20), "c5": (204, 20, 204), "c6": (20, 204, 204),
        "c7": (204, 102, 20), "c8": (20, 204, 102), "c9": (102, 20, 204),
    }
    boxes = {}
    for name, want in colors.items():
        boxes[name] = main_cluster_bbox(pixels, w, h, want, tol=40)
    if not all(boxes.values()):
        add("grid cells render", False,
            "(missing: %s)" % ",".join(k for k, v in boxes.items() if not v))
        return out

    c1, c2, c3 = boxes["c1"], boxes["c2"], boxes["c3"]
    c4, c7 = boxes["c4"], boxes["c7"]
    row_ok = (abs(c1[1] - c2[1]) <= 3 and abs(c1[1] - c3[1]) <= 3
              and 155 <= (c2[0] - c1[0]) <= 175 and 155 <= (c3[0] - c2[0]) <= 175)
    add("grid row 1 shares one row", row_ok,
        "(y0 %d/%d/%d x %d/%d/%d)" % (c1[1], c2[1], c3[1], c1[0], c2[0], c3[0]))

    add("grid auto rows below each other",
        c4[1] > c1[1] + 80 and c7[1] > c4[1] + 80,
        "(row y %d/%d/%d)" % (c1[1], c4[1], c7[1]))

    add("grid row 2 starts at column 0", abs(c4[0] - c1[0]) <= 3,
        "(c1.x0=%d c4.x0=%d)" % (c1[0], c4[0]))

    cw, ch = c1[2] - c1[0], c1[3] - c1[1]
    add("grid cell sized by template", 145 <= cw <= 165 and 70 <= ch <= 80,
        "(cell %dx%d)" % (cw, ch))
    return out


def page6_checks(pixels, w, h):
    """Engine-side CSS calc(): percentage + px mixed expressions."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    c1 = main_cluster_bbox(pixels, w, h, (51, 102, 204), tol=40)
    c2 = main_cluster_bbox(pixels, w, h, (204, 102, 51), tol=40)
    c3 = main_cluster_bbox(pixels, w, h, (51, 204, 102), tol=40)
    if not (c1 and c2 and c3):
        add("calc boxes render", False, "(c1=%s c2=%s c3=%s)" % (c1, c2, c3))
        return out

    cw1, ch1 = c1[2] - c1[0], c1[3] - c1[1]
    add("calc width = 100% - 40px", 530 <= cw1 <= 550 and 70 <= ch1 <= 80,
        "(c1 %dx%d)" % (cw1, ch1))

    add("calc margin-left = 50% - 50px", 228 <= c2[0] <= 245,
        "(c2 x0=%d)" % (c2[0],))

    cw3, ch3 = c3[2] - c3[0], c3[3] - c3[1]
    add("calc height = 100px + 20px", 170 <= ch3 <= 190 and 590 <= cw3 <= 610,
        "(c3 %dx%d)" % (cw3, ch3))
    return out



def send_wheel(hwnd, delta, count=1):
    """Post WM_MOUSEWHEEL at the client centre (delta: +120 up / -120 down)."""
    WM_MOUSEWHEEL = 0x020A
    r = ctypes.create_string_buffer(16)
    ctypes.windll.user32.GetClientRect(hwnd, r)
    l, t, rr, b = struct.unpack("4i", r.raw)
    cx, cy = (rr - l) // 2, (b - t) // 2
    lparam = (cy << 16) | cx
    wparam = (delta & 0xFFFF) << 16
    for _ in range(count):
        ctypes.windll.user32.PostMessageW(hwnd, WM_MOUSEWHEEL, wparam, lparam)


def page7_checks(pixels, w, h):
    """position:sticky BEFORE scrolling: the bar sits at its static position."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    red = main_cluster_bbox(pixels, w, h, (204, 34, 34), tol=40)
    green = main_cluster_bbox(pixels, w, h, (34, 204, 34), tol=40)
    if red:
        x0, y0, x1, y1 = red
        add("sticky bar at its static position", 500 <= y0 <= 570 and 50 <= (y1 - y0) <= 70,
            "(red y0=%d h=%d)" % (y0, y1 - y0))
    else:
        add("sticky bar renders", False)
    if green:
        x0, y0, x1, y1 = green
        add("content starts below the sticky bar", 560 <= y0 <= 640,
            "(green y0=%d)" % (y0,))
    else:
        add("content block renders", False)
    return out


def page7_after_checks(pixels, w, h):
    """position:sticky AFTER scrolling: the bar is pinned to the viewport top."""
    out = []
    def add(name, good, detail=""):
        out.append((name, good, detail))

    red = main_cluster_bbox(pixels, w, h, (204, 34, 34), tol=40)
    green = main_cluster_bbox(pixels, w, h, (34, 204, 34), tol=40)
    if red:
        x0, y0, x1, y1 = red
        add("sticky bar pinned to the viewport top", 70 <= y0 <= 115 and 50 <= (y1 - y0) <= 70,
            "(red y0=%d h=%d)" % (y0, y1 - y0))
    else:
        add("sticky bar pinned (red missing)", False)
    if green:
        x0, y0, x1, y1 = green
        add("sticky content remains visible below the bar", 80 <= y0 <= 165 and (y1 - y0) > 300,
            "(green y0=%d h=%d)" % (y0, y1 - y0))
    else:
        add("sticky content visible after scroll (missing)", False)
    return out



def _run_page(page, fn, wait):
    """Launch |page|, capture, run |fn| (plus the post-scroll part for page7).
    Returns a list of (name, good, detail) or None when no window appeared."""
    started = launch(page, wait)
    if not started:
        return None
    proc, errf, hwnd, size, pixels = started
    checks = []
    try:
        w, h = size
        print("shot: %dx%d" % (w, h))
        checks = fn(pixels, w, h)
        if page == PAGE7:
            send_wheel(hwnd, -120, count=12)
            time.sleep(1.2)
            grab(hwnd, OUT)
            size2, pixels2 = load_pixels(OUT)
            w2, h2 = size2
            print("after scroll: %dx%d" % (w2, h2))
            checks = checks + page7_after_checks(pixels2, w2, h2)
    finally:
        finish(proc, errf)
    return checks


def main():
    if not os.path.exists(EXE):
        print("FAIL: browser exe missing:", EXE)
        return 1
    ok = True
    for page, name, fn, wait in ((PAGE1, "page1 (batch1)", page1_checks, (0, 0, 255)),
                                 (PAGE2, "page2 (flex)", page2_checks, (255, 0, 0)),
                                 (PAGE3, "page3 (gradients)", page3_checks, (170, 0, 170)),
                                 (PAGE4, "page4 (transforms)", page4_checks, (136, 68, 204)),
                                 (PAGE5, "page5 (grid)", page5_checks, (204, 20, 20)),
                                 (PAGE6, "page6 (calc)", page6_checks, (51, 102, 204)),
                                 (PAGE7, "page7 (sticky)", page7_checks, (204, 34, 34))):
        print("\n=== %s ===" % name)
        checks = _run_page(page, fn, wait)
        if checks is None:
            print("FAIL: no window for", page)
            return 1
        bad = [c for c in checks if not c[1]]
        if bad:
            # A freshly created window can be captured as a stale white frame
            # (DWM race when the previous browser is still shutting down).
            # Retry the whole page once before declaring a failure.
            print("  retry page after %d failed check(s): %s"
                  % (len(bad), ", ".join(c[0] for c in bad)))
            time.sleep(1.0)
            checks2 = _run_page(page, fn, wait)
            if checks2 is not None:
                checks = checks2
        for cname, good, detail in checks:
            print("  [%s] %s %s" % ("OK" if good else "FAIL", cname, detail))
            ok = ok and good
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
