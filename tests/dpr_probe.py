# dpr_probe.py — true deviceScaleFactor / CSS-px viewport probe.
#
# Drives khtml_renderer.exe directly (no browser) and asserts the whole
# contract of the CSS-px viewport model:
#
#   1. resize {w,h,dpr} sets a LOGICAL viewport: the page's
#      window.innerWidth/innerHeight report CSS pixels (w,h), and
#      window.devicePixelRatio reports the ratio;
#   2. the frame CANVAS is physical: the "tiles" message carries
#      fw = round(w*dpr), fh = round(h*dpr);
#   3. engine damage/tiles still cover the full canvas (no gap from the
#      logical->physical mapping).
#
# Usage: python tests\dpr_probe.py
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8196
PAGE_DIR = os.path.join(ROOT, "tests", "dpr")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from scroll_probe import Pipe  # noqa: E402  (line-oriented pipe client)

PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>dpr-pending</title>
<script>
document.title = 'w=' + window.innerWidth
               + ' h=' + window.innerHeight
               + ' r=' + window.devicePixelRatio;
// The timer bridge (setTimeout) is asserted in the same run: the callback
// replaces the title, so both a correct viewport report AND a fired timer
// must be observed.
setTimeout(function () { document.title = 'timer-ok'; document.body.style.backgroundColor = '#ff0000'; }, 500);
</script></head>
<body style="margin:0;background:#0a5c50">
<div id="box" style="width:100%;height:100%;position:absolute;
     background:#0a5c50">
<span style="color:#ffffff;font-size:14px">dpr probe</span>
</div>
</body></html>
"""


def start_server():
    os.makedirs(PAGE_DIR, exist_ok=True)
    with open(os.path.join(PAGE_DIR, "dpr.html"), "w") as f:
        f.write(PAGE)
    os.chdir(PAGE_DIR)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def drive(w, h, dpr):
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    sock = f"khtml_dprprobe_{int(time.time()*1000)%10**8}"
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_renderer.exe"), "--url", "about:blank",
         "--socket", sock, "--width", "640", "--height", "400"],
        cwd=BUILD, env=env,
        stderr=open(os.path.join(ROOT, "tests", "dpr_probe_stderr.txt"), "wb"))
    titles, frames = [], []
    try:
        pipe = Pipe(sock)
        end = time.time() + 8
        got_ready = False
        while time.time() < end and not got_ready:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "ready":
                    got_ready = True
        if not got_ready:
            return None, None, "renderer never sent ready"
        # Viewport FIRST, navigation second: the page must load into the
        # CSS-px viewport it reports (the browser does the same: resize on
        # connect, navigate when ready).
        pipe.send({"cmd": "resize", "w": w, "h": h, "dpr": dpr})
        time.sleep(0.4)
        pipe.send({"cmd": "navigate",
                   "url": f"http://127.0.0.1:{PORT}/dpr.html"})
        end = time.time() + 14
        while time.time() < end:
            for _, m in pipe.recv(0.3):
                t = m.get("type")
                if t == "title":
                    titles.append(m.get("data"))
                elif t == "tiles":
                    frames.append(m.get("data"))
        pipe.close()
    finally:
        proc.kill()
    return titles, frames, None


def main():
    httpd = start_server()
    try:
        w, h, dpr = 680, 356, 1.5
        titles, frames, err = drive(w, h, dpr)
        if err:
            print("PROBE ERROR:", err)
            print("DPR PROBE FAIL")
            return 1
        print(f"titles: {titles}")
        print(f"tiles messages: {len(frames)} first={frames[0] if frames else None}")

        report = None
        for t in reversed(titles):
            if t and t.startswith("w="):
                report = t
                break
        timer_ok = any(t == "timer-ok" for t in titles)
        print(f"setTimeout callback fired: {timer_ok}")
        ok = False
        if report:
            parts = dict(p.split("=", 1) for p in report.split())
            iw, ih, rd = int(parts["w"]), int(parts["h"]), float(parts["r"])
            phys_w = round(w * dpr)
            phys_h = round(h * dpr)
            got_fw = got_fh = -1
            for f in frames:
                a = f.split(",")
                if len(a) >= 2:
                    got_fw, got_fh = int(a[0]), int(a[1])
                    break
            print(f"reported: innerWidth={iw} innerHeight={ih} dpr={rd}")
            print(f"expected: innerWidth={w} innerHeight={h} dpr={dpr}")
            print(f"canvas  : tiles fw={got_fw} fh={got_fh} "
                  f"(expect {phys_w}x{phys_h})")
            # innerWidth is the visible (non-scrollbar) CSS-px viewport; the
            # page has no scrollbar, so it must equal the logical width.
            width_ok = abs(iw - w) <= 1
            height_ok = abs(ih - h) <= 1
            ratio_ok = abs(rd - dpr) < 0.01
            canvas_ok = (got_fw, got_fh) == (phys_w, phys_h)
            ok = width_ok and height_ok and ratio_ok and canvas_ok and timer_ok
            print(f"check: innerWidth {'OK' if width_ok else 'BAD'}, "
                  f"innerHeight {'OK' if height_ok else 'BAD'}, "
                  f"devicePixelRatio {'OK' if ratio_ok else 'BAD'}, "
                  f"physical canvas {'OK' if canvas_ok else 'BAD'}")
        else:
            print("no w=.../h=.../r=... title report arrived")
        print("DPR PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
