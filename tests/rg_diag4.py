# rg_diag4.py — control experiment: the SAME debounced style move, with NO
# window resize anywhere in the run.
#
# rg_diag2 (which resizes in stage 3) shows, in the renderer trace:
#     [schedRepaint] 200,100,54,37  +  [schedRepaint] 600,60,54,37   (move)
#     [host] resize applied ... relayoutPending=1
#     [host] grabFrame ... dirtyRectCount=0 ... relayout=1
#     [host] tiles ... uploaded=2 suppressed=26
# i.e. m_relayoutPending REPLACES the move's two damage rects with the full
# viewport, and the re-render then yields pixel-identical tiles for 26/28
# tiles.  This probe removes the resize entirely: if the button lands at NEW
# without it, the "style move is never painted" hypothesis is FALSE and the
# real defect is the resize/relayout frame (engine paints stale content into
# the first post-resize frame).
#
# Usage: python tests\rg_diag4.py
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/resize_ghost.html"
OUT = os.path.join(ROOT, "tests", "rg_diag4_out.png")
ERR = os.path.join(ROOT, "tests", "rg_diag4_err.txt")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402

DPR = 1.5
BLUE = (0, 120, 212)
ANCHOR = (51, 51, 51)
BTN_W, BTN_H = 54, 37
MID = (200, 100)
NEW = (600, 60)


def find_anchor(px, w, h):
    for y in range(40, h - 10):
        for x in range(0, w - 12):
            if px[x, y] == ANCHOR and px[x + 8, y + 8] == ANCHOR \
                    and px[x + 4, y + 8] == ANCHOR and px[x + 8, y + 4] == ANCHOR:
                return x, y
    return None


def counts(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    a = find_anchor(px, w, h)
    if not a:
        return None
    ax, ay = a
    s = DPR

    def n(pos):
        x0 = ax + round(pos[0] * s)
        y0 = ay + round(pos[1] * s)
        x1 = ax + round((pos[0] + BTN_W) * s)
        y1 = ay + round((pos[1] + BTN_H) * s)
        c = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                if px[x, y] == BLUE:
                    c += 1
        return c, (x1 - x0) * (y1 - y0)
    return n(MID), n(NEW)


def main():
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env["KHTML_DAMAGE_TRACE"] = "1"
    errf = open(ERR, "wb")
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), PAGE],
                            cwd=BUILD, env=env,
                            stdout=subprocess.DEVNULL, stderr=errf)
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("no window")
            return 1
        for _ in range(40):
            grab(hwnd, OUT)
            if counts(OUT) is not None:
                break
            time.sleep(0.5)
        # NO resize from here on.  Wait well past the 500 ms debounce.
        time.sleep(8.0)
        grab(hwnd, OUT)
        mid, new = counts(OUT)
        print(f"screen (no resize at all): mid={mid[0]}/{mid[1]} "
              f"new={new[0]}/{new[1]}")
        if mid[0] <= 4 and new[0] >= int(new[1] * 0.9):
            print("CONTROL PASS: the style move IS painted correctly when no "
                  "window resize happens -> the defect is in the first "
                  "post-resize frame, not in the style-move path")
        else:
            print("CONTROL FAIL: the style move is NOT painted even without a "
                  "resize -> the style-move paint path itself is broken")
        return 0
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()


if __name__ == "__main__":
    sys.exit(main())
