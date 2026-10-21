# resize_ghost_probe.py — reproduce the "stuck ghost pixels after a live
# window resize" partial-paint failure (user-reported on cn.bing.com:
# a solid #0078D4 ghost block plus misplaced/frozen floating buttons after
# dragging the window smaller).
#
# Drives the REAL browser window through a SetWindowPos drag sequence that
# ends with a PURE-HEIGHT GROW — the case the old renderer damage policy
# only repainted as a bottom strip, leaving the moved fixed element's old
# pixels in place.  Then verifies, with a DPI-aware capture:
#   1. the resize event really reached page JS          (window title)
#   2. the moved blue button left NO pixels behind      (old rect, incl.
#      interpolated near-blue from a stretched frame)
#   3. the new rectangle is actually painted on screen  (physical area)
#   4. there is exactly ONE orange fixed element        (a second one is the
#      stale copy at the old viewport bottom)
#
# It NEVER kills pre-existing khtml_browser/khtml_renderer processes (the
# user may have a live window open); it starts its own instance and kills
# only that process tree.
#
# Usage: python tests\resize_ghost_probe.py
import ctypes
import os
import struct
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/resize_ghost.html"
OUT = os.path.join(ROOT, "tests", "resize_ghost_out.png")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402

USER32 = ctypes.windll.user32
DPR = 1.5
FRAME_W, FRAME_H = 22, 56   # physical outer-frame budget (Win11)

BLUE = (0, 120, 212)
ORANGE = (204, 102, 0)
ANCHOR = (51, 51, 51)

BTN_W, BTN_H = 54, 37       # CSS
GONE_BTN = (380, 200)       # CSS: position before the resize sequence
MID_BTN = (200, 100)        # CSS: position set by the resize JS
NEW_BTN = (600, 60)         # CSS: position set by the delayed (no-resize) move
TOG_W, TOG_H = 80, 24       # CSS
TOG_X = 200                 # CSS


def find_anchor(px, w, h):
    """Locate the 6x6 CSS (#333333) anchor block = page viewport origin."""
    for y in range(40, h - 10):
        for x in range(0, w - 12):
            if px[x, y] == ANCHOR and px[x + 8, y + 8] == ANCHOR \
                    and px[x + 4, y + 8] == ANCHOR and px[x + 8, y + 4] == ANCHOR:
                return x, y
    return None


def count_exact(px, x0, y0, x1, y1, col):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if px[x, y] == col:
                n += 1
    return n


def count_near_blue(px, x0, y0, x1, y1):
    """Interpolated/stretched remnants of the blue button."""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if b > 140 and b - r > 60 and abs(g - 120) < 70:
                n += 1
    return n


def orange_blocks(px, w, h):
    """Rows containing a >=60px-wide orange run, clustered into blocks."""
    rows = []
    for y in range(40, h, 2):
        run = None
        for x in range(0, w):
            if px[x, y] == ORANGE:
                if run is None:
                    run = [x, x]
                else:
                    run[1] = x
            else:
                if run and run[1] - run[0] >= 60:
                    rows.append((y, run[0], run[1]))
                run = None
        if run and run[1] - run[0] >= 60:
            rows.append((y, run[0], run[1]))
    blocks = []
    for y, x0, x1 in rows:
        merged = False
        for b in blocks:
            if abs(b["y1"] - y) <= 8 and not (x1 < b["x0"] - 30 or x0 > b["x1"] + 30):
                b["y1"] = y
                b["x0"] = min(b["x0"], x0)
                b["x1"] = max(b["x1"], x1)
                b["rows"] += 1
                merged = True
                break
        if not merged:
            blocks.append({"y0": y, "y1": y, "x0": x0, "x1": x1, "rows": 1})
    return [b for b in blocks if b["rows"] >= 6]


def main():
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"), PAGE],
        cwd=BUILD, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        hwnd = None
        for _ in range(20):          # cold start can take a few seconds
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("no window found")
            return 1
        # Wait until the page's own JS has run (the window creation already
        # emits resize events, so the title proves the listener is wired) —
        # otherwise the drag sequence would be applied to a still-loading
        # document and the capture could be blank.
        title = ""
        ready = False
        for _ in range(40):
            b = ctypes.create_unicode_buffer(256)
            USER32.GetWindowTextW(hwnd, b, 256)
            title = b.value
            if title.startswith("js"):
                ready = True
                break
            time.sleep(0.5)
        if not ready:
            print("page JS never ran before the drag (title=%r)" % title)
            return 1
        print("ready, title:", repr(title))
        # Drag sequence: mixed size steps, then pure-height steps ending in a
        # GROW (the case the old policy left unpainted above the old bottom).
        seq = [(1100, 560), (1000, 520), (1000, 440), (1000, 380), (1000, 460)]
        for cw, ch in seq:
            USER32.SetWindowPos(hwnd, 0, 0, 0,
                                round(cw * DPR) + FRAME_W,
                                round(ch * DPR) + FRAME_H,
                                0x0002 | 0x0004)  # NOMOVE | NOZORDER
            time.sleep(0.03)
        time.sleep(1.5)  # let the 20 ms repaint + 16 ms frame tick settle

        buf = ctypes.create_unicode_buffer(256)
        USER32.GetWindowTextW(hwnd, buf, 256)
        title = buf.value
        print("window title after resize:", repr(title))

        w, h = grab(hwnd, OUT)
        from PIL import Image
        px = Image.open(OUT).convert("RGB").load()
        anchor = find_anchor(px, w, h)
        if not anchor:
            print("anchor not found - page may not have loaded")
            return 1
        ax, ay = anchor
        s = DPR
        ok = True

        # 1) The resize event must have reached page JS, and the delayed
        #    (no-resize) move must have run too.
        if not title.startswith("js"):
            print("FAIL: resize event never reached page JS (title=%r)" % title)
            ok = False
        if "m" not in title:
            print("FAIL: delayed no-resize move did not run (title=%r)" % title)
            ok = False

        def rect(css_x, css_y, css_w, css_h):
            return (ax + round(css_x * s), ay + round(css_y * s),
                    ax + round((css_x + css_w) * s), ay + round((css_y + css_h) * s))

        # 2) Both rectangles the button has left must be background, with no
        #    exact and no interpolated remnant.  GONE is vacated by the resize
        #    JS, MID by the delayed move — MID in particular is only cleared
        #    if a partial frame's physical dirty rect was mapped to logical
        #    widget coordinates.
        for name, pos in (("old", GONE_BTN), ("mid", MID_BTN)):
            bx0, by0, bx1, by1 = rect(pos[0], pos[1], BTN_W, BTN_H)
            n = count_exact(px, bx0, by0, bx1, by1, BLUE)
            near = count_near_blue(px, bx0, by0, bx1, by1)
            print(f"{name}-rect blue pixels: {n} exact, {near} near-blue (want 0)")
            if n > 4 or near > 8:
                ok = False

        # 3) The new rect must be painted (physical pixel area).
        nx0, ny0, nx1, ny1 = rect(NEW_BTN[0], NEW_BTN[1], BTN_W, BTN_H)
        new_total = (nx1 - nx0) * (ny1 - ny0)
        new_n = count_exact(px, nx0, ny0, nx1, ny1, BLUE)
        print(f"new-rect blue pixels: {new_n}/{new_total} (want >= 90%)")
        if new_n < int(new_total * 0.90):
            ok = False

        # 4) Exactly one orange fixed element: a second block is the stale
        #    copy left at the previous viewport bottom.
        blocks = orange_blocks(px, w, h)
        print(f"orange fixed-element blocks: {len(blocks)} (want 1)")
        for b in blocks:
            print("  block x %d-%d y %d-%d, width %d"
                  % (b["x0"], b["x1"], b["y0"], b["y1"], b["x1"] - b["x0"]))
        if len(blocks) != 1:
            ok = False
        else:
            bw = blocks[0]["x1"] - blocks[0]["x0"]
            bx = blocks[0]["x0"] - ax
            print(f"  expected width ~{round(TOG_W * s)}, x ~{round(TOG_X * s)}")
            if abs(bw - round(TOG_W * s)) > 10 or abs(bx - round(TOG_X * s)) > 12:
                ok = False

        print(f"anchor physical=({ax},{ay}) shot={w}x{h}")
        print("RESIZE GHOST PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
