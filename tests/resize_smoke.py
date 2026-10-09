# resize_smoke.py — scripted maximize/restore smoke test for KHtmlLite.
# Launches khtml_browser.exe with a local test page, maximizes/restores the
# window via Win32, and verifies: window state changes, renderer alive,
# no new crash-log entries.
import ctypes, os, subprocess, sys, time

USER32 = ctypes.windll.user32
BUILD = r"C:\Users\zhouzhehong\Desktop\KHtmlLite\build-host"
CRASHLOG = os.path.join(BUILD, "khtml_renderer_crash.log")
URL = "file:///C:/Users/zhouzhehong/Desktop/KHtmlLite/tests/resize_smoke.html"

env = dict(os.environ)
env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]

before = os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1

proc = subprocess.Popen([os.path.join(BUILD, "khtml_browser.exe"), URL],
                        cwd=BUILD, env=env,
                        stderr=open(r"C:\Users\zhouzhehong\AppData\Local\Temp\kb_err.txt", "wb"))
time.sleep(8)

# find the browser's top-level window
found = []
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
def enum_cb(hwnd, lparam):
    pid = ctypes.c_ulong()
    USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value == proc.pid and USER32.IsWindowVisible(hwnd):
        length = USER32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(length + 1)
        USER32.GetWindowTextW(hwnd, buf, length + 1)
        if buf.value:
            found.append((hwnd, buf.value))
    return True
USER32.EnumWindows(WNDENUMPROC(enum_cb), 0)
if not found:
    print("FAIL: no browser window found")
    proc.kill()
    sys.exit(1)
hwnd, title = found[0]
print(f"hwnd={hwnd} title={title!r}")

def is_zoomed(h):
    return bool(USER32.IsZoomed(h))

t0 = time.time()
USER32.ShowWindowAsync(hwnd, 3)  # SW_MAXIMIZE
zoomed_at = None
for _ in range(60):
    time.sleep(0.1)
    if is_zoomed(hwnd):
        zoomed_at = time.time() - t0
        break
print(f"maximized: zoomed={zoomed_at is not None} (took {zoomed_at and round(zoomed_at*1000)}ms)")

time.sleep(3)
t1 = time.time()
USER32.ShowWindowAsync(hwnd, 9)  # SW_RESTORE
restored = False
for _ in range(60):
    time.sleep(0.1)
    if not is_zoomed(hwnd):
        restored = True
        print(f"restored: took {round((time.time()-t1)*1000)}ms")
        break

time.sleep(3)
alive = proc.poll() is None
rend_alive = False
try:
    out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq khtml_renderer.exe"],
                                  text=True, errors="replace")
    rend_alive = "khtml_renderer.exe" in out
except Exception:
    pass
after = os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1
print(f"browser alive: {alive}, renderer alive: {rend_alive}")
print(f"crashlog before={before} after={after} -> {'CLEAN' if after == before else 'NEW CRASH ENTRIES'}")

proc.kill()
time.sleep(1)
ok = alive and restored and (zoomed_at is not None) and after == before and rend_alive
print("SMOKE " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
