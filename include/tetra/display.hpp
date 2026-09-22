#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tetra/device.hpp"
#include "tetra/encoder.hpp"
#include "tetra/protocol.hpp"
#include "tetra/surface.hpp"

namespace tetra {

struct Mode {
    int width = 0;
    int height = 0;
    int refreshHz = 0;
    bool fromEdid = false;
    protocol::Timing timing{};

    std::string describe() const;
};

/// How present() paces itself.
///
/// The adapter has no vblank signal. Its interrupt endpoint coalesces, so it
/// reports a fence for roughly one frame in three and cannot be used to pace
/// against the panel. Matching the mode's refresh rate is the best available
/// approximation: it does not remove tearing, but it stops frames landing
/// several times per scanout.
enum class Pacing {
    /// Send and return, as fast as the caller can produce frames.
    immediate,
    /// Hold each frame until the mode's refresh interval has elapsed.
    refreshRate,
};

struct Fence {
    bool seen = false;
    bool decoderError = false;
    std::uint32_t completedId = 0;
};

/// What the last present() actually sent.
struct PresentStats {
    int regions = 0;
    std::size_t encodedBytes = 0;
    std::size_t pixels = 0;
    bool fullFrame = false;
};

/// A Trigger 6 attached panel, driven as a JPEG framebuffer.
///
/// Construction opens the device and sends the software-ready command, which
/// is what takes the adapter out of the idle state it powers up in. Nothing
/// appears until setMode and setPowered(true).
class Display {
public:
    explicit Display(int index = 0);

    Display(Display &&) noexcept = default;
    Display &operator=(Display &&) noexcept = default;
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;

    const std::string &product() const { return device_.product(); }
    const std::string &projectCode() const { return projectCode_; }
    int vramMegabytes() const { return vramMegabytes_; }
    bool panelAttached();
    const std::vector<std::uint8_t> &edid() const { return edid_; }

    /// Modes the adapter advertises, plus the panel's own EDID timing when
    /// that is not already among them.
    const std::vector<Mode> &modes() const { return modes_; }
    std::optional<Mode> nativeMode() const;
    std::optional<Mode> findMode(int width, int height) const;

    void setMode(const Mode &mode);
    void setPowered(bool powered);

    int width() const { return width_; }
    int height() const { return height_; }

    void setQuality(int quality) { encoder_.setQuality(quality); }
    int quality() const { return encoder_.quality(); }

    void setPacing(Pacing pacing) { pacing_ = pacing; }
    Pacing pacing() const { return pacing_; }

    /// Send only what changed.
    ///
    /// With this on, present(surface) compares each frame against the last and
    /// submits just the regions that differ; present(surface, rects) sends the
    /// regions you name, which is cheaper still. A mostly static screen then
    /// costs almost nothing.
    ///
    /// Note for anyone reading the wire format: a clip is placed by its
    /// destination address, not by the startX and startY fields of the clip
    /// header. Those are honoured for size but ignored for position.
    void setPartialUpdates(bool enabled);
    bool partialUpdates() const { return partialUpdates_; }

    /// Cap on regions per frame before they are coalesced into one. Default 4.
    void setMaxDirtyRegions(int count);


    void present(const Surface &surface);

    /// Present with the changed region supplied by the caller, which is always
    /// cheaper than letting the library work it out.
    void present(const Surface &surface, const Rect &dirty);
    void present(const Surface &surface, const Rect *regions, int count);

    /// Send a whole frame regardless of what changed.
    void presentFull(const Surface &surface);
    void presentJpeg(const void *data, std::size_t size);

    const PresentStats &lastPresent() const { return lastPresent_; }

    /// Tag frames with incrementing fence ids. Off by default.
    void setFenceReporting(bool enabled) { fenceReporting_ = enabled; }

    /// Block until the adapter reports the last presented frame complete.
    /// Advisory only: fences are coalesced, so this is for diagnostics, not
    /// for pacing a render loop.
    bool waitForFence(unsigned timeoutMs = 100);

    /// Drain pending interrupt packets, reporting frame completion.
    Fence poll(unsigned timeoutMs = 0);

    Device &device() { return device_; }

private:
    void readCapabilities();
    void readModes();
    void softwareReady();
    void pace();
    bool beginSubmission(std::size_t total);
    void encodeRegion(const Surface &surface, const Rect &region,
                      std::uint32_t frame);
    void emitPresentFlip(std::uint32_t frame);
    void syncBothBuffers(const Surface &surface);
    int diff(const Surface &surface, Rect *regions, int limit);
    void remember(const Surface &surface);

    Device device_;
    Encoder encoder_;
    std::string projectCode_;
    std::vector<std::uint8_t> edid_;
    std::vector<Mode> modes_;
    std::vector<std::uint8_t> payload_;

    int vramMegabytes_ = 0;
    int platformVersion_ = 0;
    int width_ = 0;
    int height_ = 0;

    std::uint32_t commandBase_ = 0;
    std::uint32_t commandAddress_ = 0;
    std::uint32_t frameAddress_[2] = {0, 0};
    int frameSlot_ = 0;
    std::uint32_t liveFrame_ = 0;
    int framesSent_ = 0;
    std::uint32_t fenceCounter_ = 0;
    bool fenceReporting_ = false;
    std::uint32_t pendingFence_ = 0;
    std::uint32_t completedFence_ = 0;
    Pacing pacing_ = Pacing::immediate;
    int refreshHz_ = 60;
    std::chrono::steady_clock::time_point nextFrame_{};

    bool partialUpdates_ = false;
    int maxDirtyRegions_ = 4;
    bool haveFullFrame_ = false;
    bool previousValid_ = false;
    std::vector<std::uint8_t> previous_;
    std::vector<Rect> pending_[2];
    bool buffersSynced_ = false;
    int previousWidth_ = 0;
    int previousHeight_ = 0;
    PixelFormat previousFormat_ = PixelFormat::rgb;
    PresentStats lastPresent_{};
};

}
