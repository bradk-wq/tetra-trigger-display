#include "tetra/encoder.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <utility>

#include "tetra/device.hpp"

namespace tetra {
namespace {

int turboFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::rgb:
        return TJPF_RGB;
    case PixelFormat::bgr:
        return TJPF_BGR;
    case PixelFormat::rgbx:
        return TJPF_RGBX;
    case PixelFormat::bgrx:
        return TJPF_BGRX;
    case PixelFormat::xrgb:
        return TJPF_XRGB;
    case PixelFormat::xbgr:
        return TJPF_XBGR;
    case PixelFormat::grey:
        return TJPF_GRAY;
    }
    return TJPF_RGB;
}

}

Encoder::Encoder() {
    handle_ = tj3Init(TJINIT_COMPRESS);
    if (handle_ == nullptr) {
        throw Error("could not create a JPEG encoder");
    }
}

Encoder::~Encoder() {
    release();
}

Encoder::Encoder(Encoder &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      buffer_(std::exchange(other.buffer_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)),
      quality_(other.quality_) {}

Encoder &Encoder::operator=(Encoder &&other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
        buffer_ = std::exchange(other.buffer_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        quality_ = other.quality_;
    }
    return *this;
}

void Encoder::release() noexcept {
    if (buffer_ != nullptr) {
        tj3Free(buffer_);
        buffer_ = nullptr;
        capacity_ = 0;
    }
    if (handle_ != nullptr) {
        tj3Destroy(handle_);
        handle_ = nullptr;
    }
}

void Encoder::setQuality(int quality) {
    quality_ = std::clamp(quality, 1, 100);
}

EncodedImage Encoder::encode(const Surface &surface) {
    if (surface.pixels == nullptr || surface.width <= 0
        || surface.height <= 0) {
        throw Error("cannot encode an empty surface");
    }

    tj3Set(handle_, TJPARAM_QUALITY, quality_);
    tj3Set(handle_, TJPARAM_SUBSAMP, TJSAMP_420);
    tj3Set(handle_, TJPARAM_NOREALLOC, 0);

    std::size_t size = capacity_;
    int code = tj3Compress8(handle_, surface.pixels, surface.width,
                            surface.pitch, surface.height,
                            turboFormat(surface.format), &buffer_, &size);
    if (code != 0) {
        throw Error(std::string("JPEG encode failed: ") + tj3GetErrorStr(handle_));
    }
    capacity_ = std::max(capacity_, size);
    return {buffer_, size};
}

}
