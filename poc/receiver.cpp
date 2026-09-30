// Wireless Tetra receiver: UDP in, Trigger 6 out.
//
//   tetra_receiver                       # drive a real panel on USB-A
//   tetra_receiver --sink null           # no hardware: validate and count
//
// Threads: the network thread reassembles fragments and publishes the newest
// complete frame to a one-slot mailbox; the display thread takes whatever is
// there, presents it, and acks. If the panel is slower than the network the
// mailbox overwrites, so the screen always shows the newest frame and latency
// never accumulates in a queue.

#include <turbojpeg.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "reassembler.hpp"
#include "tetra/display.hpp"
#include "touch_evdev.hpp"
#include "udp.hpp"
#include "wire.hpp"

using namespace tetra::wireless;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested = true; }

std::uint64_t microsBetween(Clock::time_point from, Clock::time_point to) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from)
            .count());
}

/// Where frames end up. The display sink is the real thing; the null sink
/// lets the whole network path run and be measured with no hardware.
class Sink {
public:
    virtual ~Sink() = default;
    /// May throw; the caller counts a fault and retries with the next frame.
    virtual void present(const std::uint8_t *jpeg, std::size_t size) = 0;
};

class NullSink : public Sink {
public:
    NullSink(int delayMs, std::string savePath)
        : delayMs_(delayMs), savePath_(std::move(savePath)) {}

    void present(const std::uint8_t *jpeg, std::size_t size) override {
        if (delayMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        }
        if (!savePath_.empty()) {
            std::ofstream(savePath_, std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char *>(jpeg),
                       static_cast<std::streamsize>(size));
        }
    }

private:
    int delayMs_;
    std::string savePath_;
};

/// Wraps tetra::Display and re-runs the whole bring-up after any USB error.
/// Display has no reconnect path of its own: a throw mid-frame can leave the
/// adapter's command ring and buffers in an unknown state, so the only safe
/// recovery is a fresh Display.
class DisplaySink : public Sink {
public:
    explicit DisplaySink(int index) : index_(index) {}

    void present(const std::uint8_t *jpeg, std::size_t size) override {
        if (!display_) {
            open();
        }
        try {
            display_->presentJpeg(jpeg, size);
        } catch (...) {
            display_.reset();
            throw;
        }
    }

    int width() {
        if (!display_) {
            open();
        }
        return display_->width();
    }
    int height() { return display_ ? display_->height() : 0; }

private:
    void open() {
        auto display = std::make_unique<tetra::Display>(index_);
        const std::optional<tetra::Mode> mode = display->nativeMode();
        if (!mode) {
            throw tetra::Error("panel reports no native mode");
        }
        display->setMode(*mode);
        display->setPowered(true);
        // No vblank exists at any layer, so hold each frame to the mode's
        // refresh interval from this machine's own clock.
        display->setPacing(tetra::Pacing::refreshRate);
        std::fprintf(stderr, "panel up: %s %s, %d MB VRAM\n",
                     display->product().c_str(), mode->describe().c_str(),
                     display->vramMegabytes());
        display_ = std::move(display);
    }

    int index_;
    std::unique_ptr<tetra::Display> display_;
};

/// One-slot, newest-wins handoff between the network and display threads.
class Mailbox {
public:
    void publish(Reassembler::Frame &&frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_) {
            ++superseded_;
        }
        slot_ = std::move(frame);
        arrived_ = Clock::now();
        ready_.notify_one();
    }

    bool take(Reassembler::Frame &frame, Clock::time_point &arrived,
              int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ready_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&] { return slot_.has_value(); })) {
            return false;
        }
        frame = std::move(*slot_);
        arrived = arrived_;
        slot_.reset();
        return true;
    }

    std::uint64_t superseded() {
        std::lock_guard<std::mutex> lock(mutex_);
        return superseded_;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Reassembler::Frame> slot_;
    Clock::time_point arrived_{};
    std::uint64_t superseded_ = 0;
};

/// The adapter's decoder wants baseline 4:2:0 JPEG at the panel's resolution
/// and presentJpeg checks none of it, so a bad frame would reach the decoder
/// (which reports errors only on the interrupt endpoint). Check here instead.
class JpegCheck {
public:
    JpegCheck() : handle_(tj3Init(TJINIT_DECOMPRESS)) {}
    ~JpegCheck() {
        if (handle_) {
            tj3Destroy(handle_);
        }
    }
    JpegCheck(const JpegCheck &) = delete;
    JpegCheck &operator=(const JpegCheck &) = delete;

    const char *problem(const Reassembler::Frame &frame, int width,
                        int height) {
        if (tj3DecompressHeader(handle_, frame.data.data(), frame.data.size())
            != 0) {
            return "not a JPEG";
        }
        if (tj3Get(handle_, TJPARAM_JPEGWIDTH) != width
            || tj3Get(handle_, TJPARAM_JPEGHEIGHT) != height) {
            return "wrong dimensions";
        }
        if (tj3Get(handle_, TJPARAM_SUBSAMP) != TJSAMP_420) {
            return "not 4:2:0";
        }
        if (tj3Get(handle_, TJPARAM_PROGRESSIVE) != 0) {
            return "progressive";
        }
        return nullptr;
    }

private:
    tjhandle handle_;
};

struct Options {
    std::uint16_t port = defaultPort;
    std::string sink = "display";
    int device = 0;
    int width = 1280;
    int height = 800;
    int sinkDelayMs = 0;
    std::string saveLast;
    std::string touchDevice;
    double exitAfterSeconds = 0;
};

void usage() {
    std::fputs(
        "usage: tetra_receiver [--port N] [--sink display|null] [--device N]\n"
        "                      [--width W --height H] [--touch-device PATH]\n"
        "                      [--sink-delay-ms N] [--save-last FILE]\n"
        "                      [--exit-after SECONDS]\n",
        stderr);
}

bool parseOptions(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char * {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        const char *value = nullptr;
        if (arg == "--port" && (value = next())) {
            o.port = static_cast<std::uint16_t>(std::atoi(value));
        } else if (arg == "--sink" && (value = next())) {
            o.sink = value;
        } else if (arg == "--device" && (value = next())) {
            o.device = std::atoi(value);
        } else if (arg == "--width" && (value = next())) {
            o.width = std::atoi(value);
        } else if (arg == "--height" && (value = next())) {
            o.height = std::atoi(value);
        } else if (arg == "--sink-delay-ms" && (value = next())) {
            o.sinkDelayMs = std::atoi(value);
        } else if (arg == "--save-last" && (value = next())) {
            o.saveLast = value;
        } else if (arg == "--touch-device" && (value = next())) {
            o.touchDevice = value;
        } else if (arg == "--exit-after" && (value = next())) {
            o.exitAfterSeconds = std::atof(value);
        } else {
            return false;
        }
    }
    return o.sink == "display" || o.sink == "null";
}

struct Shared {
    UdpSocket socket;
    Mailbox mailbox;
    std::mutex peerMutex;
    std::optional<Endpoint> peer;
    std::atomic<std::uint64_t> datagrams{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> abandoned{0};
    std::atomic<std::uint64_t> stale{0};
    std::atomic<std::uint64_t> malformed{0};
    std::atomic<std::uint64_t> presented{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> faults{0};
    std::atomic<std::uint32_t> touchSeq{0};
};

void networkThread(Shared &shared) {
    Reassembler reassembler;
    std::vector<std::uint8_t> datagram(2048);
    while (!stopRequested) {
        Endpoint from;
        const long size =
            shared.socket.receive(datagram.data(), datagram.size(), from);
        if (size <= 0) {
            continue;
        }
        ++shared.datagrams;
        FrameHeader header;
        Type type;
        if (!peekType(datagram.data(), static_cast<std::size_t>(size), type)
            || type != Type::frame
            || !parse(datagram.data(), static_cast<std::size_t>(size),
                      header)) {
            ++shared.malformed;
            continue;
        }
        {
            // Replies (acks, touch) go wherever frames come from.
            std::lock_guard<std::mutex> lock(shared.peerMutex);
            shared.peer = from;
        }
        Reassembler::Frame frame;
        if (reassembler.add(header, datagram.data() + sizeof(FrameHeader),
                            static_cast<std::size_t>(size) - sizeof(FrameHeader),
                            frame)) {
            shared.mailbox.publish(std::move(frame));
        }
        const auto &c = reassembler.counters();
        shared.completed = c.completed;
        shared.abandoned = c.abandoned;
        shared.stale = c.stale;
        shared.malformed = c.malformed;
    }
}

void displayThread(Shared &shared, Sink &sink, int width, int height) {
    JpegCheck check;
    while (!stopRequested) {
        Reassembler::Frame frame;
        Clock::time_point arrived;
        if (!shared.mailbox.take(frame, arrived, 100)) {
            continue;
        }
        if (const char *why = check.problem(frame, width, height)) {
            ++shared.rejected;
            std::fprintf(stderr, "rejected frame %u: %s\n", frame.seq, why);
            continue;
        }
        try {
            sink.present(frame.data.data(), frame.data.size());
        } catch (const std::exception &error) {
            ++shared.faults;
            std::fprintf(stderr, "present failed: %s; will re-open\n",
                         error.what());
            // Frames keep arriving and overwrite each other meanwhile.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        ++shared.presented;

        std::optional<Endpoint> peer;
        {
            std::lock_guard<std::mutex> lock(shared.peerMutex);
            peer = shared.peer;
        }
        if (peer) {
            Ack ack{};
            ack.header = makeHeader(Type::ack);
            ack.seq = frame.seq;
            ack.echoSendNs = frame.sendNs;
            ack.holdUs = static_cast<std::uint32_t>(
                microsBetween(arrived, Clock::now()));
            ack.presented = static_cast<std::uint32_t>(shared.presented.load());
            ack.superseded =
                static_cast<std::uint32_t>(shared.mailbox.superseded());
            ack.abandoned = static_cast<std::uint32_t>(shared.abandoned.load());
            ack.faults = static_cast<std::uint32_t>(shared.faults.load());
            shared.socket.send(&ack, sizeof(ack), *peer);
        }
    }
}

#ifdef __linux__
void touchThread(Shared &shared, const std::string &path) {
    try {
        EvdevTouch touch(path);
        std::fprintf(stderr, "touch: %s, %s, range %dx%d\n", path.c_str(),
                     touch.multitouch() ? "multitouch" : "single-touch",
                     touch.xMax(), touch.yMax());
        touch.run(stopRequested, [&](const ContactEvent &e) {
            std::optional<Endpoint> peer;
            {
                std::lock_guard<std::mutex> lock(shared.peerMutex);
                peer = shared.peer;
            }
            if (!peer) {
                return;
            }
            Touch message{};
            message.header = makeHeader(Type::touch);
            message.seq = ++shared.touchSeq;
            message.contact = static_cast<std::uint8_t>(e.contact);
            message.state = !e.down   ? touchUp
                            : e.began ? touchDown
                                      : touchMove;
            message.x = static_cast<std::uint16_t>(e.x);
            message.y = static_cast<std::uint16_t>(e.y);
            message.xMax = static_cast<std::uint16_t>(touch.xMax());
            message.yMax = static_cast<std::uint16_t>(touch.yMax());
            shared.socket.send(&message, sizeof(message), *peer);
        });
    } catch (const std::exception &error) {
        std::fprintf(stderr, "touch disabled: %s\n", error.what());
    }
}
#endif

}

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage();
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Shared shared;
    shared.socket.bindPort(options.port);
    shared.socket.setReceiveTimeoutMs(100);

    std::unique_ptr<Sink> sink;
    int width = options.width;
    int height = options.height;
    if (options.sink == "display") {
        auto display = std::make_unique<DisplaySink>(options.device);
        try {
            width = display->width();
            height = display->height();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "cannot bring up the panel: %s\n",
                         error.what());
            return 1;
        }
        sink = std::move(display);
    } else {
        sink = std::make_unique<NullSink>(options.sinkDelayMs,
                                          options.saveLast);
    }
    std::fprintf(stderr, "listening on udp/%u, %dx%d, sink=%s\n", options.port,
                 width, height, options.sink.c_str());

    std::thread network(networkThread, std::ref(shared));
    std::thread display(displayThread, std::ref(shared), std::ref(*sink), width,
                        height);
#ifdef __linux__
    std::thread touch;
    if (!options.touchDevice.empty()) {
        touch = std::thread(touchThread, std::ref(shared), options.touchDevice);
    }
#else
    if (!options.touchDevice.empty()) {
        std::fputs("--touch-device needs Linux evdev\n", stderr);
    }
#endif

    const auto started = Clock::now();
    std::uint64_t lastPresented = 0;
    while (!stopRequested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t presented = shared.presented.load();
        std::printf("rx: %llu dgrams  frames done=%llu shown=%llu(+%llu/s) "
                    "superseded=%llu abandoned=%llu stale=%llu bad=%llu "
                    "rejected=%llu faults=%llu\n",
                    static_cast<unsigned long long>(shared.datagrams.load()),
                    static_cast<unsigned long long>(shared.completed.load()),
                    static_cast<unsigned long long>(presented),
                    static_cast<unsigned long long>(presented - lastPresented),
                    static_cast<unsigned long long>(shared.mailbox.superseded()),
                    static_cast<unsigned long long>(shared.abandoned.load()),
                    static_cast<unsigned long long>(shared.stale.load()),
                    static_cast<unsigned long long>(shared.malformed.load()),
                    static_cast<unsigned long long>(shared.rejected.load()),
                    static_cast<unsigned long long>(shared.faults.load()));
        std::fflush(stdout);
        lastPresented = presented;
        if (options.exitAfterSeconds > 0
            && std::chrono::duration<double>(Clock::now() - started).count()
                   >= options.exitAfterSeconds) {
            stopRequested = true;
        }
    }

    network.join();
    display.join();
#ifdef __linux__
    if (touch.joinable()) {
        touch.join();
    }
#endif
    std::printf("FINAL completed=%llu presented=%llu superseded=%llu "
                "abandoned=%llu stale=%llu rejected=%llu faults=%llu\n",
                static_cast<unsigned long long>(shared.completed.load()),
                static_cast<unsigned long long>(shared.presented.load()),
                static_cast<unsigned long long>(shared.mailbox.superseded()),
                static_cast<unsigned long long>(shared.abandoned.load()),
                static_cast<unsigned long long>(shared.stale.load()),
                static_cast<unsigned long long>(shared.rejected.load()),
                static_cast<unsigned long long>(shared.faults.load()));
    return 0;
}
