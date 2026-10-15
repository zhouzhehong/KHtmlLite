# tombstone_probe.py — CPU-adaptive background-tab eviction probe.
#
# Verifies the load-adaptive tombstone policy end to end:
#   1. tab 0 (current): opener.html, quiet.
#   2. synthetic click on its target=_blank link -> busy.html opens as a
#      new CURRENT tab (its renderer decodes an animated GIF forever).
#   3. synthetic click on tab 0 in the tab bar -> busy tab goes background
#      while still burning renderer CPU (native GIF anim timer).
#   4. watch ~60 s: the watchdog samples renderer CPU each 10 s tick; a
#      background tab that keeps burning >= threshold for 3 consecutive
#      ticks is tombstoned (renderer process exits, tab shows snapshot).
#
# Reports per-renderer CPU (GetProcessTimes via typeperf-free ctypes) so
# the hot-tab threshold in checkMemoryBudget() can be calibrated against
# measured reality instead of being a guess.
#
# Usage: python tests\tombstone_probe.py
import ctypes
import http.server
import os
import socketserver
import struct
import subprocess
import sys
import threading
import time

USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32
KERNEL32 = ctypes.windll.kernel32
ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
SERVE_DIR = os.path.join(ROOT, "tests", "tombstone")
PORT = 8195
URL = f"http://127.0.0.1:{PORT}/opener.html"

SW_MAXIMIZE = 3


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


def client_rect(hwnd):
    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    l, t, r, b = struct.unpack("4i", buf.raw)
    return l, t, r, b


def client_point_to_screen(hwnd, x, y):
    l, t, _, _ = client_rect(hwnd)
    pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", pt, 0, x, y)
    USER32.ClientToScreen(hwnd, pt)
    sx, sy = struct.unpack("2i", pt.raw)
    return sx, sy


def click(hwnd, x, y):
    """Synthetic press+release at client coords (x, y)."""
    sx, sy = client_point_to_screen(hwnd, x, y)
    lparam = (sy << 16) | (sx & 0xFFFF)
    USER32.PostMessageW(hwnd, 0x0201, 1, lparam)   # WM_LBUTTONDOWN
    time.sleep(0.05)
    USER32.PostMessageW(hwnd, 0x0202, 0, lparam)    # WM_LBUTTONUP


def renderer_cpu_table():
    """{pid: cumulative cpu seconds} for every khtml_renderer.exe."""
    out = {}
    # tasklist has no CPU column; use wmic-equivalent via PowerShell is slow.
    # Enumerate PIDs via tasklist, then GetProcessTimes via ctypes.
    txt = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq khtml_renderer.exe", "/FO", "CSV"],
        text=True, errors="replace")
    for line in txt.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0] == "khtml_renderer.exe":
            try:
                pid = int(parts[1])
            except ValueError:
                continue
            h = KERNEL32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED
            if not h:
                out[pid] = -1
                continue
            class FT(ctypes.Structure):
                _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]
            ft = [FT() for _ in range(4)]
            ok = KERNEL32.GetProcessTimes(
                h, ctypes.byref(ft[0]), ctypes.byref(ft[1]),
                ctypes.byref(ft[2]), ctypes.byref(ft[3]))
            KERNEL32.CloseHandle(h)
            if not ok:
                out[pid] = -1
                continue
            k = (ft[2].hi << 32) | ft[2].lo
            u = (ft[3].hi << 32) | ft[3].lo
            out[pid] = (k + u) / 1e7  # 100ns -> seconds
    return out


def page_is_opener(hwnd):
    """Sample a probe pixel: opener.html is solid blue background."""
    _, _, w, h = client_rect(hwnd)
    hdc = USER32.GetDC(hwnd)
    px = GDI32.GetPixel(hdc, w // 2, h // 2 + 60)
    USER32.ReleaseDC(hwnd, hdc)
    r, g, b = px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF
    return r < 90 and g < 130 and b > 120  # #2060a0-ish


def main():
    httpd = start_server()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    crashlog = os.path.join(BUILD, "khtml_renderer_crash.log")
    crash_before = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env,
                            stderr=open(os.path.join(ROOT, "tests",
                                        "tombstone_stderr.txt"), "wb"))
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
        time.sleep(6)  # opener page loads

        # Open the busy tab via the target=_blank link.
        click(hwnd, 200, 40)
        time.sleep(6)          # busy tab renders; replacement prewarm spawns
        cpu0 = renderer_cpu_table()
        print(f"after opening busy tab: renderers={cpu0}")

        # Switch back to tab 0 (click the first tab in the tab bar).
        # Layout: nav bar row (y~6..40) first, THEN the tab row (y~44..80).
        switched = False
        for y in (55, 48, 62, 70):
            click(hwnd, 60, y)
            time.sleep(1.0)
            if page_is_opener(hwnd):
                switched = True
                break
        print(f"switched back to opener tab: {switched}")
        if not switched:
            print("FAIL: could not switch tabs")
            return 1

        # Watch the watchdog work: sample renderer CPU every 10 s.
        samples = []
        evicted = False
        for i in range(7):
            time.sleep(10)
            cpu = renderer_cpu_table()
            alive = {p: c for p, c in cpu.items() if c >= 0}
            prev = samples[-1][1] if samples else None
            deltas = {}
            for pid, c in alive.items():
                if prev and pid in prev:
                    deltas[pid] = round((c - prev[pid]) / 10.0 * 100, 1)
            print(f"t+{(i+1)*10}s renderers={len(alive)} "
                  f"cpu_pct_per_core={deltas}")
            samples.append((time.time(), alive))
            if len(alive) <= 2 and i >= 2:  # busy renderer gone (prewarm may remain)
                evicted = True
                print(f"busy renderer evicted at t+{(i+1)*10}s")
                break

        alive = proc.poll() is None
        crash_after = os.path.getsize(crashlog) if os.path.exists(crashlog) else -1
        print(f"browser alive: {alive}, crashlog clean: {crash_after == crash_before}")
        print(f"evicted: {evicted}")
        print("TOMBSTONE PROBE " + ("PASS" if (evicted and alive
                                               and crash_after == crash_before)
                                    else "FAIL"))
        return 0 if (evicted and alive) else 1
    finally:
        proc.kill()
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
