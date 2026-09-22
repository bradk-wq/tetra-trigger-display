#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image {

struct Rgb {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;

    int pitch() const { return width * 3; }
    bool empty() const { return pixels.empty(); }
};

std::vector<std::uint8_t> readFile(const std::string &path);

/// Decode a JPEG to packed RGB. Throws tetra::Error on failure.
Rgb decodeJpeg(const std::uint8_t *data, std::size_t size);
Rgb loadJpeg(const std::string &path);

/// Read the dimensions without decoding, for the pass-through fast path.
bool jpegSize(const std::uint8_t *data, std::size_t size, int &width,
              int &height);

/// JPEG files in a directory, sorted by name.
std::vector<std::string> listJpegs(const std::string &directory);

}
