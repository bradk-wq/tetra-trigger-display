#!/usr/bin/env python3
"""Render a BBC Test Card F style pattern.

The card is drawn 4:3 and pillarboxed onto the target resolution, which is how
it would have been presented on a wider display. --fill stretches it to the
target aspect instead, which is useful for checking the panel edges.

    uv run --with pillow testcard.py 1280x800 testcard.jpg
    uv run --with pillow testcard.py --fill 1280x800 testcard.jpg
"""

import sys

from PIL import Image, ImageDraw, ImageFont

GREY = (128, 128, 128)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
BARS = (
    (255, 255, 0),
    (0, 255, 255),
    (0, 255, 0),
    (255, 0, 255),
    (255, 0, 0),
    (0, 0, 255),
)
FONTS = (
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
)


def font(size):
    """Return a truetype face at size, or the bitmap default if none load."""
    for path in FONTS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def centred(draw, box, text, face, fill=WHITE):
    """Draw text centred in box = (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    left, top, right, bottom = draw.textbbox((0, 0), text, font=face)
    draw.text(
        (x0 + (x1 - x0 - (right - left)) / 2 - left,
         y0 + (y1 - y0 - (bottom - top)) / 2 - top),
        text, font=face, fill=fill)


def castellations(draw, w, h, t):
    """Draw the alternating black and white border blocks."""
    step = w / 16.0
    for i in range(16):
        fill = WHITE if i % 2 else BLACK
        draw.rectangle((i * step, 0, (i + 1) * step, t), fill=fill)
        draw.rectangle((i * step, h - t, (i + 1) * step, h), fill=fill)
    step = h / 12.0
    for i in range(12):
        fill = BLACK if i % 2 else WHITE
        draw.rectangle((0, i * step, t, (i + 1) * step), fill=fill)
        draw.rectangle((w - t, i * step, w, (i + 1) * step), fill=fill)


def grid(draw, w, h, t, pitch, width):
    """Draw the white graticule across the inner area."""
    x = t + pitch
    while x < w - t:
        draw.line((x, t, x, h - t), fill=WHITE, width=width)
        x += pitch
    y = t + pitch
    while y < h - t:
        draw.line((t, y, w - t, y), fill=WHITE, width=width)
        y += pitch


def gratings(draw, box, pitches):
    """Draw vertical line bursts of increasing pitch across box."""
    x0, y0, x1, y1 = box
    span = (x1 - x0) / float(len(pitches))
    for i, pitch in enumerate(pitches):
        left = x0 + i * span
        draw.rectangle((left, y0, left + span, y1), fill=BLACK)
        x = left
        while x < left + span - pitch:
            draw.rectangle((x, y0, x + pitch, y1), fill=WHITE)
            x += pitch * 2


def greyscale(draw, box, steps=8):
    """Draw a black to white step wedge across box."""
    x0, y0, x1, y1 = box
    span = (x1 - x0) / float(steps)
    for i in range(steps):
        level = int(round(255.0 * i / (steps - 1)))
        draw.rectangle((x0 + i * span, y0, x0 + (i + 1) * span, y1),
                       fill=(level, level, level))


def colourbars(draw, box):
    """Draw the six saturated colour bars across box."""
    x0, y0, x1, y1 = box
    span = (x1 - x0) / float(len(BARS))
    for i, colour in enumerate(BARS):
        draw.rectangle((x0 + i * span, y0, x0 + (i + 1) * span, y1), fill=colour)


def card(width, height, label, fill=False):
    """Render the test card into an image of the given size."""
    if fill:
        w, h = width, height
    else:
        w = min(width, int(round(height * 4 / 3.0)))
        h = int(round(w * 3 / 4.0))
    face = Image.new("RGB", (w, h), GREY)
    draw = ImageDraw.Draw(face)

    t = h / 24.0
    line = max(1, int(round(h / 400.0)))
    cx, cy = w / 2.0, h / 2.0
    radius = (h / 2.0 - t) * 0.94

    grid(draw, w, h, t, h / 12.0, line)
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius),
                 outline=WHITE, width=line * 3)

    bw, bh = w * 0.34, h * 0.30
    box = (cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
    draw.rectangle(box, fill=BLACK, outline=WHITE, width=line * 2)
    for i in (1, 2):
        draw.line((box[0] + bw * i / 3, box[1], box[0] + bw * i / 3, box[3]),
                  fill=WHITE, width=line)
        draw.line((box[0], box[1] + bh * i / 3, box[2], box[1] + bh * i / 3),
                  fill=WHITE, width=line)
    centred(draw, box, "BBC", font(int(bh * 0.30)))

    strip = h * 0.09
    colourbars(draw, (cx - bw / 2, box[3] + h * 0.03,
                      cx + bw / 2, box[3] + h * 0.03 + strip))
    greyscale(draw, (cx - bw / 2, box[1] - h * 0.03 - strip,
                     cx + bw / 2, box[1] - h * 0.03))

    gw = w * 0.16
    gratings(draw, (cx - bw / 2 - w * 0.03 - gw, cy - strip / 2,
                    cx - bw / 2 - w * 0.03, cy + strip / 2),
             (line, line * 2, line * 3, line * 4))
    gratings(draw, (cx + bw / 2 + w * 0.03, cy - strip / 2,
                    cx + bw / 2 + w * 0.03 + gw, cy + strip / 2),
             (line * 4, line * 3, line * 2, line))

    centred(draw, (cx - bw, cy - radius * 0.88, cx + bw, cy - radius * 0.68),
            "TETRA", font(int(h * 0.055)))
    centred(draw, (cx - bw, cy + radius * 0.68, cx + bw, cy + radius * 0.88),
            label, font(int(h * 0.045)))

    castellations(draw, w, h, t)

    image = Image.new("RGB", (width, height), BLACK)
    image.paste(face, ((width - w) // 2, (height - h) // 2))
    return image


def main(argv):
    fill = "--fill" in argv
    argv = [a for a in argv if not a.startswith("--")]
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    width, height = (int(v) for v in argv[0].lower().split("x"))
    image = card(width, height, "%d x %d" % (width, height), fill)
    image.save(argv[1], quality=92, subsampling=2, progressive=False)
    print("wrote %s (%dx%d)" % (argv[1], width, height))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
