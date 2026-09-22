#include "draw.hpp"

#include <algorithm>
#include <cmath>

#include "font.hpp"

namespace draw {
namespace {

constexpr int subSamples = 4;

float clampUnit(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

}

Canvas::Canvas(int width, int height)
    : pixels_(static_cast<std::size_t>(width) * height * 3, 0),
      width_(width), height_(height) {}

tetra::Surface Canvas::surface() const {
    return tetra::Surface(pixels_.data(), width_, height_, width_ * 3,
                          tetra::PixelFormat::rgb);
}

void Canvas::blend(int x, int y, Colour colour, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || coverage <= 0.0f) {
        return;
    }
    coverage = clampUnit(coverage);
    std::uint8_t *pixel = pixels_.data() + (static_cast<std::size_t>(y) * width_ + x) * 3;
    pixel[0] = static_cast<std::uint8_t>(std::lround(pixel[0] + (colour.r - pixel[0]) * coverage));
    pixel[1] = static_cast<std::uint8_t>(std::lround(pixel[1] + (colour.g - pixel[1]) * coverage));
    pixel[2] = static_cast<std::uint8_t>(std::lround(pixel[2] + (colour.b - pixel[2]) * coverage));
}

void Canvas::clear(Colour colour) {
    for (std::size_t i = 0; i < pixels_.size(); i += 3) {
        pixels_[i] = colour.r;
        pixels_[i + 1] = colour.g;
        pixels_[i + 2] = colour.b;
    }
}

void Canvas::rect(float x, float y, float width, float height, Colour colour) {
    const int left = std::max(0, static_cast<int>(std::floor(x)));
    const int top = std::max(0, static_cast<int>(std::floor(y)));
    const int right = std::min(width_, static_cast<int>(std::ceil(x + width)));
    const int bottom = std::min(height_, static_cast<int>(std::ceil(y + height)));
    for (int row = top; row < bottom; ++row) {
        const float overlapY = std::min(y + height, row + 1.0f) - std::max(y, static_cast<float>(row));
        for (int column = left; column < right; ++column) {
            const float overlapX = std::min(x + width, column + 1.0f) - std::max(x, static_cast<float>(column));
            blend(column, row, colour, clampUnit(overlapX) * clampUnit(overlapY));
        }
    }
}

void Canvas::ring(float centreX, float centreY, float radius, float thickness,
                  Colour colour) {
    const float outer = radius + thickness * 0.5f;
    const float inner = radius - thickness * 0.5f;
    const int left = std::max(0, static_cast<int>(std::floor(centreX - outer - 1)));
    const int top = std::max(0, static_cast<int>(std::floor(centreY - outer - 1)));
    const int right = std::min(width_, static_cast<int>(std::ceil(centreX + outer + 1)));
    const int bottom = std::min(height_, static_cast<int>(std::ceil(centreY + outer + 1)));
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            const float dx = column + 0.5f - centreX;
            const float dy = row + 0.5f - centreY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage =
                clampUnit(outer - distance + 0.5f) * clampUnit(distance - inner + 0.5f);
            blend(column, row, colour, coverage);
        }
    }
}

void Canvas::disc(float centreX, float centreY, float radius, Colour colour) {
    const int left = std::max(0, static_cast<int>(std::floor(centreX - radius - 1)));
    const int top = std::max(0, static_cast<int>(std::floor(centreY - radius - 1)));
    const int right = std::min(width_, static_cast<int>(std::ceil(centreX + radius + 1)));
    const int bottom = std::min(height_, static_cast<int>(std::ceil(centreY + radius + 1)));
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            const float dx = column + 0.5f - centreX;
            const float dy = row + 0.5f - centreY;
            blend(column, row, colour,
                  clampUnit(radius - std::sqrt(dx * dx + dy * dy) + 0.5f));
        }
    }
}

void Canvas::fill(const Polygon *polygons, int count, float scale,
                  float offsetX, float offsetY, Colour colour) {
    float minimumY = 1e30f;
    float maximumY = -1e30f;
    for (int i = 0; i < count; ++i) {
        for (int p = 0; p < polygons[i].count; ++p) {
            const float y = polygons[i].xy[p * 2 + 1] * scale + offsetY;
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
        }
    }
    if (minimumY > maximumY) {
        return;
    }

    const int top = std::max(0, static_cast<int>(std::floor(minimumY)));
    const int bottom = std::min(height_, static_cast<int>(std::ceil(maximumY)) + 1);

    std::vector<float> coverage(static_cast<std::size_t>(width_));
    std::vector<float> crossings;

    for (int row = top; row < bottom; ++row) {
        std::fill(coverage.begin(), coverage.end(), 0.0f);
        for (int sub = 0; sub < subSamples; ++sub) {
            const float sampleY = row + (sub + 0.5f) / subSamples;
            crossings.clear();
            for (int i = 0; i < count; ++i) {
                const Polygon &polygon = polygons[i];
                for (int p = 0; p < polygon.count; ++p) {
                    const int q = (p + 1) % polygon.count;
                    const float ax = polygon.xy[p * 2] * scale + offsetX;
                    const float ay = polygon.xy[p * 2 + 1] * scale + offsetY;
                    const float bx = polygon.xy[q * 2] * scale + offsetX;
                    const float by = polygon.xy[q * 2 + 1] * scale + offsetY;
                    if ((ay <= sampleY) == (by <= sampleY)) {
                        continue;
                    }
                    crossings.push_back(ax + (sampleY - ay) / (by - ay) * (bx - ax));
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const float start = std::max(crossings[i], 0.0f);
                const float end = std::min(crossings[i + 1], static_cast<float>(width_));
                if (end <= start) {
                    continue;
                }
                const int first = static_cast<int>(std::floor(start));
                const int last = std::min(width_ - 1, static_cast<int>(std::ceil(end)) - 1);
                for (int column = first; column <= last; ++column) {
                    const float overlap = std::min(end, column + 1.0f)
                                          - std::max(start, static_cast<float>(column));
                    if (overlap > 0.0f) {
                        coverage[static_cast<std::size_t>(column)] +=
                            overlap / subSamples;
                    }
                }
            }
        }
        for (int column = 0; column < width_; ++column) {
            blend(column, row, colour, coverage[static_cast<std::size_t>(column)]);
        }
    }
}

float Canvas::textWidth(float scale, const std::string &value) const {
    return static_cast<float>(value.size()) * font::glyphWidth * scale;
}

float Canvas::textHeight(float scale) const {
    return font::glyphHeight * scale;
}

void Canvas::text(float x, float y, float scale, Colour colour,
                  const std::string &value) {
    for (char character : value) {
        const int code = static_cast<unsigned char>(character);
        if (code >= font::firstGlyph && code <= font::lastGlyph) {
            const auto &rows = font::glyphs[code - font::firstGlyph];
            const int left = static_cast<int>(std::floor(x));
            const int top = static_cast<int>(std::floor(y));
            const int right = static_cast<int>(std::ceil(x + font::glyphWidth * scale));
            const int bottom = static_cast<int>(std::ceil(y + font::glyphHeight * scale));
            for (int row = std::max(0, top); row < std::min(height_, bottom); ++row) {
                for (int column = std::max(0, left); column < std::min(width_, right); ++column) {
                    float hits = 0.0f;
                    for (int sy = 0; sy < 2; ++sy) {
                        for (int sx = 0; sx < 2; ++sx) {
                            const float sampleX = (column + (sx + 0.5f) / 2 - x) / scale;
                            const float sampleY = (row + (sy + 0.5f) / 2 - y) / scale;
                            const int gx = static_cast<int>(sampleX);
                            const int gy = static_cast<int>(sampleY);
                            if (sampleX < 0 || sampleY < 0 || gx >= font::glyphWidth
                                || gy >= font::glyphHeight) {
                                continue;
                            }
                            if ((rows[gy] >> (font::glyphWidth - 1 - gx)) & 1) {
                                hits += 0.25f;
                            }
                        }
                    }
                    blend(column, row, colour, hits);
                }
            }
        }
        x += font::glyphWidth * scale;
    }
}

void Canvas::blit(const std::uint8_t *source, int sourceWidth, int sourceHeight,
                  int sourcePitch, float x, float y, float width, float height) {
    if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0) {
        return;
    }
    const int left = std::max(0, static_cast<int>(std::floor(x)));
    const int top = std::max(0, static_cast<int>(std::floor(y)));
    const int right = std::min(width_, static_cast<int>(std::ceil(x + width)));
    const int bottom = std::min(height_, static_cast<int>(std::ceil(y + height)));
    for (int row = top; row < bottom; ++row) {
        const int sourceY = std::clamp(
            static_cast<int>((row + 0.5f - y) / height * sourceHeight), 0,
            sourceHeight - 1);
        for (int column = left; column < right; ++column) {
            const int sourceX = std::clamp(
                static_cast<int>((column + 0.5f - x) / width * sourceWidth), 0,
                sourceWidth - 1);
            const std::uint8_t *texel = source + static_cast<std::size_t>(sourceY) * sourcePitch + sourceX * 3;
            std::uint8_t *pixel = pixels_.data() + (static_cast<std::size_t>(row) * width_ + column) * 3;
            pixel[0] = texel[0];
            pixel[1] = texel[1];
            pixel[2] = texel[2];
        }
    }
}

}
