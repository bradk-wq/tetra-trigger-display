#pragma once

// Linux evdev touch reader for the receiver. Reads a digitizer and reports
// one event per contact change. Does not grab the device, so the receiver's
// own console keeps working.
//
// UNTESTED against real hardware: the Tetra's digitizer has not been seen yet
// (see the experiments in docs/wireless-touch-investigation.md). Handles both
// multitouch (ABS_MT_*) and single-touch (ABS_X/ABS_Y + BTN_TOUCH) devices.

#ifdef __linux__

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace tetra::wireless {

struct ContactEvent {
    int contact;
    bool down;   // contact is touching after this event
    bool began;  // and was not before
    int x;
    int y;
};

class EvdevTouch {
public:
    explicit EvdevTouch(const std::string &path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error("cannot open " + path);
        }
        input_absinfo info{};
        if (::ioctl(fd_, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0
            && info.maximum > 0) {
            multitouch_ = true;
            xMax_ = info.maximum;
            if (::ioctl(fd_, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) {
                yMax_ = info.maximum;
            }
        } else if (::ioctl(fd_, EVIOCGABS(ABS_X), &info) == 0) {
            xMax_ = info.maximum;
            if (::ioctl(fd_, EVIOCGABS(ABS_Y), &info) == 0) {
                yMax_ = info.maximum;
            }
        } else {
            ::close(fd_);
            throw std::runtime_error(path + " reports no absolute axes");
        }
    }
    ~EvdevTouch() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    EvdevTouch(const EvdevTouch &) = delete;
    EvdevTouch &operator=(const EvdevTouch &) = delete;

    int xMax() const { return xMax_; }
    int yMax() const { return yMax_; }
    bool multitouch() const { return multitouch_; }

    void run(const std::atomic<bool> &stop,
             const std::function<void(const ContactEvent &)> &emit) {
        while (!stop.load()) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) {
                continue;
            }
            input_event event;
            if (::read(fd_, &event, sizeof(event)) != sizeof(event)) {
                return;  // device went away
            }
            handle(event, emit);
        }
    }

private:
    struct Slot {
        bool down = false;
        bool wasDown = false;
        bool dirty = false;
        int x = 0;
        int y = 0;
    };

    void handle(const input_event &e,
                const std::function<void(const ContactEvent &)> &emit) {
        if (e.type == EV_ABS) {
            Slot &s = slots_[current_];
            switch (e.code) {
            case ABS_MT_SLOT:
                current_ = e.value >= 0 && e.value < static_cast<int>(slots_.size())
                               ? e.value
                               : 0;
                break;
            case ABS_MT_TRACKING_ID:
                s.down = e.value >= 0;
                s.dirty = true;
                break;
            case ABS_MT_POSITION_X:
            case ABS_X:
                s.x = e.value;
                s.dirty = true;
                break;
            case ABS_MT_POSITION_Y:
            case ABS_Y:
                s.y = e.value;
                s.dirty = true;
                break;
            }
        } else if (e.type == EV_KEY && e.code == BTN_TOUCH && !multitouch_) {
            slots_[0].down = e.value != 0;
            slots_[0].dirty = true;
        } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                Slot &s = slots_[i];
                if (!s.dirty) {
                    continue;
                }
                s.dirty = false;
                if (!s.down && !s.wasDown) {
                    continue;
                }
                emit(ContactEvent{static_cast<int>(i), s.down,
                                  s.down && !s.wasDown, s.x, s.y});
                s.wasDown = s.down;
            }
        }
    }

    int fd_ = -1;
    bool multitouch_ = false;
    int xMax_ = 0;
    int yMax_ = 0;
    int current_ = 0;
    std::array<Slot, 16> slots_{};
};

}

#endif
