#include "tetra/display.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace tetra {
namespace {

constexpr std::uint16_t output = 0;
constexpr std::size_t edidBlockSize = 128;
constexpr std::size_t flipSlack = 1024;
constexpr std::uint32_t megabyte = 1024u * 1024u;
constexpr int regionAlignment = 16;

std::uint32_t align32(std::uint32_t value) {
    return (value + 31u) / 32u * 32u;
}

struct Divisors {
    std::uint16_t numerator;
    std::uint16_t denominator;
    std::uint8_t divider;
    std::uint8_t outputSelect;
};

/// Reproduces T6CalculatePixelClock: the adapter wants PLL terms, not the
/// pixel clock, so a timing record taken from the table still has to be
/// patched before it is sent back.
std::optional<Divisors> pixelClockDivisors(std::uint32_t pixelClockKhz,
                                           int baseMhz) {
    if (pixelClockKhz < 25000 || pixelClockKhz > 400000) {
        return std::nullopt;
    }
    const double megahertz = pixelClockKhz / 1000.0;
    const int selects[] = {3, 1, 2, 0};
    double multiplier = 4.0;
    for (int select : selects) {
        if (megahertz * multiplier >= 800.0) {
            const double ratio = megahertz * multiplier / baseMhz;
            const auto whole = static_cast<std::uint32_t>(ratio);
            const auto fraction =
                static_cast<std::uint32_t>((ratio - whole) * 1000.0);
            return Divisors{static_cast<std::uint16_t>(fraction), 1000,
                            static_cast<std::uint8_t>(whole),
                            static_cast<std::uint8_t>(select)};
        }
        multiplier *= 2.0;
    }
    return std::nullopt;
}

bool plausible(const protocol::Timing &timing) {
    return timing.horizontalActive > 0 && timing.horizontalActive <= 4096
           && timing.verticalActive > 0 && timing.verticalActive <= 4096
           && timing.refreshHz >= 1 && timing.refreshHz <= 240
           && timing.horizontalTotal >= timing.horizontalActive
           && timing.verticalTotal >= timing.verticalActive;
}

Mode toMode(const protocol::Timing &timing, bool fromEdid) {
    Mode mode;
    mode.width = timing.horizontalActive;
    mode.height = timing.verticalActive;
    mode.refreshHz = timing.refreshHz;
    mode.fromEdid = fromEdid;
    mode.timing = timing;
    return mode;
}

bool hasEdidHeader(const std::vector<std::uint8_t> &edid) {
    static const std::uint8_t header[8] = {0x00, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0x00};
    return edid.size() >= edidBlockSize
           && std::memcmp(edid.data(), header, sizeof(header)) == 0;
}

/// Build a timing record from the EDID's first detailed timing descriptor.
///
/// The adapter's table only holds standard CEA modes, so a panel whose native
/// mode is absent needs a record flagged as a custom timing.
std::optional<protocol::Timing> edidTiming(
    const std::vector<std::uint8_t> &edid) {
    if (!hasEdidHeader(edid)) {
        return std::nullopt;
    }
    const std::uint8_t *d = edid.data() + 0x36;
    const std::uint32_t clock =
        (static_cast<std::uint32_t>(d[1]) << 8 | d[0]) * 10u;
    if (clock == 0) {
        return std::nullopt;
    }

    const int horizontalActive = d[2] | ((d[4] & 0xf0) << 4);
    const int horizontalBlank = d[3] | ((d[4] & 0x0f) << 8);
    const int verticalActive = d[5] | ((d[7] & 0xf0) << 4);
    const int verticalBlank = d[6] | ((d[7] & 0x0f) << 8);
    const int horizontalOffset = d[8] | ((d[11] & 0xc0) << 2);
    const int horizontalSync = d[9] | ((d[11] & 0x30) << 4);
    const int verticalOffset = (d[10] >> 4) | ((d[11] & 0x0c) << 2);
    const int verticalSync = (d[10] & 0x0f) | ((d[11] & 0x03) << 4);

    const int horizontalTotal = horizontalActive + horizontalBlank;
    const int verticalTotal = verticalActive + verticalBlank;
    if (horizontalTotal <= 0 || verticalTotal <= 0) {
        return std::nullopt;
    }

    protocol::Timing timing{};
    timing.pixelClockKhz = clock;
    timing.refreshHz = static_cast<std::uint8_t>(std::lround(
        clock * 1000.0 / (static_cast<double>(horizontalTotal) * verticalTotal)));
    timing.horizontalTotal = static_cast<std::uint16_t>(horizontalTotal);
    timing.horizontalActive = static_cast<std::uint16_t>(horizontalActive);
    timing.horizontalSyncStart =
        static_cast<std::uint16_t>(horizontalActive + horizontalOffset);
    timing.horizontalSyncWidth = static_cast<std::uint16_t>(horizontalSync);
    timing.verticalTotal = static_cast<std::uint16_t>(verticalTotal);
    timing.verticalActive = static_cast<std::uint16_t>(verticalActive);
    timing.verticalSyncStart =
        static_cast<std::uint16_t>(verticalActive + verticalOffset);
    timing.verticalSyncWidth = static_cast<std::uint16_t>(verticalSync);
    timing.horizontalSyncPolarity = (d[17] & 0x02) ? 1 : 0;
    timing.verticalSyncPolarity = (d[17] & 0x04) ? 1 : 0;
    timing.flags = protocol::timingFlagCustom;
    return timing;
}

}

std::string Mode::describe() const {
    std::string text = std::to_string(width) + "x" + std::to_string(height)
                       + " @" + std::to_string(refreshHz) + "Hz "
                       + std::to_string(timing.pixelClockKhz) + "kHz";
    if (fromEdid) {
        text += " (EDID)";
    }
    return text;
}

Display::Display(int index) : device_(index) {
    readCapabilities();
    softwareReady();
    readModes();

    frameAddress_[0] =
        static_cast<std::uint32_t>(std::max(vramMegabytes_ - 8, 1)) * megabyte;
    frameAddress_[1] =
        static_cast<std::uint32_t>(std::max(vramMegabytes_ - 4, 2)) * megabyte;
    commandBase_ = 0;
    commandAddress_ = commandBase_;
}

void Display::softwareReady() {
    device_.controlOut(protocol::requestSoftwareReady, 0,
                       protocol::signatureDisplay);
}

void Display::readCapabilities() {
    std::uint8_t version[8] = {0};
    device_.controlIn(protocol::requestGetVersion, 0, 0, version, 8);
    platformVersion_ = version[0] | (version[1] << 8) | (version[2] << 16)
                       | (version[3] << 24);

    std::uint8_t code[8] = {0};
    device_.controlIn(protocol::requestGetVersion, 0, 3, code, 8);
    projectCode_.assign(reinterpret_cast<char *>(code), sizeof(code));
    while (!projectCode_.empty()
           && (projectCode_.back() == '\0' || projectCode_.back() == ' ')) {
        projectCode_.pop_back();
    }

    std::uint8_t vram = 0;
    device_.controlIn(protocol::requestGetVramSize, 0, 0, &vram, 1);
    vramMegabytes_ = vram;
    if (vramMegabytes_ < 16) {
        throw Error("implausible VRAM size: " + std::to_string(vramMegabytes_)
                    + " MB");
    }

    edid_.assign(edidBlockSize, 0);
    device_.controlIn(protocol::requestGetEdid, 0, output, edid_.data(),
                      edidBlockSize);
}

bool Display::panelAttached() {
    std::uint8_t connected = 0;
    device_.controlIn(protocol::requestGetConnection, output, 0, &connected, 1);
    return connected != 0;
}

void Display::readModes() {
    modes_.clear();

    std::uint32_t count = 0;
    device_.controlIn(protocol::requestGetResolutionCount, output, 0, &count, 4);
    if (count > 0 && count <= 128) {
        std::vector<protocol::Timing> table(count);
        device_.controlIn(
            protocol::requestGetTimingTable, output, 0, table.data(),
            static_cast<std::uint16_t>(count * sizeof(protocol::Timing)));
        for (const protocol::Timing &timing : table) {
            if (plausible(timing)) {
                modes_.push_back(toMode(timing, false));
            }
        }
    }

    if (std::optional<protocol::Timing> timing = edidTiming(edid_)) {
        const bool listed =
            std::any_of(modes_.begin(), modes_.end(), [&](const Mode &mode) {
                return mode.width == timing->horizontalActive
                       && mode.height == timing->verticalActive;
            });
        if (!listed) {
            modes_.push_back(toMode(*timing, true));
        }
    }
}

std::optional<Mode> Display::nativeMode() const {
    if (!hasEdidHeader(edid_)) {
        return std::nullopt;
    }
    const std::uint8_t *d = edid_.data() + 0x36;
    const int width = d[2] | ((d[4] & 0xf0) << 4);
    const int height = d[5] | ((d[7] & 0xf0) << 4);
    return findMode(width, height);
}

std::optional<Mode> Display::findMode(int width, int height) const {
    auto match = std::find_if(modes_.begin(), modes_.end(), [&](const Mode &m) {
        return m.width == width && m.height == height;
    });
    if (match == modes_.end()) {
        return std::nullopt;
    }
    return *match;
}

void Display::setMode(const Mode &mode) {
    protocol::Timing timing = mode.timing;
    const std::optional<Divisors> divisors =
        pixelClockDivisors(timing.pixelClockKhz, platformVersion_ == 0 ? 48 : 40);
    if (!divisors) {
        throw Error("pixel clock " + std::to_string(timing.pixelClockKhz)
                    + " kHz is outside the PLL range");
    }
    timing.pllNumerator = divisors->numerator;
    timing.pllDenominator = divisors->denominator;
    timing.pllDivider = divisors->divider;
    timing.outputSelect = divisors->outputSelect;

    device_.controlOut(protocol::requestSetDetailTiming, output, 0, &timing,
                       sizeof(timing));
    softwareReady();

    width_ = mode.width;
    height_ = mode.height;
    refreshHz_ = mode.refreshHz > 0 ? mode.refreshHz : 60;
    buffersSynced_ = false;
    pending_[0].clear();
    pending_[1].clear();
    nextFrame_ = std::chrono::steady_clock::now();
    framesSent_ = 0;
    commandAddress_ = commandBase_;
}

void Display::setPowered(bool powered) {
    device_.controlOut(protocol::requestSetMonitor, output, powered ? 1 : 0);
}

void Display::setPartialUpdates(bool enabled) {
    partialUpdates_ = enabled;
    buffersSynced_ = false;
    pending_[0].clear();
    pending_[1].clear();
    previous_.clear();
    previousValid_ = false;
    haveFullFrame_ = false;
}

void Display::setMaxDirtyRegions(int count) {
    maxDirtyRegions_ = std::clamp(count, 1, 64);
}

void Display::pace() {
    if (pacing_ != Pacing::refreshRate) {
        return;
    }
    const auto interval =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(refreshHz_, 1)));
    const auto now = std::chrono::steady_clock::now();
    if (nextFrame_ > now) {
        std::this_thread::sleep_for(nextFrame_ - now);
    }
    nextFrame_ =
        std::max(nextFrame_ + interval, std::chrono::steady_clock::now());
}

void Display::remember(const Surface &surface) {
    const int bpp = bytesPerPixel(surface.format);
    const std::size_t rowBytes = static_cast<std::size_t>(surface.width) * bpp;
    previous_.resize(rowBytes * surface.height);
    for (int y = 0; y < surface.height; ++y) {
        std::memcpy(previous_.data() + y * rowBytes,
                    surface.pixels + static_cast<std::size_t>(y) * surface.pitch,
                    rowBytes);
    }
    previousWidth_ = surface.width;
    previousHeight_ = surface.height;
    previousFormat_ = surface.format;
    previousValid_ = true;
}

int Display::diff(const Surface &surface, Rect *regions, int limit) {
    const int bpp = bytesPerPixel(surface.format);
    const std::size_t rowBytes = static_cast<std::size_t>(surface.width) * bpp;

    std::vector<int> firstColumn(surface.height, -1);
    std::vector<int> lastColumn(surface.height, -1);
    bool any = false;
    for (int y = 0; y < surface.height; ++y) {
        const std::uint8_t *now =
            surface.pixels + static_cast<std::size_t>(y) * surface.pitch;
        const std::uint8_t *was = previous_.data() + y * rowBytes;
        if (std::memcmp(now, was, rowBytes) == 0) {
            continue;
        }
        std::size_t first = 0;
        while (first < rowBytes && now[first] == was[first]) {
            ++first;
        }
        std::size_t last = rowBytes - 1;
        while (last > first && now[last] == was[last]) {
            --last;
        }
        firstColumn[y] = static_cast<int>(first) / bpp;
        lastColumn[y] = static_cast<int>(last) / bpp;
        any = true;
    }
    if (!any) {
        return 0;
    }

    std::vector<Rect> bands;
    for (int y = 0; y < surface.height;) {
        if (firstColumn[y] < 0) {
            ++y;
            continue;
        }
        const int top = y;
        int left = firstColumn[y];
        int right = lastColumn[y];
        while (y < surface.height && firstColumn[y] >= 0) {
            left = std::min(left, firstColumn[y]);
            right = std::max(right, lastColumn[y]);
            ++y;
        }
        bands.push_back(Rect{left, top, right - left + 1, y - top});
    }

    // Every region costs its own JPEG header and a pair of bulk transfers, so
    // merge the pair that grows least until the count fits.
    while (static_cast<int>(bands.size()) > limit) {
        std::size_t best = 0;
        long bestGrowth = -1;
        for (std::size_t i = 0; i + 1 < bands.size(); ++i) {
            const Rect merged = bands[i].unionWith(bands[i + 1]);
            const long growth =
                static_cast<long>(merged.width) * merged.height
                - static_cast<long>(bands[i].width) * bands[i].height
                - static_cast<long>(bands[i + 1].width) * bands[i + 1].height;
            if (bestGrowth < 0 || growth < bestGrowth) {
                bestGrowth = growth;
                best = i;
            }
        }
        bands[best] = bands[best].unionWith(bands[best + 1]);
        bands.erase(bands.begin() + static_cast<long>(best) + 1);
    }

    int count = 0;
    for (const Rect &band : bands) {
        if (count >= limit) {
            break;
        }
        regions[count++] =
            band.alignOut(regionAlignment).clampTo(surface.width, surface.height);
    }
    return count;
}

void Display::present(const Surface &surface) {
    if (!partialUpdates_ || !haveFullFrame_ || !previousValid_
        || surface.width != previousWidth_
        || surface.height != previousHeight_
        || surface.format != previousFormat_) {
        presentFull(surface);
        return;
    }

    std::vector<Rect> regions(static_cast<std::size_t>(maxDirtyRegions_));
    const int count = diff(surface, regions.data(), maxDirtyRegions_);
    if (count == 0) {
        lastPresent_ = PresentStats{};
        return;
    }
    present(surface, regions.data(), count);
    remember(surface);
}

void Display::present(const Surface &surface, const Rect &dirty) {
    present(surface, &dirty, 1);
}

void Display::present(const Surface &surface, const Rect *regions, int count) {
    if (width_ == 0 || height_ == 0) {
        throw Error("set a mode before presenting");
    }
    if (!haveFullFrame_) {
        presentFull(surface);
        return;
    }

    // Each buffer keeps its own outstanding regions, because the back buffer is
    // two flips stale and needs everything that changed since it was last live.
    for (int i = 0; i < count; ++i) {
        const Rect region = regions[i]
                                .alignOut(regionAlignment)
                                .clampTo(surface.width, surface.height);
        if (region.empty()) {
            continue;
        }
        pending_[0].push_back(region);
        pending_[1].push_back(region);
    }

    const int back = frameSlot_ ^ 1;
    long queued = 0;
    for (const Rect &region : pending_[back]) {
        queued += static_cast<long>(region.width) * region.height;
    }
    const long area = static_cast<long>(surface.width) * surface.height;
    if (pending_[back].size() > 32 || queued * 2 > area) {
        presentFull(surface);
        return;
    }
    if (pending_[back].empty()) {
        lastPresent_ = PresentStats{};
        return;
    }

    pace();
    lastPresent_ = PresentStats{};
    const std::uint32_t frame = frameAddress_[back];
    for (const Rect &region : pending_[back]) {
        encodeRegion(surface, region, frame);
        ++lastPresent_.regions;
        lastPresent_.pixels +=
            static_cast<std::size_t>(region.width) * region.height;
    }
    pending_[back].clear();

    emitPresentFlip(frame);
    frameSlot_ = back;
    liveFrame_ = frame;
    previousValid_ = false;
}

void Display::emitPresentFlip(std::uint32_t frame) {
    const std::size_t total = sizeof(protocol::FlipHeader) + flipSlack;
    beginSubmission(total);

    const std::uint32_t pitch = align32(static_cast<std::uint32_t>(width_));
    const std::uint32_t lumaBlock =
        pitch * align32(static_cast<std::uint32_t>(height_)) + 1024u;

    protocol::FlipHeader header{};
    header.command = protocol::videoFlipPrimary;
    header.payloadSize = static_cast<std::uint32_t>(flipSlack);
    header.targetFormat = protocol::colourNv12;
    header.lumaPitch = static_cast<std::uint16_t>(pitch);
    header.chromaPitch = static_cast<std::uint16_t>(pitch);
    header.lumaAddress = frame;
    header.chromaAddress = frame + lumaBlock;
    header.sourceFormat = protocol::colourNv12;

    payload_.assign(total, 0);
    std::memcpy(payload_.data(), &header, sizeof(header));
    device_.bulkOut(payload_.data(), payload_.size());
}

void Display::encodeRegion(const Surface &surface, const Rect &region,
                           std::uint32_t frame) {
    static const bool trace = std::getenv("TETRA_TRACE_CLIPS") != nullptr;
    if (trace) {
        std::fprintf(stderr, "clip x=%4d y=%4d w=%4d h=%4d  end=%4d bottom=%4d\n",
                     region.x, region.y, region.width, region.height,
                     region.x + region.width, region.y + region.height);
    }
    const int bpp = bytesPerPixel(surface.format);
    Surface view;
    view.pixels = surface.pixels
                  + static_cast<std::size_t>(region.y) * surface.pitch
                  + static_cast<std::size_t>(region.x) * bpp;
    view.width = region.width;
    view.height = region.height;
    view.pitch = surface.pitch;
    view.format = surface.format;

    const EncodedImage image = encoder_.encode(view);
    lastPresent_.encodedBytes += image.size;

    const std::size_t total =
        image.size + sizeof(protocol::DirectHeader) + flipSlack;
    const bool reset = beginSubmission(total);

    const std::uint32_t pitch = align32(static_cast<std::uint32_t>(width_));
    const std::uint32_t lumaBlock =
        pitch * align32(static_cast<std::uint32_t>(height_)) + 1024u;
    const std::uint32_t lumaDestination =
        frame + static_cast<std::uint32_t>(region.y) * pitch
        + static_cast<std::uint32_t>(region.x);
    const std::uint32_t chromaDestination =
        frame + lumaBlock + static_cast<std::uint32_t>(region.y / 2) * pitch
        + static_cast<std::uint32_t>(region.x);

    protocol::DirectHeader header{};
    header.command = protocol::videoFramebufferDirect;
    if (fenceReporting_) {
        header.fenceId = ++fenceCounter_;
        pendingFence_ = fenceCounter_;
    }
    header.payloadSize =
        static_cast<std::uint32_t>(total - sizeof(protocol::DirectHeader));
    header.targetFormat = protocol::colourNv12;
    header.width = static_cast<std::uint16_t>(region.width);
    header.height = static_cast<std::uint16_t>(region.height);
    header.lumaPitch = static_cast<std::uint16_t>(pitch);
    header.chromaPitch = static_cast<std::uint16_t>(pitch);
    header.lumaAddress = lumaDestination;
    header.chromaAddress = chromaDestination;
    header.sourceFormat = protocol::colourJpeg;
    header.flags = reset ? protocol::flipFlagResetJpeg : 0;

    payload_.assign(total, 0);
    std::memcpy(payload_.data(), &header, sizeof(header));
    std::memcpy(payload_.data() + sizeof(header), image.data, image.size);
    device_.bulkOut(payload_.data(), payload_.size());
}

void Display::presentFull(const Surface &surface) {
    const EncodedImage image = encoder_.encode(surface);
    presentJpeg(image.data, image.size);

    if (partialUpdates_) {
        if (!buffersSynced_) {
            // Once, on entry, put the same frame in both buffers so later
            // partial updates can flip between them.
            presentJpeg(image.data, image.size);
            buffersSynced_ = true;
            pending_[0].clear();
            pending_[1].clear();
        } else {
            // Otherwise the buffer just flipped to is current and the other
            // one owes a whole frame, which it pays when its turn comes.
            pending_[frameSlot_].clear();
            pending_[frameSlot_ ^ 1].assign(
                1, Rect{0, 0, surface.width, surface.height});
        }
        remember(surface);
    }

    haveFullFrame_ = true;
    lastPresent_ = PresentStats{
        1, image.size,
        static_cast<std::size_t>(surface.width) * surface.height, true};
}

bool Display::beginSubmission(std::size_t total) {
    const std::uint32_t stride =
        total < megabyte ? megabyte
                         : (total < 2 * megabyte ? 2 * megabyte : 3 * megabyte);
    bool reset = false;
    if (commandAddress_ + stride > frameAddress_[0]) {
        commandAddress_ = commandBase_;
        reset = true;
    }
    if (framesSent_ < 10) {
        reset = true;
    }

    protocol::BulkCommand command{};
    command.signature = protocol::signatureDisplay;
    command.payloadLength = static_cast<std::uint32_t>(total);
    command.payloadAddress = commandAddress_;
    command.packetLength = static_cast<std::uint32_t>(total);
    device_.bulkOut(&command, sizeof(command));

    commandAddress_ += stride;
    ++framesSent_;
    return reset;
}

void Display::presentJpeg(const void *data, std::size_t size) {
    if (width_ == 0 || height_ == 0) {
        throw Error("set a mode before presenting");
    }
    pace();

    const std::size_t total = size + sizeof(protocol::FlipHeader) + flipSlack;
    const bool reset = beginSubmission(total);

    // Whole frames always alternate. A clip targets liveFrame_, which is the
    // buffer the last whole frame went to, so it is never stale.
    frameSlot_ ^= 1;
    liveFrame_ = frameAddress_[frameSlot_];

    const std::uint32_t pitch = align32(static_cast<std::uint32_t>(width_));
    const std::uint32_t lumaBlock =
        pitch * align32(static_cast<std::uint32_t>(height_)) + 1024u;

    protocol::FlipHeader header{};
    header.command = protocol::videoFlipPrimary;
    if (fenceReporting_) {
        header.fenceId = ++fenceCounter_;
        pendingFence_ = fenceCounter_;
    }
    header.payloadSize =
        static_cast<std::uint32_t>(total - sizeof(protocol::FlipHeader));
    header.targetFormat = protocol::colourNv12;
    header.lumaPitch = static_cast<std::uint16_t>(pitch);
    header.chromaPitch = static_cast<std::uint16_t>(pitch);
    header.lumaAddress = liveFrame_;
    header.chromaAddress = liveFrame_ + lumaBlock;
    header.sourceFormat = protocol::colourJpeg;
    header.flags = reset ? protocol::flipFlagResetJpeg : 0;

    payload_.assign(total, 0);
    std::memcpy(payload_.data(), &header, sizeof(header));
    std::memcpy(payload_.data() + sizeof(header), data, size);
    device_.bulkOut(payload_.data(), payload_.size());
}

bool Display::waitForFence(unsigned timeoutMs) {
    if (pendingFence_ == 0 || completedFence_ >= pendingFence_) {
        return true;
    }
    // One blocking read at a time. Polling this endpoint in a tight loop
    // makes macOS stall the pipe.
    protocol::InterruptPacket packet{};
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!device_.readInterrupt(&packet, sizeof(packet), timeoutMs)) {
            break;
        }
        if ((packet.functionMask & protocol::interruptMaskDisplay) != 0
            && (packet.displayEvent & protocol::displayEventFenceId) != 0) {
            completedFence_ = packet.displayData;
            if (completedFence_ >= pendingFence_) {
                return true;
            }
        }
    }
    return false;
}

Fence Display::poll(unsigned timeoutMs) {
    Fence fence;
    protocol::InterruptPacket packet{};
    timeoutMs = std::max(timeoutMs, 1u);
    while (device_.readInterrupt(&packet, sizeof(packet), timeoutMs)) {
        if ((packet.functionMask & protocol::interruptMaskDisplay) != 0) {
            if ((packet.displayEvent & protocol::displayEventFenceId) != 0) {
                fence.seen = true;
                fence.completedId = packet.displayData;
            }
            if ((packet.displayEvent & protocol::displayEventJpegDecoderError)
                != 0) {
                fence.decoderError = true;
            }
        }
        timeoutMs = 1;
    }
    return fence;
}

}
