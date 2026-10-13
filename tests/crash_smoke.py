# crash_smoke.py — renderer crash-resilience smoke test for KHtmlLite.
#
# Scenario:
#   1. launch khtml_browser.exe on a simple page, wait for the renderer child
#   2. record the renderer PID (child khtml_renderer.exe of the browser)
#   3. forcibly kill the renderer (taskkill /F) to simulate a crash
#   4. assert the BROWSER process survives and its window stays
#   5. click the center of the window (should hit the crashWidget's
#      "reload" area) and assert a NEW renderer process appears
#
# IMPORTANT: requires a real desktop + the C++ build.  Run in MSYS2/Windows
# after building khtml_browser.exe.  NOT runnable in the agent sandbox.

import ctypes
import os
import subprocess
import sys
import time

USER32 = ctypes.windll.user32
KERNEL32 = ctypes.windll.kernel32

BUILD = r"C:\Users\zhouzhehong\Desktop\KHtmlLite\build-host"
CRASHLOG = os.path.join(BUILD, "khtml_renderer_crash.log")
URL = "file:///C:/Users/zhouzhehong/Desktop/KHtmlLite/tests/live_smoke.html"

env = dict(os.environ)
env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]


def crashlog_size():
    return os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1


def renderer_pids():
    """All khtml_renderer.exe PIDs (there may be several tabs/processes)."""
    try:
        out = subprocess.check_output(
            ["wmic", "process", "where", "name='khtml_renderer.exe'",
             "get", "ProcessId", "/format:list"],
            text=True, errors="replace")
    except Exception:
        return set()
    pids = set()
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("ProcessId="):
            pids.add(int(line.split("=", 1)[1]))
    return pids


def renderer_child_of(parent_pid):
    """Renderer PID whose parent is the browser (via WMI)."""
    try:
        out = subprocess.check_output(
            ["wmic", "process", "where",
             "name='khtml_renderer.exe' and ParentProcessId=%d" % parent_pid,
             "get", "ProcessId", "/format:list"],
            text=True, errors="replace")
    except Exception:
        return None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("ProcessId="):
            return int(line.split("=", 1)[1])
    return None


def find_browser_window(pid):
    found = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def enum_cb(hwnd, lparam):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            if USER32.GetWindowTextLengthW(hwnd) > 0:
                found.append(hwnd)
        return True

    USER32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return found[0] if found else None


def click_center(hwnd):
    rect = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, rect)
    import struct
    _, _, w, h = struct.unpack("4i", rect.raw)
    x, y = w // 2, h // 2
    lparam = (y << 16) | (x & 0xFFFF)
    USER32.PostMessageW(hwnd, 0x0201, 1, lparam)   # WM_LBUTTONDOWN
    time.sleep(0.05)
    USER32.PostMessageW(hwnd, 0x0202, 0, lparam)   # WM_LBUTTONUP


def main():
    before = crashlog_size()
    proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                            cwd=BUILD, env=env,
                            stderr=open(r"C:\Users\zhouzhehong\AppData\Local\Temp\kb_crash_err.txt", "wb"))
    time.sleep(10)

    rend_pid = renderer_child_of(proc.pid)
    if not rend_pid:
        # Maybe the browser hasn't spawned it yet with wmic visibility lag.
        time.sleep(3)
        rend_pid = renderer_child_of(proc.pid)
    if not rend_pid:
        print("FAIL: no renderer child found")
        proc.kill()
        return 1
    print(f"browser pid={proc.pid} renderer pid={rend_pid}")

    # Simulated crash.
    subprocess.run(["taskkill", "/F", "/PID", str(rend_pid)],
                   capture_output=True)
    time.sleep(3)

    browser_alive = proc.poll() is None
    hwnd = find_browser_window(proc.pid)
    crashless = renderer_pids() - {rend_pid}
    print(f"after kill: browser alive={browser_alive} window={bool(hwnd)}")

    # Click the crash placeholder to trigger reload, expect a fresh renderer.
    if hwnd:
        click_center(hwnd)
    time.sleep(5)
    new_renders = renderer_pids() - {rend_pid}
    respawned = len(new_renders) > 0
    print(f"after crash-click: renderer pids now {sorted(renderer_pids())} "
          f"(respawned={respawned})")

    after = crashlog_size()
    print(f"crashlog before={before} after={after} "
          f"(entries added: {after - before if before >= 0 and after >= 0 else 'n/a'})")

    proc.kill()
    time.sleep(1)

    # A crash *should* write a crashlog entry (crash was real); the browser
    # must stay alive and must respawn a renderer on crash-widget click.
    ok = browser_alive and bool(hwnd) and respawned
    print("CRASH SMOKE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
