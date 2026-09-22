#pragma once

#include <cstdint>

namespace tetra {

/// Byte order in memory, not packed-word order.
///
/// SDL's packed formats are little-endian words, so SDL_PIXELFORMAT_ARGB8888
/// is bgrx here and SDL_PIXELFORMAT_ABGR8888 is rgbx.
enum class PixelFormat {
    rgb,
    bgr,
    rgbx,
    bgrx,
    xrgb,
    xbgr,
    grey,
};

int bytesPerPixel(PixelFormat format);

/// An axis-aligned region of a frame, in pixels.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
    int right() const { return x + width; }
    int bottom() const { return y + height; }

    Rect unionWith(const Rect &other) const;
    Rect clampTo(int boundsWidth, int boundsHeight) const;

    /// Grow outward until the edges land on multiples of `to`.
    Rect alignOut(int to) const;
};

/// Non-owning view of pixel data to be presented.
struct Surface {
    const std::uint8_t *pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
    PixelFormat format = PixelFormat::rgb;

    Surface() = default;

    Surface(const std::uint8_t *pixels, int width, int height, int pitch,
            PixelFormat format)
        : pixels(pixels), width(width), height(height), pitch(pitch),
          format(format) {}

    Surface(const std::uint8_t *pixels, int width, int height,
            PixelFormat format)
        : Surface(pixels, width, height, width * bytesPerPixel(format),
                  format) {}
};

}
