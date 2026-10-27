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
# randomized frame-size/damage sequences.  The renderer does NOT render whole
# tiles: it composes ONE frame per grabFrame() and crops tiles out of it, and a
# PARTIAL frame starts from the PREVIOUS frame's pixels (only the damage rects
# are repainted, at their true coordinates) rather than from white.  The model
# reproduces that composition, then for every step recomposites the browser
# canvas and asserts it pixel-matches a straight render of the true page image.
# This catches edge-clip, resize-carry, stale-tile AND "partial frame must not
# start from white" bugs in the protocol logic without needing the C++ build.
# A negative control re-runs the same sequences with the OLD white-based
# partial composition and asserts it DOES produce a wrong canvas, proving the
# property test above is discriminating rather than self-fulfilling.
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

    # The scroll fast path must reset the tile-hash suppression cache in the
    # same step that blit-shifts the browser canvas: the blit displaces every
    # slot's pixels, so a stale hash would suppress the re-upload of any
    # scroll-invariant (position:fixed) tile forever — the fixed-header ghost
    # found by the 2026-10-23 five-agent audit.  Part 5's negative control
    # covers the same property behaviourally in the reference model.
    assert re.search(
        r"=== Scroll fast path[\s\S]{0,6000}?m_tileHashes\.fill\(0\);", rend), \
        "scroll fast path does not reset the tile-hash cache after the blit"
    print("  scroll blit resets the tile-hash suppression cache")

    # WebKit-style render-once: the renderer must paint the frame surface
    # exactly once per grabFrame (tiles are crops of it).  A per-tile
    # widget->render() loop was a 40x render cost bug; keep it from
    # creeping back.
    n_render = len(re.findall(r"\bw->render\(&", rend))
    assert n_render == 1, "expected exactly 1 frame render call, got %d" % n_render
    print("  frame rendered exactly once per grabFrame (tiles are crops)")

    # Partial-frame composition contract (the 2026-10-21/22 ghost fix).  A
    # PARTIAL frame must start from the PREVIOUS composed frame and then repaint
    # only the damage rects at their true coordinates.  Starting every partial
    # frame from white re-uploads white into the un-damaged remainder of a
    # damaged 256x256 tile (tiles are CROPS of the frame), erasing correct
    # browser canvas content — the erasure half of the user-visible ghost bug.
    # Guarded statically here; Part 2's negative control covers the same
    # property behaviourally in the reference model.
    assert "frameImage = m_lastFrame" in rend, \
        "partial frames no longer start from the previous composed frame"
    assert re.search(r"m_lastFrame\.size\(\)\s*!=\s*frameImage\.size\(\)", rend), \
        "full-frame decision ignores the previous frame size"
    print("  partial frames compose onto the previous frame (not white)")

    # The frame base must be dropped on every path that swaps in a NEW document
    # (link click, navigate command, reload), or the new document composites
    # onto the old one's pixels...
    n_drop = rend.count("dropFrameBase();")
    assert n_drop >= 3, "expected >=3 dropFrameBase() sites, got %d" % n_drop
    assert re.search(r"dropFrameBase\(\);\s*\n\s*m_view->begin\(localFile\)", rend), \
        "link-click document swap does not drop the frame base first"
    # ...but deliberately KEPT on the same-document 'refreshed' re-feed (same
    # URL/layout, scroll+zoom preserved): dropping it there would restart the
    # next partial frame from white, i.e. the erasure bug again.
    ref = rend.split("PageLoader::refreshed", 1)
    assert len(ref) == 2, "PageLoader::refreshed handler not found"
    assert "dropFrameBase()" not in ref[1][:900], \
        "refreshed re-feed must not drop the frame base"
    print("  frame base dropped on new-document paths, kept on refreshed")

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
    # Presentation contract (DWM composition, kernel adapter):
    #   steady state / resize preview / scroll preview are composed in PHYSICAL
    #   pixels at the surface size and handed to khtml::KHTMLCompositor, whose
    #   D3D11 (hardware -> WARP) device presents a DXGI composition swap chain
    #   and DWM composites it.  There is deliberately no QPainter/GDI fallback.
    assert "khtml::KHTMLCompositor *m_dwm" in brow, \
        "PageSurface must present through the kernel KHTMLCompositor"
    assert "m_dwm->present(src.constBits(), src.width(), src.height()," in brow, \
        "PageSurface must hand the composed frame to the DWM canvas"
    assert "qAbs(m_frame.width() - pw) <= 1" in brow and \
        "qAbs(m_frame.height() - ph) <= 1" in brow, \
        "steady-state DWM present must be gated on physical frame==surface"
    assert "composed = QImage(pw, ph, QImage::Format_RGB32);" in brow, \
        "resize/scroll preview must compose at physical surface size"
    assert "cp.drawImage(QPoint(qRound(m_previewDX * dpr)," in brow, \
        "scroll preview offset must be applied in physical pixels, signed like applyScrollAuthority()"
    assert "m_forceGdi" not in brow, \
        "QPainter/GDI fallback must not exist (policy: D3D11 -> WARP only)"
    print("  PageSurface: physical-size composition -> kernel KHTMLCompositor "
          "(DWM), no GDI fallback")

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
    assert ("blockSignals(true)" in m.group(1)
            or "blockPageTabSignals(true)" in m.group(1)), \
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
    """Models khtml_renderer.cpp grabFrame() frame composition + tile bookkeeping.

    The renderer does NOT render whole tiles: it composes ONE frame and crops
    tiles out of it ("Pass 1: render the frame ONCE into a surface, then treat
    tiles as CROPS of that surface").  Rules modelled here (dpr == 1, so
    logical CSS px == framebuffer px):

      1. damage (m_dirtyRegion) is LOGICAL CSS px; fw = round(lw*dpr).
      2. fullFrame = (dirty == QRegion(logicalRect))
                     || m_lastFrame.size() != frameImage.size()
                     || dimsChanged.  A FULL frame starts from WHITE; a
         PARTIAL frame starts from the PREVIOUS frame's pixels
         (`frameImage = m_lastFrame`).
      3. A relayout frame (m_relayoutPending) or a change in the tile COLUMN
         count sets dirty = the whole logical rect (=> full frame).
      4. Exactly ONE paint: w->render(&fp, src.topLeft(), QRegion(dirty)) with
         src = dirty.boundingRect().  QWidget::render cancels the source
         region's own translation, so damaged content lands at its TRUE
         coordinates: this model repaints ONLY the pixels inside the effective
         damage rects, with the true page pixels (no offset/anchor error).
      5. Tiles are CROPS of that composed frame; a tile is uploaded only when
         its FNV-1a hash differs from the previous frame's tile in that slot.
      6. The composed frame becomes the base for the next grabFrame().

    buggy_compose=True reproduces the OLD composition and is used ONLY by the
    negative control: the frame starts from WHITE on EVERY partial frame and
    only the damage rects are painted.  Since a tile is a 256x256 CROP, its
    un-damaged remainder is cropped as white and the upload erases correct
    browser canvas content (the real user-visible bug).  Never the default.
    """

    def __init__(self, buggy_compose=False):
        self.fw = 0
        self.fh = 0
        self.new_size = (0, 0)
        self.tiles_x = 0
        self.tiles_y = 0
        self.dirty = []  # list of rects (x, y, w, h), LOGICAL CSS px
        self.relayout_pending = False
        # Same-tile suppression cache (mirrors m_tileHashes): 0 = unknown.
        self.tile_hashes = []
        # m_lastFrame: the previously composed physical frame (rows of
        # pixels), or None while there is no valid base (null QImage).
        self.last_frame = None
        self.buggy_compose = buggy_compose

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

    @staticmethod
    def _crop_tile(frame, t, fw, fh, tiles_x):
        """Crop tile slot t out of the composed frame, mirroring the C++
        upload loop: rows outside the frame hash as white, and the region to
        the right of the frame within a row is white-padded to kTileSize."""
        cxx, cyy = t % tiles_x, t // tiles_x
        tile_x, tile_y = cxx * TILE, cyy * TILE
        img = []
        for y in range(TILE):
            fy = tile_y + y
            if 0 <= fy < fh:
                xs = min(max(tile_x, 0), fw)           # qBound(0, tileX, fw)
                xe = min(max(xs, tile_x + TILE), fw)   # qBound(xs, .., fw)
                row = frame[fy][xs:xe] + [(255, 255, 255)] * (TILE - (xe - xs))
            else:
                row = [(255, 255, 255)] * TILE  # whole row outside the frame
            img.append(row)
        return img

    def grab_frame(self, page_pixel):
        """page_pixel(x, y) -> (r, g, b) TRUE page pixel at frame coords.

        Returns:
          None                        — pure no-op (no damage, no size change)
          ("suppressed", tx, ty)      — damaged, but every dirty tile composed
                                        identical to the last upload: no
                                        upload, no IPC either
          (tx, ty, tiles, images)     — tiles actually uploaded this frame
                                        (browser is notified and composes)"""
        fw, fh = self.new_size
        tx, ty = grid(fw, fh)
        # Rule 3: a relayout frame, or a change in the tile COLUMN count
        # (slot indices no longer name the same canvas rect), replaces the
        # damage with the whole logical rect, which makes the frame full.
        if self.relayout_pending or tx != self.tiles_x:
            self.dirty = [(0, 0, fw, fh)]
        self.relayout_pending = False

        # Effective damage = m_dirtyRegion.intersected(logicalRect).
        eff = []
        for r in self.dirty:
            x1, y1 = max(r[0], 0), max(r[1], 0)
            x2, y2 = min(r[0] + r[2], fw), min(r[1] + r[3], fh)
            if x2 > x1 and y2 > y1:
                eff.append((x1, y1, x2 - x1, y2 - y1))
        dims_changed = (fw != self.fw or fh != self.fh)
        if not eff and not dims_changed:
            return None  # nothing changed; m_lastFrame is NOT touched

        # Rule 2: full frame iff the dirty region IS the whole logical rect,
        # there is no previous frame, or the frame dimensions changed.
        dirty_is_full = len(eff) == 1 and eff[0] == (0, 0, fw, fh)
        full_frame = dirty_is_full or self.last_frame is None \
            or self.fw != fw or self.fh != fh

        # Compose the frame the engine paints into.  A PARTIAL frame keeps the
        # previous frame's pixels outside the damage (`frameImage =
        # m_lastFrame`, rule 2); the buggy control starts white there instead.
        if not full_frame and not self.buggy_compose:
            frame = [row[:] for row in self.last_frame]      # = m_lastFrame
        else:
            frame = [[(255, 255, 255)] * fw for _ in range(fh)]  # fill white
        # Rule 4: ONE paint in TRUE coordinates, only inside the damage.
        for (x, y, rw, rh) in eff:
            for py in range(y, y + rh):
                row = frame[py]
                for px in range(x, x + rw):
                    row[px] = page_pixel(px, py)

        # Dirty tiles are hit-tested against the physical damage, row-major.
        dirty_tiles = []
        for yy in range(ty):
            for xx in range(tx):
                tr = (xx * TILE, yy * TILE, TILE, TILE)
                if any(self._rects_intersect(r, tr) for r in eff):
                    dirty_tiles.append(yy * tx + xx)

        # Same-tile suppression cache.  Column count changed => every slot is
        # renamed, reset all; same columns, different rows => keep the prefix
        # the way QVector::resize does (surviving slots keep their hash, the
        # new tail starts at the 0 sentinel).
        if self.tiles_x != tx:
            self.tile_hashes = [0] * (tx * ty)
        elif len(self.tile_hashes) != tx * ty:
            self.tile_hashes = (self.tile_hashes[:tx * ty]
                                + [0] * max(0, tx * ty - len(self.tile_hashes)))

        # Rule 5: crop+hash each dirty tile, keep only changed ones (mirrors
        # grabFrame pass 1 with deferred hash commit).
        up_tiles, up_images, up_hashes = [], [], []
        for t in dirty_tiles:
            img = self._crop_tile(frame, t, fw, fh, tx)
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

        # Rule 6: this composition is the base for the NEXT partial frame.
        self.last_frame = frame
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


def _fmt_mismatch(m):
    x, y = m["pixel"]
    return ("seed=%d step=%d action=%s pixel(%d,%d) got=%s want=%s"
            % (m["seed"], m["step"], m["action"], x, y, m["got"], m["want"]))


def run_random_sequence(seed, steps=40, dim=512, verbose=False,
                        buggy_compose=False, sample_step=7):
    """Drive one randomized damage/resize sequence through the reference models
    and verify the browser canvas after every committed step.

    buggy_compose=True selects the OLD white-based partial composition and is
    used ONLY by the negative control.  Returns (stats, first_mismatch), where
    first_mismatch is None on success or a dict with seed/step/action/pixel/
    got/want describing the FIRST canvas-vs-true-page disagreement."""
    rnd = random.Random(seed)
    renderer = RendererModel(buggy_compose=buggy_compose)
    browser = BrowserModel()
    w, h = dim, dim
    # True page: per-pixel content revision.  Rev 0 everywhere at first.
    true_rev = [[0 for _ in range(w)] for _ in range(h)]
    rev_counter = 0
    renderer.resize(w, h, size_changed=True)

    def page_pixel(px, py):
        # TRUE page pixel: what a full render would put at (px, py).  The
        # composition's damage paint writes exactly these, so a correct
        # partial frame ends up equal to a straight render of the page.
        return region_color(true_rev[py][px], px, py)

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

        result = renderer.grab_frame(page_pixel)
        if result is None:
            stats["none"] += 1
        elif result[0] == "suppressed":
            stats["suppressed"] += 1
        else:
            tx, ty, up_tiles, up_images = result
            stats["upload"] += 1
            browser.commit(w, h, tx, ty, up_tiles, up_images)

        # Verify: browser canvas == true page, everywhere (sampled).  Return
        # the FIRST disagreement so the negative control can report it.
        if browser.canvas is None:
            continue
        assert len(browser.canvas) == h and len(browser.canvas[0]) == w
        mismatch = None
        for y in range(0, h, sample_step):  # dense enough for regressions
            for x in range(0, w, sample_step):
                got = browser.canvas[y][x]
                want = page_pixel(x, y)
                if got != want:
                    mismatch = {"seed": seed, "step": step, "action": action,
                                "pixel": (x, y), "got": got, "want": want}
                    break
            if mismatch:
                break
        if mismatch:
            return stats, mismatch
        if verbose:
            print("  step %02d ok (%s, %dx%d)" % (step, action, w, h))

    # Suppression must actually engage: noop damages change no pixels, so a
    # frame containing only noop damage is always fully suppressed.
    if stats["noop_dmg"] >= 3:
        assert stats["suppressed"] >= 1, \
            "same-tile suppression never fired: %s" % (stats,)
    return stats, None


def test_model():
    total = {"none": 0, "suppressed": 0, "upload": 0, "noop_dmg": 0}
    for seed in range(20):
        s, mismatch = run_random_sequence(seed)
        if mismatch is not None:
            fail(_fmt_mismatch(mismatch))
        for k in total:
            total[k] += s[k]
    print("  20 random sequences x 40 steps: partial-frame composition "
          "(previous frame base) -> canvas matches true page")
    print("  frame stats: %s" % total)
    assert total["suppressed"] > 0, "suppression path never exercised"


def test_buggy_composition_negative_control():
    """Discriminating-power check for the model asserted in test_model().

    Re-run the SAME random sequences with the OLD white-based partial
    composition (buggy_compose=True): the frame starts from WHITE on every
    partial frame and only the damage rects are painted, so the un-damaged
    remainder of each dirty 256x256 CROP is uploaded as white and erases
    correct browser canvas content.  At least one step must therefore show a
    browser canvas that does NOT match the true page.  Finding it proves the
    composition rule encoded in RendererModel (and asserted above) is what the
    bug violates, instead of the property test being untestable by
    construction.  Dense sampling here so a small erasure cannot slip through
    the 7-px grid used by the positive test."""
    first = None
    for seed in range(20):
        _, mismatch = run_random_sequence(seed, buggy_compose=True,
                                          sample_step=1)
        if mismatch is not None:
            first = mismatch
            break
    assert first is not None, (
        "negative control did not reproduce a mismatch: the white-base "
        "composition is NOT detected, so the positive test proves nothing "
        "about partial frames")
    print("  negative control: white-base partial composition reproduced the "
          "bug -> " + _fmt_mismatch(first))


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
    """Deterministic scroll-blit scenario mirroring the fast path, WITH the
    renderer's tile-hash suppression cache in the loop (2026-10-23 audit
    item): a tall page plus a NON-UNIFORM position:fixed band covering the
    whole first tile row, so tile row 0 is scroll-invariant — the geometry
    of the fixed-header ghost the audit found.  The blit displaces the
    fixed band on the browser canvas; the band re-renders pixel-identical
    content; a STALE hash then suppresses its re-upload forever.  Run
    twice: with the hash cache RESET on the blit (the fix) the canvas must
    converge to the true page; WITHOUT the reset (negative control) the
    stale hashes must keep the displaced pixels on screen."""
    fw, fh = 512, 900           # viewport: 4 tile rows, not aligned on y
    content_h = 4000            # tall page
    FIXED_ROWS = 256             # fixed band == exactly tile row 0
    # True content: rev-like pattern indexed by absolute content coords.
    def ccolor(cx, cy):
        return ((cx * 3 + cy) % 256, (cy * 7) % 256, ((cx ^ cy) * 5) % 256)

    # Non-uniform fixed content (a gradient header): identical in every
    # frame regardless of scroll.  A UNIFORM band would make the blit
    # displacement invisible, which is why the band varies by row.
    def fcolor(vy, vx):
        return ((vy * 5) % 256, 9, (vx % 32))

    def run(reset_cache_on_blit):

        browser = BrowserModel()
        rnd = random.Random(7)
        tx, ty = grid(fw, fh)
        hashes = {}   # slot -> hash of last UPLOADED tile (m_tileHashes)

        def render_tile_at(t, scroll_y):
            cxx, cyy = t % tx, t // tx
            img = []
            for y in range(TILE):
                row = []
                for x in range(TILE):
                    vx, vy = cxx * TILE + x, cyy * TILE + y
                    if vy >= fh or vx >= fw:
                        row.append((255, 255, 255))
                    elif vy < FIXED_ROWS:
                        row.append(fcolor(vy, vx))
                    else:
                        row.append(ccolor(vx, vy + scroll_y))
                img.append(row)
            return img

        def commit_with_suppression(tiles, scroll):
            up, imgs = [], []
            for t in tiles:
                img = render_tile_at(t, scroll)
                h = hash(repr(img))
                if h == hashes.get(t):
                    continue      # suppressed: slot believed identical
                hashes[t] = h
                up.append(t)
                imgs.append(img)
            if up:
                browser.commit(fw, fh, tx, ty, up, imgs)
            return len(tiles) - len(up)

        # Initial frame at scroll offset 0: full upload.
        commit_with_suppression(list(range(tx * ty)), 0)

        def shift(canvas, dy):
            shifted = [[(255, 255, 255) for _ in range(fw)] for _ in range(fh)]
            for y in range(fh):
                sy = y + dy
                if 0 <= sy < fh:
                    shifted[y] = list(canvas[sy])
            return shifted

        # Predictive-scroll state, mirroring the C++ design: the wheel
        # prediction is a PRESENTATION-ONLY draw offset (never touches
        # committed pixels — a wrong prediction can never evict rows from
        # the canvas), and the renderer's authoritative absolute offset
        # shifts the canvas by exactly (cx - base), then resets the preview.
        base = 0              # content offset the canvas is committed at
        preview = 0           # transient visual offset (PageSurface)
        scroll_y = 0          # renderer's true absolute content offset
        suppressed_total = 0
        mismatches = []

        # A series of wheel steps.  The browser displays a (possibly wrong)
        # prediction FIRST, then the authoritative (absolute) message
        # re-anchors the canvas exactly and resets the preview.
        for dy_chain in (120, 60, -90, 300, -45):
            scroll_y += dy_chain
            assert 0 <= scroll_y <= content_h - fh

            # 1) browser preview: presentation only, arbitrarily wrong.
            noise = rnd.choice((-40, -15, 0, 12, 33, 90))
            preview += dy_chain + noise

            # 2) renderer authority "scroll sdx,sdy,cx,cy": exact canvas
            #    shift by the true delta; preview resets to zero.
            true_delta = scroll_y - base
            browser.canvas = shift(browser.canvas, true_delta)
            base = scroll_y
            preview = 0
            assert preview == 0 and base == scroll_y
            # The fix under test: the blit invalidated every browser slot,
            # so the suppression cache must be reset with it.
            if reset_cache_on_blit:
                hashes = {}

            # 3) strip tiles for the newly exposed viewport area (C++ strip
            #    math, expressed against the TRUE delta), uploaded through
            #    the same hash-suppression rule as grabFrame().
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
            suppressed_total += commit_with_suppression(strip_tiles, scroll_y)

            # 4) deferred convergence frame (250 ms in C++): full damage,
            #    re-render everything, suppression still applies.
            suppressed_total += commit_with_suppression(
                list(range(tx * ty)), scroll_y)

            # After convergence the canvas must equal the true page at the
            # new offset, fixed band included, no matter how badly the
            # prediction missed.
            for y in range(fh):
                for x in range(0, fw, 13):
                    want = (fcolor(y, x) if y < FIXED_ROWS
                            else ccolor(x, y + scroll_y))
                    got = browser.canvas[y][x]
                    if got != want and len(mismatches) < 4:
                        mismatches.append((x, y, dy_chain, got, want))
        return mismatches, suppressed_total

    mism, supp = run(True)
    assert not mism, ("scroll with hash-cache reset still mismatched: %s"
                      % mism[:2])
    print("  predictive scroll + residual reconciliation (5 steps, noisy "
          "predictions, fixed band): canvas converges to true page "
          "(suppressed re-uploads: %d)" % supp)

    bad, _ = run(False)
    assert bad, ("negative control failed to reproduce: without the "
                 "hash-cache reset on the blit, stale hashes must keep the "
                 "displaced fixed-band pixels on screen")
    x, y, dy, got, want = bad[0]
    print("  negative control: stale-hash suppression reproduced the "
          "fixed-band ghost -> scroll dy=%d pixel(%d,%d) got=%s want=%s"
          % (dy, x, y, got, want))


if __name__ == "__main__":
    print("[1/5] static protocol consistency (renderer vs browser)")
    test_static_consistency()
    print("[2/5] segment layout worst-case bounds")
    test_segment_layout()
    print("[3/5] reference model property tests (random damage/resize)")
    test_model()
    print("[4/5] negative control: white-base composition must FAIL")
    test_buggy_composition_negative_control()
    print("[5/5] scroll blit scenario")
    test_scroll_blit()
    print("TILE PROTOCOL PASS")
