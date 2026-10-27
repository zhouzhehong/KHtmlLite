# promise_microtask_probe.py — verifies QuickJS promise jobs are drained.
#
# Before the fix runMicrotasks() had no call sites, so Promise.then and
# async/await continuations never ran.  The DOM bridge now drains the job
# queue after: page <script> evaluation, timer callbacks and DOM event
# handlers.  The page queues all four kinds and reports through document.title
# once every marker arrived; a 1.5 s fallback reports how many ran.
#
# Usage: python tests\promise_microtask_probe.py
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
SERVE = os.path.join(ROOT, "tests", "promise")
PORT = 8199
URL = f"http://127.0.0.1:{PORT}/index.html"
TRACE = os.path.join(ROOT, "tests", "promise_microtask.err")

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window  # noqa: E402


def main():
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    class Srv(socketserver.TCPServer):
        allow_reuse_address = True

        def handle_error(self, *a):
            pass

    os.chdir(SERVE)
    httpd = Srv(("127.0.0.1", PORT), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env["KHTML_TOMBSTONE_DEBUG"] = "1"
    tracef = open(TRACE, "wb")
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"), URL],
        cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=tracef)
    title = ""
    try:
        hwnd = None
        for _ in range(40):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            print("PROMISE MICROTASK PROBE FAIL")
            return 1
        import ctypes
        user32 = ctypes.windll.user32
        buf = ctypes.create_unicode_buffer(2048)
        for _ in range(80):
            user32.GetWindowTextW(hwnd, buf, 2048)
            if buf.value.startswith("MICRO|"):
                title = buf.value.replace(" - KHtmlLite", "")
                if all(m in title for m in ("then", "await", "timer-then",
                                            "timer-sync", "timer-await")):
                    break
            time.sleep(0.1)
        print("  report:", title or "(none)")
        ok = title.startswith("MICRO|") and "TIMEOUT" not in title
        for marker in ("then", "await", "timer-then", "timer-sync", "timer-await"):
            good = marker in title
            print(f"  [{'OK' if good else 'FAIL'}] {marker} continuation ran")
            ok = ok and good
        print("PROMISE MICROTASK PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        httpd.shutdown()
        try:
            tracef.close()
        except Exception:
            pass
        try:
            os.remove(TRACE)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
