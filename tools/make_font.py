#!/usr/bin/env python3
"""Bake a monospace bitmap font into a header for the examples.

One bit per pixel, 16x32 per glyph, ASCII 32 to 126. The examples supersample
when drawing, so it stays readable at fractional scales.

    uv run --with pillow tools/make_font.py examples/font.hpp
"""

import sys

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 16, 32
FIRST, LAST = 32, 126
FACES = (
    ("/System/Library/Fonts/Menlo.ttc", 26),
    ("/System/Library/Fonts/Supplemental/Courier New Bold.ttf", 28),
)


def face():
    for path, size in FACES:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    raise SystemExit("no usable monospace font found")


def glyph(font, code):
    """Return HEIGHT rows of WIDTH bits for one character."""
    image = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    left, top, right, bottom = draw.textbbox((0, 0), chr(code), font=font)
    draw.text(((WIDTH - (right - left)) / 2 - left, 2), chr(code),
              font=font, fill=255)
    pixels = image.load()
    rows = []
    for y in range(HEIGHT):
        bits = 0
        for x in range(WIDTH):
            if pixels[x, y] >= 128:
                bits |= 1 << (WIDTH - 1 - x)
        rows.append(bits)
    return rows


def main(argv):
    if len(argv) != 1:
        print(__doc__.strip())
        return 2
    font = face()
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace font {",
        "",
        "inline constexpr int glyphWidth = %d;" % WIDTH,
        "inline constexpr int glyphHeight = %d;" % HEIGHT,
        "inline constexpr int firstGlyph = %d;" % FIRST,
        "inline constexpr int lastGlyph = %d;" % LAST,
        "",
        "inline constexpr std::uint16_t glyphs[][%d] = {" % HEIGHT,
    ]
    for code in range(FIRST, LAST + 1):
        rows = ", ".join("0x%04x" % bits for bits in glyph(font, code))
        lines.append("    {%s}," % rows)
    lines.append("};")
    lines.append("")
    lines.append("}")
    lines.append("")
    open(argv[0], "w").write("\n".join(lines))
    print("%s: %d glyphs at %dx%d" % (argv[0], LAST - FIRST + 1, WIDTH, HEIGHT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
