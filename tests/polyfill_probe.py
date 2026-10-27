# polyfill_probe.py — validates the injected JS polyfill bundle.
#
# Three stages, chosen because the engine's document pipeline delivers an
# initial document before the final rewrite (both now carry the polyfill) and
# its Promise implementation does not reliably drain microtasks:
#   1. extract kJsPolyfill from src/host/pageloader.cpp and `node --check` it
#      (catches the one-stray-semicolon SyntaxError that window.onerror hid);
#   2. execute the extracted bundle in node with a document stub and assert
#      localStorage semantics + the explicit fetch() rejection message;
#   3. run the engine on an inline copy of the bundle and read a synchronous
#      report from document.title: typeof console/fetch/localStorage,
#      localStorage empty-string/prototype-key/key()/clear(), matches()
#      against class/tag/id/attribute selectors, and fetch() returning a
#      thenable (its rejection is covered by stage 2).
#
# A static check also pins that BOTH deliverInitialDocument() and
# rewriteAndSave() inject the bundle (page scripts must never run without it).
#
# Usage: python tests\polyfill_probe.py
import ast
import ctypes
import http.server
import io
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
BUILD = os.path.join(ROOT, "build", "host")
PAGELOADER = os.path.join(ROOT, "src", "host", "pageloader.cpp")
PORT = 8198
USER32 = ctypes.windll.user32

sys.path.insert(0, os.path.join(ROOT, "tests"))
from win_shot import find_window  # noqa: E402


def extract_polyfill_js():
    text = io.open(PAGELOADER, encoding="utf-8").read()
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines) if "static const char *kJsPolyfill" in l)
    end = next(i for i in range(start, len(lines)) if '})();";' in lines[i])
    block = "\n".join(lines[start:end + 1])
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', block, re.S)
    return "".join(ast.literal_eval('"' + p + '"') for p in parts)


def find_node():
    node = shutil.which("node")
    if node:
        return node
    cand = os.path.join(os.path.expanduser("~"), ".dsh", "dsh-runtimes",
                        "dsh-primary-runtime", "dependencies", "node",
                        "bin", "node.exe")
    return cand if os.path.exists(cand) else None


def node_syntax(js, node):
    fd, path = tempfile.mkstemp(suffix=".js")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(js)
        r = subprocess.run([node, "--check", path], capture_output=True, text=True)
        print(f"  polyfill: {len(js)} bytes, node --check exit={r.returncode}")
        if r.returncode:
            print(r.stdout)
            print(r.stderr)
        return r.returncode == 0
    finally:
        os.remove(path)


def node_semantics(js, node):
    fd, path = tempfile.mkstemp(suffix=".js")
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
        f.write("global.document={documentElement:null,body:null};\n")
        f.write("try{delete global.fetch;delete global.localStorage;}catch(e){}\n")
        f.write(js + "\n")
        f.write("""
var report={};
try {
  report.console = typeof console;
  report.fetch = typeof fetch;
  report.ls = typeof localStorage;
  localStorage.clear();
  localStorage.setItem('empty', '');
  report.empty = localStorage.getItem('empty') === '';
  report.missing = localStorage.getItem('missing') === null;
  report.proto = localStorage.getItem('toString') === null;
  localStorage.clear();
  localStorage.setItem('k', 'v');
  report.key = localStorage.key(0) === 'k';
  localStorage.clear();
  report.cleared = localStorage.getItem('k') === null;
  fetch('data.txt').then(function(){ report.fetchrej = 0; }, function(e){
    report.fetchrej = String(e).indexOf('XMLHttpRequest') >= 0 ? 1 : 0;
  });
} catch (e) { report.err = String(e); }
var done = false;
function emit(){ if (done) return; done = true; console.log('NODE|' + JSON.stringify(report)); }
fetch; setTimeout(emit, 300);
""")
    r = subprocess.run([node, path], capture_output=True, text=True)
    os.remove(path)
    line = next((l for l in r.stdout.splitlines() if l.startswith("NODE|")), "")
    print("  node semantics:", line)
    if r.returncode != 0:
        print(r.stderr)
    try:
        return __import__("json").loads(line.split("NODE|", 1)[1])
    except Exception:
        return {}


def engine_inline_report(js):
    page_dir = os.path.join(ROOT, "tests", "dpr")
    os.makedirs(page_dir, exist_ok=True)
    page = ('<!DOCTYPE html><html><head><meta charset="utf-8"><title>poly-inline</title>'
            '</head><body><script>' + js + '</script>'
            '<div id="m1" class="a" data-x="1">x</div>'
            '<script>'
            'var r=[];'
            'r.push("console="+typeof console);'
            'r.push("wconsole="+typeof window.console);'
            'r.push("wls="+typeof window.localStorage);'
            'r.push("wfetch="+typeof window.fetch);'
            'r.push("wmatch="+typeof window.matchMedia);'
            'r.push("wiob="+typeof window.IntersectionObserver);'
            'r.push("fetch="+typeof fetch);'
            'r.push("ls="+typeof localStorage);'
            'try{localStorage.clear();localStorage.setItem("empty","");'
            'r.push("empty="+(localStorage.getItem("empty")===""));'
            'r.push("missing="+(localStorage.getItem("missing")===null));'
            'r.push("proto="+(localStorage.getItem("toString")===null));'
            'localStorage.clear();localStorage.setItem("k","v");r.push("key="+(localStorage.key(0)==="k"));'
            'localStorage.clear();r.push("cleared="+(localStorage.getItem("k")===null));}'
            'catch(e){r.push("lsErr="+e);}'
            'try{var m=document.getElementById("m1");'
            'r.push("ma="+m.matches(".a"));'
            'r.push("mb="+m.matches(".b"));'
            'r.push("mtag="+m.matches("div.a"));'
            'r.push("mid="+m.matches("#m1"));'
            'r.push("mattr="+m.matches("[data-x=\\"1\\"]"));}'
            'catch(e){r.push("mErr="+e);}'
            'try{var p=fetch("data.txt");r.push("thenable="+(typeof p.then==="function"));}'
            'catch(e){r.push("fetchThrow="+e);}'
            'document.title="POLYINLINE|"+r.join("|");'
            '</script></body></html>')
    path = os.path.join(page_dir, "polyinline.html")
    io.open(path, "w", encoding="utf-8", newline="\n").write(page)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    class Srv(socketserver.TCPServer):
        allow_reuse_address = True

        def handle_error(self, *a):
            pass
    os.chdir(page_dir)
    httpd = Srv(("127.0.0.1", PORT), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    env["KHTML_TOMBSTONE_DEBUG"] = "1"
    trace = os.path.join(ROOT, "tests", "polyfill_inline.err")
    tracef = open(trace, "wb")
    proc = subprocess.Popen(
        [os.path.join(BUILD, "khtml_browser.exe"), f"http://127.0.0.1:{PORT}/polyinline.html"],
        cwd=BUILD, env=env, stdout=subprocess.DEVNULL, stderr=tracef)
    try:
        hwnd = None
        for _ in range(40):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            print("FAIL: no browser window")
            return {}
        title = ""
        for _ in range(80):
            try:
                tracef.flush()
                with open(trace, "rb") as f:
                    text = f.read().decode("utf-8", "replace")
            except OSError:
                text = ""
            for line in text.splitlines():
                if "[tomb] titleChanged title=POLYINLINE|" in line:
                    title = line.split("titleChanged title=", 1)[1].rsplit(" cur=", 1)[0]
            if title:
                break
            time.sleep(0.25)
        print("  engine report:", title)
        if not title:
            return {}
        fields = {}
        for item in title.split("POLYINLINE|", 1)[1].split("|"):
            if "=" in item:
                k, v = item.split("=", 1)
                fields[k] = v
        return fields
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        httpd.shutdown()
        try:
            tracef.close()
        except Exception:
            pass
        for f2 in (path, trace):
            try:
                os.remove(f2)
            except OSError:
                pass


def main():
    ok = True
    node = find_node()
    print("[0/3] loader wiring")
    src = io.open(PAGELOADER, encoding="utf-8").read()
    n_polish = src.count("html = polishHtmlForEngine(html);")
    has_inject = "injectPolyfillBundle(html)" in src
    print(f"  polish call sites: {n_polish} (want >= 2: initial + final); "
          f"polyfill inside polish: {has_inject}")
    ok = (n_polish >= 2 and has_inject) and ok

    print("[1/3] syntax")
    js = extract_polyfill_js()
    ok = (node is not None and node_syntax(js, node)) and ok

    print("[2/3] node semantics (localStorage + fetch rejection)")
    ns = node_semantics(js, node) if node else {}
    for name, want in (("console", "object"), ("fetch", "function"), ("ls", "object"),
                       ("empty", 1), ("missing", 1), ("proto", 1),
                       ("key", 1), ("cleared", 1), ("fetchrej", 1)):
        got = ns.get(name)
        good = got == want
        print(f"  [{'OK' if good else 'FAIL'}] {name}={got!r} (want {want!r})")
        ok = ok and good

    print("[3/3] engine semantics (sync)")
    ef = engine_inline_report(js)
    def flag(name):
        v = ef.get(name)
        return v in ("1", "true")

    checks = [
        ("console exposed", ef.get("console") in ("object", "function")),
        ("fetch exposed", ef.get("fetch") == "function"),
        ("localStorage exposed", ef.get("ls") == "object"),
        ("getItem preserves empty string", flag("empty")),
        ("missing key -> null", flag("missing")),
        ("prototype keys are not storage keys", flag("proto")),
        ("key(0) returns stored key", flag("key")),
        ("clear() empties storage", flag("cleared")),
        ("matches('.a') true", flag("ma")),
        ("matches('.b') false", ef.get("mb") in ("0", "false")),
        ("matches('div.a') true", flag("mtag")),
        ("matches('#m1') true", flag("mid")),
        ("matches('[data-x=\"1\"]') true", flag("mattr")),
        ("fetch returns a thenable", flag("thenable")),
        ("window.console exposed", ef.get("wconsole") in ("object", "function")),
        ("window.localStorage exposed", ef.get("wls") == "object"),
        ("window.fetch exposed", ef.get("wfetch") == "function"),
        ("window.matchMedia exposed", ef.get("wmatch") == "function"),
        ("window.IntersectionObserver exposed", ef.get("wiob") in ("function", "object")),
    ]
    for name, good in checks:
        print(f"  [{'OK' if good else 'FAIL'}] {name}")
        ok = ok and good
    for k in ("err", "lsErr", "mErr", "fetchThrow", "pageErr"):
        if k in ef:
            print("  page error:", k, "=", ef[k])
            ok = False

    print("POLYFILL PROBE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
