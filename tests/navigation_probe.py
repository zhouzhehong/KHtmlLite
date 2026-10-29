# navigation_probe.py — local navigation lifecycle coverage (fresh process per
# scenario, so history/state from one case cannot mask another).
#
# Covers: normal link navigation, second page, back/forward after link click,
# HTTP redirect, rapid consecutive clicks cancelling a slow load, resource 404
# still renderable, JS document.title, and main-document 404 -> visible error
# page instead of a white tab.
#
# Usage: python tests\navigation_probe.py
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
PORT = 8205
DPR = 1.5
PAGE_ORIGIN = (11, 87)
CRASH = os.path.join(BUILD, "khtml_renderer_crash.log")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab, force_foreground  # noqa: E402
from PIL import Image  # noqa: E402

USER32 = ctypes.windll.user32


def link_point(i):
    # CSS px: top 18 + 50*i, centre +20; Win32 client coords are physical.
    return (PAGE_ORIGIN[0] + int(128 * DPR),
            PAGE_ORIGIN[1] + int((38 + i * 50) * DPR))


LINKS = {
    "b": link_point(0), "c": link_point(1), "redirect": link_point(2),
    "missing": link_point(3), "slow": link_point(4), "js": link_point(5),
    "notfound": link_point(6),
}
BACK_BTN = (34, 66)
FORWARD_BTN = (68, 66)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, title, body="", status=200, delay=0):
        if delay:
            time.sleep(delay)
        data = (f'<!DOCTYPE html><html><head><meta charset="utf-8">'
                f'<title>{title}</title></head><body>{body}</body></html>')
        raw = data.encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except OSError:
            pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/a.html":
            links = "".join(
                f'<a href="{href}" style="display:block;width:220px;height:40px;'
                f'margin:10px;background:{color};color:#fff">{label}</a>'
                for href, color, label in (
                    ("/b.html", "#2266cc", "B"), ("/c.html", "#33aa55", "C"),
                    ("/redirect", "#aa7722", "R"), ("/missing.html", "#7744aa", "M"),
                    ("/slow.html", "#777777", "S"), ("/jstitle.html", "#226688", "J"),
                    ("/notfound.html", "#883333", "E")))
            self._send("A|ready", links)
        elif path == "/b.html":
            self._send("B|ready", '<div style="background:#2266cc;height:400px">B</div>')
        elif path == "/c.html":
            self._send("C|ready", '<div style="background:#33aa55;height:400px">C</div>')
        elif path == "/redirect":
            try:
                self.send_response(302)
                self.send_header("Location", "/b.html")
                self.end_headers()
            except OSError:
                pass
        elif path == "/missing.html":
            self._send("Missing|ready",
                       '<div style="background:#7744aa;height:300px">M</div>'
                       '<img src="/nope.png"><link rel="stylesheet" href="/nope.css">')
        elif path == "/slow.html":
            self._send("SlowPage|ready",
                       '<div style="background:#777;height:300px">S</div>', delay=1.5)
        elif path == "/jstitle.html":
            self._send("pending", '<div style="background:#226688;height:300px">J</div>'
                       '<script>setTimeout(function(){document.title="JSTitle|ready";},150);</script>')
        else:
            self._send("nope", "not found", status=404)


def click(hwnd, x, y):
    force_foreground(hwnd)
    lp = (y << 16) | (x & 0xFFFF)
    USER32.PostMessageW(hwnd, 0x0200, 0, lp)
    time.sleep(0.03)
    USER32.PostMessageW(hwnd, 0x0201, 1, lp)
    time.sleep(0.05)
    USER32.PostMessageW(hwnd, 0x0202, 0, lp)


class Session:
    def __init__(self, path):
        self.trace = os.path.join(ROOT, "tests", f"nav_{os.getpid()}_{path.replace('/', '_')}.err")
        self.tracef = open(self.trace, "wb")
        env = dict(os.environ)
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
        env["KHTML_TOMBSTONE_DEBUG"] = "1"
        self.proc = subprocess.Popen(
            [os.path.join(BUILD, "khtml_browser.exe"),
             f"http://127.0.0.1:{PORT}{path}"],
            cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=self.tracef)
        self.hwnd = None
        for _ in range(40):
            self.hwnd = find_window(self.proc.pid)
            if self.hwnd:
                break
            time.sleep(0.5)

    def titles(self):
        try:
            self.tracef.flush()
            text = open(self.trace, "rb").read().decode("utf-8", "replace")
        except OSError:
            return []
        return [line.split("titleChanged title=", 1)[1].rsplit(" cur=", 1)[0]
                for line in text.splitlines() if "titleChanged title=" in line]

    def wait(self, needle, timeout=12.0):
        end = time.time() + timeout
        while time.time() < end:
            titles = self.titles()
            if any(needle in t for t in titles):
                return True
            time.sleep(0.2)
        return False

    def shot(self, path):
        grab(self.hwnd, path)
        return path

    def stop(self):
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.proc.pid)],
                       capture_output=True)
        try:
            self.tracef.close()
        except Exception:
            pass
        try:
            os.remove(self.trace)
        except OSError:
            pass


def non_white(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    n = sum(1 for y in range(90, min(h, 500), 8)
            for x in range(16, min(w, 1200), 8) if px[x, y] != (255, 255, 255))
    if n < 200:
        print(f"    non_white={n}")
    return n >= 200


def main():
    class Srv(socketserver.TCPServer):
        allow_reuse_address = True

        def handle_error(self, *a):
            pass
    server = Srv(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    shot = os.path.join(ROOT, "tests", "navigation_probe.png")
    crash_before = os.path.getsize(CRASH) if os.path.exists(CRASH) else -1
    ok = True

    def check(name, good):
        nonlocal ok
        print(f"  [{'OK' if good else 'FAIL'}] {name}")
        ok = ok and good

    try:
        s = Session("/a.html")
        try:
            check("initial page A", s.wait("A|ready", 10))
            # Native Win11 frame creation can deliver the title one beat before
            # the first DWM-composed client frame; retry the pixel gate instead
            # of treating that as a white-page failure.
            initial_painted = False
            for _ in range(12):
                if non_white(s.shot(shot)):
                    initial_painted = True
                    break
                time.sleep(0.25)
            check("initial page not white", initial_painted)
            click(s.hwnd, *LINKS["b"])
            check("link navigation to page B", s.wait("B|ready"))
            check("page B not white", non_white(s.shot(shot)))
            click(s.hwnd, *BACK_BTN)
            check("Back to A after link click", s.wait("A|ready"))
            click(s.hwnd, *FORWARD_BTN)
            check("Forward to B again", s.wait("B|ready"))
        finally:
            s.stop()

        s = Session("/a.html")
        try:
            s.wait("A|ready", 10)
            click(s.hwnd, *LINKS["c"])
            check("second page C loads", s.wait("C|ready"))
        finally:
            s.stop()

        s = Session("/a.html")
        try:
            s.wait("A|ready", 10)
            click(s.hwnd, *LINKS["redirect"])
            check("HTTP redirect lands on B", s.wait("B|ready"))
        finally:
            s.stop()

        s = Session("/a.html")
        try:
            s.wait("A|ready", 10)
            click(s.hwnd, *LINKS["slow"])
            time.sleep(0.12)
            click(s.hwnd, *LINKS["c"])
            check("rapid click cancels slow load and shows C", s.wait("C|ready"))
            time.sleep(2.0)
            check("cancelled SlowPage never surfaced",
                  not any("SlowPage|ready" in t for t in s.titles()))
        finally:
            s.stop()

        s = Session("/a.html")
        try:
            s.wait("A|ready", 10)
            click(s.hwnd, *LINKS["missing"])
            check("resource 404 page renders", s.wait("Missing|ready"))
            check("resource 404 page not white", non_white(s.shot(shot)))
        finally:
            s.stop()

        s = Session("/a.html")
        try:
            s.wait("A|ready", 10)
            click(s.hwnd, *LINKS["js"])
            check("JS document.title change", s.wait("JSTitle|ready"))
        finally:
            s.stop()

        s = Session("/notfound.html")
        try:
            check("main 404 shows error page", s.wait("LoadError"))
            painted = False
            for _ in range(12):
                time.sleep(0.5)
                if non_white(s.shot(shot)):
                    painted = True
                    break
            check("error page not white (after paint)", painted)
        finally:
            s.stop()

        crash_after = os.path.getsize(CRASH) if os.path.exists(CRASH) else -1
        check("no renderer crash", crash_after == crash_before)
        print("NAVIGATION PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        server.shutdown()
        try:
            os.remove(shot)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
