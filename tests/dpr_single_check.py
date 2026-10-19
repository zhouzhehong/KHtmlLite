# dpr_single_check.py — one-off: does a page-loaded-directly (single document,
# no about:blank dance) run its setTimeout bridge callback?
#
# The dpr_probe navigate-after-about:blank path showed the page script running
# TWICE in two QuickJS worlds and the later world's timer never firing.
# This isolates the single-document cold-start path.
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8197

sys.path.insert(0, os.path.join(ROOT, "tests"))
from scroll_probe import Pipe  # noqa: E402

PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>dpr-pending</title>
<script>
document.title = 'sync';
setTimeout(function () {
  document.title = 'timer-ok';
  document.body.style.backgroundColor = '#ff0000';
}, 500);
</script></head>
<body style="margin:0;background:#0a5c50"><p>dpr single</p></body></html>
"""


def main():
    page_dir = os.path.join(ROOT, "tests", "dpr")
    os.makedirs(page_dir, exist_ok=True)
    with open(os.path.join(page_dir, "single.html"), "w") as f:
        f.write(PAGE)
    os.chdir(os.environ.get("SINGLE_DIR", page_dir))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                   http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    sock = f"khtml_dprsingle_{int(time.time()*1000)%10**8}"
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_renderer.exe"),
         "--url", os.environ.get("SINGLE_URL", f"http://127.0.0.1:{PORT}/single.html"),
         "--socket", sock, "--width", "680", "--height", "356"],
        cwd=BUILD, env=env,
        stderr=open(os.path.join(ROOT, "tests", "dpr_single_stderr.txt"), "wb"))
    titles = []
    seen = []
    try:
        pipe = Pipe(sock)
        end = time.time() + 8
        ready = False
        while time.time() < end and not ready:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "ready":
                    ready = True
        pipe.send({"cmd": "resize", "w": 680, "h": 356, "dpr": 1.5})
        end = time.time() + 10
        while time.time() < end:
            for _, m in pipe.recv(0.3):
                seen.append((m.get("type"), m.get("data")))
                if m.get("type") == "title":
                    titles.append(m.get("data"))
        pipe.close()
    finally:
        proc.kill()
        httpd.shutdown()
    print("titles:", titles)
    print("all messages:", seen[:40])
    print("timer fired:", "timer-ok" in titles)
    return 0 if "timer-ok" in titles else 1


if __name__ == "__main__":
    sys.exit(main())
