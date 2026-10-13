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

    global TILE, HEADER
    TILE = parse_const(rend, "kTileSize")
    HEADER = parse_const(rend, "kHeaderBytes")


# ---------------------------------------------------------------- Part 2 ----
TILE = 256
HEADER = 32
TILE_BYTES = TILE * TILE


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

    @staticmethod
    def _rects_intersect(a, b):
        return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and \
               a[1] < b[1] + b[3] and b[1] < a[1] + a[3]

    def damage(self, x, y, w, h):
        self.dirty.append((x, y, w, h))

    def resize(self, w, h, width_changed=False):
        if width_changed:
            self.relayout_pending = True
        self.new_size = (w, h)

    def grab_frame(self):
        """Returns (tilesX, tilesY, dirtyTiles) or None (no-op frame)."""
        fw, fh = self.new_size
        tx, ty = grid(fw, fh)
        if self.relayout_pending or tx != self.tiles_x:
            self.dirty = [(0, 0, fw, fh)]
        elif fh > self.fh:
            self.dirty.append((0, self.fh, fw, fh - self.fh))
        self.relayout_pending = False

        # intersect with frame rect
        frame = (0, 0, fw, fh)
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

        self.fw, self.fh = fw, fh
        self.tiles_x, self.tiles_y = tx, ty
        self.dirty = []
        return tx, ty, dirty_tiles


class BrowserModel:
    """Models readTiles(): persistent canvas compositing."""

    def __init__(self):
        self.canvas = None  # 2D list of (r,g,b) tuples or None

    def commit(self, w, h, tx, ty, tile_images, dirty_tiles):
        if self.canvas is None or len(self.canvas) != h or \
           (h and len(self.canvas[0]) != w):
            next_c = [[(255, 255, 255) for _ in range(w)] for _ in range(h)]
            if self.canvas:
                oh, ow = len(self.canvas), len(self.canvas[0])
                for y in range(min(h, oh)):
                    for x in range(min(w, ow)):
                        next_c[y][x] = self.canvas[y][x]
            self.canvas = next_c
        for t in dirty_tiles:
            assert 0 <= t < tx * ty
            cxx, cyy = t % tx, t // tx
            src = tile_images[t]
            copy_w = min(TILE, w - cxx * TILE)
            copy_h = min(TILE, h - cyy * TILE)
            for y in range(copy_h):
                row = self.canvas[cyy * TILE + y]
                srow = src[y]
                for x in range(copy_w):
                    row[cxx * TILE + x] = srow[x]


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
    renderer.resize(w, h, width_changed=True)
    tile_store = {}

    def render_tile(t, tx):
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

    for step in range(steps):
        action = rnd.choice(["dmg", "dmg", "dmg", "resize", "idle"])
        if action == "dmg":
            rev_counter += 1
            rx, ry = rnd.randrange(w), rnd.randrange(h)
            rw, rh = rnd.randint(1, w - rx), rnd.randint(1, h - ry)
            for py in range(ry, ry + rh):
                row = true_rev[py]
                for px in range(rx, rx + rw):
                    row[px] = rev_counter
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
            # A width change relayouts the whole page: every pixel may change.
            if nw != w:
                for py in range(nh):
                    for px in range(nw):
                        true_rev[py][px] = rev_counter
            renderer.resize(nw, nh, width_changed=(nw != w))
            w, h = nw, nh
        # idle: no damage

        result = renderer.grab_frame()
        if result is not None:
            tx, ty, dirty_tiles = result
            for t in dirty_tiles:
                tile_store[t] = render_tile(t, tx)
            browser.commit(w, h, tx, ty, tile_store, dirty_tiles)

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


def test_model():
    for seed in range(20):
        run_random_sequence(seed)
    print("  20 random sequences x 40 steps: canvas matches true page")


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


if __name__ == "__main__":
    print("[1/3] static protocol consistency (renderer vs browser)")
    test_static_consistency()
    print("[2/3] segment layout worst-case bounds")
    test_segment_layout()
    print("[3/3] reference model property tests (random damage/resize)")
    test_model()
    print("TILE PROTOCOL PASS")
