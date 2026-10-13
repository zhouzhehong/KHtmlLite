# run_startup_capture.py — one-shot launcher: runs khtml_browser with
# startup timing enabled, waits, closes it, prints the merged log.
# Usage: python tests\run_startup_capture.py [seconds-to-wait]

import os
import subprocess
import sys
import time

BUILD = os.environ.get("KHTML_BUILD_DIR",
                       r"C:\Users\zhouzhehong\Desktop\KHtmlLite\build\host")
LOG = os.path.join(BUILD, "khtml_startup.log")
URL = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith(("http", "file")) \
    else "https://cn.bing.com"
WAIT = int(sys.argv[2]) if len(sys.argv) > 2 else 20

if os.path.exists(LOG):
    os.remove(LOG)

env = dict(os.environ)
env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
env["KHTML_STARTUP_TIMING"] = "1"

proc = subprocess.Popen(
    [os.path.join(BUILD, "khtml_browser.exe"), URL],
    cwd=BUILD, env=env,
    stdout=subprocess.DEVNULL,
    stderr=open(os.path.join(os.environ.get("TEMP", "."), "kb_startup_err.txt"), "wb"))

print(f"launched pid={proc.pid}, waiting {WAIT}s ...")
time.sleep(WAIT)

rc = proc.poll()
if rc is None:
    print("browser still running; terminating")
    proc.terminate()
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
else:
    print(f"browser exited early, rc={rc}")

if os.path.exists(LOG):
    print(f"--- {LOG} ---")
    with open(LOG, encoding="utf-8", errors="replace") as f:
        print(f.read())
else:
    print("!! no startup log produced — marks never fired or app failed to start")
    err = os.path.join(os.environ.get("TEMP", "."), "kb_startup_err.txt")
    if os.path.exists(err) and os.path.getsize(err):
        with open(err, errors="replace") as f:
            print("--- stderr tail ---")
            print(f.read()[-2000:])
