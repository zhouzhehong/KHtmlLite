# iframe_crash_probe.py — regression for the Qt5Core+0x23d747 crash.
#
# The portable bundle has no KParts khtmlpart plugin, so KHTMLPart::createPart()
# returns null for <iframe>.  connectToChildPart() used to dereference it
# (QObject::setObjectName -> [rcx+8]) and the renderer died at
# Qt5Core.dll+0x23d747 while the browser process stayed alive.  The fix fails
# the frame via childLoadFailure() instead; this probe checks the parent page
# still paints and no new renderer crash/dump appears.
#
# Usage: python tests\iframe_crash_probe.py
import ctypes
import glob
import http.server
import os
import socket
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8263
CRASH = os.path.join(BUILD, "khtml_renderer_crash.log")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab  # noqa: E402
from PIL import Image  # noqa: E402

USER32 = ctypes.windll.user32


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/frame.html":
            body = ('<!DOCTYPE html><html><head><meta charset="utf-8">'
                    '<title>frame</title></head><body style="margin:0;'
                    'background:#33aa55">frame</body></html>')
        elif path == "/parent.html":
            body = ('<!DOCTYPE html><html><head><meta charset="utf-8">'
                    '<title>iframe-parent</title></head><body style="margin:0;'
                    'background:#ffffff"><div style="height:100px;'
                    'background:#2266cc"></div>'
                    '<iframe src="/frame.html" width="320" height="200"></iframe>'
                    '<p>after frame</p></body></html>')
        else:
            body = "<html><body>404</body></html>"
        raw = body.encode("utf-8")
        try:
            self.send_response(200 if path in ("/parent.html", "/frame.html") else 404)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except OSError:
            pass


def main():
    class Srv(socketserver.TCPServer):
        allow_reuse_address = True

        def handle_error(self, *a):
            pass

    server = Srv(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    crash_before = os.path.getsize(CRASH) if os.path.exists(CRASH) else -1
    dumps_before = set(glob.glob(os.path.join(BUILD, "*.dmp")))
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    trace = os.path.join(ROOT, "tests", "iframe_crash_probe.err")
    tracef = open(trace, "wb")
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"),
         f"http://127.0.0.1:{PORT}/parent.html"],
        cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=tracef)
    shot = os.path.join(ROOT, "tests", "iframe_crash_probe.png")
    ok = False
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if hwnd:
            time.sleep(4.0)
            # title trace proves parsing completed (the crash happened before it)
            tracef.flush()
            text = open(trace, "rb").read().decode("utf-8", "replace")
            titled = "title=iframe-parent" in text
            grab(hwnd, shot)
            im = Image.open(shot).convert("RGB")
            px = im.load()
            w, h = im.size
            blue = sum(1 for y in range(90, min(h, 300), 4)
                       for x in range(5, min(w, 1200), 4)
                       if px[x, y][2] > 150 and px[x, y][0] < 80)
            print(f"  title={titled} blue_pixels={blue}")
            ok = titled and blue > 500
        crash_after = os.path.getsize(CRASH) if os.path.exists(CRASH) else -1
        dumps_after = set(glob.glob(os.path.join(BUILD, "*.dmp")))
        no_crash = crash_after == crash_before and dumps_after == dumps_before
        print(f"  no renderer crash/dump: {no_crash}")
        ok = ok and no_crash
        print("IFRAME CRASH PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        proc.kill()
        server.shutdown()
        tracef.close()
        for f in (shot, trace):
            try:
                os.remove(f)
            except OSError:
                pass
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
