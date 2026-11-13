#!/usr/bin/env python3
"""Print and draw the furniture rectangles in src/desktop_collide.inc.

Command Center screens are not copied. Collision reads them from
zell_monitor_layout and zell_mana_console_layout.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "src" / "desktop_collide.inc"
ROOMS = (
    (0, "cave", ROOT / "assets/v0.1.0/cave.png", None),
    (1, "command", ROOT / "assets/v0.1.1/command-center.png", None),
    (2, "laundry", ROOT / "assets/v0.1.1/laundry.png", None),
    (3, "vault", ROOT / "assets/v0.1.1/vault.png", None),
    (4, "workshop", ROOT / "assets/v0.1.1/workshop.png", None),
    (5, "game", ROOT / "assets/v0.1.1/game-room.png", ROOT / "assets/v0.1.1/game-room-foreground.png"),
    (6, "theater", ROOT / "assets/v0.1.1/movie-theater.png", ROOT / "assets/v0.1.1/movie-theater-foreground.png"),
    (7, "meditation", ROOT / "assets/v0.1.1/meditation-room.png", ROOT / "assets/v0.1.1/meditation-room-foreground.png"),
    (8, "library", ROOT / "assets/v0.1.1/library-room.png", ROOT / "assets/v0.1.1/library-room-foreground.png"),
)
RECT = re.compile(
    r"\{(\d+)\s*,\s*([0-9.]+)f\s*,\s*([0-9.]+)f\s*,\s*([0-9.]+)f\s*,\s*([0-9.]+)f\s*\}"
)


def rectangles(text):
    found = []
    for room, x0, y0, x1, y1 in RECT.findall(text):
        found.append((int(room), float(x0), float(y0), float(x1), float(y1)))
    return found


def draw(blocks, out_dir):
    from PIL import Image, ImageDraw

    out_dir.mkdir(parents=True, exist_ok=True)
    for room, name, path, front in ROOMS:
        image = Image.open(path).convert("RGBA")
        if front is not None:
            image.alpha_composite(Image.open(front).convert("RGBA"))
        image = image.convert("RGB")
        pen = ImageDraw.Draw(image)
        step_x = image.width * 0.05
        step_y = image.height * 0.05
        x = step_x
        while x < image.width:
            pen.line([(x, 0), (x, image.height)], fill=(80, 170, 190), width=1)
            x += step_x
        y = step_y
        while y < image.height:
            pen.line([(0, y), (image.width, y)], fill=(80, 170, 190), width=1)
            y += step_y
        for block_room, x0, y0, x1, y1 in blocks:
            if block_room != room:
                continue
            pen.rectangle([x0, y0, x1 - 1, y1 - 1], outline=(220, 32, 32), width=3)
        image.save(out_dir / f"blocks-{name}.png")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--print", action="store_true", dest="do_print")
    parser.add_argument("--out", type=Path, help="directory for gridded PNGs")
    args = parser.parse_args(argv)
    blocks = rectangles(TABLE.read_text())
    if not blocks:
        print("room_grid: no rectangles in", TABLE, file=sys.stderr)
        return 1
    if args.do_print or args.out is None:
        print("# rectangles from src/desktop_collide.inc zell_blocks")
        print("# room 1 screens come from zell_monitor_layout and zell_mana_console_layout")
        for room, x0, y0, x1, y1 in blocks:
            print(f"{room} {x0:.0f} {y0:.0f} {x1:.0f} {y1:.0f}")
    if args.out is not None:
        draw(blocks, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
