# rg_diag2.py — decisive diagnostic for the remaining partial-frame failure.
#
# The strengthened resize_ghost_probe shows that AFTER the delayed
# (no-resize) move of the blue button the screen still shows it at the MID
# position and nothing at the NEW position.  Two very different causes:
#   (a) the renderer/canvas never received the new frame  (engine damage
#       starvation -> the browser canvas itself is stale), or
#   (b) the canvas is correct but the widget was never repainted there
#       (browser update region / physical->logical mapping).
# Discriminator: force FULL frames afterwards (window resizes; the page stops
# reacting to resize once settled) and re-capture.  Under (b) the button then
# appears at NEW and MID is clean; under (a) it stays at MID.
#
# Usage: python tests\rg_diag2.py
import ctypes
import os
import struct
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/resize_ghost.html"
A_PNG = os.path.join(ROOT, "tests", "rg_diag2_a.png")
B_PNG = os.path.join(ROOT, "tests", "rg_diag2_b.png")
C_PNG = os.path.join(ROOT, "tests", "rg_diag2_c.png")
ERR = os.path.join(ROOT, "tests", "rg_diag2_err.txt")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402

USER32 = ctypes.windll.user32
DPR = 1.5
FRAME_W, FRAME_H = 22, 56
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
    # The browser passes its environment to the renderer (QProcess inherits),
    # and the renderer inherits the browser's stderr — so one file captures
    # the engine damage traces (KHTML_DAMAGE_TRACE) of the REAL browser path.
    env["KHTML_DAMAGE_TRACE"] = "1"
    env["KHTML_FRAME_DUMP"] = "1"     # renderer saves its painted frame
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
        # Wait until the page has painted (anchor visible), then give the
        # debounced no-resize move ample time to fire.  No window resize is
        # performed from here on, so the page's own resize listener cannot
        # move the button again during the measurement.
        for _ in range(40):
            grab(hwnd, A_PNG)
            if counts(A_PNG) is not None:
                break
            time.sleep(0.5)
        time.sleep(8.0)   # debounce (500 ms) + repaint, with a wide margin
        grab(hwnd, A_PNG)
        mid_a, new_a = counts(A_PNG)
        print(f"screen BEFORE forced repaint: mid={mid_a[0]}/{mid_a[1]} new={new_a[0]}/{new_a[1]}")
        # Snapshot what the RENDERER itself painted at this moment (its
        # frame_dump.png is overwritten on every grab that passes the
        # ~1 Hz throttle, so copy it now, before the click re-renders).
        import shutil
        try:
            shutil.copy(os.path.join(BUILD, "frame_dump.png"),
                        os.path.join(ROOT, "tests", "rg_diag2_frame_A.png"))
        except OSError:
            pass

        # Force the widget to repaint ITSELF from the frame it already holds:
        # RedrawWindow with RDW_UPDATENOW produces WM_PAINT synchronously and
        # asks the renderer for NOTHING (no resize, no new frame image).
        RDW_INVALIDATE, RDW_UPDATENOW, RDW_ALLCHILDREN = 0x0001, 0x0100, 0x0080
        USER32.RedrawWindow(hwnd, None, None,
                            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN)
        time.sleep(0.6)
        grab(hwnd, B_PNG)
        mid_b, new_b = counts(B_PNG)
        print(f"screen AFTER  forced repaint: mid={mid_b[0]}/{mid_b[1]} new={new_b[0]}/{new_b[1]}")

        # Stage 3: force a RE-RENDER without touching the page logic, via a
        # small deterministic window resize (the page's resize handler early-
        # returns once settled, so the button is NOT moved again; only the
        # geometry changes).  A click was tried first but SetForegroundWindow
        # is racy and the click sometimes landed on another window.
        wr = ctypes.create_string_buffer(16)
        USER32.GetWindowRect(hwnd, wr)
        rl, rt, rr, rb = struct.unpack("4i", wr.raw)
        USER32.SetWindowPos(hwnd, 0, 0, 0, (rr - rl) + 8, rb - rt, 0x0002 | 0x0004)
        time.sleep(1.2)
        grab(hwnd, C_PNG)
        mid_c, new_c = counts(C_PNG)
        print(f"screen AFTER  resize re-render: mid={mid_c[0]}/{mid_c[1]} new={new_c[0]}/{new_c[1]}")
        import shutil
        try:
            shutil.copy(os.path.join(BUILD, "frame_dump.png"),
                        os.path.join(ROOT, "tests", "rg_diag2_frame_C.png"))
        except OSError:
            pass

        if mid_a[0] <= 4 and new_a[0] >= int(new_a[1] * 0.9):
            print("VERDICT: no failure observed in this run (screen already "
                  "matched the new position)")
        elif mid_b[0] <= 4 and new_b[0] >= int(new_b[1] * 0.9):
            print("VERDICT: the held frame/canvas is CORRECT and a plain "
                  "repaint shows the new position -> the failing piece is the "
                  "browser's partial-frame update (case b)")
        elif mid_c[0] <= 4 and new_c[0] >= int(new_c[1] * 0.9):
            print("VERDICT: the render tree was already correct; the engine "
                  "never reported damage for the style move (case a1: damage "
                  "notification starvation)")
        else:
            print("VERDICT: even a resize-driven re-render shows the old "
                  "position -> the render tree itself never moved the element "
                  "(case a2: style change did not reach the render tree)")
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()

    # --- renderer-side damage trace for the REAL browser path ---
    # Anchor: the page's debounced no-resize move is the LAST "[qjs timer"
    # callback (the resize handler reschedules it on every resize; after the
    # drag it fires exactly once).  What the engine/host did AFTER that line
    # is the failing partial frame.
    lines = open(ERR, encoding="utf-8", errors="replace").read().splitlines()
    anchors = [i for i, l in enumerate(lines) if l.startswith("[qjs timer")]
    print("stderr lines: %d, qjs timer anchors: %d" % (len(lines), len(anchors)))
    if anchors:
        a = anchors[-1]
        keys = ("[setStyle]", "[schedRepaint]", "[flush]", "[host]")
        post = {k: [] for k in keys}
        for l in lines[a + 1:]:
            for k in keys:
                if l.startswith(k):
                    post[k].append(l)
        for k in keys:
            print("%s: %d lines after the debounced move" % (k, len(post[k])))
            for l in post[k][-6:]:
                print("   ", l)
        if post["[host]"]:
            print("RENDERER-SIDE: damage DID reach the renderer host after "
                  "the move -> the loss is in tiles/upload/browser compositing")
        elif post["[schedRepaint]"]:
            print("RENDERER-SIDE: damage was scheduled but never reached the "
                  "host hook in the browser path")
        else:
            print("RENDERER-SIDE: no damage at all reached the renderer "
                  "after the debounced move")
    return 0


if __name__ == "__main__":
    sys.exit(main())
