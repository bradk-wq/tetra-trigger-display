#!/usr/bin/env python3
"""Trace a black-on-transparent logo into polygon outlines.

Emits a header of normalised contours for even-odd filling, so the examples
carry vector outlines rather than a bundled bitmap.

    uv run --with pillow --with numpy --with scikit-image \
        tools/trace_logo.py logo.png examples/logo.hpp
"""

import sys

import numpy as np
from PIL import Image
from skimage import measure

EPSILON = 0.4


def ink(path):
    """Return a boolean mask of the logo's dark pixels, alpha composited."""
    image = Image.open(path).convert("RGBA")
    pixels = np.asarray(image).astype(np.float32) / 255.0
    alpha = pixels[:, :, 3]
    luma = pixels[:, :, :3] @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    return (luma * alpha + (1.0 - alpha)) < 0.5


def simplify(points, epsilon):
    """Ramer-Douglas-Peucker on an open polyline."""
    if len(points) < 3:
        return points
    start, end = points[0], points[-1]
    line = end - start
    length = np.hypot(*line)
    offset = points - start
    if length == 0:
        distances = np.hypot(offset[:, 0], offset[:, 1])
    else:
        distances = np.abs(line[0] * offset[:, 1]
                           - line[1] * offset[:, 0]) / length
    index = int(np.argmax(distances))
    if distances[index] <= epsilon:
        return np.array([start, end])
    left = simplify(points[:index + 1], epsilon)
    right = simplify(points[index:], epsilon)
    return np.vstack([left[:-1], right])


def contours(mask, epsilon):
    """Return simplified closed contours in pixel coordinates."""
    padded = np.pad(mask.astype(np.float32), 1)
    traced = []
    for contour in measure.find_contours(padded, 0.5):
        points = simplify(contour[:, ::-1] - 1.0, epsilon)
        if len(points) > 2 and np.allclose(points[0], points[-1]):
            points = points[:-1]
        if len(points) >= 3:
            traced.append(points)
    return traced


def emit(traced, width, height, out):
    """Write the contours as a C++ header, normalised to unit width."""
    scale = 1.0 / width
    lines = [
        "/*",
        " * The Mobile Pixels logo, traced from their published artwork.",
        " *",
        " * The mark belongs to Mobile Pixels Inc. It is reproduced here only to",
        " * demonstrate this library driving a Mobile Pixels panel. Regenerate this",
        " * file from other artwork with tools/trace_logo.py to use something else.",
        " */",
        "",
        "#pragma once",
        "",
        "namespace logo {",
        "",
        "struct Contour {",
        "    const float *xy;",
        "    int count;",
        "};",
        "",
        "inline constexpr float aspect = %.6ff;" % (height / float(width)),
        "",
    ]
    for index, points in enumerate(traced):
        values = ", ".join("%.5ff, %.5ff" % (x * scale, y * scale)
                           for x, y in points)
        lines.append("inline constexpr float contour%d[] = {%s};" % (index, values))
    lines.append("")
    lines.append("inline constexpr Contour contours[] = {")
    for index, points in enumerate(traced):
        lines.append("    {contour%d, %d}," % (index, len(points)))
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr int contourCount = %d;" % len(traced))
    lines.append("")
    lines.append("}")
    lines.append("")
    open(out, "w").write("\n".join(lines))


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    mask = ink(argv[0])
    height, width = mask.shape
    traced = contours(mask, EPSILON)
    emit(traced, width, height, argv[1])
    print("%s: %d contours, %d points, %dx%d"
          % (argv[1], len(traced), sum(len(p) for p in traced), width, height))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
