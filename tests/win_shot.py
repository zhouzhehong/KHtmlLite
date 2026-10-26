# win_shot.py — DPI-aware capture of an ALREADY RUNNING khtml_browser window.
#
# Since the page canvas is presented through Windows DWM (DirectComposition),
# the composed pixels are NOT part of the window's GDI redirection surface:
# a BitBlt from the window DC would come back blank.  Capture therefore reads
# the desktop DC at the client-area origin (the same pixels DWM shows on
# screen), after bringing the window to the foreground so nothing occludes it.
#
# Usage: python tests\win_shot.py <out.png> [browser_pid]
import ctypes
import os
import struct
import sys
import time

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PER_MONITOR_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

USER32 = ctypes.windll.user32
GDI32 = ctypes.windll.gdi32


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


def force_foreground(hwnd):
    """Raise the browser above other windows (DWM screen capture needs it)."""
    if USER32.IsIconic(hwnd):
        USER32.ShowWindow(hwnd, 9)      # SW_RESTORE
    if USER32.GetForegroundWindow() == hwnd:
        return True
    fg = USER32.GetForegroundWindow()
    tid_fg = USER32.GetWindowThreadProcessId(fg, None) if fg else 0
    tid_me = USER32.GetWindowThreadProcessId(hwnd, None)
    attached = False
    if tid_fg and tid_me and tid_fg != tid_me:
        attached = USER32.AttachThreadInput(tid_me, tid_fg, True) != 0
    try:
        USER32.ShowWindow(hwnd, 9)
        USER32.BringWindowToTop(hwnd)
        USER32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            USER32.AttachThreadInput(tid_me, tid_fg, False)
    for _ in range(20):
        if USER32.GetForegroundWindow() == hwnd:
            return True
        time.sleep(0.01)
    return False


def grab(hwnd, out_path):
    force_foreground(hwnd)
    time.sleep(0.04)                    # let DWM present the raised window

    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    l, t, r, b = struct.unpack("4i", buf.raw)
    w, h = r - l, b - t

    pt = ctypes.create_string_buffer(8)
    struct.pack_into("2i", pt, 0, 0, 0)
    USER32.ClientToScreen(hwnd, pt)
    ox, oy = struct.unpack("2i", pt.raw)

    # Desktop DC: contains the DWM-composited window content.
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
    bih = BIH()
    bih.biSize = ctypes.sizeof(BIH)
    bih.biWidth, bih.biHeight, bih.biPlanes, bih.biBitCount = w, -h, 1, 32
    raw = ctypes.create_string_buffer(w * 4 * h)
    GDI32.GetDIBits(mem, bmp, 0, h, raw, ctypes.byref(bih), 0)
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(0, hdc)

    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), raw.raw, "raw", "BGRA", 0, 1)
    img.convert("RGB").save(out_path)
    return w, h


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "shot.png")
    pid = int(sys.argv[2]) if len(sys.argv) > 2 else None
    if pid is None:
        import subprocess
        o = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "(Get-Process khtml_browser -ErrorAction SilentlyContinue | Select-Object -First 1).Id"],
                           capture_output=True, text=True).stdout.strip()
        pid = int(o)
    hwnd = find_window(pid)
    if not hwnd:
        print("no window for pid", pid)
        return 1
    # Physical window rect vs client rect, and the OS DPI, for diagnostics.
    wr = ctypes.create_string_buffer(16)
    USER32.GetWindowRect(hwnd, wr)
    wl, wt, wrr, wb = struct.unpack("4i", wr.raw)
    cb = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, cb)
    cl, ct, cr, cbb = struct.unpack("4i", cb.raw)
    dpi = USER32.GetDpiForWindow(hwnd) if hasattr(USER32, "GetDpiForWindow") else 0
    w, h = grab(hwnd, out)
    print(f"shot {out} client={cr-cl}x{cbb-ct} window={wrr-wl}x{wb-wt} "
          f"dpi={dpi} scale={dpi/96.0 if dpi else '?'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
