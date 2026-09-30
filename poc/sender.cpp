// Wireless Tetra sender: render, JPEG-encode, fragment, send.
//
//   tetra_sender --to receiver.local              # animated test pattern
//   tetra_sender --to 192.168.1.50:47800 --jpeg frame.jpg
//
// Stands in for the Mac app: a real one replaces `renderPattern` with its own
// pixels. Also logs acks (round trip, receiver hold time) and touch events.
// Needs no USB device; it links the library only for the JPEG encoder.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "tetra/encoder.hpp"
#include "tetra/surface.hpp"
#include "udp.hpp"
#include "wire.hpp"

using namespace tetra::wireless;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested = true; }

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::string to;
    std::string jpegPath;
    int width = 1280;
    int height = 800;
    int fps = 60;
    int quality = 80;
    double seconds = 0;
    double loss = 0;
    bool shuffle = false;
};

void usage() {
    std::fputs("usage: tetra_sender --to HOST[:PORT] [--jpeg FILE]\n"
               "                     [--fps N] [--quality N] [--seconds S]\n"
               "                     [--loss FRACTION] [--shuffle]\n",
               stderr);
}

bool parseOptions(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char * {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        const char *value = nullptr;
        if (arg == "--to" && (value = next())) {
            o.to = value;
        } else if (arg == "--jpeg" && (value = next())) {
            o.jpegPath = value;
        } else if (arg == "--fps" && (value = next())) {
            o.fps = std::max(1, std::atoi(value));
        } else if (arg == "--quality" && (value = next())) {
            o.quality = std::atoi(value);
        } else if (arg == "--seconds" && (value = next())) {
            o.seconds = std::atof(value);
        } else if (arg == "--loss" && (value = next())) {
            o.loss = std::atof(value);
        } else if (arg == "--shuffle") {
            o.shuffle = true;
        } else if (arg == "--width" && (value = next())) {
            o.width = std::atoi(value);
        } else if (arg == "--height" && (value = next())) {
            o.height = std::atoi(value);
        } else {
            return false;
        }
    }
    return !o.to.empty();
}

/// A gradient with a sweeping bar and the frame number in binary along the
/// top edge: enough to see motion, and to read the frame number off a
/// 240 fps camera pointed at the panel for glass-to-glass timing.
class Pattern {
public:
    Pattern(int width, int height)
        : width_(width), height_(height),
          background_(static_cast<std::size_t>(width) * height * 3),
          pixels_(background_.size()) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::uint8_t *p = &background_[(static_cast<std::size_t>(y) * width + x) * 3];
                p[0] = static_cast<std::uint8_t>(x * 255 / width);
                p[1] = static_cast<std::uint8_t>(y * 255 / height);
                p[2] = 96;
            }
        }
    }

    tetra::Surface render(std::uint32_t frame) {
        pixels_ = background_;
        const int bar = static_cast<int>((frame * 12) % width_);
        for (int y = 0; y < height_; ++y) {
            for (int dx = 0; dx < 24; ++dx) {
                fill((bar + dx) % width_, y, 255, 255, 255);
            }
        }
        for (int bit = 0; bit < 24; ++bit) {
            const bool on = (frame >> (23 - bit)) & 1u;
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 40; ++x) {
                    fill(bit * 40 + x, y, on ? 255 : 0, on ? 255 : 0,
                         on ? 255 : 0);
                }
            }
        }
        return tetra::Surface(pixels_.data(), width_, height_,
                              tetra::PixelFormat::rgb);
    }

private:
    void fill(int x, int y, int r, int g, int b) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) {
            return;
        }
        std::uint8_t *p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 3];
        p[0] = static_cast<std::uint8_t>(r);
        p[1] = static_cast<std::uint8_t>(g);
        p[2] = static_cast<std::uint8_t>(b);
    }

    int width_;
    int height_;
    std::vector<std::uint8_t> background_;
    std::vector<std::uint8_t> pixels_;
};

struct Window {
    std::mutex mutex;
    std::vector<double> rttMs;
    std::vector<double> holdMs;
    std::uint64_t acks = 0;
    std::uint32_t receiverPresented = 0;
    std::uint32_t receiverSuperseded = 0;
    std::uint32_t receiverAbandoned = 0;
    std::uint32_t receiverFaults = 0;
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(p * (values.size() - 1))];
}

void listenThread(UdpSocket &socket, Window &window) {
    std::vector<std::uint8_t> datagram(2048);
    while (!stopRequested) {
        Endpoint from;
        const long size = socket.receive(datagram.data(), datagram.size(), from);
        if (size <= 0) {
            continue;
        }
        Type type;
        if (!peekType(datagram.data(), static_cast<std::size_t>(size), type)) {
            continue;
        }
        if (type == Type::ack) {
            Ack ack;
            if (!parse(datagram.data(), static_cast<std::size_t>(size), ack)) {
                continue;
            }
            const double rtt = (nowNs() - ack.echoSendNs) / 1e6;
            std::lock_guard<std::mutex> lock(window.mutex);
            window.rttMs.push_back(rtt);
            window.holdMs.push_back(ack.holdUs / 1e3);
            ++window.acks;
            window.receiverPresented = ack.presented;
            window.receiverSuperseded = ack.superseded;
            window.receiverAbandoned = ack.abandoned;
            window.receiverFaults = ack.faults;
        } else if (type == Type::touch) {
            Touch touch;
            if (!parse(datagram.data(), static_cast<std::size_t>(size), touch)) {
                continue;
            }
            static const char *names[] = {"up", "down", "move"};
            std::printf("touch #%u contact=%u %-4s x=%.3f y=%.3f (raw %u,%u of "
                        "%u,%u)\n",
                        touch.seq, touch.contact,
                        touch.state < 3 ? names[touch.state] : "?",
                        touch.xMax ? double(touch.x) / touch.xMax : 0.0,
                        touch.yMax ? double(touch.y) / touch.yMax : 0.0,
                        touch.x, touch.y, touch.xMax, touch.yMax);
            std::fflush(stdout);
        }
    }
}

}

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage();
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    UdpSocket socket;
    socket.setReceiveTimeoutMs(100);
    const Endpoint receiver = resolve(options.to, defaultPort);

    Window window;
    std::thread listener(listenThread, std::ref(socket), std::ref(window));

    std::vector<std::uint8_t> fixedJpeg;
    if (!options.jpegPath.empty()) {
        std::ifstream file(options.jpegPath, std::ios::binary);
        fixedJpeg.assign(std::istreambuf_iterator<char>(file),
                         std::istreambuf_iterator<char>());
        if (fixedJpeg.empty()) {
            std::fprintf(stderr, "cannot read %s\n", options.jpegPath.c_str());
            stopRequested = true;
        }
    }

    tetra::Encoder encoder;
    encoder.setQuality(options.quality);
    Pattern pattern(options.width, options.height);
    std::mt19937 rng(std::random_device{}());
    std::bernoulli_distribution drop(std::clamp(options.loss, 0.0, 1.0));

    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / options.fps));
    const auto started = Clock::now();
    auto next = started;
    auto lastReport = started;
    std::uint32_t seq = 0;
    std::uint64_t sent = 0, bytes = 0, sentThisSecond = 0;
    std::uint64_t encodeUsTotal = 0;

    std::vector<std::uint8_t> packet(sizeof(FrameHeader) + fragmentPayload);
    while (!stopRequested) {
        if (options.seconds > 0
            && std::chrono::duration<double>(Clock::now() - started).count()
                   >= options.seconds) {
            break;
        }

        const auto t0 = Clock::now();
        const std::uint8_t *jpeg;
        std::size_t size;
        if (!fixedJpeg.empty()) {
            jpeg = fixedJpeg.data();
            size = fixedJpeg.size();
        } else {
            const tetra::EncodedImage image = encoder.encode(pattern.render(seq));
            jpeg = image.data;
            size = image.size;
        }
        encodeUsTotal += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
                .count());

        const std::size_t count = (size + fragmentPayload - 1) / fragmentPayload;
        std::vector<std::size_t> order(count);
        for (std::size_t i = 0; i < count; ++i) {
            order[i] = i;
        }
        if (options.shuffle) {
            std::shuffle(order.begin(), order.end(), rng);
        }
        const std::uint64_t stamp = nowNs();
        for (std::size_t index : order) {
            if (options.loss > 0 && drop(rng)) {
                continue;
            }
            const std::size_t begin = index * fragmentPayload;
            const std::size_t length = std::min(fragmentPayload, size - begin);
            FrameHeader header{makeHeader(Type::frame),
                               seq,
                               stamp,
                               static_cast<std::uint32_t>(size),
                               static_cast<std::uint16_t>(index),
                               static_cast<std::uint16_t>(count),
                               static_cast<std::uint16_t>(options.width),
                               static_cast<std::uint16_t>(options.height)};
            std::memcpy(packet.data(), &header, sizeof(header));
            std::memcpy(packet.data() + sizeof(header), jpeg + begin, length);
            socket.send(packet.data(), sizeof(header) + length, receiver);
        }
        ++seq;
        ++sent;
        ++sentThisSecond;
        bytes += size;

        const auto now = Clock::now();
        if (now - lastReport >= std::chrono::seconds(1)) {
            std::vector<double> rtt, hold;
            std::uint64_t acks;
            std::uint32_t shown, superseded, abandoned, faults;
            {
                std::lock_guard<std::mutex> lock(window.mutex);
                rtt.swap(window.rttMs);
                hold.swap(window.holdMs);
                acks = window.acks;
                window.acks = 0;
                shown = window.receiverPresented;
                superseded = window.receiverSuperseded;
                abandoned = window.receiverAbandoned;
                faults = window.receiverFaults;
            }
            const double holdMedian = percentile(hold, 0.5);
            std::printf("tx: %llu fps  %.1f KB/frame  enc %.1f ms | acks %llu/s  "
                        "rtt p50 %.1f p95 %.1f max %.1f ms  hold p50 %.1f ms  "
                        "=> network one-way ~%.1f ms | rx shown=%u superseded=%u "
                        "abandoned=%u faults=%u\n",
                        static_cast<unsigned long long>(sentThisSecond),
                        sent ? bytes / 1024.0 / sent : 0.0,
                        sent ? encodeUsTotal / 1e3 / sent : 0.0,
                        static_cast<unsigned long long>(acks),
                        percentile(rtt, 0.5), percentile(rtt, 0.95),
                        percentile(rtt, 1.0), holdMedian,
                        std::max(0.0, (percentile(rtt, 0.5) - holdMedian) / 2),
                        shown, superseded, abandoned, faults);
            std::fflush(stdout);
            sentThisSecond = 0;
            lastReport = now;
        }

        next += interval;
        if (next < Clock::now()) {
            next = Clock::now();  // behind: do not burst to catch up
        }
        std::this_thread::sleep_until(next);
    }

    stopRequested = true;
    listener.join();
    std::printf("SENT frames=%llu\n", static_cast<unsigned long long>(sent));
    return 0;
}
