#include "tetra/surface.hpp"

#include <algorithm>

namespace tetra {

int bytesPerPixel(PixelFormat format) {
    switch (format) {
    case PixelFormat::rgb:
    case PixelFormat::bgr:
        return 3;
    case PixelFormat::grey:
        return 1;
    default:
        return 4;
    }
}

Rect Rect::unionWith(const Rect &other) const {
    if (empty()) {
        return other;
    }
    if (other.empty()) {
        return *this;
    }
    const int left = std::min(x, other.x);
    const int top = std::min(y, other.y);
    const int right = std::max(this->right(), other.right());
    const int bottom = std::max(this->bottom(), other.bottom());
    return Rect{left, top, right - left, bottom - top};
}

Rect Rect::clampTo(int boundsWidth, int boundsHeight) const {
    const int left = std::clamp(x, 0, boundsWidth);
    const int top = std::clamp(y, 0, boundsHeight);
    const int right = std::clamp(this->right(), left, boundsWidth);
    const int bottom = std::clamp(this->bottom(), top, boundsHeight);
    return Rect{left, top, right - left, bottom - top};
}

Rect Rect::alignOut(int to) const {
    if (to <= 1 || empty()) {
        return *this;
    }
    const int left = x / to * to;
    const int top = y / to * to;
    const int right = (this->right() + to - 1) / to * to;
    const int bottom = (this->bottom() + to - 1) / to * to;
    return Rect{left, top, right - left, bottom - top};
}

}
