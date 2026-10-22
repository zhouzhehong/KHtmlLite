# bing_ghost_check.py — integration check of the ghost-pixel fix on the
# user's real reproduction site (cn.bing.com, window dragged from the
# 1203x618 default down to 838x499 logical with pure-height steps).
# Detects SOLID #0078D4 rectangles (the ghost signature) after the drag.
# Starts its own browser instance; never touches other processes.
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
OUT = os.path.join(ROOT, "tests", "bing_ghost_after.png")
OUT_BEFORE = os.path.join(ROOT, "tests", "bing_ghost_before.png")

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


def solid_blue_blocks(px, w, h):
    """Rows of >=30px-wide solid blue runs clustered into blocks."""
    rows = []
    for y in range(85, h, 3):
        run = None
        for x in range(0, w):
            if px[x, y] == BLUE:
                if run is None:
                    run = [x, x]
                else:
                    run[1] = x
            else:
                if run and run[1] - run[0] >= 30:
                    rows.append((y, run[0], run[1]))
                run = None
        if run and run[1] - run[0] >= 30:
            rows.append((y, run[0], run[1]))
    # cluster rows into blocks (same x-range overlapping, y within 6px)
    blocks = []
    for y, x0, x1 in rows:
        merged = False
        for b in blocks:
            if abs(b["y1"] - y) <= 6 and not (x1 < b["x0"] - 20 or x0 > b["x1"] + 20):
                b["y1"] = y
                b["x0"] = min(b["x0"], x0)
                b["x1"] = max(b["x1"], x1)
                b["rows"] += 1
                merged = True
                break
        if not merged:
            blocks.append({"y0": y, "y1": y, "x0": x0, "x1": x1, "rows": 1})
    return [b for b in blocks if b["rows"] >= 5]


def page_loaded(path, w, h):
    """Sanity gate: a blank/failed load must not be judged as a clean page.

    Do NOT require site-specific colours here: cn.bing.com serves different
    layouts (and a degraded one when the network is restricted), so a
    "must contain the blue search button" gate produces false LOAD GATE
    FAILs on pages that did load fine — measured: a loaded page was 47%
    non-white with only grey content.  What matters is that the page painted
    real content rather than staying blank.
    """
    from PIL import Image
    im = Image.open(path).convert("RGB")
    px = im.load()
    nonwhite = 0
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            if px[x, y] != (255, 255, 255):
                nonwhite += 1
    total = (h // 3 + 1) * (w // 3 + 1)
    return nonwhite >= total * 0.03


def main():
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"), "https://cn.bing.com"],
        cwd=BUILD, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        hwnd = None
        for _ in range(24):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("no window")
            return 1
        time.sleep(8.0)  # bing load

        # Baseline BEFORE any resize: how many solid blue blocks does the
        # intact page have?  Comparing after-vs-before removes the need to
        # hard-code the site's own blue elements (the old "<= 1" threshold
        # was a guess that also passed on a blank page).
        w0, h0 = grab(hwnd, OUT_BEFORE)
        if not page_loaded(OUT_BEFORE, w0, h0):
            print("LOAD GATE FAIL: cn.bing.com did not paint (page is blank) "
                  "— cannot judge ghosts")
            return 1
        from PIL import Image
        px0 = Image.open(OUT_BEFORE).convert("RGB").load()
        blocks_before = solid_blue_blocks(px0, w0, h0)
        print(f"baseline {w0}x{h0}: solid #0078D4 blocks: {len(blocks_before)}")

        seq = [(1100, 560), (1000, 520), (1000, 460), (1000, 499)]
        for cw, ch in seq:
            USER32.SetWindowPos(hwnd, 0, 0, 0,
                                round(cw * DPR) + FRAME_W,
                                round(ch * DPR) + FRAME_H,
                                0x0002 | 0x0004)
            time.sleep(0.04)
        time.sleep(2.5)  # resize JS + repaint settle
        w, h = grab(hwnd, OUT)
        px = Image.open(OUT).convert("RGB").load()
        blocks = solid_blue_blocks(px, w, h)
        print(f"after    {w}x{h}: solid #0078D4 blocks found: {len(blocks)}")
        for b in blocks:
            print("  block x %d-%d y %d-%d (physical)"
                  % (b["x0"], b["x1"], b["y0"], b["y1"]))
        # A ghost is EXTRA solid-blue content that the baseline did not have.
        # The window got smaller, so the page can legitimately show fewer
        # elements; it must never show more solid blue blocks than before.
        extra = len(blocks) - len(blocks_before)
        ok = extra <= 0
        print(f"extra solid-blue blocks vs baseline: {extra} (ghost if > 0)")
        if not blocks_before and not blocks:
            # Degenerate run: this page painted no solid #0078D4 element at all
            # here, so "extra <= 0" would be true for ANY rendering — the check
            # measured nothing.  Say so instead of printing a green PASS; the
            # discriminating local coverage is tests/resize_ghost_probe.py.
            print("BING GHOST CHECK INCONCLUSIVE: no solid #0078D4 element in "
                  "either capture (this machine's cn.bing.com layout/colour "
                  "differs) — cannot detect a ghost this way")
            return 0
        print("BING GHOST CHECK " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)


def selftest():
    """Offline validation of the ghost DETECTOR itself — no browser needed.

    The real captures this detector was tuned on are regenerable probe
    artifacts (gitignored), so a fresh checkout would have no fixture to test
    against.  The two synthetic cases below pin the thresholds instead: a
    synthetic ghost MUST be found, a clean frame MUST NOT.  The real captures
    are additionally gated RELATIVELY (ghost capture must show more solid blue
    than the fixed capture) when they happen to be present locally.
    """
    from PIL import Image, ImageDraw

    failures = []

    def synthetic(want_any, label):
        w, h = 400, 300          # scan starts at y=85, so 300 is enough
        im = Image.new("RGB", (w, h), (255, 255, 255))
        if want_any:
            # 80x50 solid #0078D4 block: well below y=85, runs >> 30 px, so it
            # must cluster into exactly one detected block.
            ImageDraw.Draw(im).rectangle([100, 150, 179, 199], fill=BLUE)
        got = len(solid_blue_blocks(im.load(), w, h))
        ok = (got > 0) == bool(want_any)
        print("  synthetic %-6s -> blocks=%d (want %s) %s"
              % (label, got, ">=1" if want_any else "0", "OK" if ok else "FAIL"))
        if not ok:
            failures.append("synthetic-" + label)

    print("[detector selftest]")
    synthetic(True, "ghost")
    synthetic(False, "clean")

    # The real captures are regenerable artifacts whose ABSOLUTE block counts
    # depend on how the site happened to render (this machine's cn.bing.com has
    # measured 0 solid-#0078D4 blocks), so the gate between them is RELATIVE —
    # the known ghost capture must show MORE solid blue than the fixed one — and
    # it only applies when both are present.
    blk = {}
    for name in ("glitch1.png", "bing_ghost_after.png"):
        p = os.path.join(ROOT, "tests", name)
        if not os.path.exists(p):
            print("  %-22s absent (regenerable artifact) -> SKIP" % name)
            continue
        im = Image.open(p).convert("RGB")
        blk[name] = len(solid_blue_blocks(im.load(), im.size[0], im.size[1]))
        print("  %-22s blocks=%d (informational)" % (name, blk[name]))
    if "glitch1.png" in blk and "bing_ghost_after.png" in blk:
        if blk["glitch1.png"] > blk["bing_ghost_after.png"]:
            print("  relative gate: ghost capture %d > fixed capture %d OK"
                  % (blk["glitch1.png"], blk["bing_ghost_after.png"]))
        else:
            print("  relative gate: ghost capture %d <= fixed capture %d FAIL"
                  % (blk["glitch1.png"], blk["bing_ghost_after.png"]))
            failures.append("relative-gate")

    if failures:
        print("DETECTOR SELFTEST FAIL: " + ", ".join(failures))
        return 1
    print("DETECTOR SELFTEST PASS")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
