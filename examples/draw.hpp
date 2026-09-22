#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tetra/surface.hpp"

namespace draw {

struct Colour {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

inline constexpr Colour black{0, 0, 0};
inline constexpr Colour white{255, 255, 255};
inline constexpr Colour grey{128, 128, 128};

/// A closed outline as interleaved x, y pairs. Several together fill even-odd,
/// so counters and holes come out right.
struct Polygon {
    const float *xy = nullptr;
    int count = 0;
};

/// Packed 24-bit RGB target with a small antialiased rasteriser.
class Canvas {
public:
    Canvas(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    std::uint8_t *pixels() { return pixels_.data(); }
    const std::uint8_t *pixels() const { return pixels_.data(); }
    int pitch() const { return width_ * 3; }

    tetra::Surface surface() const;

    void clear(Colour colour);
    void rect(float x, float y, float width, float height, Colour colour);
    void disc(float centreX, float centreY, float radius, Colour colour);
    void ring(float centreX, float centreY, float radius, float thickness,
              Colour colour);
    void fill(const Polygon *polygons, int count, float scale, float offsetX,
              float offsetY, Colour colour);

    void text(float x, float y, float scale, Colour colour,
              const std::string &value);
    float textWidth(float scale, const std::string &value) const;
    float textHeight(float scale) const;

    /// Nearest-neighbour blit of a packed RGB image into the given box.
    void blit(const std::uint8_t *source, int sourceWidth, int sourceHeight,
              int sourcePitch, float x, float y, float width, float height);

private:
    void blend(int x, int y, Colour colour, float coverage);

    std::vector<std::uint8_t> pixels_;
    int width_ = 0;
    int height_ = 0;
};

}
