# title_probe.py — document.title reporting probe (fresh vs adopted paths).
#
# The tombstone probe found that a new tab opened via target=_blank shows
# an EMPTY title: the adopted (prewarmed) renderer reports "" from
# executeScript("document.title") after its second begin/write/end cycle,
# while a fresh renderer on the same URL reports the real title.  This
# probe isolates the two paths by driving khtml_renderer.exe directly:
#
#   case fresh  : --url http://.../titled.html, no pre-navigation
#   case adopted: --url about:blank, then navigate cmd over the pipe
#
# It prints every "title" message the renderer sends, so the divergence
# is observable without the whole browser.
#
# Usage: python tests\title_probe.py
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PORT = 8193
URL = f"http://127.0.0.1:{PORT}/titled.html"

sys.path.insert(0, os.path.join(ROOT, "tests"))
from scroll_probe import Pipe  # noqa: E402  (line-oriented pipe client)


def start_server():
    os.chdir(os.path.join(ROOT, "tests", "title"))
    httpd = socketserver.TCPServer(("127.0.0.1", PORT),
                                    http.server.SimpleHTTPRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def drive(case, url, navigate_to=None, browser_dance=False):
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    sock = f"khtml_titleprobe_{case}_{int(time.time()*1000)%10**8}"
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_renderer.exe"), "--url", url,
         "--socket", sock, "--width", "800", "--height", "560"],
        cwd=BUILD, env=env,
        stderr=open(os.path.join(ROOT, "tests", f"title_probe_{case}_stderr.txt"),
                    "wb"))
    titles = []
    try:
        pipe = Pipe(sock)
        # Drain the ready handshake.
        got_ready = False
        end = time.time() + 5
        while time.time() < end and not got_ready:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "ready":
                    got_ready = True
        if navigate_to:
            time.sleep(1.0)  # let about:blank settle first (prewarm reality)
            if browser_dance:
                # Replicate the browser's adoption order exactly:
                # connected() -> flushPendingResize (container size), then
                # currentChanged -> visible(1), then ready -> navigate.
                pipe.send({"cmd": "resize", "w": 680, "h": 356})
                pipe.send({"cmd": "visible", "v": 1})
                time.sleep(0.2)
            pipe.send({"cmd": "navigate", "url": navigate_to})
        end = time.time() + 12
        while time.time() < end:
            for _, m in pipe.recv(0.3):
                if m.get("type") == "title":
                    titles.append((round(time.time() % 60, 1), m.get("data")))
                if m.get("type") == "error":
                    titles.append((round(time.time() % 60, 1),
                                   f"<error:{m.get('data')}>"))
                if m.get("type") == "url":
                    titles.append((round(time.time() % 60, 1),
                                   f"<url:{m.get('data')}>"))
        pipe.close()
    finally:
        proc.kill()
    return titles


def main():
    os.makedirs(os.path.join(ROOT, "tests", "title"), exist_ok=True)
    with open(os.path.join(ROOT, "tests", "title", "titled.html"), "w") as f:
        f.write("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<title>titled-page</title></head>"
                "<body><p>hello</p></body></html>")
    httpd = start_server()
    try:
        fresh = drive("fresh", URL)
        print(f"fresh   : {fresh}")
        adopted = drive("adopted", "about:blank", navigate_to=URL)
        print(f"adopted : {adopted}")
        adopted2 = drive("adopted2", "about:blank", navigate_to=URL,
                         browser_dance=True)
        print(f"adopted2: {adopted2}")
        fresh_ok = any(t == "titled-page" for _, t in fresh)
        adopted_ok = any(t == "titled-page" for _, t in adopted)
        adopted2_ok = any(t == "titled-page" for _, t in adopted2)
        print(f"fresh ok: {fresh_ok}, adopted ok: {adopted_ok}, "
              f"adopted(browser dance) ok: {adopted2_ok}")
        ok = fresh_ok and adopted_ok and adopted2_ok
        print("TITLE PROBE " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
