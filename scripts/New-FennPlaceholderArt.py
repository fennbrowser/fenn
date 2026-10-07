"""Generate Fenn's placeholder icon set into branding/fenn.

Fenn is short for Fennec. The owner's artwork has not started, so this draws a
simple stand-in: a front-facing fennec fox face, with its oversized ears, in
sand tones on a dusk-blue rounded square, and a purple variant for private
browsing. It is deliberately unlike Firefox's logo (no orange fox wrapped
around a globe). It writes every image Firefox's Windows branding uses, at the
sizes upstream's directory uses, plus matching SVGs. Pure Python (no Pillow),
deterministic output.

Replace the generated files with real artwork when it exists; this script is only
for the placeholder. See the rebrand plan.
"""
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "branding/fenn"

DUSK = ((0x3a, 0x5f, 0x8f), (0x12, 0x22, 0x3d))     # top, bottom of the plate
PURPLE = ((0x7a, 0x52, 0xb3), (0x2b, 0x1b, 0x4d))
SAND = (0xe6, 0xb9, 0x7a)
CREAM = (0xf8, 0xea, 0xd2)
EAR = (0xf2, 0xc9, 0xb4)
DARK = (0x2a, 0x1e, 0x1a)

PLATE = (0.06, 0.06, 0.94, 0.94, 0.22)
# Shapes in a 0..1 square, painted in order: polygons (with a rounding radius),
# circles and capsules (segments with a radius).
FOX = [
    ("poly", [(0.22, 0.17), (0.47, 0.44), (0.26, 0.60)], 0.03, SAND),     # left ear
    ("poly", [(0.78, 0.17), (0.53, 0.44), (0.74, 0.60)], 0.03, SAND),     # right ear
    ("poly", [(0.26, 0.26), (0.41, 0.44), (0.29, 0.53)], 0.015, EAR),
    ("poly", [(0.74, 0.26), (0.59, 0.44), (0.71, 0.53)], 0.015, EAR),
    ("circle", (0.50, 0.58, 0.19), SAND),                                  # head
    ("poly", [(0.28, 0.58), (0.72, 0.58), (0.50, 0.84)], 0.05, SAND),     # jaw
    ("poly", [(0.39, 0.66), (0.61, 0.66), (0.50, 0.83)], 0.035, CREAM),   # muzzle
    ("circle", (0.41, 0.58, 0.028), DARK),                                 # eyes
    ("circle", (0.59, 0.58, 0.028), DARK),
    ("circle", (0.50, 0.80, 0.03), DARK),                                  # nose
]


def sd_round_rect(x, y, rect):
    x0, y0, x1, y1, r = rect
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    dx, dy = abs(x - cx) - hx, abs(y - cy) - hy
    outside = math.hypot(max(dx, 0), max(dy, 0))
    return outside + min(max(dx, dy), 0) - r


def sd_capsule(x, y, seg, min_radius=0.0):
    ax, ay, bx, by, r = seg
    r = max(r, min_radius)
    px, py, vx, vy = x - ax, y - ay, bx - ax, by - ay
    t = max(0.0, min(1.0, (px * vx + py * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - vx * t, py - vy * t) - r


def sd_polygon(x, y, points):
    """Signed distance to a polygon (negative inside)."""
    d = float("inf")
    sign = 1.0
    n = len(points)
    for i in range(n):
        (ax, ay), (bx, by) = points[i], points[i - 1]
        ex, ey, wx, wy = bx - ax, by - ay, x - ax, y - ay
        t = max(0.0, min(1.0, (wx * ex + wy * ey) / (ex * ex + ey * ey)))
        d = min(d, math.hypot(wx - ex * t, wy - ey * t))
        c1, c2, c3 = y >= ay, y < by, ex * wy > ey * wx
        if (c1 and c2 and c3) or (not c1 and not c2 and not c3):
            sign = -sign
    return sign * d


def sd_shape(x, y, shape, min_radius):
    kind = shape[0]
    if kind == "poly":
        return sd_polygon(x, y, shape[1]) - shape[2]
    if kind == "circle":
        cx, cy, r = shape[1]
        return math.hypot(x - cx, y - cy) - max(r, min_radius)
    return sd_capsule(x, y, shape[1], min_radius)


def over(dst, color, alpha):
    if alpha <= 0:
        return dst
    r, g, b, a = dst
    out_a = alpha + a * (1 - alpha)
    if out_a == 0:
        return (0, 0, 0, 0)
    mix = lambda c, d: (c * alpha + d * a * (1 - alpha)) / out_a
    return (mix(color[0], r), mix(color[1], g), mix(color[2], b), out_a)


def coverage(d, px):
    return max(0.0, min(1.0, 0.5 - d / px))


def paint_fox(pixel, x, y, px, clip=1.0):
    min_radius = 0.6 * px              # keep small features visible at 16 px
    for shape in FOX:
        pixel = over(pixel, shape[-1], coverage(sd_shape(x, y, shape, min_radius), px) * clip)
    return pixel


def render(size, plate=DUSK, scale=1.0, width=None, height=None, background=None):
    """RGBA rows of the icon, `scale` of the shorter side, centred on a canvas."""
    width, height = width or size, height or size
    side = min(width, height) * scale
    ox, oy = (width - side) / 2, (height - side) / 2
    px = 1 / side                      # one pixel in icon units
    rows = []
    for j in range(height):
        row = []
        for i in range(width):
            x, y = (i + 0.5 - ox) / side, (j + 0.5 - oy) / side
            pixel = (*background, 1.0) if background else (0, 0, 0, 0)
            cover = coverage(sd_round_rect(x, y, PLATE), px)
            if cover > 0:
                t = max(0.0, min(1.0, (y - PLATE[1]) / (PLATE[3] - PLATE[1])))
                top, bottom = plate
                pixel = over(pixel, tuple(top[k] + (bottom[k] - top[k]) * t for k in range(3)), cover)
                pixel = paint_fox(pixel, x, y, px, cover)
            row.append(pixel)
        rows.append(row)
    return rows


def document(size, pdf=False):
    """A page with a folded corner and a small fox, for file associations."""
    page = (0.20, 0.08, 0.80, 0.92)
    px = 1 / size
    rows = []
    for j in range(size):
        row = []
        for i in range(size):
            x, y = (i + 0.5) / size, (j + 0.5) / size
            d = sd_round_rect(x, y, (*page, 0.04))
            fold = (x - y - 0.54) / math.sqrt(2)    # cut the top-right corner along (0.62, 0.08)-(0.80, 0.26)
            pixel = over((0, 0, 0, 0), (0x9a, 0xa5, 0xa8), coverage(d, px) * coverage(fold, px * 1.4))
            inner = coverage(d + 0.02, px) * coverage(fold + 0.02, px)
            pixel = over(pixel, (0xfb, 0xfb, 0xfb), inner)
            if pdf and 0.70 < y < 0.84:
                pixel = over(pixel, (0xc6, 0x28, 0x28), inner)
            # The fox at 45% of the page, centred in its upper part.
            fx, fy, s = (x - 0.275) / 0.45, (y - 0.20) / 0.45, 0.45
            pixel = paint_fox(pixel, fx, fy, px / s, inner)
            row.append(pixel)
        rows.append(row)
    return rows


def glyph(size, kind):
    """Jump-list glyphs: a window outline, or a tab outline with a plus."""
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
            row.append((0x20, 0x2a, 0x2c, c))
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


def logo_svg(plate=DUSK):
    hexc = lambda c: "#%02x%02x%02x" % c
    u = lambda v: f"{v * 512:g}"
    x0, y0, x1, y1, r = PLATE
    parts = []
    for shape in FOX:
        kind, color = shape[0], hexc(shape[-1])
        if kind == "poly":
            points = " ".join(f"{u(px)},{u(py)}" for px, py in shape[1])
            # A round-joined stroke of twice the radius reproduces the rounded corners.
            parts.append(f'<polygon points="{points}" fill="{color}" stroke="{color}" '
                         f'stroke-width="{u(shape[2] * 2)}" stroke-linejoin="round"/>')
        elif kind == "circle":
            cx, cy, rad = shape[1]
            parts.append(f'<circle cx="{u(cx)}" cy="{u(cy)}" r="{u(rad)}" fill="{color}"/>')
        else:
            ax, ay, bx, by, rad = shape[1]
            parts.append(f'<line x1="{u(ax)}" y1="{u(ay)}" x2="{u(bx)}" y2="{u(by)}" stroke="{color}" '
                         f'stroke-width="{u(rad * 2)}" stroke-linecap="round"/>')
    return ("<!-- Placeholder Fenn logo generated by scripts/New-FennPlaceholderArt.py. -->\n"
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">\n'
            f'  <defs><linearGradient id="plate" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="{hexc(plate[0])}"/><stop offset="1" stop-color="{hexc(plate[1])}"/>'
            "</linearGradient></defs>\n"
            f'  <rect x="{u(x0)}" y="{u(y0)}" width="{u(x1 - x0)}" height="{u(y1 - y0)}" rx="{u(r)}" '
            'fill="url(#plate)"/>\n'
            + "".join(f"  {p}\n" for p in parts) + "</svg>\n")


WORDMARK = """<!-- Placeholder Fenn wordmark generated by scripts/New-FennPlaceholderArt.py. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 372 99">
  <text x="0" y="80" fill="context-fill" font-family="Segoe UI Variable Display, Segoe UI, sans-serif"
        font-size="96" font-weight="600" letter-spacing="-2">Fenn</text>
</svg>
"""

DOCUMENT_PDF_SVG = """<!-- Placeholder Fenn PDF document icon generated by scripts/New-FennPlaceholderArt.py. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path fill="#fbfbfb" stroke="#9aa5a8" d="M3.5 1.5h6l3 3v10h-9z"/>
  <rect x="3.5" y="10" width="9" height="3" fill="#c62828"/>
</svg>
"""


def write(relative, data):
    path = OUT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
    print(f"wrote {relative}")


def main():
    cache = {}

    def icon(size, plate=DUSK, scale=1.0, width=None, height=None, background=None):
        key = (size, plate, scale, width, height, background)
        if key not in cache:
            cache[key] = render(size, plate, scale, width, height, background)
        return cache[key]

    for size in (16, 22, 24, 32, 48, 64, 128, 256):
        write(f"default{size}.png", png_bytes(icon(size)))
    write("firefox.ico", ico_bytes([icon(s) for s in (16, 32, 48, 256)]))
    write("firefox64.ico", ico_bytes([icon(s) for s in (16, 32, 48, 64)]))
    write("pbmode.ico", ico_bytes([icon(s, PURPLE) for s in (16, 32, 48, 256)]))
    write("document.ico", ico_bytes([document(s) for s in (16, 32, 48, 256)]))
    write("document_pdf.ico", ico_bytes([document(256, pdf=True)]))
    write("newwindow.ico", ico_bytes([glyph(s, "window") for s in (16, 32)]))
    write("newtab.ico", ico_bytes([glyph(s, "tab") for s in (16, 32)]))
    # Start tiles show the logo on the tile colour from the VisualElements manifests.
    write("VisualElements_150.png", png_bytes(icon(300, scale=0.6)))
    write("VisualElements_70.png", png_bytes(icon(142, scale=0.6)))
    write("PrivateBrowsing_150.png", png_bytes(icon(300, PURPLE, scale=0.6)))
    write("PrivateBrowsing_70.png", png_bytes(icon(142, PURPLE, scale=0.6)))
    write("content/about-logo.png", png_bytes(icon(192)))
    write("content/about-logo@2x.png", png_bytes(icon(384)))
    write("content/about-logo-private.png", png_bytes(icon(192, PURPLE)))
    write("content/about-logo-private@2x.png", png_bytes(icon(384, PURPLE)))
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
    write("wizWatermark.bmp", bmp_bytes(icon(164, width=164, height=314, scale=0.8, background=DUSK[1]), DUSK[1]))


if __name__ == "__main__":
    main()
