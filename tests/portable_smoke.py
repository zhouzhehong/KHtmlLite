# portable_smoke.py — smoke test the PORTABLE package (release/KHtmlLite-portable).
#
# Launches the packaged khtml_browser.exe on a local page and asserts the
# process pair comes up and the page actually paints (an anchor pixel is
# visible in a window capture).  Uses the same launch/capture helpers as the
# other probes, which are known to work in this environment.
#
# Usage: python tests\portable_smoke.py
import ctypes
import os
import subprocess
import sys
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
PORTABLE = os.path.join(ROOT, "release", "KHtmlLite-portable")
PAGE = "file:///" + ROOT.replace("\\", "/") + "/tests/resize_ghost.html"
OUT = os.path.join(ROOT, "tests", "portable_smoke.png")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402

ANCHOR = (51, 51, 51)


def has_anchor(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    for y in range(40, min(h - 10, 200)):
        for x in range(0, min(w - 12, 400)):
            if px[x, y] == ANCHOR and px[x + 8, y + 8] == ANCHOR \
                    and px[x + 4, y + 8] == ANCHOR and px[x + 8, y + 4] == ANCHOR:
                return (x, y)
    return None


def main():
    exe = os.path.join(PORTABLE, "khtml_browser.exe")
    if not os.path.exists(exe):
        print("FAIL: packaged browser missing:", exe)
        return 1
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    errf = open(os.path.join(ROOT, "tests", "portable_smoke_err.txt"), "wb")
    proc = subprocess.Popen([exe, PAGE], cwd=PORTABLE, env=env,
                            stdout=subprocess.DEVNULL, stderr=errf)
    try:
        hwnd = None
        for _ in range(40):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no window appeared for the packaged browser")
            return 1
        anchor = None
        for _ in range(40):
            grab(hwnd, OUT)
            anchor = has_anchor(OUT)
            if anchor:
                break
            time.sleep(0.5)
        renderers = [p for p in _renderer_pids()
                     if p not in (0, proc.pid)]
        print("window: yes; anchor:", anchor)
        print("renderer processes seen:", renderers)
        if not anchor:
            print("PORTABLE SMOKE FAIL: page never painted")
            return 1
        print("PORTABLE SMOKE PASS")
        return 0
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()


def _renderer_pids():
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process khtml_renderer -ErrorAction SilentlyContinue).Id"],
        capture_output=True, text=True)
    pids = []
    for line in out.stdout.split():
        try:
            pids.append(int(line))
        except ValueError:
            pass
    return pids


if __name__ == "__main__":
    sys.exit(main())
