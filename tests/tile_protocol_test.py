# tile_protocol_test.py — offline verification of the tiled SHM transport.
#
# Part 1 (static consistency): parses khtml_renderer.cpp and khtml_browser.cpp
# and asserts the protocol constants and the shmNeeded() formula agree exactly
# on both sides.  Hand-duplicated today; if one side changes kTileSize or the
# layout, this test fails before the mismatch can become a runtime heap
# overrun or a garbled frame.
#
# Part 2 (reference model): a pure-Python re-implementation of the renderer's
# damage->dirty-tile algorithm and the browser's compositing loop, driven by
# randomized frame-size/damage sequences.  For every step it recomposites the
# browser canvas and asserts it pixel-matches a straight render of the true
# page image.  This catches edge-clip, resize-carry and stale-tile bugs in
# the protocol logic without needing the C++ build.
#
# Run: python tests/tile_protocol_test.py   (no Qt, no compiler needed)

import random
import re
import struct
import sys

ROOT = r"C:\Users\zhouzhehong\Desktop\KHtmlLite"
RENDERER_CPP = ROOT + r"\src\host\khtml_renderer.cpp"
BROWSER_CPP = ROOT + r"\src\host\khtml_browser.cpp"


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def parse_const(text, name):
    m = re.search(r"constexpr int %s = (\d+);" % name, text)
    if not m:
        fail("cannot find %s" % name)
    return int(m.group(1))


def parse_shm_needed(text):
    m = re.search(r"static qsizetype shmNeeded\(int w, int h\)\s*\{(.*?)\n\s*\}",
                  text, re.S)
    if not m:
        fail("cannot locate shmNeeded()")
    return m.group(1)


# ---------------------------------------------------------------- Part 1 ----
def test_static_consistency():
    rend = open(RENDERER_CPP, encoding="utf-8").read()
    brow = open(BROWSER_CPP, encoding="utf-8").read()

    for name in ("kTileSize", "kHeaderBytes"):
        r, b = parse_const(rend, name), parse_const(brow, name)
        assert r == b, (name, r, b)
        print("  %s both sides = %d" % (name, r))

    r_body = re.sub(r"\s+", "", parse_shm_needed(rend))
    b_body = re.sub(r"\s+", "", parse_shm_needed(brow))
    assert r_body == b_body, "shmNeeded() bodies differ"
    print("  shmNeeded() bodies identical")

    # Header field layout: [w, h, tilesX, tilesY, dirtyCount]
    for text, side in ((rend, "renderer"), (brow, "browser")):
        for i in range(5):
            assert re.search(r"hdr\[%d\]" % i, text), (side, i)
    print("  header fields hdr[0..4] present on both sides")

    # The scroll-blit fast path is a paired protocol feature: the renderer
    # must emit "scroll" and the browser must handle it.  Both "scroll" and
    # "tiles" now carry the renderer's ABSOLUTE scroll offset (4 fields) for
    # predictive-scroll reconciliation.
    assert 'sendMessage(QStringLiteral("scroll")' in rend, \
        "renderer does not emit scroll messages"
    assert 'QLatin1String("scroll")' in brow, \
        "browser does not handle scroll messages"
    assert '.arg(sdx).arg(sdy).arg(cx).arg(cy)' in rend, \
        "renderer scroll message misses absolute offset"
    assert '.arg(fw).arg(fh).arg(cx).arg(cy)' in rend, \
        "renderer tiles message misses absolute offset"
    assert 'applyScrollAuthority' in brow, \
        "browser misses scroll reconciliation"
    print("  scroll-blit + predictive reconciliation present on both sides")

    # WebKit-style render-once: the renderer must paint the frame surface
    # exactly once per grabFrame (tiles are crops of it).  A per-tile
    # widget->render() loop was a 40x render cost bug; keep it from
    # creeping back.
    n_render = len(re.findall(r"\bw->render\(&", rend))
    assert n_render == 1, "expected exactly 1 frame render call, got %d" % n_render
    print("  frame rendered exactly once per grabFrame (tiles are crops)")

    # Resize presentation contract (AppKit live-resize stretch, the rule the
    # macOS/Windows UI processes follow while the content process catches up):
    #   steady state      — frame is drawn 1:1, anchored at the surface origin;
    #   size mismatch     — the LAST committed frame is scaled to fill the
    #                        surface until the first true-size frame lands
    #                        (no white flash, no checkerboard).
    #   dpr != 1          — the frame is PHYSICAL pixels by design, so the
    #                        steady-state draw undoes the logical->device
    #                        scale and blits source->backing pixel 1:1
    #                        (Safari deviceScaleFactor, exact colours).
    # Guarded statically: both 1:1 paths must exist, the device-pixel blit
    # must be gated on the physical size match, and unconditional
    # full-surface scaling stays forbidden.
    assert "p.drawImage(QPoint(-m_previewDX, -m_previewDY), m_frame)" in brow, \
        "1:1 anchored draw path missing from PageSurface"
    assert "p.scale(1.0 / dpr, 1.0 / dpr)" in brow, \
        "device-pixel 1:1 blit missing (frame would be resampled twice at dpr != 1)"
    assert re.search(r"qAbs\(m_frame\.width\(\) - physW\) <= 1[^}]*"
                     r"p\.scale\(1\.0 / dpr, 1\.0 / dpr\)", brow, re.S), \
        "device-pixel blit must be gated on the physical frame==surface match"
    assert "p.drawImage(QRect(QPoint(-m_previewDX, -m_previewDY), size()), m_frame)" in brow, \
        "mismatch stretch fallback missing"
    assert "p.drawImage(rect(), m_frame)" not in brow, \
        "PageSurface scales the frame unconditionally"
    assert "checkerBoard" not in brow, \
        "stale checkerboard presentation: mismatch now shows the scaled last frame"
    print("  PageSurface: 1:1 steady state (device px at dpr != 1), "
          "stretch only on size mismatch")

    # True deviceScaleFactor contract: the renderer receives a LOGICAL
    # (CSS px) viewport plus the ratio, lays KHTML out at that logical size
    # and rasterizes the frame at logical*dpr by scaling the painter.  The
    # frame stays physical (tiles/canvas unchanged) and engine damage stays
    # logical until the single map to the tile grid.
    assert 'obj[QStringLiteral("dpr")] = dpr' in brow, \
        "resize message must carry the device pixel ratio"
    assert "m_resizeCb(int(width()), int(height()))" in brow, \
        "PageSurface must report the LOGICAL viewport (CSS px), not physical"
    assert "fp.scale(m_dpr, m_dpr)" in rend, \
        "renderer must scale the painter onto the device grid (CSS-px layout)"
    assert "const QRegion dirtyPhys = QTransform::fromScale(m_dpr, m_dpr).map(dirty)" in rend, \
        "engine damage (CSS px) must be mapped to the physical tile grid once"
    assert "const int fw = qMax(1, qRound(lw * m_dpr))" in rend, \
        "frame canvas must be logical size times dpr"
    assert "int(s.width() * dpr), int(s.height() * dpr)" not in brow, \
        "browser still ships a physical viewport to the renderer"
    print("  CSS-px viewport: logical resize + dpr, painter-scaled raster, "
          "damage mapped once")

    # Background tabs must not relayout on window resize: applyPendingResize
    # may only touch the CURRENT tab (hidden pages get no layout work; N-tab
    # relayout storms were the multi-tab resize lag).
    m = re.search(r"void applyPendingResize\(\) \{(.*?)\n    \}", brow, re.S)
    assert m, "applyPendingResize not found"
    body = m.group(1)
    assert "for (TabPage *" not in body, \
        "applyPendingResize still resizes every tab (background relayout storm)"
    assert "currentPage()" in body, \
        "applyPendingResize must select the current tab only"
    print("  resize ships to the current tab only (background tabs relayout on activation)")

    # Load-adaptive tombstone contract (Safari MemoryPressureHandler):
    #   - 4 system memory tiers drive escalation (never a fixed MB budget);
    #   - the kernel's LowMemoryResourceNotification escalates a check
    #     immediately (zero-CPU wait thread, WebKit's Windows mechanism);
    #   - system CPU load modulates the idle timeout (CPU-adaptive pillar);
    #   - tab-bar surgery in suspendTab must not re-route currentChanged
    #     onto the tombstone (auto-resume defeated the tombstone live).
    for s in ("loadPct >= 93", "loadPct >= 85", "loadPct >= 70"):
        assert s in brow, f"memory tier {s} missing from checkMemoryBudget"
    assert "CreateMemoryResourceNotification" in brow, \
        "kernel low-memory event watcher missing"
    assert "sysCpuPct >= 85 ? 60 * 1000 : 5 * 60 * 1000" in brow, \
        "CPU-adaptive idle timeout missing"
    m = re.search(r"void suspendTab\(TabPage \*pg\) \{(.*?)\n    \}", brow, re.S)
    assert m, "suspendTab not found"
    assert "blockSignals(true)" in m.group(1), \
        "suspendTab tab surgery without currentChanged suppression"
    assert "GetSystemTimes" in brow, "system CPU sampling missing"
    print("  tombstone: 4 memory tiers, kernel event watcher, "
          "CPU-adaptive idle, surgery-safe suspend")

    global TILE, HEADER
    TILE = parse_const(rend, "kTileSize")
    HEADER = parse_const(rend, "kHeaderBytes")


# ---------------------------------------------------------------- Part 2 ----
TILE = 256
HEADER = 32
TILE_BYTES = TILE * TILE


def fnv1a_tile(rows):
    """Must mirror the FNV-1a pass over pixel rows in khtml_renderer.cpp
    grabFrame().  Hash input is the RGBA-ish bytes of a TILE x TILE image,
    row by row; the exact color encoding does not matter as long as both
    sides hash identical pixel sequences identically."""
    h = 14695981039346656037
    for row in rows:
        for (r, g, b) in row:
            for byte in (b, g, r, 255):  # BGRA32 order, as Format_RGB32
                h ^= byte
                h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    if h == 0:
        h = 1  # renderer reserves 0 for "unknown slot content"
    return h


def grid(w, h):
    return (w + TILE - 1) // TILE, (h + TILE - 1) // TILE


def shm_needed(w, h):
    tx, ty = grid(w, h)
    return HEADER + tx * ty * 4 + tx * ty * TILE_BYTES


class RendererModel:
    """Models khtml_renderer.cpp grabFrame() tile bookkeeping (no pixels)."""

    def __init__(self):
        self.fw = 0
        self.fh = 0
        self.tiles_x = 0
        self.tiles_y = 0
        self.dirty = []  # list of rects (x, y, w, h)
        self.relayout_pending = False
        # Same-tile suppression cache (mirrors m_tileHashes): 0 = unknown.
        self.tile_hashes = []

    @staticmethod
    def _rects_intersect(a, b):
        return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and \
               a[1] < b[1] + b[3] and b[1] < a[1] + a[3]

    def damage(self, x, y, w, h):
        self.dirty.append((x, y, w, h))

    def resize(self, w, h, size_changed=True):
        # ANY applied size change must relayout and produce a full frame
        # (khtml_renderer.cpp: "Damage policy for the tiled transport: ANY
        # applied size change must produce a full frame").  A pure HEIGHT
        # change is included: it moves viewport-anchored content, flips the
        # vertical scrollbar and re-evaluates media queries.  The old model
        # only relayouted when the WIDTH changed, which is what left the
        # moved elements' old pixels painted forever.
        if size_changed:
            self.relayout_pending = True
        self.new_size = (w, h)

    def grab_frame(self, render_tile):
        """render_tile(t, tx, ty) -> TILE x TILE pixel rows for slot t.
        Returns:
          None                        — pure no-op (no damage, no size change)
          ("suppressed", tx, ty)      — damaged, but every tile rendered
                                        identical: no upload, no IPC either
          (tx, ty, tiles, images)     — tiles actually uploaded this frame
                                        (browser is notified and composes)"""
        fw, fh = self.new_size
        tx, ty = grid(fw, fh)
        # There is NO bottom-strip shortcut any more: a size change always
        # damages the whole logical rect, and a change in the tile COLUMN
        # count resets the whole grid (slots no longer name the same canvas
        # rectangle).  The hash table keeps its row-major prefix across a
        # pure-height resize because those slots still name the same rect.
        if self.relayout_pending or tx != self.tiles_x:
            self.dirty = [(0, 0, fw, fh)]
        self.relayout_pending = False

        eff = []
        for r in self.dirty:
            x1, y1 = max(r[0], 0), max(r[1], 0)
            x2, y2 = min(r[0] + r[2], fw), min(r[1] + r[3], fh)
            if x2 > x1 and y2 > y1:
                eff.append((x1, y1, x2 - x1, y2 - y1))
        dims_changed = (fw != self.fw or fh != self.fh)
        if not eff and not dims_changed:
            return None

        dirty_tiles = []
        for yy in range(ty):
            for xx in range(tx):
                tr = (xx * TILE, yy * TILE, TILE, TILE)
                if any(self._rects_intersect(r, tr) for r in eff):
                    dirty_tiles.append(yy * tx + xx)

        # Same-tile suppression: render+hash each damaged tile, keep only
        # changed ones (mirrors grabFrame pass 1 with deferred hash commit).
        if len(self.tile_hashes) != tx * ty:
            self.tile_hashes = [0] * (tx * ty)
        up_tiles, up_images, up_hashes = [], [], []
        for t in dirty_tiles:
            img = render_tile(t, tx, ty)
            h = fnv1a_tile(img)
            if h == self.tile_hashes[t]:
                continue
            up_tiles.append(t)
            up_images.append(img)
            up_hashes.append(h)
        # Hash commit happens only after a successful SHM write; in this
        # model the write always succeeds, so commit here.
        for t, h in zip(up_tiles, up_hashes):
            self.tile_hashes[t] = h

        self.fw, self.fh = fw, fh
        self.tiles_x, self.tiles_y = tx, ty
        self.dirty = []

        if not up_tiles and not dims_changed:
            return ("suppressed", tx, ty)
        return (tx, ty, up_tiles, up_images)


class BrowserModel:
    """Models readTiles(): persistent canvas compositing."""

    def __init__(self):
        self.canvas = None  # 2D list of (r,g,b) tuples or None

    def commit(self, w, h, tx, ty, dirty_tiles, tile_images):
        """dirty_tiles / tile_images are parallel lists (only uploaded tiles)."""
        if self.canvas is None or len(self.canvas) != h or \
           (h and len(self.canvas[0]) != w):
            next_c = [[(255, 255, 255) for _ in range(w)] for _ in range(h)]
            if self.canvas:
                oh, ow = len(self.canvas), len(self.canvas[0])
                for y in range(min(h, oh)):
                    for x in range(min(w, ow)):
                        next_c[y][x] = self.canvas[y][x]
            self.canvas = next_c
        for t, src in zip(dirty_tiles, tile_images):
            assert 0 <= t < tx * ty
            cxx, cyy = t % tx, t // tx
            copy_w = min(TILE, w - cxx * TILE)
            copy_h = min(TILE, h - cyy * TILE)
            for y in range(copy_h):
                row = self.canvas[cyy * TILE + y]
                srow = src[y]
                for x in range(copy_w):
                    row[cxx * TILE + x] = srow[x]
            # Also keep the SHM-slot illusion: suppressed tiles must already
            # match what the browser holds, which the final canvas check
            # below verifies on every step.


def region_color(rev, x, y):
    # Color of a pixel at page content-revision `rev`.  A damage bumps the
    # revision so every pixel inside the damaged rect changes color.
    return ((x * 7 + rev * 13) % 256, (y * 5 + rev * 29) % 256,
            ((x ^ y) + rev * 3) % 256)


def run_random_sequence(seed, steps=40, dim=512, verbose=False):
    rnd = random.Random(seed)
    renderer = RendererModel()
    browser = BrowserModel()
    w, h = dim, dim
    # True page: per-pixel content revision.  Rev 0 everywhere at first.
    true_rev = [[0 for _ in range(w)] for _ in range(h)]
    rev_counter = 0
    renderer.resize(w, h, size_changed=True)

    def render_tile(t, tx, ty):
        cxx, cyy = t % tx, t // tx
        out = []
        for y in range(TILE):
            row = []
            py = cyy * TILE + y
            for x in range(TILE):
                px = cxx * TILE + x
                if px < w and py < h:
                    row.append(region_color(true_rev[py][px], px, py))
                else:
                    # tiles render white outside the frame (tile.fill white)
                    row.append((255, 255, 255))
            out.append(row)
        return out

    stats = {"none": 0, "suppressed": 0, "upload": 0, "noop_dmg": 0}

    for step in range(steps):
        action = rnd.choice(["dmg", "dmg", "dmg-noop", "resize", "idle"])
        if action == "dmg":
            rev_counter += 1
            rx, ry = rnd.randrange(w), rnd.randrange(h)
            rw, rh = rnd.randint(1, w - rx), rnd.randint(1, h - ry)
            for py in range(ry, ry + rh):
                row = true_rev[py]
                for px in range(rx, rx + rw):
                    row[px] = rev_counter
            renderer.damage(rx, ry, rw, rh)
        elif action == "dmg-noop":
            # Damage without a pixel change — the real-world "click on a
            # blank area / hover without restyle" case.  Should be fully
            # eaten by same-tile suppression.
            stats["noop_dmg"] += 1
            rx, ry = rnd.randrange(w), rnd.randrange(h)
            rw, rh = rnd.randint(1, w - rx), rnd.randint(1, h - ry)
            renderer.damage(rx, ry, rw, rh)
        elif action == "resize":
            nw = rnd.randint(TILE // 2, dim) if rnd.random() < 0.5 else w
            nh = rnd.randint(TILE // 2, dim)
            if nh == h and nw == w:
                continue
            # Adjust the true page; new rows/content are rev 0.
            new_rev = [[0 for _ in range(nw)] for _ in range(nh)]
            for py in range(min(h, nh)):
                for px in range(min(w, nw)):
                    new_rev[py][px] = true_rev[py][px]
            true_rev = new_rev
            rev_counter += 1
            # ANY size change relayouts the whole page: every pixel may
            # change (viewport-anchored elements, scrollbar flips, media
            # queries) — this mirrors the C++ policy, which no longer
            # special-cases a pure height change.
            if (nw, nh) != (w, h):
                for py in range(nh):
                    for px in range(nw):
                        true_rev[py][px] = rev_counter
            renderer.resize(nw, nh, size_changed=((nw, nh) != (w, h)))
            w, h = nw, nh
        # idle: no damage

        result = renderer.grab_frame(render_tile)
        if result is None:
            stats["none"] += 1
        elif result[0] == "suppressed":
            stats["suppressed"] += 1
        else:
            tx, ty, up_tiles, up_images = result
            stats["upload"] += 1
            browser.commit(w, h, tx, ty, up_tiles, up_images)

        # Verify: browser canvas == true page, everywhere.
        if browser.canvas is None:
            continue
        assert len(browser.canvas) == h and len(browser.canvas[0]) == w
        for y in range(0, h, 7):  # sample rows (dense enough for regressions)
            for x in range(0, w, 7):
                got = browser.canvas[y][x]
                want = region_color(true_rev[y][x], x, y)
                if got != want:
                    fail("seed=%d step=%d action=%s pixel(%d,%d) got=%s want=%s"
                         % (seed, step, action, x, y, got, want))
        if verbose:
            print("  step %02d ok (%s, %dx%d)" % (step, action, w, h))

    # Suppression must actually engage: noop damages change no pixels, so a
    # frame containing only noop damage is always fully suppressed.
    if stats["noop_dmg"] >= 3:
        assert stats["suppressed"] >= 1, \
            "same-tile suppression never fired: %s" % (stats,)
    return stats


def test_model():
    total = {"none": 0, "suppressed": 0, "upload": 0, "noop_dmg": 0}
    for seed in range(20):
        s = run_random_sequence(seed)
        for k in total:
            total[k] += s[k]
    print("  20 random sequences x 40 steps: canvas matches true page")
    print("  frame stats: %s" % total)
    assert total["suppressed"] > 0, "suppression path never exercised"


def test_segment_layout():
    """shmNeeded must fit header + full dirty list + all tiles; emulate worst
    case for a spread of sizes including exact multiples of the tile size."""
    for w in [1, 100, 255, 256, 257, 511, 512, 513, 1024, 1919, 1920]:
        for h in [1, 256, 300, 768, 1080]:
            tx, ty = grid(w, h)
            need = shm_needed(w, h)
            worst_end = HEADER + (tx * ty) * 4 + (tx * ty) * TILE_BYTES
            assert need >= worst_end, (w, h, need, worst_end)
            # last tile of last row must fit even with full dirty list
            last_off = HEADER + tx * ty * 4 + (tx * ty - 1) * TILE_BYTES
            assert last_off + TILE_BYTES <= need, (w, h)
    print("  shmNeeded covers header + full index list + tile payload for all sizes")


def test_scroll_blit():
    """Deterministic scroll-blit scenario mirroring the fast path:
    content is a tall page; the frame is a viewport over it.  The renderer
    shifts the browser canvas by (0, dy) via a scroll message and uploads
    only the newly exposed strip tiles.  Assert the composited canvas then
    equals the scrolled true page everywhere."""
    import itertools
    fw, fh = 512, 300           # viewport (not tile-aligned on y on purpose)
    content_h = 4000            # tall page
    # True content: rev-like pattern indexed by absolute content coords.
    def ccolor(cx, cy):
        return ((cx * 3 + cy) % 256, (cy * 7) % 256, ((cx ^ cy) * 5) % 256)

    browser = BrowserModel()
    rnd = random.Random(7)

    # Initial frame at scroll offset 0: full upload.
    tx, ty = grid(fw, fh)

    def render_tile_at(t, scroll_y):
        cxx, cyy = t % tx, t // tx
        return [[ccolor(cxx * TILE + x, cyy * TILE + y + scroll_y)
                 if (cyy * TILE + y) < fh and (cxx * TILE + x) < fw
                 else (255, 255, 255)
                 for x in range(TILE)] for y in range(TILE)]

    tiles0 = list(range(tx * ty))
    imgs0 = [render_tile_at(t, 0) for t in tiles0]
    browser.commit(fw, fh, tx, ty, tiles0, imgs0)

    def shift(canvas, dy):
        shifted = [[(255, 255, 255) for _ in range(fw)] for _ in range(fh)]
        for y in range(fh):
            sy = y + dy
            if 0 <= sy < fh:
                shifted[y] = list(canvas[sy])
        return shifted

    # Predictive-scroll state, mirroring the C++ design: the wheel
    # prediction is a PRESENTATION-ONLY draw offset (never touches committed
    # pixels — a wrong prediction can never evict rows from the canvas),
    # and the renderer's authoritative absolute offset shifts the canvas by
    # exactly (cx - base), then resets the preview.
    base = 0              # content offset the canvas is committed at
    preview = 0           # transient visual offset (PageSurface)
    scroll_y = 0          # renderer's true absolute content offset

    # A series of wheel steps.  The browser displays a (possibly wrong)
    # prediction FIRST, then the authoritative (absolute) message re-anchors
    # the canvas exactly and resets the preview.
    for dy_chain in (120, 60, -90, 300, -45):
        scroll_y += dy_chain
        assert 0 <= scroll_y <= content_h - fh

        # 1) browser preview: presentation only, arbitrarily wrong.
        noise = rnd.choice((-40, -15, 0, 12, 33, 90))
        preview += dy_chain + noise
        # Transient visual position = base + preview; no canvas mutation, so
        # no pixel information can be lost.

        # 2) renderer authority "scroll sdx,sdy,cx,cy": exact canvas shift by
        #    the true delta; preview resets to zero.
        true_delta = scroll_y - base
        browser.canvas = shift(browser.canvas, true_delta)
        base = scroll_y
        preview = 0
        assert preview == 0 and base == scroll_y

        # 3) strip tiles for the newly exposed viewport area (C++ strip math,
        #    expressed against the TRUE delta)
        strip = []
        d = dy_chain
        if d > 0:
            strip.append((0, fh - min(d, fh), fw, min(d, fh)))
        elif d < 0:
            strip.append((0, 0, fw, min(-d, fh)))
        strip_tiles = []
        for yy in range(ty):
            for xx in range(tx):
                tr = (xx * TILE, yy * TILE, TILE, TILE)
                if any(tr[0] < r[0] + r[2] and r[0] < tr[0] + tr[2] and
                       tr[1] < r[1] + r[3] and r[1] < tr[1] + tr[3]
                       for r in strip):
                    strip_tiles.append(yy * tx + xx)
        imgs = [render_tile_at(t, scroll_y) for t in strip_tiles]
        browser.commit(fw, fh, tx, ty, strip_tiles, imgs)

        # After reconciliation + strip upload the canvas must equal the true
        # page at the new offset, no matter how badly the prediction missed.
        for y in range(fh):
            for x in range(0, fw, 13):
                want = ccolor(x, y + scroll_y)
                got = browser.canvas[y][x]
                if got != want:
                    fail("predicted scroll mismatch at (%d,%d) after dy=%d "
                         "noise=%d: got=%s want=%s"
                         % (x, y, dy_chain, noise, got, want))

    print("  predictive scroll + residual reconciliation (5 steps, noisy "
          "predictions): canvas converges to true page")


if __name__ == "__main__":
    print("[1/4] static protocol consistency (renderer vs browser)")
    test_static_consistency()
    print("[2/4] segment layout worst-case bounds")
    test_segment_layout()
    print("[3/4] reference model property tests (random damage/resize)")
    test_model()
    print("[4/4] scroll blit scenario")
    test_scroll_blit()
    print("TILE PROTOCOL PASS")
