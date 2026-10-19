# input_coord_probe.py — click-coordinate probe for the CSS-px viewport.
#
# Before the deviceScaleFactor rework the renderer's widget was PHYSICAL
# pixels while the browser already shipped LOGICAL (CSS px) input points, so
# every click landed at 1/dpr of its true position (a 1.5x offset on this
# 150% display).  With the CSS-px viewport the widget *is* CSS pixels and the
# mapping is identity.  This probe proves it by clicking two page targets at
# known CSS coordinates and reading back the title each handler sets.
#
# Usage: python tests\input_coord_probe.py
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8198

sys.path.insert(0, os.path.join(ROOT, "tests"))
from scroll_probe import Pipe  # noqa: E402

PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>input-pending</title>
<style>
 body { margin:0 }
 #tl { position:absolute; left:0; top:0; width:200px; height:150px;
       background:#2255aa }
 #br { position:absolute; left:480px; top:200px; width:200px; height:150px;
       background:#aa5522 }
</style></head><body>
<div id="tl" onclick="document.title='hit-tl'"></div>
<div id="br" onclick="document.title='hit-br'"></div>
</body></html>
"""


def click(pipe, x, y):
    pipe.send({"cmd": "input", "type": "mousepress", "x": x, "y": y,
               "button": 1, "buttons": 1, "modifiers": 0})
    pipe.send({"cmd": "input", "type": "mouserelease", "x": x, "y": y,
               "button": 1, "buttons": 0, "modifiers": 0})


def main():
    page_dir = os.path.join(ROOT, "tests", "dpr")
    os.makedirs(page_dir, exist_ok=True)
    with open(os.path.join(page_dir, "input.html"), "w") as f:
        f.write(PAGE)
    os.chdir(page_dir)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    sock = f"khtml_inputprobe_{int(time.time()*1000)%10**8}"
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_renderer.exe"), "--url", "about:blank",
         "--socket", sock, "--width", "640", "--height", "400"],
        cwd=BUILD, env=env,
        stderr=open(os.path.join(ROOT, "tests", "input_probe_stderr.txt"), "wb"))
    titles = []
    try:
        pipe = Pipe(sock)
        end = time.time() + 8
        ready = False
        while time.time() < end and not ready:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "ready":
                    ready = True
        # 680x356 CSS px viewport at dpr 1.5 -> physical frame 1020x534.
        pipe.send({"cmd": "resize", "w": 680, "h": 356, "dpr": 1.5})
        time.sleep(0.3)
        pipe.send({"cmd": "navigate", "url": f"http://127.0.0.1:{PORT}/input.html"})
        # Let the page lay out, then click the two targets at their CSS
        # centers ((100,75) and (580,275)).
        deadline = time.time() + 6
        while time.time() < deadline:
            got = pipe.recv(0.3)
            for _, m in got:
                if m.get("type") == "title":
                    titles.append(m.get("data"))
        click(pipe, 100, 75)
        deadline = time.time() + 1.5
        while time.time() < deadline:
            for _, m in pipe.recv(0.2):
                if m.get("type") == "title":
                    titles.append(m.get("data"))
        click(pipe, 580, 275)
        deadline = time.time() + 1.5
        while time.time() < deadline:
            for _, m in pipe.recv(0.2):
                if m.get("type") == "title":
                    titles.append(m.get("data"))
        pipe.close()
    finally:
        proc.kill()
        httpd.shutdown()
    print("titles:", titles)
    tl_ok = "hit-tl" in titles
    br_ok = "hit-br" in titles
    print(f"top-left target hit: {tl_ok}, bottom-right target hit: {br_ok}")
    ok = tl_ok and br_ok
    print("INPUT COORD PROBE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
