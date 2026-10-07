"""Generate neutral placeholder images for branding/fenn.

The owner has not provided Fenn's brand assets yet, and placeholder art must not
invent branding. So every image is a plain neutral grey rounded square with no
motif, lettering or brand colour (a darker grey for private browsing, so the two
stay distinguishable). Document icons are a plain page, and the jump-list
glyphs are generic window and new-tab outlines. The about dialog's wordmark is
the product name in the system font.

It writes every image Firefox's Windows branding uses, at the sizes upstream's
directory uses. Pure Python (no Pillow), deterministic output. Replace the
generated files with the final brand assets when they are provided.
"""
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "branding/fenn"

NEUTRAL = (0x8a, 0x8a, 0x8a)
PRIVATE = (0x5c, 0x5c, 0x5c)
PLATE = (0.06, 0.06, 0.94, 0.94, 0.22)


def sd_round_rect(x, y, rect):
    x0, y0, x1, y1, r = rect
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    dx, dy = abs(x - cx) - hx, abs(y - cy) - hy
    return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - r


def sd_capsule(x, y, seg, min_radius=0.0):
    ax, ay, bx, by, r = seg
    r = max(r, min_radius)
    px, py, vx, vy = x - ax, y - ay, bx - ax, by - ay
    t = max(0.0, min(1.0, (px * vx + py * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - vx * t, py - vy * t) - r


def coverage(d, px):
    return max(0.0, min(1.0, 0.5 - d / px))


def over(dst, color, alpha):
    if alpha <= 0:
        return dst
    r, g, b, a = dst
    out_a = alpha + a * (1 - alpha)
    mix = lambda c, d: (c * alpha + d * a * (1 - alpha)) / out_a
    return (mix(color[0], r), mix(color[1], g), mix(color[2], b), out_a)


def render(size, color=NEUTRAL, scale=1.0, width=None, height=None, background=None):
    """RGBA rows of a plain rounded square, `scale` of the shorter side, centred."""
    width, height = width or size, height or size
    side = min(width, height) * scale
    ox, oy = (width - side) / 2, (height - side) / 2
    px = 1 / side
    rows = []
    for j in range(height):
        row = []
        for i in range(width):
            x, y = (i + 0.5 - ox) / side, (j + 0.5 - oy) / side
            pixel = (*background, 1.0) if background else (0, 0, 0, 0)
            row.append(over(pixel, color, coverage(sd_round_rect(x, y, PLATE), px)))
        rows.append(row)
    return rows


def document(size):
    """A plain page with a folded corner."""
    page = (0.20, 0.08, 0.80, 0.92)
    px = 1 / size
    rows = []
    for j in range(size):
        row = []
        for i in range(size):
            x, y = (i + 0.5) / size, (j + 0.5) / size
            d = sd_round_rect(x, y, (*page, 0.04))
            fold = (x - y - 0.54) / math.sqrt(2)    # cut the top-right corner
            pixel = over((0, 0, 0, 0), (0x9a, 0x9a, 0x9a), coverage(d, px) * coverage(fold, px * 1.4))
            pixel = over(pixel, (0xfb, 0xfb, 0xfb), coverage(d + 0.02, px) * coverage(fold + 0.02, px))
            row.append(pixel)
        rows.append(row)
    return rows


def glyph(size, kind):
    """Generic jump-list glyphs: a window outline, or a tab outline with a plus."""
    rows = []
    px = 1 / size
    for j in range(size):
        row = []
        for i in range(size):
            x, y = (i + 0.5) / size, (j + 0.5) / size
            frame = (0.12, 0.18, 0.88, 0.84, 0.08) if kind == "window" else (0.12, 0.28, 0.88, 0.84, 0.06)
            c = coverage(abs(sd_round_rect(x, y, frame)) - max(0.045, 0.6 * px), px)
            if kind == "window":
                c = max(c, coverage(sd_capsule(x, y, (0.14, 0.34, 0.86, 0.34, 0.03), 0.6 * px), px))
            else:
                plus = min(sd_capsule(x, y, (0.5, 0.42, 0.5, 0.70, 0.04), 0.6 * px),
                           sd_capsule(x, y, (0.36, 0.56, 0.64, 0.56, 0.04), 0.6 * px))
                c = max(c, coverage(plus, px))
            row.append((0x20, 0x20, 0x20, c))
        rows.append(row)
    return rows


def png_bytes(rows):
    raw = b"".join(b"\x00" + bytes(v for p in row for v in (*(round(min(255, max(0, c))) for c in p[:3]),
                                                             round(p[3] * 255))) for row in rows)
    chunk = lambda tag, data: struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    header = struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def dib_bytes(rows):
    """A 32-bit icon DIB: bottom-up BGRA plus an empty AND mask."""
    size = len(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes(v for p in row for v in (round(p[2]), round(p[1]), round(p[0]), round(p[3] * 255)))
                      for row in reversed(rows))
    mask = b"\x00" * (((size + 31) // 32) * 4 * size)
    return header + pixels + mask


def ico_bytes(images):
    """images: rows per size; 256 px is stored as PNG, smaller sizes as DIB."""
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for rows in images:
        size = len(rows)
        blob = png_bytes(rows) if size >= 256 else dib_bytes(rows)
        entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs)


def bmp_bytes(rows, background):
    """A 24-bit BMP for the NSIS installer, flattened onto `background`."""
    height, width = len(rows), len(rows[0])
    stride = (width * 3 + 3) // 4 * 4
    body = b""
    for row in reversed(rows):
        line = b""
        for r, g, b, a in row:
            line += bytes(round(c * a + bg * (1 - a)) for c, bg in ((b, background[2]), (g, background[1]),
                                                                       (r, background[0])))
        body += line + b"\x00" * (stride - len(line))
    header = struct.pack("<2sIHHI", b"BM", 54 + len(body), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    return header + info + body


def logo_svg(color=NEUTRAL):
    x0, y0, x1, y1, r = PLATE
    u = lambda v: f"{v * 512:g}"
    return ("<!-- Neutral placeholder generated by scripts/New-FennPlaceholderArt.py; not brand art. -->\n"
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">\n'
            f'  <rect x="{u(x0)}" y="{u(y0)}" width="{u(x1 - x0)}" height="{u(y1 - y0)}" rx="{u(r)}" '
            f'fill="#%02x%02x%02x"/>\n</svg>\n' % color)


WORDMARK = """<!-- Neutral placeholder generated by scripts/New-FennPlaceholderArt.py: the product
     name in the system font, not a brand wordmark. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 372 99">
  <text x="0" y="80" fill="context-fill" font-family="sans-serif" font-size="96">Fenn</text>
</svg>
"""

DOCUMENT_PDF_SVG = """<!-- Neutral placeholder generated by scripts/New-FennPlaceholderArt.py. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path fill="#fbfbfb" stroke="#9a9a9a" d="M3.5 1.5h6l3 3v10h-9z"/>
</svg>
"""


def write(relative, data):
    path = OUT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
    print(f"wrote {relative}")


def main():
    cache = {}

    def icon(size, color=NEUTRAL, scale=1.0, width=None, height=None, background=None):
        key = (size, color, scale, width, height, background)
        if key not in cache:
            cache[key] = render(size, color, scale, width, height, background)
        return cache[key]

    for size in (16, 22, 24, 32, 48, 64, 128, 256):
        write(f"default{size}.png", png_bytes(icon(size)))
    write("firefox.ico", ico_bytes([icon(s) for s in (16, 32, 48, 256)]))
    write("firefox64.ico", ico_bytes([icon(s) for s in (16, 32, 48, 64)]))
    write("pbmode.ico", ico_bytes([icon(s, PRIVATE) for s in (16, 32, 48, 256)]))
    write("document.ico", ico_bytes([document(s) for s in (16, 32, 48, 256)]))
    write("document_pdf.ico", ico_bytes([document(256)]))
    write("newwindow.ico", ico_bytes([glyph(s, "window") for s in (16, 32)]))
    write("newtab.ico", ico_bytes([glyph(s, "tab") for s in (16, 32)]))
    write("VisualElements_150.png", png_bytes(icon(300, scale=0.6)))
    write("VisualElements_70.png", png_bytes(icon(142, scale=0.6)))
    write("PrivateBrowsing_150.png", png_bytes(icon(300, PRIVATE, scale=0.6)))
    write("PrivateBrowsing_70.png", png_bytes(icon(142, PRIVATE, scale=0.6)))
    write("content/about-logo.png", png_bytes(icon(192)))
    write("content/about-logo@2x.png", png_bytes(icon(384)))
    write("content/about-logo-private.png", png_bytes(icon(192, PRIVATE)))
    write("content/about-logo-private@2x.png", png_bytes(icon(384, PRIVATE)))
    write("content/about.png", png_bytes(icon(236, width=300, height=236, scale=0.9)))
    write("content/about-logo.svg", logo_svg())
    write("content/about-wordmark.svg", WORDMARK)
    write("content/firefox-wordmark.svg", WORDMARK)
    write("content/document_pdf.svg", DOCUMENT_PDF_SVG)
    assets = {
        "Document44x44.png": document(44),
        "LargeTile.scale-200.png": icon(620, scale=0.5),
        "MedTile.scale-200.png": icon(300, scale=0.6),
        "SmallTile.scale-200.png": icon(142, scale=0.7),
        "Square150x150Logo.scale-200.png": icon(300, scale=0.6),
        "Square44x44Logo.altform-lightunplated_targetsize-256.png": icon(256),
        "Square44x44Logo.altform-unplated_targetsize-256.png": icon(256),
        "Square44x44Logo.scale-200.png": icon(88),
        "Square44x44Logo.targetsize-256.png": icon(256),
        "StoreLogo.scale-200.png": icon(100),
        "Wide310x150Logo.scale-200.png": icon(300, scale=0.6, width=620, height=300),
    }
    for name, rows in assets.items():
        write(f"msix/Assets/{name}", png_bytes(rows))
    white = (0xff, 0xff, 0xff)
    write("wizHeader.bmp", bmp_bytes(icon(57, width=150, height=57, scale=0.85, background=white), white))
    write("wizHeaderRTL.bmp", bmp_bytes(icon(57, width=150, height=57, scale=0.85, background=white), white))
    write("wizWatermark.bmp", bmp_bytes(icon(164, width=164, height=314, scale=0.8, background=white), white))


if __name__ == "__main__":
    main()
