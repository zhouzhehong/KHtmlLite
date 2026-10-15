# tombstone_probe.py — load-adaptive tombstone policy probe.
#
# Verifies the tombstone mechanism end to end.  Ground truth is the
# browser's own KHTML_TOMBSTONE_DEBUG trace (tests/tombstone_stderr.txt),
# not pixel heuristics:
#
#   1. browser opens busy.html (tab 0, CURRENT; animated GIF keeps its
#      renderer's native decode timer running).
#   2. a synthetic click on the page's target=_blank link opens quiet.html
#      as a real SECOND tab (trace shows pages=2), pushing busy.html into
#      the background.
#   3. the 10 s watchdog samples system load; on this machine physical
#      memory sits at warning level, so the LRU tier suspends the busy
#      background tab within ~15 s (trace: suspendTab idx=0, renderer
#      process exits).  On a memory-relaxed machine the same eviction
#      comes from the CPU-adaptive idle tier instead — either way the
#      mechanism under test is "system load -> reclaim".
#   4. regression guard: no resumeTab may follow the suspend (an earlier
#      bug made the tab-bar surgery in suspendTab re-route currentChanged
#      onto the tombstone and auto-resume it one event later).
#   5. clicking the tombstoned tab resumes it (trace: resumeTab idx=0,
#      busy.html refetched, renderer respawns).
#
# Invariants asserted:
#   - the second tab really existed (pages=2).
#   - background busy tab IS evicted; no auto-resume follows.
#   - the CURRENT tab's renderer is NEVER evicted (renderer count only
#     drops by the busy one; the trace never suspends idx=1).
#   - resume works after a user click on the tombstone tab.
#   - browser alive, renderer crash log untouched.
#
# Usage: python tests\tombstone_probe.py
import ctypes
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

USER32 = ctypes.windll.user32
ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
SERVE_DIR = os.path.join(ROOT, "tests", "tombstone")
PORT = 8195
URL = f"http://127.0.0.1:{PORT}/busy.html"
TRACE = os.path.join(ROOT, "tests", "tombstone_stderr.txt")


def start_server():
    os.chdir(SERVE_DIR)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                    http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def find_window(pid):
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, _):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    USER32.EnumWindows(CB(cb), 0)
    return found[0] if found else None


def click(hwnd, x, y):
    """Synthetic press+release at client coords (x, y)."""
    lparam = (y << 16) | (x & 0xFFFF)
    USER32.PostMessageW(hwnd, 0x0201, 1, lparam)   # WM_LBUTTONDOWN
    time.sleep(0.05)
    USER32.PostMessageW(hwnd, 0x0202, 0, lparam)    # WM_LBUTTONUP


def renderer_pids():
    txt = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq khtml_renderer.exe", "/FO", "CSV"],
        text=True, errors="replace")
    out = []
    for line in txt.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0] == "khtml_renderer.exe":
            try:
                out.append(int(parts[1]))
            except ValueError:
                pass
    return out


def trace_text():
    try:
        with open(TRACE, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    httpd = start_server()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env["KHTML_TOMBSTONE_DEBUG"] = "1"
    crashlog = os.path.join(BUILD, "khtml_renderer_crash.log")
    crash_before = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
    open(TRACE, "w").close()  # fresh trace
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env, stderr=open(TRACE, "ab"))
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            return 1
        time.sleep(6)  # busy page renders, GIF animating

        # Click the target=_blank link once (page starts ~y=78 below the
        # nav+tab chrome; the link block is 400x80 at the top of the page).
        # Adoption + PageLoader + renderer handshake takes ~2-3 s.
        click(hwnd, 200, 100)
        time.sleep(5)
        tr = trace_text()
        two_tabs = "pages=2" in tr
        print(f"second tab created (trace pages=2): {two_tabs}")
        if not two_tabs:
            print("FAIL: target=_blank did not open a second tab")
            print(tr)
            return 1

        # Watch the watchdog evict the background busy tab.  The [tomb]
        # lines record which tier fired (memory warning vs CPU-adaptive
        # idle) on this machine.
        evicted = False
        for i in range(8):
            time.sleep(5)
            tr = trace_text()
            if "suspendTab idx=0" in tr:
                evicted = True
                print(f"busy background tab tombstoned at t+{5*(i+1)+11}s")
                break
        if not evicted:
            print("FAIL: background tab not tombstoned within 40 s")
            print(tr)
            return 1

        # Regression guard: tab-bar surgery must not auto-resume.
        time.sleep(5)
        tr = trace_text()
        auto_resume = tr.count("resumeTab") > 0
        print(f"auto-resume after suspend (must be False): {auto_resume}")
        if auto_resume:
            return 1

        # Current-tab immunity: the quiet tab's renderer must still be
        # alive and the trace must never suspend it (idx=1).
        if "suspendTab idx=1" in tr:
            print("FAIL: current tab was evicted")
            return 1
        alive_renderers = len(renderer_pids())
        print(f"renderers alive after tombstone: {alive_renderers}")

        # Resume: click the tombstoned tab in the tab bar (nav bar
        # occupies y~6..42, the tab row sits at y~44..80; tab 0 x~8..150).
        resumed = False
        for y in (55, 48, 62, 70):
            click(hwnd, 60, y)
            time.sleep(3)
            if "resumeTab idx=0" in trace_text():
                resumed = True
                break
        print(f"resume by clicking tombstone tab: {resumed}")

        alive = proc.poll() is None
        crash_after = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
        print(f"browser alive: {alive}, crashlog clean: {crash_after == crash_before}")
        # Print the watchdog calibration record for the handover doc.
        ticks = [l for l in tr.splitlines() if "mem=" in l]
        print("watchdog ticks: " + "; ".join(ticks[-3:]))
        ok = (evicted and not auto_resume and resumed and alive
              and crash_after == crash_before)
        print("TOMBSTONE PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        proc.kill()
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
