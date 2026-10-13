# startup_timing_report.py — render a startup timeline from khtml_startup.log.
#
# Usage:
#   1.  set KHTML_STARTUP_TIMING=1
#   2.  run khtml_browser.exe once (open a page, wait until content shows, quit)
#   3.  python tests\startup_timing_report.py [path-to-build-host]
#
# The log lives at <build>\khtml_startup.log (both processes append to it).
# Time is ms since that process started; the renderer is spawned by the
# browser, so its clock begins later — the report merges both clocks by
# anchoring the renderer's "browser connected" to the browser's
# "socket connected" mark.

import os
import sys

DEFAULT_BUILD = r"C:\Users\zhouzhehong\Desktop\KHtmlLite\build-host"


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 3 and parts[0].isdigit():
                rows.append((int(parts[0]), parts[1], parts[2]))
    return rows


def skew_offset(rows):
    """Exact inter-process clock skew from the ready handshake: the renderer
    sends its local elapsed ms; the browser logs a 'clock skew ms=N' mark.
    N = renderer-local-ms→browser-clock offset for all renderer rows."""
    for ms, proc, lbl in rows:
        if proc.startswith("KHtmlLite") and lbl.startswith("clock skew ms="):
            try:
                # label carries N = browser_elapsed − renderer_elapsed;
                # adding N to renderer rows approximates browser-clock times
                # (true offset ≤ N by the one-way delivery delay).
                return int(lbl.split("=", 1)[1])
            except ValueError:
                pass
    return None


def anchor_offset(rows):
    """Compute renderer-clock offset so its timeline lines up with the
    browser clock: the browser-side 'socket connected' corresponds to the
    renderer-side 'browser connected'."""
    b = next((ms for ms, proc, lbl in rows
              if proc.startswith("KHtmlLite") and lbl == "socket connected"),
             None)
    r = next((ms for ms, proc, lbl in rows
              if proc.startswith("KHtmlLiteRenderer") and lbl == "browser connected"),
             None)
    if b is None or r is None:
        return 0
    return b - r


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BUILD
    log = os.path.join(build, "khtml_startup.log")
    if not os.path.exists(log):
        print(f"no log at {log} — run with KHTML_STARTUP_TIMING=1 first")
        return 1

    rows = load(log)
    if not rows:
        print("log is empty")
        return 1

    off = skew_offset(rows)
    if off is None:
        off = anchor_offset(rows)
        print("(renderer clock aligned via socket-connect anchor; no "
              "ready-payload skew mark found)")
    events = []
    for ms, proc, label in rows:
        is_rend = proc.startswith("KHtmlLiteRenderer")
        abs_ms = ms + (off if is_rend else 0)
        events.append((abs_ms, "rend " if is_rend else "brow ", label, ms))
    events.sort()

    print("Merged startup timeline (ms, browsers clock):")
    print("=" * 74)
    prev = None
    for abs_ms, side, label, raw in events:
        delta = f"+{abs_ms - prev:>5}" if prev is not None else "     0"
        prev = abs_ms
        print(f"{abs_ms:>7} {delta}  {side} {label}")
    print("=" * 74)

    # Highlight the biggest gaps.
    prev = None
    gaps = []
    for abs_ms, side, label, _ in events:
        if prev is not None:
            gaps.append((abs_ms - prev[0], prev[1] + " -> " + label))
        prev = (abs_ms, label)
    gaps.sort(reverse=True)
    print("\nTop gaps:")
    for gap, desc in gaps[:3]:
        print(f"  {gap:>6} ms  {desc}")

    first_frame = next((abs_ms for abs_ms, _, lbl, _ in events
                        if lbl == "first frame composited"), None)
    if first_frame is not None:
        print(f"\ntime-to-first-frame: {first_frame} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
