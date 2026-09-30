#pragma once

#include <cstdint>
#include <vector>

#include "wire.hpp"

namespace tetra::wireless {

/// Puts frame fragments back together, newest frame wins.
///
/// Only one frame is ever in flight here. A fragment of a newer frame abandons
/// the incomplete one; a fragment of anything older than what was already
/// completed or started is dropped as stale. There is deliberately no queue:
/// every frame is a whole picture, so skipping one costs nothing but time.
class Reassembler {
public:
    struct Frame {
        std::uint32_t seq = 0;
        std::uint64_t sendNs = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::vector<std::uint8_t> data;
    };

    struct Counters {
        std::uint64_t completed = 0;
        std::uint64_t abandoned = 0;  // replaced by a newer frame mid-assembly
        std::uint64_t stale = 0;      // fragment of an already-superseded frame
        std::uint64_t duplicate = 0;
        std::uint64_t malformed = 0;
    };

    /// Feed one fragment. Returns true when it completed a frame into `out`.
    bool add(const FrameHeader &header, const std::uint8_t *payload,
             std::size_t length, Frame &out) {
        if (!valid(header, length)) {
            ++counters_.malformed;
            return false;
        }
        if (haveDone_ && !newer(header.seq, doneSeq_)) {
            ++counters_.stale;
            return false;
        }
        if (active_ && header.seq != seq_) {
            if (!newer(header.seq, seq_)) {
                ++counters_.stale;
                return false;
            }
            ++counters_.abandoned;
            active_ = false;
        }
        if (!active_) {
            start(header);
        } else if (header.frameSize != size_ || header.fragCount != count_) {
            ++counters_.malformed;
            return false;
        }

        if (got_[header.fragIndex]) {
            ++counters_.duplicate;
            return false;
        }
        got_[header.fragIndex] = true;
        std::memcpy(buffer_.data() + header.fragIndex * fragmentPayload,
                    payload, length);
        if (--remaining_ != 0) {
            return false;
        }

        out.seq = seq_;
        out.sendNs = sendNs_;
        out.width = width_;
        out.height = height_;
        out.data = std::move(buffer_);
        buffer_.clear();
        active_ = false;
        haveDone_ = true;
        doneSeq_ = seq_;
        ++counters_.completed;
        return true;
    }

    const Counters &counters() const { return counters_; }

private:
    /// Wrap-safe: true when `a` is later than `b`.
    static bool newer(std::uint32_t a, std::uint32_t b) {
        return static_cast<std::int32_t>(a - b) > 0;
    }

    static bool valid(const FrameHeader &h, std::size_t length) {
        if (h.fragCount == 0 || h.fragCount > maxFragments
            || h.fragIndex >= h.fragCount || h.frameSize == 0
            || h.frameSize > maxFrameBytes || h.width == 0 || h.height == 0) {
            return false;
        }
        const std::size_t before =
            static_cast<std::size_t>(h.fragCount - 1) * fragmentPayload;
        if (h.frameSize <= before || h.frameSize > before + fragmentPayload) {
            return false;
        }
        const std::size_t expected =
            h.fragIndex + 1 == h.fragCount ? h.frameSize - before
                                           : fragmentPayload;
        return length == expected;
    }

    void start(const FrameHeader &h) {
        active_ = true;
        seq_ = h.seq;
        sendNs_ = h.sendNs;
        size_ = h.frameSize;
        count_ = h.fragCount;
        width_ = h.width;
        height_ = h.height;
        remaining_ = h.fragCount;
        buffer_.assign(h.frameSize, 0);
        got_.assign(h.fragCount, false);
    }

    bool active_ = false;
    bool haveDone_ = false;
    std::uint32_t seq_ = 0;
    std::uint32_t doneSeq_ = 0;
    std::uint64_t sendNs_ = 0;
    std::uint32_t size_ = 0;
    std::uint16_t count_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::size_t remaining_ = 0;
    std::vector<std::uint8_t> buffer_;
    std::vector<bool> got_;
    Counters counters_;
};

}
