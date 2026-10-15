# scroll_probe.py — quantified wheel-scroll latency probe (engine side).
#
# Drives khtml_renderer.exe DIRECTLY over the QLocalSocket pipe protocol
# (no browser, so no predictive-scroll layer masks the engine). For each
# wheel notch it records the timeline of authoritative scroll offsets
# (cx,cy ride on every "scroll" and "tiles" message):
#
#   px-per-notch      : final |dcy| produced by one -120 wheel notch.
#   convergence_ms    : time from wheel send to the LAST authority message
#                       that still changes cy. With KHTML smooth scrolling
#                       this is ~128ms of animated intermediate offsets;
#                       with instant (compositor-style) scrolling it should
#                       collapse to a single step within one frame tick.
#   steps             : number of distinct cy values observed per notch
#                       (1 = instant, >1 = engine-side animation).
#
# Usage: python tests\scroll_probe.py [notches] [--page scroll|probe]
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time

import win32file
import win32pipe
import pywintypes

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
RENDERER = os.path.join(BUILD, "khtml_renderer.exe")
# The page is served over local HTTP: file:// top-level loads are a known
# broken path (KIO), while the PageLoader http path is the production one.
PORT = 8199
PAGES = {
    "scroll": f"http://127.0.0.1:{PORT}/scroll_smoke.html",
    "probe": f"http://127.0.0.1:{PORT}/resize_probe.html",
}


def start_server():
    os.chdir(os.path.join(ROOT, "tests"))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                    http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


class Pipe:
    """Line-oriented client for the renderer's QLocalSocket JSON protocol."""

    def __init__(self, name):
        self.pipe = None
        deadline = time.time() + 15
        path = rf"\\.\pipe\{name}"
        while time.time() < deadline:
            try:
                win32pipe.WaitNamedPipe(path, 300)
                self.pipe = win32file.CreateFile(
                    path, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0, None, win32file.OPEN_EXISTING, 0, None)
                break
            except pywintypes.error:
                time.sleep(0.2)
        if self.pipe is None:
            raise RuntimeError("could not connect to renderer pipe")
        self.buf = b""

    def send(self, obj):
        win32file.WriteFile(self.pipe,
                            json.dumps(obj).encode() + b"\n", None)

    def recv(self, timeout):
        """Return list of (t_recv, dict) messages read within timeout."""
        out = []
        end = time.time() + timeout
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                try:
                    out.append((time.time(), json.loads(line)))
                except (ValueError, json.JSONDecodeError):
                    continue
            else:
                left = end - time.time()
                if left <= 0:
                    return out
                try:
                    hr, data = win32file.ReadFile(self.pipe, 65536)
                    self.buf += data
                except pywintypes.error:
                    return out

    def close(self):
        try:
            win32file.CloseHandle(self.pipe)
        except Exception:
            pass


def offsets(msg):
    """(cx, cy) from a scroll/tiles message payload, else None."""
    if msg.get("type") in ("scroll", "tiles"):
        parts = msg["data"].split(",")
        if len(parts) >= 4:
            return int(parts[2]), int(parts[3])
    return None


def main():
    httpd = start_server()
    try:
        return run(httpd)
    finally:
        httpd.shutdown()


def run(httpd):
    notches = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 4
    url = PAGES["scroll"]
    if "--page" in sys.argv:
        url = PAGES[sys.argv[sys.argv.index("--page") + 1]]

    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    sock = f"khtml_scrollprobe_{int(time.time()*1000)%10**8}"
    proc = subprocess.Popen(
        [RENDERER, "--url", url, "--socket", sock,
         "--width", "1024", "--height", "512"],
        cwd=BUILD, env=env,
        stderr=open(os.path.join(ROOT, "tests", "scroll_probe_stderr.txt"), "wb"))
    try:
        pipe = Pipe(sock)
        # Wait for ready + first tiles, then let layout settle.
        deadline = time.time() + 10
        got_frame = False
        while time.time() < deadline and not got_frame:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "tiles":
                    got_frame = True
        time.sleep(1.5)

        results = []
        cy_base = None
        for i in range(notches):
            # Drain pending messages, remember current offset.
            for _, m in pipe.recv(0.1):
                off = offsets(m)
                if off:
                    cy_base = off[1]
            t_send = time.time()
            pipe.send({"cmd": "input", "type": "wheel",
                       "x": 512, "y": 256, "deltaX": 0, "deltaY": -120,
                       "button": 0, "buttons": 0, "modifiers": 0})
            timeline = []
            end = time.time() + 1.2
            while time.time() < end:
                for t_recv, m in pipe.recv(0.05):
                    off = offsets(m)
                    if off:
                        timeline.append((round((t_recv - t_send) * 1000, 1), off[1]))
            if timeline:
                cys = [c for _, c in timeline]
                first_cy, last_cy = cys[0], cys[-1]
                prev = cy_base if cy_base is not None else 0
                per_notch = abs(last_cy - prev)
                # convergence: last timestamp at which cy was still moving
                conv = 0.0
                last_val = None
                for t, c in timeline:
                    if last_val is None or c != last_val:
                        conv = t
                        last_val = c
                steps = len(set(cys))
                results.append((per_notch, conv, steps, len(timeline)))
                cy_base = last_cy
            else:
                results.append((0, -1, 0, 0))
            time.sleep(0.3)

        print(f"{notches} notches of deltaY=-120:")
        for i, (px, conv_ms, steps, msgs) in enumerate(results):
            print(f"  notch {i}: px={px} converge={conv_ms}ms "
                  f"distinct_cy={steps} authority_msgs={msgs}")
        px_vals = [r[0] for r in results if r[0] > 0]
        conv_vals = [r[1] for r in results if r[1] >= 0]
        if px_vals:
            print(f"median px-per-notch(-120): {sorted(px_vals)[len(px_vals)//2]}")
        if conv_vals:
            print(f"median convergence: "
                  f"{sorted(conv_vals)[len(conv_vals)//2]}ms")
        alive = proc.poll() is None
        print(f"renderer alive: {alive}")
        smooth = any(r[2] > 1 for r in results if r[2] > 0)
        print(f"engine-side scroll animation detected: {smooth}")
        return 0 if alive else 1
    finally:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
