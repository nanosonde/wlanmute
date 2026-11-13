"""Generate wlanmute's multi-resolution .ico files (requires Pillow).

Run from the repository root:  python tools/make_icons.py
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SCALE = 8  # supersampling factor for smooth anti-aliasing
ROOT = Path(__file__).resolve().parent.parent

STATES = {
    # name: (gradient top, gradient bottom, arc alpha, slash, ethernet badge)
    "monitoring": ((0x3A, 0x8D, 0xF5), (0x15, 0x4F, 0xC0), 255, False, False),
    "muted": ((0x3A, 0x8D, 0xF5), (0x15, 0x4F, 0xC0), 120, True, True),
    "paused": ((0x9A, 0xA3, 0xAE), (0x5E, 0x67, 0x73), 255, False, False),
}


def rounded_gradient(px, top, bottom):
    gradient = Image.new("RGBA", (px, px))
    pixels = gradient.load()
    for y in range(px):
        t = y / (px - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
        for x in range(px):
            pixels[x, y] = color
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, px - 1, px - 1), radius=px * 0.22, fill=255)
    gradient.putalpha(mask)
    return gradient


def arc_with_round_caps(draw, center, radius, width, color):
    cx, cy = center
    draw.arc((cx - radius, cy - radius, cx + radius, cy + radius), 225, 315, fill=color, width=width)
    middle = radius - width / 2
    for angle in (225, 315):
        x = cx + middle * math.cos(math.radians(angle))
        y = cy + middle * math.sin(math.radians(angle))
        draw.ellipse((x - width / 2, y - width / 2, x + width / 2, y + width / 2), fill=color)


def render(size, state):
    top, bottom, arc_alpha, slash, badge = STATES[state]
    px = size * SCALE
    u = px / 256  # design units are based on a 256x256 canvas
    image = rounded_gradient(px, top, bottom)

    layer = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    white = (255, 255, 255, arc_alpha)
    small = size <= 24
    center = (128 * u, 196 * u)
    # Fewer, bolder arcs keep the symbol legible at tray sizes.
    radii, stroke = ([62, 118], 34) if small else ([52, 96, 140], 26)
    for radius in radii:
        arc_with_round_caps(draw, center, radius * u, round(stroke * u), white)
    dot = (24 if small else 19) * u
    draw.ellipse((center[0] - dot, center[1] - dot, center[0] + dot, center[1] + dot), fill=white)
    image.alpha_composite(layer)

    draw = ImageDraw.Draw(image)
    if slash:
        start, end = (52 * u, 52 * u), (204 * u, 204 * u)
        outline, line = round((54 if small else 44) * u), round((30 if small else 24) * u)
        for width, color in ((outline, (21, 79, 192, 255)), (line, (255, 92, 77, 255))):
            draw.line((start, end), fill=color, width=width)
            for x, y in (start, end):
                draw.ellipse((x - width / 2, y - width / 2, x + width / 2, y + width / 2), fill=color)

    if badge:
        # Green Ethernet-port badge: "the wired connection is in use".
        cx, cy, r = 186 * u, 186 * u, (58 if small else 50) * u
        draw.ellipse((cx - r - 8 * u, cy - r - 8 * u, cx + r + 8 * u, cy + r + 8 * u), fill=(255, 255, 255, 255))
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(0x22, 0xB5, 0x73, 255))
        if not small:
            w, h = 46 * u, 34 * u
            x0, y0 = cx - w / 2, cy - h / 2 + 4 * u
            port = [(x0, y0), (x0 + w, y0), (x0 + w, y0 + h), (x0 + w * 0.68, y0 + h),
                    (x0 + w * 0.68, y0 + h + 9 * u), (x0 + w * 0.32, y0 + h + 9 * u),
                    (x0 + w * 0.32, y0 + h), (x0, y0 + h)]
            draw.polygon([(x, y - 8 * u) for x, y in port], fill=(255, 255, 255, 255))
            for i in range(4):
                x = x0 + w * (0.2 + 0.2 * i)
                draw.line(((x, y0 - 2 * u), (x, y0 + 10 * u)), fill=(0x22, 0xB5, 0x73, 255), width=round(5 * u))

    return image.resize((size, size), Image.LANCZOS)


def main():
    icon_dir = ROOT / "src" / "icons"
    icon_dir.mkdir(parents=True, exist_ok=True)
    for state in STATES:
        frames = [render(size, state) for size in SIZES]
        frames[-1].save(icon_dir / f"{state}.ico", format="ICO",
                        sizes=[(s, s) for s in SIZES], append_images=frames[:-1])

    # Preview sheet for the README: each state at 256, 48, 32, 24 and 16 px.
    preview_sizes = [256, 48, 32, 24, 16]
    gap = 24
    width = sum(preview_sizes) + gap * (len(preview_sizes) + 1)
    sheet = Image.new("RGBA", (width, (256 + gap) * len(STATES) + gap), (0, 0, 0, 0))
    for row, state in enumerate(STATES):
        x = gap
        y = gap + row * (256 + gap)
        for size in preview_sizes:
            sheet.alpha_composite(render(size, state), (x, y + (256 - size) // 2))
            x += size + gap
    docs = ROOT / "docs"
    docs.mkdir(exist_ok=True)
    sheet.save(docs / "icons.png")


if __name__ == "__main__":
    main()
