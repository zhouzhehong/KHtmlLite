# renderer_solo.py — launch khtml_renderer.exe alone, speak the minimal
# IPC (named-pipe JSON lines), let it render, then inspect frame_dump.png
# (requires KHTML_FRAME_DUMP=1 in env).  Usage:
#   python tests/renderer_solo.py <url> [seconds]
import os
import subprocess
import sys
import time

try:
    import win32file
    import win32pipe
    import pywintypes
except ImportError:
    print("pywin32 missing")
    sys.exit(2)

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
RENDERER = os.path.join(BUILD, "khtml_renderer.exe")
URL = sys.argv[1]
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 6

env = dict(os.environ)
env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
env["KHTML_FRAME_DUMP"] = "1"
SOCK = f"khtml_solo_{int(time.time())}"
PIPE = rf"\\.\pipe\{SOCK}"

proc = subprocess.Popen([RENDERER, "--url", URL, "--socket", SOCK,
                         "--width", "800", "--height", "560"],
                        cwd=BUILD, env=env)
pipe = None
try:
    # Wait for the pipe to exist, then connect.
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            win32pipe.WaitNamedPipe(PIPE, 300)
            pipe = win32file.CreateFile(
                PIPE, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None)
            win32pipe.SetNamedPipeHandleState(pipe, win32pipe.PIPE_READMODE_BYTE,
                                              None, None)
            break
        except pywintypes.error:
            time.sleep(0.2)
    if pipe is None:
        print("FAIL: could not connect pipe")
        sys.exit(1)

    # Read messages for WAIT seconds; collect types.
    win32pipe.SetNamedPipeHandleState(pipe, win32pipe.PIPE_READMODE_BYTE, None, None)
    import msvcrt
    fd = msvcrt.open_osfhandle(int(pipe), 0)
    f = os.fdopen(fd, "rb", buffering=0)
    types = {}
    t0 = time.time()
    buf = b""
    while time.time() - t0 < WAIT:
        try:
            chunk = f.read(65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    import json
                    typ = json.loads(line).get("type")
                    if typ:
                        types[typ] = types.get(typ, 0) + 1
                except Exception:
                    pass
        except OSError:
            break
    print("ipc message types:", types)
    alive = proc.poll() is None
    print("renderer alive:", alive)
finally:
    try:
        if pipe:
            win32file.CloseHandle(pipe)
    except Exception:
        pass
    proc.kill()

# Analyze the dumped frame.
try:
    from PIL import Image
    from collections import Counter
    im = Image.open(os.path.join(BUILD, "frame_dump.png")).convert("RGB")
    print("frame size:", im.size)
    for col, cnt in Counter(im.getdata()).most_common(8):
        print("   ", col, cnt)
except FileNotFoundError:
    print("FAIL: no frame_dump.png")
    sys.exit(1)
