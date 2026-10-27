# scroll_direction_probe.py — direction-correct wheel/predictive scrolling.
#
# A single colored block sits at a known Y.  Wheel-down must move it UP
# immediately (predictive preview AND the authoritative frame); wheel-up must
# move it back DOWN.  This pins the sign convention that used to be inverted
# in PageSurface's predictive preview (content jumped the wrong way until the
# renderer's scroll message arrived).
#
# Usage: python tests\scroll_direction_probe.py
import ctypes
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8207
URL = f"http://127.0.0.1:{PORT}/scrolldir.html"
BLOCK = (0xCC, 0x00, 0x33)
TOL = 30

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab, force_foreground  # noqa: E402
from PIL import Image  # noqa: E402

USER32 = ctypes.windll.user32


def send_wheel(hwnd, delta, x, y, count=1):
    lp = (y << 16) | (x & 0xFFFF)
    wparam = (delta << 16) & 0xFFFFFFFF
    for _ in range(count):
        USER32.PostMessageW(hwnd, 0x020A, wparam, lp)
        time.sleep(0.02)


def block_top(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    ys = []
    for y in range(90, min(h, 920), 2):
        for x in range(16, min(w, 1200), 4):
            r, g, b = px[x, y]
            if abs(r - BLOCK[0]) <= TOL and abs(g - BLOCK[1]) <= TOL and abs(b - BLOCK[2]) <= TOL:
                ys.append(y)
                break
    return min(ys) if ys else None


def main():
    # Normal flow (absolute positioning is not reliably painted by this
    # engine yet); a 300px spacer places the block at a known document Y.
    page = ('<!DOCTYPE html><html><head><meta charset="utf-8"><title>scroll-dir</title>'
            '</head><body style="margin:0;background:#ffffff">'
            '<div style="height:300px"></div>'
            '<div style="width:240px;height:160px;background:#cc0033"></div>'
            '<div style="height:3000px"></div></body></html>')
    d = os.path.join(ROOT, "tests", "dpr")
    os.makedirs(d, exist_ok=True)
    path_html = os.path.join(d, "scrolldir.html")
    open(path_html, "w", encoding="utf-8").write(page)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    class Srv(socketserver.TCPServer):
        allow_reuse_address = True

    os.chdir(d)
    httpd = Srv(("127.0.0.1", PORT), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    trace = os.path.join(ROOT, "tests", "scroll_direction.err")
    tracef = open(trace, "wb")
    env["KHTML_TOMBSTONE_DEBUG"] = "1"
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env, stdout=subprocess.DEVNULL,
                            stderr=tracef)
    shot = os.path.join(ROOT, "tests", "scroll_direction.png")
    ok = True

    def check(name, good, detail=""):
        nonlocal ok
        print(f"  [{'OK' if good else 'FAIL'}] {name} {detail}")
        ok = ok and good

    try:
        hwnd = None
        for _ in range(40):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            return 1
        force_foreground(hwnd)
        time.sleep(2.0)
        grab(hwnd, shot)
        try:
            tracef.flush()
            tl = [l for l in open(trace, encoding="utf-8", errors="replace").read().splitlines()
                  if "titleChanged" in l]
            print("  titles:", tl[-2:])
        except OSError:
            pass
        y0 = block_top(shot)
        check("colored block visible", y0 is not None, f"y={y0}")
        if y0 is None:
            return 1

        # One wheel notch DOWN (negative delta): content must move UP fast.
        send_wheel(hwnd, -120, 900, 500)
        time.sleep(0.08)
        grab(hwnd, shot)
        y_down = block_top(shot)
        check("wheel-down moves content UP (preview/authority)",
              y_down is not None and y_down < y0, f"{y0} -> {y_down}")
        time.sleep(0.9)
        grab(hwnd, shot)
        y_settled = block_top(shot)
        check("wheel-down settled still above baseline",
              y_settled is not None and y_settled < y0, f"settled {y_settled}")

        # One wheel notch UP: content must move DOWN immediately.
        send_wheel(hwnd, 120, 900, 500)
        time.sleep(0.08)
        grab(hwnd, shot)
        y_up = block_top(shot)
        check("wheel-up moves content DOWN immediately",
              y_up is not None and y_up > (y_settled or 0), f"{y_settled} -> {y_up}")
        time.sleep(1.0)
        grab(hwnd, shot)
        y_final = block_top(shot)
        check("wheel-up returns near baseline",
              y_final is not None and abs(y_final - y0) <= 12, f"final {y_final} vs {y0}")

        alive = proc.poll() is None
        check("browser alive", alive)
        print("SCROLL DIRECTION PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        httpd.shutdown()
        try:
            tracef.close()
        except Exception:
            pass
        for f in (shot, path_html, trace):
            try:
                os.remove(f)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
