#pragma once

#include <cstddef>
#include <cstdint>

#include "tetra/surface.hpp"

namespace tetra {

struct EncodedImage {
    const std::uint8_t *data;
    std::size_t size;
};

/// Baseline 4:2:0 JPEG encoder, which is what the adapter's decoder expects.
class Encoder {
public:
    Encoder();
    ~Encoder();

    Encoder(Encoder &&other) noexcept;
    Encoder &operator=(Encoder &&other) noexcept;
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    void setQuality(int quality);
    int quality() const { return quality_; }

    /// Encode into an internal buffer, valid until the next call.
    EncodedImage encode(const Surface &surface);

private:
    void release() noexcept;

    void *handle_ = nullptr;
    std::uint8_t *buffer_ = nullptr;
    std::size_t capacity_ = 0;
    int quality_ = 85;
};

}
