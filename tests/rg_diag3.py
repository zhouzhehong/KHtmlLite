# rg_diag3.py — isolate the engine damage path for a style-driven element
# move, driving khtml_renderer.exe DIRECTLY (no browser, no window resize,
# no user input).  The page moves the element on a timer; the host process
# is built with KHTML_DAMAGE_TRACE=1 tracing, so the log shows whether:
#   [setStyle]      the style change reached RenderObject::setStyle (and with
#                   which diff) — a missing line means the JS style write did
#                   not trigger a style recalculation at all;
#   [schedRepaint]  KHTMLView::scheduleRepaint was called with the damage;
#   [host]          the renderer host received contentsUpdated with the rect.
#
# Usage: python tests\rg_diag3.py
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/rg_move.html"
ERR = os.path.join(ROOT, "tests", "rg_diag3_err.txt")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from scroll_probe import Pipe  # noqa: E402


def main():
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env["KHTML_DAMAGE_TRACE"] = "1"
    sock = "khtml_rgdiag3_%d" % (int(time.time() * 1000) % 10 ** 8)
    # Launch through cmd with an inline `set`: passing env= to subprocess did
    # not make the variables visible to the freshly launched renderer here
    # (verified: the page's JS ran but no getenv-gated log appeared).
    inner = ('set KHTML_DAMAGE_TRACE=1&& set KHTML_QJS_TRACE=1&& '
             'set KHTML_STARTUP_TIMING=1&& "%s" '
             '--url "%s" --socket %s --width 1000 --height 600 --dpr 1.5'
             % (os.path.join(BUILD, "khtml_renderer.exe"), PAGE, sock))
    errf = open(os.path.join(ROOT, "tests", "rg_diag3_err.txt"), "wb")
    proc = subprocess.Popen("cmd /c " + inner, cwd=BUILD, env=env,
                            stdout=subprocess.DEVNULL, stderr=errf)
    try:
        pipe = Pipe(sock)
        deadline = time.time() + 20
        ready = False
        while time.time() < deadline and not ready:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "ready":
                    ready = True
        print("ready:", ready)
        pipe.send({"cmd": "visible", "v": 1})
        # Wait past both page timers (1.5 s move, 3.0 s colour change) and
        # record every message type so a page that never loads is obvious.
        seen = []
        end = time.time() + 6.0
        while time.time() < end:
            for _, m in pipe.recv(0.3):
                t = m.get("type")
                if t in ("title", "url", "error", "load"):
                    seen.append((t, m.get("data")))
                else:
                    seen.append((t, None))
        print("messages:", seen)
        proc.terminate()
        time.sleep(0.3)
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()

    lines = open(ERR, encoding="utf-8", errors="replace").read().splitlines()
    print("stderr lines: %d" % len(lines))
    keys = ("[setStyle]", "[schedRepaint]", "[flush]", "[host]")
    # Anchor: the page's first timer callback (the 1.5 s move).  Everything
    # before it is page-load noise; the question is what happens AFTER it.
    anchor = next((i for i, l in enumerate(lines) if l.startswith("[qjs timer")), None)
    print("first qjs timer line index:", anchor)
    marks = {k: [] for k in keys}
    post = {k: 0 for k in keys}
    for i, l in enumerate(lines):
        for k in keys:
            if l.startswith(k):
                marks[k].append(l)
                if anchor is not None and i > anchor:
                    post[k] += 1
    for k in keys:
        print("%-16s %d lines (%d after the first timer)"
              % (k, len(marks[k]), post[k]))
        for l in marks[k][-4:]:
            print("   ", l)
    print("--- verdict (post-move lines) ---")
    ss, sr, fl, ho = (post["[setStyle]"], post["[schedRepaint]"],
                      post["[flush]"], post["[host]"])
    if not ss:
        print("the JS style write never reached RenderObject::setStyle "
              "(style recalculation is not triggered)")
    elif not sr:
        print("setStyle ran but no repaint was ever scheduled -> the engine "
              "damage path does not notify for this change")
    elif not fl:
        print("damage was scheduled but the repaint timer never flushed it "
              "(layout/early-return path ate the update region)")
    elif not ho:
        print("damage was flushed but never reached the host hook "
              "(repaintContents -> contentsUpdated is broken)")
    else:
        print("damage flows setStyle -> scheduleRepaint -> flush -> host "
              "for the post-move frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
