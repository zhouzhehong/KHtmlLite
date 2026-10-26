# quick_shot.py — open a URL in khtml_browser, wait, dump window pixels
# and a color histogram.  Usage: python tests/quick_shot.py <url> [seconds]
import ctypes
import os
import struct
import subprocess
import sys
import time

# DWM-composited page content is not in the window's GDI surface: capture
# from the desktop DC at the client origin (screen capture) instead.
# KHTML_SCREEN_CAPTURE
try:
    from win_shot import force_foreground as _force_foreground
except Exception:
    _force_foreground = None
USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32
ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")


def find_window(pid):
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def enum_cb(hwnd, lparam):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    USER32.EnumWindows(CB(enum_cb), 0)
    return found[0] if found else None


def dump(hwnd, out):
    from PIL import Image
    rect = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, rect)
    _, _, w, h = struct.unpack("4i", rect.raw)
    if _force_foreground:
        _force_foreground(hwnd)
    time.sleep(0.04)
    _pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", _pt, 0, 0, 0)
    USER32.ClientToScreen(hwnd, _pt)
    ox, oy = struct.unpack("2i", _pt.raw)
    hdc = USER32.GetDC(0)
    mem = GDI32.CreateCompatibleDC(hdc)
    bmp = GDI32.CreateCompatibleBitmap(hdc, w, h)
    GDI32.SelectObject(mem, bmp)
    GDI32.BitBlt(mem, 0, 0, w, h, hdc, ox, oy, 0x00CC0020)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    bmi = BIH()
    bmi.biSize = ctypes.sizeof(BIH)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * 4 * h)
    GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", w * 4, 1)
    img.save(out)
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(0, hdc)
    from collections import Counter
    rgb = img.convert("RGB")
    c = Counter(rgb.getdata())
    for col, cnt in c.most_common(10):
        print("   ", col, cnt)


def main():
    url = sys.argv[1]
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    build = os.environ.get("KHTML_BUILD", BUILD)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    err = open(os.path.join(ROOT, "tests", "quick_shot_stderr.txt"), "wb+")
    proc = subprocess.Popen([os.path.join(build, "khtml_browser.exe"), url],
                            cwd=build, env=env, stderr=err)
    try:
        hwnd = None
        for _ in range(30):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no window")
            return 1
        tlen = USER32.GetWindowTextLengthW(hwnd)
        tbuf = ctypes.create_unicode_buffer(tlen + 1)
        USER32.GetWindowTextW(hwnd, tbuf, tlen + 1)
        print(f"hwnd={hwnd} title={tbuf.value!r}")
        time.sleep(wait)
        tlen = USER32.GetWindowTextLengthW(hwnd)
        tbuf = ctypes.create_unicode_buffer(tlen + 1)
        USER32.GetWindowTextW(hwnd, tbuf, tlen + 1)
        print(f"after wait: title={tbuf.value!r}")
        out = os.path.join(ROOT, "tests", "quick_shot.png")
        dump(hwnd, out)
        print("dumped", out)
        try:
            tl = subprocess.check_output(["tasklist"], text=True, errors="replace")
            rend = [t for t in tl.splitlines() if "khtml_renderer" in t.lower()]
            print(f"renderer processes: {len(rend)}")
        except Exception as e:
            print("tasklist err", e)
        alive = proc.poll() is None
        print("alive:", alive)
        err.flush(); err.seek(0)
        tail = err.read().decode(errors="replace")[-1500:]
        if tail.strip():
            print("--- stderr tail ---")
            print(tail)
        return 0 if alive else 1
    finally:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
