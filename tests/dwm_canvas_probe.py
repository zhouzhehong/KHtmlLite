# dwm_canvas_probe.py — verifies the kernel-side DWM presentation adapter.
#
# The page canvas is presented by khtml::KHTMLCompositor (KF5KHtml.dll) through
# Windows DWM/DirectComposition:
#   1. hardware D3D11 device            backend "d3d11-hw"
#   2. GPU unavailable / forced WARP    backend "warp"
#   3. simulated GPU failure            backend "warp"
# There is deliberately NO GDI/QPainter fallback, so every tier must show the
# page pixels on screen.  Screenshots come from the desktop DC (win_shot),
# because DWM-composited content is not part of the window's GDI surface.
#
# Usage: python tests\dwm_canvas_probe.py
import os
import subprocess
import sys
import time
import http.server
import socketserver
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from win_shot import find_window, grab  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8283

PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>dwm-canvas</title>
<style>
  html, body { margin: 0; padding: 0; background: #ffffff; }
  #c { position: absolute; left: 40px; top: 40px; width: 300px; height: 200px;
       background-color: #2266cc; }
</style></head><body><div id="c"></div></body></html>"""


def launch(extra_env):
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env.update(extra_env)
    err_path = os.path.join(ROOT, "tests", "dwm_canvas_probe.err")
    errf = open(err_path, "wb")
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"),
         "http://127.0.0.1:%d/dwmtest.html" % PORT],
        cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=errf)
    hwnd = None
    for _ in range(40):
        hwnd = find_window(proc.pid)
        if hwnd:
            break
        time.sleep(0.5)
    return proc, hwnd, errf, err_path


def canvas_lines(text):
    return [l for l in text.splitlines() if "[canvas]" in l]


def run_mode(tag, extra_env, expect_backend):
    proc, hwnd, errf, err_path = launch(extra_env)
    if not hwnd:
        print("FAIL: no window for", tag)
        proc.kill()
        errf.close()
        return False
    ok = True
    out = os.path.join(ROOT, "tests", "dwm_canvas_probe.png")
    try:
        time.sleep(4.0)
        w, h = grab(hwnd, out)
        px = Image.open(out).convert("RGB").load()
        sample = px[200, 250]          # inside the #2266cc box
        good_pixel = abs(sample[0] - 34) <= 12 and abs(sample[1] - 102) <= 12 \
            and abs(sample[2] - 204) <= 12
        errf.flush()
        text = ""
        try:
            with open(err_path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            pass
        lines = canvas_lines(text)
        joined = "\n".join(lines)
        active = "dwm composition active" in joined
        backend_ok = (("backend=%s " % expect_backend) in joined
                      or ("backend=%s\n" % expect_backend) in joined
                      or ("backend=%s d" % expect_backend) in joined)
        if expect_backend is None:
            backend_ok = "backend=d3d11-hw" in joined or "backend=warp" in joined
        no_failure = "backend=none" not in joined
        print("=== %s ===" % tag)
        print("  screenshot %dx%d sample=%s" % (w, h, sample))
        print("  canvas:", lines[:3])
        for name, good in (("page pixels on screen", good_pixel),
                           ("dwm composition active", active),
                           ("expected backend (%s)" % (expect_backend or "hw-or-warp"), backend_ok),
                           ("no backend failure", no_failure)):
            print("  [%s] %s" % ("OK" if good else "FAIL", name))
            ok = ok and good

        # Resize must rebuild the composition swap chain without falling back.
        from ctypes import windll, byref, create_string_buffer
        import struct
        user32 = windll.user32
        wr = create_string_buffer(16)
        user32.GetWindowRect(hwnd, wr)
        x, y, x2, y2 = struct.unpack("4i", wr.raw)
        user32.SetWindowPos(hwnd, 0, x, y, 1400, 760, 0x0004 | 0x0010)
        time.sleep(1.6)
        grab(hwnd, out)
        px2 = Image.open(out).convert("RGB").load()
        sample2 = px2[200, 250]
        good2 = abs(sample2[0] - 34) <= 12 and abs(sample2[1] - 102) <= 12 \
            and abs(sample2[2] - 204) <= 12
        with open(err_path, encoding="utf-8", errors="replace") as f:
            tail = f.read()
        resize_fail = "backend=none" in "\n".join(canvas_lines(tail))
        print("  resize sample=%s" % (sample2,))
        print("  [%s] page survives resize" % ("OK" if good2 else "FAIL"))
        print("  [%s] no failure after resize" % ("OK" if not resize_fail else "FAIL"))
        ok = ok and good2 and not resize_fail
    finally:
        try:
            os.remove(out)
        except OSError:
            pass
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        errf.close()
        try:
            os.remove(err_path)
        except OSError:
            pass
    return ok


def main():
    d = os.path.join(ROOT, "tests", "dpr")
    os.makedirs(d, exist_ok=True)
    page = os.path.join(d, "dwmtest.html")
    with open(page, "w", encoding="utf-8") as f:
        f.write(PAGE)
    os.chdir(d)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    ok = True
    try:
        ok = run_mode("hardware (default)", {}, None) and ok
        ok = run_mode("forced WARP", {"KHTML_FORCE_WARP": "1"}, "warp") and ok
        ok = run_mode("simulated GPU failure", {"KHTML_FORCE_D3D_FAIL": "1"}, "warp") and ok
    finally:
        httpd.shutdown()
        try:
            os.remove(page)
        except OSError:
            pass
    print("DWM CANVAS PROBE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
