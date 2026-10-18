#!/usr/bin/env python3
"""Generate deterministic, transparent 2.5D building icons with Pillow.

The previous Blender Workbench pipeline emitted fully transparent PNGs while
reporting success. This replacement draws readable, supersampled isometric
silhouettes directly, so no Blender installation or project asset is required.
Outputs are copied to both historical icon source folders.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw


SIZE = 256
SCALE = 4
CANVAS = SIZE * SCALE
OUTLINE = (35, 49, 57, 255)
WINDOW = (120, 198, 220, 255)
WINDOW_HI = (211, 239, 245, 255)

BUILDINGS = {
    "House": {"wall": (206, 157, 103), "side": (158, 111, 72), "roof": (157, 69, 56), "height": 63, "style": "house"},
    "Apartment": {"wall": (201, 207, 210), "side": (136, 151, 159), "roof": (80, 105, 119), "height": 119, "style": "tower"},
    "Office": {"wall": (125, 161, 179), "side": (69, 101, 123), "roof": (54, 78, 95), "height": 132, "style": "glass"},
    "Factory": {"wall": (184, 183, 161), "side": (119, 126, 119), "roof": (81, 96, 97), "height": 69, "style": "factory"},
    "Hospital": {"wall": (228, 232, 225), "side": (167, 185, 181), "roof": (125, 151, 153), "height": 96, "style": "hospital"},
    "School": {"wall": (222, 190, 110), "side": (163, 125, 77), "roof": (147, 71, 57), "height": 88, "style": "school"},
    "FireStation": {"wall": (201, 119, 91), "side": (139, 67, 58), "roof": (89, 68, 64), "height": 74, "style": "fire"},
    "PowerPlant": {"wall": (132, 145, 145), "side": (76, 91, 95), "roof": (55, 71, 78), "height": 80, "style": "power"},
    "GreenSpace": {"wall": (88, 151, 91), "side": (55, 113, 70), "roof": (50, 125, 77), "height": 44, "style": "park"},
}


def pt(point: tuple[float, float]) -> tuple[int, int]:
    return round(point[0] * SCALE), round(point[1] * SCALE)


def polygon(draw: ImageDraw.ImageDraw, points: Iterable[tuple[float, float]], fill, outline=OUTLINE, width=1.5):
    points = list(points)
    draw.polygon([pt(point) for point in points], fill=fill)
    if outline:
        draw.line([pt(point) for point in points + [points[0]]], fill=outline, width=max(1, round(width * SCALE)), joint="curve")


def mix(color, target, amount):
    return tuple(round(color[i] * (1.0 - amount) + target[i] * amount) for i in range(3)) + (255,)


def diamond(cx, front_y, width, depth):
    return [
        (cx, front_y),
        (cx + width / 2, front_y - depth / 2),
        (cx, front_y - depth),
        (cx - width / 2, front_y - depth / 2),
    ]


def facade_point(cx, base_y, width, depth, height, u, t, side="right"):
    """Point on a visible wall; u=0 bottom..1 top, t=0..1 across wall."""
    y_lift = u * height
    if side == "right":
        return (cx + t * width / 2, base_y - t * depth / 2 - y_lift)
    return (cx - (1.0 - t) * width / 2, base_y - (1.0 - t) * depth / 2 - y_lift)


def draw_windows(draw, cx, base_y, width, depth, height, rows, cols, side="right", color=WINDOW):
    for row in range(rows):
        low = 0.12 + row * (0.74 / max(1, rows))
        high = low + 0.47 / max(1, rows)
        for col in range(cols):
            low_t = 0.10 + col * (0.78 / max(1, cols))
            high_t = low_t + 0.48 / max(1, cols)
            corners = [
                facade_point(cx, base_y, width, depth, height, low, low_t, side),
                facade_point(cx, base_y, width, depth, height, low, high_t, side),
                facade_point(cx, base_y, width, depth, height, high, high_t, side),
                facade_point(cx, base_y, width, depth, height, high, low_t, side),
            ]
            polygon(draw, corners, color, outline=mix(color[:3], (42, 61, 69), 0.30), width=0.75)
            # A small upper-left highlight keeps each pane legible at HUD size.
            a, b, c, d = corners
            polygon(draw, [a, (a[0] * 0.55 + b[0] * 0.45, a[1] * 0.55 + b[1] * 0.45),
                           (d[0] * 0.55 + c[0] * 0.45, d[1] * 0.55 + c[1] * 0.45), d], WINDOW_HI, outline=None)


def draw_prism(draw, cx, base_y, width, depth, height, wall, side, roof, *, windows=True, rows=3, cols=3):
    base = diamond(cx, base_y, width, depth)
    top = diamond(cx, base_y - height, width, depth)
    # Cast shadow and a thin terrain plinth make the silhouette read on both dark and light HUDs.
    polygon(draw, [(x + 9, y + 8) for x, y in base], (28, 39, 43, 56), outline=None)
    polygon(draw, base, (221, 226, 214, 255), outline=(101, 119, 116, 255), width=1.3)
    polygon(draw, [base[3], base[0], top[0], top[3]], side, width=1.5)
    polygon(draw, [base[0], base[1], top[1], top[0]], wall, width=1.5)
    polygon(draw, top, roof, width=1.5)
    if windows:
        draw_windows(draw, cx, base_y, width, depth, height, rows, cols, "right")
        if width >= 80:
            draw_windows(draw, cx, base_y, width, depth, height, rows, max(1, cols - 1), "left", mix(WINDOW[:3], (34, 63, 78), 0.22))
    return top


def draw_roof_house(draw, cx, base_y, width, depth, wall_height, wall, side, roof):
    base = diamond(cx, base_y, width, depth)
    top_y = base_y - wall_height
    top = diamond(cx, top_y, width, depth)
    polygon(draw, [(x + 8, y + 8) for x, y in base], (28, 39, 43, 56), outline=None)
    polygon(draw, [base[3], base[0], top[0], top[3]], side, width=1.4)
    polygon(draw, [base[0], base[1], top[1], top[0]], wall, width=1.4)
    # Two sloped roof planes meet on a raised ridge.
    ridge_front = (cx, top_y - 28)
    ridge_back = (cx, top_y - depth - 28)
    polygon(draw, [top[3], ridge_front, ridge_back, top[2]], mix(roof, (255, 255, 255), 0.12), width=1.5)
    polygon(draw, [ridge_front, top[1], top[2], ridge_back], roof, width=1.5)
    # Gable end and front door.
    polygon(draw, [top[0], ridge_front, top[1]], mix(wall, (255, 255, 255), 0.12), width=1.0)
    polygon(draw, [(cx - 11, base_y - 5), (cx + 4, base_y - 12), (cx + 4, base_y - 46), (cx - 11, base_y - 39)], (100, 66, 48, 255), width=0.8)
    polygon(draw, [(cx + 18, base_y - 34), (cx + 34, base_y - 40), (cx + 34, base_y - 58), (cx + 18, base_y - 52)], WINDOW, width=0.8)
    # Chimney on the rear roof slope.
    polygon(draw, [(cx + 20, top_y - 27), (cx + 34, top_y - 33), (cx + 34, top_y - 58), (cx + 20, top_y - 52)], (117, 68, 57, 255), width=0.8)


def draw_tree(draw, x, y, scale=1.0):
    trunk_h = 21 * scale
    polygon(draw, [(x - 4 * scale, y), (x + 4 * scale, y), (x + 4 * scale, y - trunk_h), (x - 4 * scale, y - trunk_h)], (112, 77, 52, 255), width=0.9)
    for layer, color in enumerate(((103, 163, 92, 255), (77, 143, 79, 255), (58, 120, 72, 255))):
        cy = y - trunk_h - layer * 11 * scale
        r = (17 - layer * 2) * scale
        polygon(draw, [(x, cy - r), (x + r, cy + r * 0.55), (x - r, cy + r * 0.55)], color, width=0.7)


def draw_special(draw, style, cx, base_y, width, depth, height, colors):
    wall, side, roof = colors["wall"], colors["side"], colors["roof"]
    if style == "house":
        draw_roof_house(draw, cx, base_y, 88, 70, 56, wall, side, roof)
    elif style in ("tower", "glass", "hospital", "school", "fire"):
        rows = {"tower": 5, "glass": 6, "hospital": 4, "school": 4, "fire": 3}[style]
        cols = {"tower": 3, "glass": 4, "hospital": 3, "school": 3, "fire": 2}[style]
        draw_prism(draw, cx, base_y, 90 if style in ("tower", "glass") else 104, 78, height, wall, side, roof, rows=rows, cols=cols)
        if style == "hospital":
            # Rooftop red cross, intentionally large enough to survive at 48 px.
            polygon(draw, [(cx - 9, base_y - height - 30), (cx + 9, base_y - height - 38),
                           (cx + 9, base_y - height - 56), (cx - 9, base_y - height - 48)], (188, 55, 54, 255), width=0.6)
            polygon(draw, [(cx - 21, base_y - height - 34), (cx - 21, base_y - height - 43),
                           (cx + 21, base_y - height - 24), (cx + 21, base_y - height - 15)], (188, 55, 54, 255), width=0.6)
        elif style == "school":
            polygon(draw, [(cx - 13, base_y - 18), (cx + 13, base_y - 29),
                           (cx + 13, base_y - 58), (cx - 13, base_y - 47)], (100, 78, 56, 255), width=0.8)
            polygon(draw, [(cx - 40, base_y - height - 12), (cx + 40, base_y - height - 47),
                           (cx + 40, base_y - height - 57), (cx - 40, base_y - height - 22)], (227, 216, 174, 255), width=0.8)
        elif style == "fire":
            # Wide red garage doors mark the rescue service building.
            for offset in (-19, 18):
                polygon(draw, [(cx + offset - 10, base_y - 4), (cx + offset + 4, base_y - 11),
                               (cx + offset + 4, base_y - 46), (cx + offset - 10, base_y - 39)], (114, 143, 151, 255), width=0.8)
    elif style == "factory":
        draw_prism(draw, cx, base_y, 132, 82, 53, wall, side, roof, windows=False)
        # Saw-tooth industrial roof and loading doors.
        for dx in (-34, 8, 46):
            polygon(draw, [(cx + dx - 10, base_y - 7), (cx + dx + 4, base_y - 14),
                           (cx + dx + 4, base_y - 43), (cx + dx - 10, base_y - 36)], (108, 125, 128, 255), width=0.8)
        polygon(draw, [(cx - 41, base_y - 53), (cx - 27, base_y - 59), (cx - 27, base_y - 100), (cx - 41, base_y - 94)], (110, 115, 110, 255), width=1.0)
        polygon(draw, [(cx - 44, base_y - 100), (cx - 27, base_y - 107), (cx - 24, base_y - 99), (cx - 40, base_y - 92)], (151, 73, 58, 255), width=0.8)
    elif style == "power":
        draw_prism(draw, cx, base_y, 112, 82, 58, wall, side, roof, windows=False)
        # Transformer blocks and a clearly distinct stack.
        for dx in (-33, 5, 36):
            polygon(draw, [(cx + dx - 9, base_y - 53), (cx + dx + 2, base_y - 58),
                           (cx + dx + 2, base_y - 77), (cx + dx - 9, base_y - 72)], (198, 176, 105, 255), width=0.8)
        polygon(draw, [(cx + 43, base_y - 48), (cx + 57, base_y - 54), (cx + 57, base_y - 132), (cx + 43, base_y - 126)], (105, 116, 117, 255), width=1.0)
        polygon(draw, [(cx + 40, base_y - 132), (cx + 59, base_y - 140), (cx + 59, base_y - 130), (cx + 40, base_y - 122)], (145, 70, 57, 255), width=0.8)
    elif style == "park":
        # Isometric park pad, winding path, three different canopy sizes, and a bench.
        polygon(draw, diamond(cx, base_y, 156, 85), (109, 177, 112, 255), outline=(64, 119, 78, 255), width=1.4)
        polygon(draw, [(cx - 64, base_y - 30), (cx - 4, base_y - 54), (cx + 58, base_y - 24), (cx + 6, base_y - 2)], (208, 194, 154, 255), outline=(147, 135, 107, 255), width=0.8)
        draw_tree(draw, cx - 39, base_y - 42, 0.9)
        draw_tree(draw, cx + 17, base_y - 73, 1.04)
        draw_tree(draw, cx + 60, base_y - 40, 0.76)
        # A small bench and path lamp.
        polygon(draw, [(cx - 25, base_y - 18), (cx - 2, base_y - 28), (cx - 2, base_y - 34), (cx - 25, base_y - 24)], (132, 84, 55, 255), width=0.8)
        polygon(draw, [(cx + 72, base_y - 25), (cx + 75, base_y - 26), (cx + 75, base_y - 58), (cx + 72, base_y - 57)], (61, 75, 74, 255), width=0.8)
        draw.ellipse((round((cx + 68) * SCALE), round((base_y - 67) * SCALE), round((cx + 79) * SCALE), round((base_y - 56) * SCALE)), fill=(245, 213, 121, 255))


def render(kind: str, data: dict) -> Image.Image:
    image = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw_special(draw, data["style"], 128, 210, 100, 82, data["height"], data)
    image = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    return image


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    output_dirs = [
        repo_root / "Content" / "UI" / "Icons" / "Buildings",
        repo_root / "Content" / "Buildings" / "Icons",
    ]
    for kind, data in BUILDINGS.items():
        icon = render(kind, data)
        filename = f"T_Icon_{kind}_01.png"
        for output_dir in output_dirs:
            output_dir.mkdir(parents=True, exist_ok=True)
            icon.save(output_dir / filename, format="PNG", optimize=True)
        bbox = icon.getbbox()
        if not bbox or bbox[2] - bbox[0] < 70 or bbox[3] - bbox[1] < 70:
            raise RuntimeError(f"Generated icon is unexpectedly empty: {kind}")
        print(f"Rendered {kind}: {bbox}, {icon.getbbox() and icon.getchannel('A').getbbox()}")


if __name__ == "__main__":
    main()
