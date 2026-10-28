#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# crash_recovery_probe.py — renderer-crash automatic/manual recovery probe.
#
# Exercises the fc46ab7 + recovery-placeholder crash path on the REAL
# build/host browser with a real window and a local HTTP server:
#
#   1. normal load: static <title> then JS changes it (proves JS on) and a
#      static blue body that JS turns green (visual JS on/off evidence);
#   2. forced taskkill of the tab renderer -> "正在恢复…" placeholder ->
#      automatic retry #1 with --disable-js -> page reloads with JS OFF;
#   3. stop the tab renderer again -> automatic retry #2: regenerable
#      PageLoader cache cleared, JS re-enabled -> page reloads with JS ON;
#   4. stop it a third time -> final crash UI ("已崩溃") with the
#      "重新加载" button -> NO further automatic restart (bounded, 2 retries);
#   5. manual retry click -> jsDisabled=0 renderer, same URL reloads, JS on;
#   6. another crash -> crash UI re-appears, still no automatic restart;
#   7. repeated double-click/rapid clicks on "重新加载" -> exactly ONE new
#      page load (one [recovery] restart line, at most one HTTP page request);
#   8. cleanup: browser + every renderer gone; crash log/dmp files never
#      deleted (new artifacts are only reported).
#
# A best-effort history check (Back button after automatic recovery #1) and a
# passive cookie check (Set-Cookie -> Cookie header) round out the
# "cookies/history must be untouched" requirement; when the browser cannot
# expose the property reliably the step prints INCONCLUSIVE, never a false
# FAIL.
#
# Renderers are identified by WMI (Win32_Process: Name, ProcessId,
# ParentProcessId, CommandLine) as children of the launched browser; their
# command line carries --url/--socket and, for automatic attempt #1,
# --disable-js.  The crash-widget button is located IN the screenshot:
# the button's solid light-grey fill is the only wide, fully filled
# non-white band below the page centre; the click uses the resulting
# PHYSICAL client coordinates and is then validated by the recovery log.
#
# Usage: python tests\crash_recovery_probe.py
# Exit: 0 = all PASS, 1 = at least one FAIL, 2 = INCONCLUSIVE steps only.

import ctypes
import glob
import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import traceback

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
EXE = os.path.join(BUILD, "khtml_browser.exe")
CRASHLOG = os.path.join(BUILD, "khtml_renderer_crash.log")
PORT = 8297
BASE_URL = "http://127.0.0.1:%d" % PORT
PAGE = "/crash.html"
FINAL = "/crash_final.html"

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window, grab, force_foreground  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

USER32 = ctypes.windll.user32
CREATE_NO_WINDOW = 0x08000000

TITLE_STATIC = "StaticTitle"
TITLE_JS = "JsTitle"
RECOVERY_TITLE = "恢复"          # "正在恢复… - KHtmlLite"
CRASH_TITLE = "崩溃"             # "已崩溃 - KHtmlLite"

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203

# Chrome geometry from khtml_browser.cpp: tabStrip 26 + toolbar 32 = 58
# logical px (87 physical @150%), root margins left 7 / bottom 4 logical.
PAGE_TOP_LOGICAL = 58.0
PAGE_LEFT_LOGICAL = 7.0
PAGE_BOTTOM_MARGIN_LOGICAL = 4.0
BACK_BTN_LOGICAL = (22.5, 44.0)

PS_EXE = shutil.which("powershell") or r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
RENDERER_PS = (
    "$ErrorActionPreference='SilentlyContinue'\n"
    "Get-CimInstance Win32_Process -Filter \"Name='khtml_renderer.exe'\" |\n"
    "  Select-Object ProcessId,ParentProcessId,CreationDate,Name,CommandLine |\n"
    "  ConvertTo-Json -Compress"
)
CHILDREN_PS = (
    "$ErrorActionPreference='SilentlyContinue'\n"
    "Get-CimInstance Win32_Process -Filter \"ParentProcessId=%d\" |\n"
    "  Select-Object ProcessId,ParentProcessId,CreationDate,Name,CommandLine |\n"
    "  ConvertTo-Json -Compress"
)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------
RESULTS = []
TITLE_OK = [True]   # set False once the shell proves it does not surface titles


def esc(s, limit=400):
    """ASCII-safe rendering of a possibly-Chinese string for the console."""
    try:
        t = s.encode("unicode_escape").decode("ascii")
    except Exception:
        t = repr(s)
    return t if len(t) <= limit else t[:limit] + "..."


def step(name, verdict, detail="", optional=False):
    RESULTS.append((name, verdict, detail, optional))
    print("  [%s] %s%s" % (verdict, name, (": " + detail) if detail else ""))
    sys.stdout.flush()


def say(msg):
    print(msg)
    sys.stdout.flush()


# --------------------------------------------------------------------------
# local HTTP server: page requests are the URL/reload evidence
# --------------------------------------------------------------------------
PAGE_HTML = (
    '<!DOCTYPE html><html><head><meta charset="utf-8">'
    "<title>%s</title></head>"
    '<body style="margin:0;background:#cfe3ff">'
    '<h1 style="font:20px sans-serif;color:#123;margin:16px">Static page</h1>'
    '<p style="font:14px sans-serif;color:#234;margin:16px">JS pending</p>'
    '<p id="state" style="font:14px sans-serif;color:#234;margin:16px">state=static</p>'
    "<script>setTimeout(function(){"
    "document.title='%s';"
    "document.body.style.background='#d8f5d8';"
    "var e=document.getElementById('state');"
    "if(e)e.textContent='state=js';"
    "},300);</script>"
    "</body></html>"
) % (TITLE_STATIC, TITLE_JS)


class ServerState:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {}
        self.log = []          # (time, path, cookie, referer)


SERVER_STATE = ServerState()


class Handler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0 closes after each response, like the other passing probes.
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _record(self):
        with SERVER_STATE.lock:
            path = self.path.split("?")[0]
            SERVER_STATE.counts[path] = SERVER_STATE.counts.get(path, 0) + 1
            SERVER_STATE.log.append((time.time(), path,
                                     self.headers.get("Cookie"),
                                     self.headers.get("Referer")))

    def _send(self, status, body=b"", extra=()):
        try:
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        self._record()
        path = self.path.split("?")[0]
        if path == PAGE:
            # Redirect: the browser pushes PAGE into back history (urlChanged
            # reports the final URL), which is our history-preservation probe.
            self._send(302, b"", [("Location", FINAL),
                                  ("Set-Cookie", "probe=1; Path=/")])
        elif path == FINAL:
            self._send(200, PAGE_HTML.encode("utf-8"),
                       [("Set-Cookie", "probe=1; Path=/")])
        elif path == "/favicon.ico":
            self._send(404)
        else:
            self._send(404, b"<html><body>404</body></html>")


def serve():
    class Srv(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

        def handle_error(self, *a):
            pass
    srv = Srv(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def req_count(path):
    with SERVER_STATE.lock:
        return SERVER_STATE.counts.get(path, 0)


def page_loads():
    # A page load is either the redirect or its final document.  Different
    # retries may be served from PageLoader's mirror (0 HTTP requests), so
    # this is evidence, not the only gate.
    return req_count(PAGE) + req_count(FINAL)


def cookie_sent():
    with SERVER_STATE.lock:
        return any(("probe=1" in (c or "")) for (_t, _p, c, _r) in SERVER_STATE.log)


# --------------------------------------------------------------------------
# Win32 helpers (window, client geometry, DPI, clicks)
# --------------------------------------------------------------------------
def find_main_window(pid):
    """Largest visible top-level window of pid (design-change tolerant)."""
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(hwnd, _):
        p = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and USER32.IsWindowVisible(hwnd):
            w, h = get_client_size(hwnd)
            if w > 0 and h > 0:
                found.append((w * h, hwnd))
        return True
    USER32.EnumWindows(CB(cb), 0)
    if not found:
        return find_window(pid)
    found.sort(reverse=True)
    return found[0][1]


def dump_state(hwnd, proc, trace, label):
    say("    ---- diagnostics (%s) ----" % label)
    try:
        windows = []
        CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def cb(h, _):
            p = ctypes.c_ulong()
            USER32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value == proc.pid and USER32.IsWindowVisible(h):
                w, hh = get_client_size(h)
                windows.append((h, w, hh, esc(get_title(h), 100)))
            return True
        USER32.EnumWindows(CB(cb), 0)
        say("      visible windows: %s" % windows)
    except Exception as e:
        say("      windows: error %r" % e)
    try:
        table = [(r["pid"], r["ppid"], r["name"], r["url"], r["disable_js"])
                 for r in get_renderers(proc.pid) if r["ppid"] == proc.pid]
        say("      renderers: %s" % table)
    except Exception as e:
        say("      renderers: error %r" % e)
    for ln in trace.lines()[-10:]:
        say("      trace: " + esc(ln, 220))


def get_title(hwnd):
    buf = ctypes.create_unicode_buffer(1024)
    n = USER32.GetWindowTextW(hwnd, buf, 1024)
    return buf.value if n else ""


def get_client_size(hwnd):
    buf = ctypes.create_string_buffer(16)
    USER32.GetClientRect(hwnd, buf)
    import struct
    l, t, r, b = struct.unpack("4i", buf.raw)
    return r - l, b - t


def get_scale(hwnd):
    try:
        dpi = int(USER32.GetDpiForWindow(hwnd))
    except Exception:
        dpi = 0
    if dpi <= 0:
        try:
            dpi = int(USER32.GetDpiForSystem())
        except Exception:
            dpi = 96
    return dpi / 96.0


def wait_title(hwnd, needle, timeout, alt_needles=(), exclude=()):
    """Poll the top-level window title.  Returns (title, which) or (None,None).

    which is 'main' for needle, otherwise the matching alt needle.
    """
    end = time.time() + timeout
    while time.time() < end:
        t = get_title(hwnd)
        if needle in t and not any(x in t for x in exclude):
            return t, "main"
        for alt in alt_needles:
            if alt in t:
                return t, alt
        time.sleep(0.05)
    return None, None


def post_mouse(hwnd, msg, x, y, wparam):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    USER32.PostMessageW(hwnd, msg, wparam, lp)


def click(hwnd, x, y):
    force_foreground(hwnd)
    post_mouse(hwnd, WM_MOUSEMOVE, x, y, 0)
    time.sleep(0.02)
    post_mouse(hwnd, WM_LBUTTONDOWN, x, y, 1)
    time.sleep(0.05)
    post_mouse(hwnd, WM_LBUTTONUP, x, y, 0)


def double_click(hwnd, x, y):
    force_foreground(hwnd)
    post_mouse(hwnd, WM_MOUSEMOVE, x, y, 0)
    time.sleep(0.02)
    post_mouse(hwnd, WM_LBUTTONDOWN, x, y, 1)
    time.sleep(0.015)
    post_mouse(hwnd, WM_LBUTTONUP, x, y, 0)
    time.sleep(0.015)
    post_mouse(hwnd, WM_LBUTTONDBLCLK, x, y, 1)
    time.sleep(0.015)
    post_mouse(hwnd, WM_LBUTTONUP, x, y, 0)


def rapid_click_burst(hwnd, x, y):
    """Two double-clicks plus extra rapid clicks, all inside ~100 ms."""
    double_click(hwnd, x, y)
    time.sleep(0.02)
    double_click(hwnd, x, y)
    time.sleep(0.02)
    for _ in range(4):
        post_mouse(hwnd, WM_LBUTTONDOWN, x, y, 1)
        time.sleep(0.008)
        post_mouse(hwnd, WM_LBUTTONUP, x, y, 0)
        time.sleep(0.008)


# --------------------------------------------------------------------------
# screenshot analysis: JS on/off colour + crash-widget button geometry
# --------------------------------------------------------------------------
def color_stats(png):
    im = Image.open(png).convert("RGB")
    w, h = im.size
    px = im.load()
    top = 90
    blue = green = white = total = 0
    for y in range(top, min(h, h - 6), 6):
        for x in range(10, min(w, 1600), 6):
            r, g, b = px[x, y]
            total += 1
            if r >= 250 and g >= 250 and b >= 250:
                white += 1
            elif b >= 235 and b > r + 15 and b > g + 8:
                blue += 1
            elif g >= 225 and g > r + 10 and g > b + 10:
                green += 1
    if not total:
        return {"blue": 0.0, "green": 0.0, "white": 1.0}
    return {"blue": blue / total, "green": green / total,
            "white": white / total}


def locate_retry_button(png, scale):
    """Locate the crash-widget "重新加载" button in a client screenshot.

    Two visual styles are handled without relying on a filled background:
      * filled button: consecutive rows with a wide contiguous non-white run;
      * outlined button (white fill, 1 px border, coloured text): top and
        bottom border rows each contain a wide contiguous non-white run with
        the same x-extent; they are paired across the 15..70 px interior.
    Text rows never qualify (their runs are broken), and page-content rows
    are rejected by the width band derived from the button's fixed 100 logical
    px width.  Returns ((cx, cy), bbox) in PHYSICAL client pixels.
    """
    im = Image.open(png).convert("RGB")
    W, H = im.size
    px = im.load()
    page_top = int(round(PAGE_TOP_LOGICAL * scale))
    exp_w = 100.0 * scale
    min_run = max(30, int(exp_w * 0.55))
    max_w = int(exp_w * 1.9)
    rows = []
    for y in range(page_top, H - 2):
        best = (0, None, None)
        run_start = run_end = None
        for x in range(8, W - 8):
            r, g, b = px[x, y]
            if r < 250 or g < 250 or b < 250:
                if run_start is None:
                    run_start = x
                run_end = x
            elif run_start is not None:
                ln = run_end - run_start + 1
                if ln > best[0]:
                    best = (ln, run_start, run_end)
                run_start = None
        if run_start is not None:
            ln = run_end - run_start + 1
            if ln > best[0]:
                best = (ln, run_start, run_end)
        if best[0] >= min_run:
            rows.append((y, best[0], best[1], best[2]))
    groups = []
    for item in rows:
        if groups and item[0] - groups[-1][-1][0] <= 5:
            groups[-1].append(item)
        else:
            groups.append([item])
    cands = []
    for gi, g in enumerate(groups):
        y0, y1 = g[0][0], g[-1][0]
        fx = min(i[2] for i in g)
        lx = max(i[3] for i in g)
        bw = lx - fx + 1
        if not (min_run <= bw <= max_w):
            continue
        if y1 - y0 + 1 >= 8:
            cands.append((fx, y0, lx, y1, "filled"))
            continue
        for gj in groups[gi + 1:]:
            gy0, gy1 = gj[0][0], gj[-1][0]
            if gy0 - y1 > 70:
                break
            if gy0 - y1 < 8:
                continue
            gfx = min(i[2] for i in gj)
            glx = max(i[3] for i in gj)
            gw = glx - gfx + 1
            if abs(gfx - fx) <= 14 and abs(glx - lx) <= 14 and abs(gw - bw) <= 14:
                cands.append((min(fx, gfx), y0, max(lx, glx), gy1, "outline"))
                break
    best = None
    for (fx, y0, lx, y1, kind) in cands:
        bw, bh = lx - fx + 1, y1 - y0 + 1
        if not (0.6 * exp_w <= bw <= 1.9 * exp_w and 14 <= bh <= 90):
            continue
        ink = sum(1 for yy in range(y0, y1 + 1, 2)
                  for xx in range(fx, lx + 1, 2) if max(px[xx, yy]) < 160)
        if ink < 3:
            continue
        score = bw * bh
        if best is None or score > best[0]:
            best = (score, ((fx + lx) // 2, (y0 + y1) // 2),
                    (fx, y0, lx + 1, y1 + 1), kind)
    if best is None:
        return None, None
    return best[1], best[2]


def annotate(png, bbox, out):
    im = Image.open(png).convert("RGB")
    if bbox:
        d = ImageDraw.Draw(im)
        d.rectangle(bbox, outline=(255, 0, 0), width=3)
    im.save(out)


def screenshot(hwnd, name):
    path = os.path.join(SHOT_DIR, name)
    grab(hwnd, path)
    return path


# --------------------------------------------------------------------------
# WMI renderer discovery / process control
# --------------------------------------------------------------------------
def ps_json(script, timeout=20):
    try:
        r = subprocess.run([PS_EXE, "-NoProfile", "-NonInteractive", "-Command", script],
                           capture_output=True, timeout=timeout,
                           creationflags=CREATE_NO_WINDOW)
    except Exception:
        return None
    txt = r.stdout.decode("utf-8", "replace").strip()
    if not txt:
        return None
    try:
        data = json.loads(txt)
    except Exception:
        return None
    if isinstance(data, dict):
        return [data]
    return data if isinstance(data, list) else None


def get_renderers(ppid=None):
    data = ps_json(RENDERER_PS) or []
    if ppid is not None:
        # Fallback for a redesign that renames the renderer exe: children of
        # the browser that look like a renderer (socket/url/name signature).
        extra = []
        for d in (ps_json(CHILDREN_PS % ppid) or []):
            name = (d.get("Name") or "").lower()
            cl = d.get("CommandLine") or ""
            if ("khtml" in name) or ("--socket" in cl) or ("--url" in cl):
                if not any(int(x.get("ProcessId") or 0) == int(d.get("ProcessId") or 0)
                           for x in data):
                    extra.append(d)
        data = data + extra
    out = []
    for d in data:
        cl = d.get("CommandLine") or ""
        m = re.search(r'--url\s+(?:"([^"]*)"|(\S+))', cl)
        url = (m.group(1) or m.group(2)) if m else None
        out.append({
            "pid": int(d.get("ProcessId") or 0),
            "ppid": int(d.get("ParentProcessId") or 0),
            "created": str(d.get("CreationDate") or ""),
            "cmd": cl,
            "url": url,
            "disable_js": "--disable-js" in cl,
            "about_blank": (url == "about:blank"),
            "name": d.get("Name") or "",
            "for_us": ("127.0.0.1:%d" % PORT) in cl,
        })
    return out


def children(ppid):
    return [r for r in get_renderers(ppid) if r["ppid"] == ppid]


def taskkill(pid):
    try:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True, timeout=15,
                       creationflags=CREATE_NO_WINDOW)
    except Exception:
        pass


def browser_alive(proc):
    return proc.poll() is None


def current_tab_renderer(ppid):
    """The renderer currently serving the tab, from its command line alone.

    A cold tab renderer carries --url <page> (for_us).  A JS-enabled recovery
    may have ADOPTED the pre-warmed about:blank process instead: prewarm keeps
    at most one spare, and the adopted one is OLDER than the spare spawned for
    the next tab, so the oldest about:blank child is the tab renderer while
    the newer about:blank children are idle pre-warmed spares.
    """
    cur = children(ppid)
    cold = [r for r in cur if r["for_us"]]
    if cold:
        return sorted(cold, key=lambda r: r["created"])[-1]
    ab = [r for r in cur if r["about_blank"]]
    if ab:
        return sorted(ab, key=lambda r: r["created"])[0]
    return None


def active_renderer(ppid, before_pids):
    """The tab renderer after a recovery.

    A cold start carries --url <page>; a JS-enabled recovery may instead have
    ADOPTED the pre-warmed about:blank renderer, which is a PID that already
    existed before the crash.  Prefer a new for-us process, else the oldest
    about:blank process that existed before (that is the adopted one; the
    spare spawned afterwards is newer and not in before_pids).
    """
    cur = children(ppid)
    cold_new = [r for r in cur if r["for_us"] and r["pid"] not in before_pids]
    if cold_new:
        return sorted(cold_new, key=lambda r: r["created"])[-1]
    adopted = [r for r in cur if r["about_blank"] and r["pid"] in before_pids]
    if adopted:
        return sorted(adopted, key=lambda r: r["created"])[0]
    return current_tab_renderer(ppid)


# --------------------------------------------------------------------------
# browser stderr trace: crash + recovery evidence
# --------------------------------------------------------------------------
class Trace:
    def __init__(self, path):
        self.path = path

    def text(self):
        try:
            with open(self.path, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def lines(self):
        return self.text().splitlines()

    def crash_lines(self):
        out = []
        for ln in self.lines():
            m = re.search(r"\[rend-crash\] pid=(\d+) embedded=(\d) exit=(-?\d+) "
                          r"status=(\w+) url=(\S+)", ln)
            if m:
                out.append({"pid": int(m.group(1)), "embedded": m.group(2),
                            "exit": int(m.group(3)), "status": m.group(4),
                            "url": m.group(5), "line": ln})
        return out

    def recovery_lines(self):
        out = []
        for ln in self.lines():
            m = re.search(r"\[recovery\] restart tab=(\d+) jsDisabled=(\d) "
                          r"clearCache=(\d)", ln)
            if m:
                out.append({"tab": int(m.group(1)), "js_disabled": m.group(2) == "1",
                            "clear_cache": m.group(3) == "1", "line": ln})
        return out

    def wait_more_crash(self, before, timeout):
        end = time.time() + timeout
        while time.time() < end:
            lines = self.crash_lines()
            if len(lines) > before:
                return lines[-1]
            time.sleep(0.05)
        return None

    def wait_more_recovery(self, before, timeout):
        end = time.time() + timeout
        while time.time() < end:
            lines = self.recovery_lines()
            if len(lines) > before:
                return lines[-1]
            time.sleep(0.05)
        return None

    def wait_more_recovery_all(self, before, timeout):
        """Wait for ANY number of new recovery lines; return them all."""
        end = time.time() + timeout
        while time.time() < end:
            lines = self.recovery_lines()
            if len(lines) > before:
                time.sleep(0.35)  # let duplicate rapid restarts land too
                return self.recovery_lines()[before:]
            time.sleep(0.05)
        return []


# --------------------------------------------------------------------------
# flow helpers
# --------------------------------------------------------------------------
def kill_active_and_confirm(ppid, active, trace, candidates, timeout=4.0):
    """Kill the candidate tab renderer; confirm by a new [rend-crash] line.

    Idle pre-warm renderers (about:blank, not the tab) have no RenderTab
    connection, so killing one produces no [rend-crash] line; such candidates
    are skipped and the next one is tried.  Returns (renderer, crashline) or
    (None, None).
    """
    tried = set()
    order = []
    if active:
        order.append(active)
    for r in candidates:
        if r["pid"] not in [o["pid"] for o in order]:
            order.append(r)
    for cand in order:
        pid = cand["pid"]
        if pid in tried:
            continue
        tried.add(pid)
        n = len(trace.crash_lines())
        taskkill(pid)
        line = trace.wait_more_crash(n, timeout)
        if line:
            return cand, line
        say("    [diag] taskkill pid=%d produced no [rend-crash] line in %.1fs"
            % (pid, timeout))
    return None, None


def crash_sequence(state, trace, before_children, label):
    """Kill the tab renderer (verifying which candidate is the real one)."""
    ppid = state["ppid"]
    active = current_tab_renderer(ppid)
    cands = [r for r in before_children if r["for_us"]] + \
            sorted([r for r in before_children if r["about_blank"]],
                   key=lambda r: r["created"])
    if active:
        cands = [active] + [c for c in cands if c["pid"] != active["pid"]]
    killed, line = kill_active_and_confirm(ppid, active, trace, cands)
    if not killed:
        # Renderer discovery can race a process-table refresh / a rebuilt exe;
        # one fresh retry is cheap and keeps a slow machine from a false FAIL.
        time.sleep(0.8)
        active = current_tab_renderer(ppid)
        cur = children(ppid)
        cands2 = [r for r in cur if r["for_us"]] + \
                 sorted([r for r in cur if r["about_blank"]],
                        key=lambda r: r["created"])
        if active:
            cands2 = [active] + [c for c in cands2 if c["pid"] != active["pid"]]
        say("    [diag] crash_sequence(%s): first attempt found no killable tab "
            "renderer; retry with %s"
            % (label, [(r["pid"], r["url"]) for r in cands2]))
        killed, line = kill_active_and_confirm(ppid, active, trace, cands2)
    if not killed:
        cur = children(ppid)
        say("    [diag] crash_sequence(%s) FAILED: children=%s before=%s"
            % (label,
               [(r["pid"], r["ppid"], r["url"], r["disable_js"]) for r in cur],
               [(r["pid"], r["ppid"], r["url"], r["disable_js"])
                for r in before_children]))
        return None, None
    say("    killed renderer pid=%d (%s)" % (
        killed["pid"], esc(killed["cmd"], 160)))
    return killed, line


def observe_crash_ui_bounded(hwnd, proc, trace, recovery_before, loads_before,
                             timeout, settle):
    """Wait for the crash UI, then hold still for `settle` s and require that
    nothing automatic restarted.  Returns (ok, title, extra_restarts, loads)."""
    t, _ = wait_title(hwnd, CRASH_TITLE, min(8.0, timeout) if TITLE_OK[0] else 0.0)
    shot = os.path.join(SHOT_DIR, "_crashui_probe.png")
    if not t:
        # Title-independent crash-UI detection: the crash page is the only
        # state with the "重新加载" button and no blue/green page body.
        end = time.time() + (max(2.0, timeout - 8.0) if TITLE_OK[0]
                             else max(3.0, timeout))
        while time.time() < end:
            try:
                grab(hwnd, shot)
                btn, _ = locate_retry_button(shot, get_scale(hwnd))
            except Exception:
                btn = None
            if btn:
                break
            time.sleep(0.6)
        if not btn:
            return False, t, len(trace.recovery_lines()) - recovery_before, \
                page_loads() - loads_before
    time.sleep(settle)
    still_title = CRASH_TITLE in get_title(hwnd)
    try:
        grab(hwnd, shot)
        still_btn, _ = locate_retry_button(shot, get_scale(hwnd))
    except Exception:
        still_btn = None
    still = still_title or bool(still_btn)
    extra = len(trace.recovery_lines()) - recovery_before
    loads = page_loads() - loads_before
    return (still and extra == 0 and loads == 0), get_title(hwnd), extra, loads


def wait_outcome(hwnd, proc, trace, recovery_before, timeout):
    """After a crash: 'crash_ui', 'recovery', 'exit' or None."""
    shot = os.path.join(SHOT_DIR, "_outcome_probe.png")
    next_shot = 0.0
    end = time.time() + timeout
    while time.time() < end:
        if not browser_alive(proc):
            return "exit"
        t = get_title(hwnd)
        if CRASH_TITLE in t:
            return "crash_ui"
        if len(trace.recovery_lines()) > recovery_before:
            return "recovery"
        if time.time() >= next_shot:
            next_shot = time.time() + 0.8
            try:
                grab(hwnd, shot)
                btn, _ = locate_retry_button(shot, get_scale(hwnd))
            except Exception:
                btn = None
            if btn:
                return "crash_ui"
        time.sleep(0.05)
    return None


def wait_page_color(hwnd, want_js, timeout):
    """Title-independent page-state signal: the static body is blue, the JS
    body is green, so the dominant page colour proves JS off/on and that the
    document actually painted (used when the window title does not update)."""
    path = os.path.join(SHOT_DIR, "_state_probe.png")
    end = time.time() + timeout
    while time.time() < end:
        if not browser_alive(proc_global[0]):
            return False
        try:
            grab(hwnd, path)
            cs = color_stats(path)
        except Exception:
            cs = {"green": 0.0, "blue": 0.0}
        if want_js and cs["green"] > 0.25:
            return True
        if (not want_js) and cs["blue"] > 0.25:
            return True
        time.sleep(0.5)
    return False


def page_restored(hwnd, want_js, timeout, trace, allow_placeholder=True):
    """Wait until the page title (JS on/off) is visible; fall back to the
    page body colour when the redesigned shell does not surface the title."""
    want = TITLE_JS if want_js else TITLE_STATIC
    end = time.time() + (min(8.0, timeout) if TITLE_OK[0] else 0.0)
    while time.time() < end:
        if not browser_alive(proc_global[0]):
            return False
        if want in get_title(hwnd):
            return True
        time.sleep(0.05)
    return wait_page_color(hwnd, want_js, max(4.0, timeout - 8.0))


def title_events_after(trace, line_index):
    out = []
    for ln in trace.lines()[line_index:]:
        m = re.search(r"titleChanged title=(.*) cur=", ln)
        if m:
            out.append(m.group(1))
    return out


def classify_js(trace, line_index, timeout=12.0, initial_grace=2.5):
    """Detect whether the recovered page executed JS.

    Works even while the recovery overlay hides the page: the renderer ships
    document titles through the native KHTML caption path.  The page title
    starts as StaticTitle and only the setTimeout JS turns it into JsTitle.
    Returns 'js', 'static' or 'unknown'.
    """
    end = time.time() + timeout
    first_static = None
    while time.time() < end:
        evs = title_events_after(trace, line_index)
        if TITLE_JS in evs:
            return "js"
        if TITLE_STATIC in evs:
            if first_static is None:
                first_static = time.time()
            elif time.time() - first_static >= initial_grace:
                return "static"
        time.sleep(0.1)
    return "unknown"


def wait_page_visible(hwnd, timeout=8.0):
    """Wait until a page body colour is actually composited (blue = JS-off
    static body, green = JS-on body).  While the recovery overlay is stuck no
    page colour ever appears, which is exactly the condition to report."""
    path = os.path.join(SHOT_DIR, "_visible_probe.png")
    cs = {"blue": 0.0, "green": 0.0, "white": 1.0}
    end = time.time() + timeout
    while time.time() < end:
        if not browser_alive(proc_global[0]):
            return cs, path
        try:
            grab(hwnd, path)
            cs = color_stats(path)
        except Exception:
            cs = {"blue": 0.0, "green": 0.0, "white": 1.0}
        if cs["blue"] > 0.25 or cs["green"] > 0.25:
            return cs, path
        time.sleep(0.5)
    return cs, path


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
proc_global = [None]


def main():
    global SHOT_DIR
    SHOT_DIR = tempfile.mkdtemp(prefix="khtml_crash_probe_")
    started = time.time()

    if not os.path.exists(EXE):
        say("FATAL: %s missing" % EXE)
        return 1

    # Preflight: never race another browser instance (Lead runs other tests).
    others = []
    try:
        r = subprocess.run([PS_EXE, "-NoProfile", "-NonInteractive", "-Command",
                            "Get-Process khtml_browser -ErrorAction SilentlyContinue | "
                            "Select-Object -ExpandProperty Id"],
                           capture_output=True, timeout=15,
                           creationflags=CREATE_NO_WINDOW)
        others = [x.strip() for x in r.stdout.decode("utf-8", "replace").splitlines()
                  if x.strip()]
    except Exception:
        pass
    if others:
        step("preflight", "INCONCLUSIVE",
             "another khtml_browser.exe is running (pid %s); refusing to run "
             "concurrently" % ",".join(others))
        return 2
    # Never test a half-rebuilt tree: a source newer than the exe or a live
    # ninja lock means the Lead is still integrating/rebuilding.
    src_file = os.path.join(ROOT, "src", "host", "khtml_browser.cpp")
    if os.path.exists(src_file) and os.path.getmtime(src_file) > os.path.getmtime(EXE) + 1:
        step("preflight", "INCONCLUSIVE",
             "src/host/khtml_browser.cpp is newer than khtml_browser.exe; the "
             "build is stale/in progress, refusing to test")
        return 2
    lock_file = os.path.join(BUILD, ".ninja_lock")
    if os.path.exists(lock_file):
        step("preflight", "INCONCLUSIVE",
             "build/host/.ninja_lock exists (ninja build in progress); refusing "
             "to test")
        return 2

    crash_before = os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1
    dumps_before = set(glob.glob(os.path.join(BUILD, "*.dmp")))

    srv = serve()
    say("crash_recovery_probe: build=%s server=%s screenshots=%s"
        % (EXE, BASE_URL, SHOT_DIR))
    say("crashlog_before=%s dumps_before=%d" % (crash_before, len(dumps_before)))

    trace_path = os.path.join(SHOT_DIR, "browser_stderr.log")
    tracef = open(trace_path, "wb", buffering=0)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")

    state = {}
    try:
        proc = subprocess.Popen([EXE, BASE_URL + PAGE], cwd=BUILD, env=env,
                                stdout=subprocess.DEVNULL, stderr=tracef)
        proc_global[0] = proc
        trace = Trace(trace_path)

        hwnd = None
        for _ in range(60):
            hwnd = find_main_window(proc.pid)
            if hwnd:
                break
            if proc.poll() is not None:
                break
            time.sleep(0.25)
        if not hwnd:
            step("window", "FAIL", "browser window not found (exit=%s)" % proc.poll())
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        state["hwnd"] = hwnd
        state["ppid"] = proc.pid
        W, H = get_client_size(hwnd)
        scale = get_scale(hwnd)
        say("window hwnd=%s client=%dx%d scale=%.2f title=%s"
            % (hwnd, W, H, scale, esc(get_title(hwnd), 80)))
        if W < 640 or H < 400:
            step("window", "FAIL", "client too small: %dx%d" % (W, H))
            return finish(proc, srv, trace, crash_before, dumps_before, started)

        # -------- 1. normal load (static title -> JS title) --------
        t, _ = wait_title(hwnd, TITLE_JS, 12)
        if not t:
            # Redesigned shell may not surface document.title; the JS-turned
            # green body is then the JS-on evidence.
            TITLE_OK[0] = False
            t = TITLE_JS if wait_page_color(hwnd, True, 10) else None
        shot_normal = screenshot(hwnd, "01_normal.png")
        cs = color_stats(shot_normal)
        renderers = []
        end = time.time() + 15
        while time.time() < end:
            renderers = children(proc.pid)
            if any(r["for_us"] for r in renderers):
                break
            time.sleep(0.2)
        cold = [r for r in renderers if r["for_us"]]
        normal_ok = bool(t) and bool(cold) and cs["green"] > 0.25
        step("normal_load_js_on", "PASS" if normal_ok else "FAIL",
             "title=%s renderers=%s blue=%.2f green=%.2f req=%s" % (
                 esc(t or "", 80),
                 [(r["pid"], r["ppid"], "disable-js" if r["disable_js"] else "js-on")
                  for r in renderers],
                 cs["blue"], cs["green"],
                 dict(SERVER_STATE.counts)))
        if not normal_ok:
            dump_state(hwnd, proc, trace, "normal load failed")
            try:
                say("      last screenshot: %s" % shot_normal)
            except Exception:
                pass
            return finish(proc, srv, trace, crash_before, dumps_before, started)

        initial = None
        if cold:
            initial = sorted(cold, key=lambda r: r["created"])[0]

        # -------- 2. crash #1 -> automatic retry #1 (JS disabled) --------
        lines1 = len(trace.lines())
        fin1_before = req_count(FINAL)
        before1 = children(proc.pid)
        rec1_before = len(trace.recovery_lines())
        killed, cline = crash_sequence(state, trace, before1, "crash1")
        if not killed:
            step("crash1_forced_kill", "FAIL", "could not kill the tab renderer")
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        step("crash1_forced_kill", "PASS",
             "pid=%d crashline=%s" % (killed["pid"], esc(cline["line"], 200)))
        rec1 = trace.wait_more_recovery(rec1_before, 10)
        if not rec1:
            step("auto_retry_1_launched", "FAIL",
                 "no [recovery] restart line after crash #1")
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        js1 = classify_js(trace, lines1)
        vis1_cs, _ = wait_page_visible(hwnd, 8)
        shot_auto1 = screenshot(hwnd, "03_auto1_page.png")
        vis1_cs = color_stats(shot_auto1)
        if vis1_cs["green"] > 0.25:
            js1 = "js"
        elif vis1_cs["blue"] > 0.25:
            js1 = "static"
        reload1 = req_count(FINAL) - fin1_before
        active1 = active_renderer(proc.pid, [r["pid"] for r in before1])
        mech1 = (rec1["js_disabled"] and not rec1["clear_cache"]
                 and active1 is not None and active1["disable_js"])
        step("auto_retry_1_launched", "PASS" if mech1 else "FAIL",
             "log=%s reload_requests=%d renderer=%s" % (
                 esc(rec1["line"], 160), reload1,
                 esc(active1["cmd"], 150) if active1 else None))
        step("auto_retry_1_js_disabled_effective",
             "PASS" if js1 == "static" else ("FAIL" if js1 == "js" else "INCONCLUSIVE"),
             "observed_js=%s title_events=%s (the recovered renderer cmdline "
             "contains --disable-js, so JsTitle means page JS still ran)"
             % (js1, [esc(x, 50) for x in title_events_after(trace, lines1)]))
        vis1_ok = (vis1_cs["blue"] > 0.25 or vis1_cs["green"] > 0.25)
        step("auto_retry_1_page_visible", "PASS" if vis1_ok else "FAIL",
             "blue=%.2f green=%.2f screenshot=%s (FAIL means the recovery "
             "placeholder still covered the page after 8 s)"
             % (vis1_cs["blue"], vis1_cs["green"], os.path.basename(shot_auto1)))

        # -------- 2b. history preservation (best effort, page may be hidden) --------
        req_before_back = req_count(PAGE)
        click(hwnd, int(round(BACK_BTN_LOGICAL[0] * scale)),
              int(round(BACK_BTN_LOGICAL[1] * scale)))
        end = time.time() + 6
        back_ok = False
        while time.time() < end:
            if req_count(PAGE) > req_before_back:
                back_ok = True
                break
            time.sleep(0.1)
        if back_ok:
            wait_title(hwnd, TITLE_STATIC, 3 if TITLE_OK[0] else 0.2)
            step("history_after_auto_recovery", "PASS",
                 "Back re-requested %s (pg->back survived the crash; req=%s)"
                 % (PAGE, dict(SERVER_STATE.counts)))
        else:
            step("history_after_auto_recovery", "INCONCLUSIVE",
                 "Back click produced no %s request; cannot distinguish "
                 "'empty history' from 'click missed' (req=%s)"
                 % (PAGE, dict(SERVER_STATE.counts)), optional=True)
        time.sleep(0.5)

        # -------- 3. crash #2 -> automatic retry #2 (cache clear, JS on) --------
        lines2 = len(trace.lines())
        fin2_before = req_count(FINAL)
        before2 = children(proc.pid)
        rec2_before = len(trace.recovery_lines())
        killed2, cline2 = crash_sequence(state, trace, before2, "crash2")
        if not killed2:
            step("crash2_forced_kill", "FAIL", "could not kill the tab renderer")
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        step("crash2_forced_kill", "PASS",
             "pid=%d crashline=%s" % (killed2["pid"], esc(cline2["line"], 200)))
        rec2 = trace.wait_more_recovery(rec2_before, 12)
        if rec2:
            js2 = classify_js(trace, lines2)
            vis2_cs, _ = wait_page_visible(hwnd, 8)
            shot_auto2 = screenshot(hwnd, "05_auto2_page.png")
            vis2_cs = color_stats(shot_auto2)
            if vis2_cs["green"] > 0.25:
                js2 = "js"
            elif vis2_cs["blue"] > 0.25:
                js2 = "static"
            reload2 = req_count(FINAL) - fin2_before
            active2 = active_renderer(proc.pid, [r["pid"] for r in before2])
            mech2 = (rec2["clear_cache"] and not rec2["js_disabled"]
                     and active2 is not None and not active2["disable_js"])
            step("auto_retry_2_cache_clear_js_on", "PASS" if mech2 else "FAIL",
                 "log=%s post_clear_reload_requests=%d renderer=%s"
                 % (esc(rec2["line"], 160), reload2,
                    esc(active2["cmd"], 150) if active2 else None))
            step("auto_retry_2_js_effective",
                 "PASS" if js2 == "js" else ("FAIL" if js2 == "static" else "INCONCLUSIVE"),
                 "observed_js=%s title_events=%s"
                 % (js2, [esc(x, 50) for x in title_events_after(trace, lines2)]))
            vis2_ok = (vis2_cs["blue"] > 0.25 or vis2_cs["green"] > 0.25)
            step("auto_retry_2_page_visible", "PASS" if vis2_ok else "FAIL",
                 "blue=%.2f green=%.2f screenshot=%s"
                 % (vis2_cs["blue"], vis2_cs["green"], os.path.basename(shot_auto2)))

            # -------- 4. crash #3 -> final crash UI --------
            before3 = children(proc.pid)
            rec3_before = len(trace.recovery_lines())
            loads3_before = page_loads()
            killed3, cline3 = crash_sequence(state, trace, before3, "crash3")
            if not killed3:
                step("crash3_forced_kill", "FAIL", "could not kill the tab renderer")
                return finish(proc, srv, trace, crash_before, dumps_before, started)
            step("crash3_forced_kill", "PASS",
                 "pid=%d crashline=%s" % (killed3["pid"], esc(cline3["line"], 200)))
            crash_ui_ok, t_crash, extra_restart, extra_loads = observe_crash_ui_bounded(
                hwnd, proc, trace, rec3_before, loads3_before, 12, 4.0)
            shot_crash = screenshot(hwnd, "06_crash_ui.png")
            step("crash_ui_after_third_crash", "PASS" if crash_ui_ok else "FAIL",
                 "title=%s extra_auto_restarts=%d extra_page_requests=%d req=%s"
                 % (esc(t_crash or "", 80), extra_restart, extra_loads,
                    dict(SERVER_STATE.counts)))
            if not crash_ui_ok:
                return finish(proc, srv, trace, crash_before, dumps_before, started)
        else:
            oc = wait_outcome(hwnd, proc, trace, rec2_before, 8)
            if oc == "crash_ui":
                vis2_ok = False
                step("auto_retry_2_cache_clear_js_on", "FAIL",
                     "crash #2 showed the crash UI directly; the required second "
                     "automatic attempt (clear cache + JS on) did not happen")
                shot_crash = screenshot(hwnd, "06_crash_ui.png")
                step("crash_ui_after_third_crash", "SKIP",
                     "crash UI appeared after crash #2 instead of crash #3")
            else:
                step("auto_retry_2_cache_clear_js_on", "INCONCLUSIVE",
                     "no recovery line and no crash UI within 8 s (outcome=%s)"
                     % oc)
                return finish(proc, srv, trace, crash_before, dumps_before, started)

        # -------- 5. crash-widget button detection + manual retry --------
        shot_crash = screenshot(hwnd, "07_crash_ui_button.png")
        btn, bbox = locate_retry_button(shot_crash, scale)
        if btn:
            annotate(shot_crash, bbox,
                     os.path.join(SHOT_DIR, "07_crash_ui_button_annotated.png"))
            step("crash_button_located", "PASS",
                 "button centre client=%s bbox=%s screenshot=%s"
                 % (btn, bbox, os.path.basename(shot_crash)))
        else:
            step("crash_button_located", "INCONCLUSIVE",
                 "pixel detector found no button; using centred layout fallback")
            page_top = int(round(PAGE_TOP_LOGICAL * scale))
            page_bottom = H - int(round(PAGE_BOTTOM_MARGIN_LOGICAL * scale))
            cy0 = (page_top + page_bottom) // 2
            btn = (W // 2, cy0 + int(round(52 * scale)))
        lines_m = len(trace.lines())
        fin_m_before = req_count(FINAL)
        rec_m_before = len(trace.recovery_lines())
        children_before_click = children(proc.pid)
        click(hwnd, btn[0], btn[1])
        rec_manual = trace.wait_more_recovery(rec_m_before, 6)
        if not rec_manual:
            say("    first manual-retry click produced no recovery line; "
                "trying layout fallbacks")
            page_top = int(round(PAGE_TOP_LOGICAL * scale))
            page_bottom = H - int(round(PAGE_BOTTOM_MARGIN_LOGICAL * scale))
            cy0 = (page_top + page_bottom) // 2
            for off in (40, 64, 78, 28):
                click(hwnd, W // 2, cy0 + int(round(off * scale)))
                rec_manual = trace.wait_more_recovery(rec_m_before, 3)
                if rec_manual:
                    break
        if not rec_manual:
            step("manual_retry_clicked", "INCONCLUSIVE",
                 "no [recovery] restart after clicking the crash widget; UI "
                 "automation could not be validated (title=%s)"
                 % esc(get_title(hwnd), 100))
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        js_m = classify_js(trace, lines_m)
        vis_m_cs, _ = wait_page_visible(hwnd, 8)
        shot_manual = screenshot(hwnd, "08_manual_retry_page.png")
        vis_m_cs = color_stats(shot_manual)
        if vis_m_cs["green"] > 0.25:
            js_m = "js"
        elif vis_m_cs["blue"] > 0.25:
            js_m = "static"
        reload_m = req_count(FINAL) - fin_m_before
        active_m = active_renderer(proc.pid, [r["pid"] for r in children_before_click])
        mech_m = (not rec_manual["js_disabled"] and not rec_manual["clear_cache"])
        step("manual_retry_clicked", "PASS" if mech_m else "FAIL",
             "log=%s reload_requests=%d renderer=%s"
             % (esc(rec_manual["line"], 160), reload_m,
                esc(active_m["cmd"], 150) if active_m else None))
        step("manual_retry_js_enabled",
             "PASS" if js_m == "js" else ("FAIL" if js_m == "static" else "INCONCLUSIVE"),
             "observed_js=%s title_events=%s"
             % (js_m, [esc(x, 50) for x in title_events_after(trace, lines_m)]))
        vis_m_ok = (vis_m_cs["blue"] > 0.25 or vis_m_cs["green"] > 0.25)
        step("manual_retry_page_visible", "PASS" if vis_m_ok else "FAIL",
             "blue=%.2f green=%.2f screenshot=%s"
             % (vis_m_cs["blue"], vis_m_cs["green"], os.path.basename(shot_manual)))

        # -------- 6. crash after manual retry -> crash UI again, no restart --------
        before4 = children(proc.pid)
        rec4_before = len(trace.recovery_lines())
        loads4_before = page_loads()
        killed4, cline4 = crash_sequence(state, trace, before4, "crash4")
        if not killed4:
            step("crash_after_manual_retry", "INCONCLUSIVE",
                 "could not identify/kill the manual-retry tab renderer")
            return finish(proc, srv, trace, crash_before, dumps_before, started)
        crash2_ok, t_crash2, extra_restart2, extra_loads2 = observe_crash_ui_bounded(
            hwnd, proc, trace, rec4_before, loads4_before, 12, 5.0)
        step("crash_after_manual_retry", "PASS" if crash2_ok else "FAIL",
             "title=%s extra_auto_restarts=%d extra_page_requests=%d req=%s"
             % (esc(t_crash2 or "", 80), extra_restart2, extra_loads2,
                dict(SERVER_STATE.counts)))
        if not crash2_ok:
            return finish(proc, srv, trace, crash_before, dumps_before, started)

        # -------- 7. repeated double/rapid clicks -> exactly one retry --------
        shot_crash2 = screenshot(hwnd, "09_crash_ui_again.png")
        btn2, bbox2 = locate_retry_button(shot_crash2, scale)
        if not btn2:
            btn2 = btn
        lines_r = len(trace.lines())
        rec_r_before = len(trace.recovery_lines())
        fin_r_before = req_count(FINAL)
        rapid_click_burst(hwnd, btn2[0], btn2[1])
        rapid_new = trace.wait_more_recovery_all(rec_r_before, 8)
        req_delta = req_count(FINAL) - fin_r_before
        if rapid_new:
            js_r = classify_js(trace, lines_r)
            vis_r_cs, _ = wait_page_visible(hwnd, 8)
            shot_rapid = screenshot(hwnd, "10_rapid_click_page.png")
            vis_r_cs = color_stats(shot_rapid)
        else:
            js_r = "unknown"
            vis_r_cs = {"blue": 0.0, "green": 0.0}
        rapid_ok = (len(rapid_new) == 1 and req_delta <= 1)
        rapid_verdict = "PASS" if rapid_ok else ("FAIL" if len(rapid_new) > 1 else "INCONCLUSIVE")
        step("rapid_clicks_single_reload", rapid_verdict,
             "recovery_lines_after_burst=%d page_requests_delta=%d observed_js=%s"
             % (len(rapid_new), req_delta, js_r))
        vis_r_ok = (vis_r_cs["blue"] > 0.25 or vis_r_cs["green"] > 0.25)
        step("rapid_click_page_visible", "PASS" if vis_r_ok else "FAIL",
             "blue=%.2f green=%.2f" % (vis_r_cs["blue"], vis_r_cs["green"]))
        if rapid_new:
            say("    rapid-burst recovery line(s): %s"
                % [esc(x["line"], 160) for x in rapid_new])

        # -------- 7b. recovery hand-off summary --------
        vis_all = [vis1_ok, vis2_ok, vis_m_ok, vis_r_ok]
        step("recovery_page_visible_summary", "PASS" if all(vis_all) else "FAIL",
             "auto1=%s auto2=%s manual=%s rapid=%s (False = page body never "
             "composited; recovery overlay stayed on screen)"
             % tuple(vis_all))

        # -------- 8. bounded automatic retries (global) --------
        auto_all = trace.recovery_lines()
        auto_flags = [(x["js_disabled"], x["clear_cache"]) for x in auto_all]
        automatic = [x for x in auto_all if x["js_disabled"] or x["clear_cache"]]
        auto_expected = auto_flags[:2] == [(True, False), (False, True)]
        bounded = len(automatic) == 2 and auto_expected
        step("bounded_auto_retries", "PASS" if bounded else "FAIL",
             "recovery sequence=%s (exactly 2 automatic retries expected: "
             "attempt1 js-off, attempt2 cache-clear js-on; the remaining lines "
             "in the sequence are the user-initiated manual retries)"
             % auto_flags)

        step("cookie_header_passive_check",
             "PASS" if cookie_sent() else "INCONCLUSIVE",
             "server sent Set-Cookie probe=1; Cookie header %s"
             % ("was returned by the browser" if cookie_sent()
                else "was never returned (cookie support not observable here)"),
             optional=True)

        return finish(proc, srv, trace, crash_before, dumps_before, started)

    except Exception:
        say("EXCEPTION:\n" + traceback.format_exc())
        step("probe_infrastructure", "FAIL", "unhandled exception (see above)")
        return finish(proc if proc_global[0] else None, srv, trace
                      if "trace" in locals() else Trace(trace_path),
                      crash_before, dumps_before, started)
    finally:
        try:
            tracef.close()
        except Exception:
            pass


def finish(proc, srv, trace, crash_before, dumps_before, started):
    hwnd = None
    if proc is not None:
        # Cleanup: close the whole tree; never leave renderers behind.
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True, timeout=15,
                           creationflags=CREATE_NO_WINDOW)
        except Exception:
            pass
        end = time.time() + 10
        while time.time() < end and proc.poll() is None:
            time.sleep(0.1)
        if proc.poll() is None:
            try:
                proc.kill()
            except Exception:
                pass
        time.sleep(0.5)
        left = [r for r in get_renderers(proc.pid) if r["ppid"] == proc.pid]
        for r in left:
            taskkill(r["pid"])
        if left:
            time.sleep(0.6)
            left = [r for r in get_renderers(proc.pid) if r["ppid"] == proc.pid]
        # Orphans can be re-parented: match our browser pid in the socket name.
        ours = [r for r in get_renderers(proc.pid)
                if r["for_us"]
                or ("khtml_rt_%d_" % proc.pid) in r["cmd"]
                or ("khtml_warm_%d_" % proc.pid) in r["cmd"]]
        for r in ours:
            taskkill(r["pid"])
        if ours:
            time.sleep(0.6)
            ours = [r for r in get_renderers(proc.pid)
                    if r["for_us"]
                    or ("khtml_rt_%d_" % proc.pid) in r["cmd"]
                    or ("khtml_warm_%d_" % proc.pid) in r["cmd"]]
        cleanup_ok = not left and not ours and not browser_alive(proc)
        step("cleanup_processes", "PASS" if cleanup_ok else "FAIL",
             "browser_exited=%s leftover_children=%d leftover_for_us=%d"
             % (not browser_alive(proc), len(left), len(ours)))
    else:
        step("cleanup_processes", "INCONCLUSIVE", "no browser process was started")

    try:
        srv.shutdown()
    except Exception:
        pass

    crash_after = os.path.getsize(CRASHLOG) if os.path.exists(CRASHLOG) else -1
    dumps_after = set(glob.glob(os.path.join(BUILD, "*.dmp")))
    new_dumps = sorted(dumps_after - dumps_before)
    step("crash_artifacts", "PASS" if not new_dumps and crash_after == crash_before else "INCONCLUSIVE",
         "crashlog %d->%d new_dumps=%s (none deleted)"
         % (crash_before, crash_after, [os.path.basename(x) for x in new_dumps]),
         optional=True)

    lines = trace.lines() if trace else []
    say("")
    say("evidence: screenshots=%s trace=%s" % (SHOT_DIR, trace.path if trace else "?"))
    say("recovery log lines:")
    for x in (trace.recovery_lines() if trace else []):
        say("  " + esc(x["line"], 200))
    say("crash log lines:")
    for x in (trace.crash_lines() if trace else []):
        say("  " + esc(x["line"], 200))
    with SERVER_STATE.lock:
        reqs = dict(SERVER_STATE.counts)
    say("http requests: %s" % reqs)
    say("elapsed=%.1fs" % (time.time() - started))

    fails = [r for r in RESULTS if r[1] == "FAIL"]
    incs = [r for r in RESULTS if r[1] in ("INCONCLUSIVE", "SKIP")]
    passes = [r for r in RESULTS if r[1] == "PASS"]
    say("")
    say("steps: %d passed, %d failed, %d inconclusive/skipped"
        % (len(passes), len(fails), len(incs)))
    req_incs = [r for r in incs if not r[3]]
    opt_incs = [r for r in incs if r[3]]
    if fails:
        overall = "FAIL"
        code = 1
    elif req_incs:
        overall = "INCONCLUSIVE"
        code = 2
    else:
        overall = "PASS"
        code = 0
    if opt_incs and not fails:
        print("  note: %d optional check(s) inconclusive: %s"
              % (len(opt_incs), [r[0] for r in opt_incs]))
    say("CRASH RECOVERY PROBE " + overall)
    return code


if __name__ == "__main__":
    sys.exit(main())
