#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>

#include "reassembler.hpp"

using namespace tetra::wireless;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

std::vector<std::uint8_t> pattern(std::size_t size, unsigned salt) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(i * 31 + salt);
    }
    return data;
}

struct Fragment {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

std::vector<Fragment> split(const std::vector<std::uint8_t> &jpeg,
                            std::uint32_t seq) {
    const std::size_t count =
        (jpeg.size() + fragmentPayload - 1) / fragmentPayload;
    std::vector<Fragment> fragments;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t begin = i * fragmentPayload;
        const std::size_t end = std::min(begin + fragmentPayload, jpeg.size());
        Fragment f;
        f.header = FrameHeader{makeHeader(Type::frame),
                               seq,
                               1000 + seq,
                               static_cast<std::uint32_t>(jpeg.size()),
                               static_cast<std::uint16_t>(i),
                               static_cast<std::uint16_t>(count),
                               1280,
                               800};
        f.payload.assign(jpeg.begin() + begin, jpeg.begin() + end);
        fragments.push_back(std::move(f));
    }
    return fragments;
}

bool feed(Reassembler &r, const Fragment &f, Reassembler::Frame &out) {
    return r.add(f.header, f.payload.data(), f.payload.size(), out);
}

void inOrder() {
    Reassembler r;
    const auto jpeg = pattern(20000, 1);
    Reassembler::Frame out;
    bool done = false;
    for (const Fragment &f : split(jpeg, 1)) {
        CHECK(!done);
        done = feed(r, f, out);
    }
    CHECK(done);
    CHECK(out.data == jpeg);
    CHECK(out.seq == 1 && out.sendNs == 1001);
    CHECK(r.counters().completed == 1);
}

void shuffledAndDuplicated() {
    Reassembler r;
    const auto jpeg = pattern(12345, 2);
    auto fragments = split(jpeg, 7);
    std::mt19937 rng(42);
    std::shuffle(fragments.begin(), fragments.end(), rng);
    std::vector<Fragment> withDuplicate;
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        withDuplicate.push_back(fragments[i]);
        if (i == 1) {
            withDuplicate.push_back(fragments[i]);
        }
    }
    fragments = std::move(withDuplicate);
    Reassembler::Frame out;
    int completions = 0;
    for (const Fragment &f : fragments) {
        completions += feed(r, f, out) ? 1 : 0;
    }
    CHECK(completions == 1);
    CHECK(out.data == jpeg);
    CHECK(r.counters().duplicate == 1);
}

void newerFrameAbandonsIncomplete() {
    Reassembler r;
    auto a = split(pattern(9000, 3), 10);
    auto b = split(pattern(9000, 4), 11);
    Reassembler::Frame out;
    CHECK(!feed(r, a[0], out));
    CHECK(!feed(r, a[1], out));  // frame 10 never finishes
    bool done = false;
    for (const Fragment &f : b) {
        done = feed(r, f, out);
    }
    CHECK(done && out.seq == 11);
    CHECK(r.counters().abandoned == 1);
    // A straggler from the abandoned frame is now stale, not reassembled.
    CHECK(!feed(r, a[2], out));
    CHECK(r.counters().stale == 1);
}

void olderFrameNeverDisplacesNewer() {
    Reassembler r;
    auto older = split(pattern(5000, 5), 20);
    auto newer = split(pattern(5000, 6), 21);
    Reassembler::Frame out;
    bool done = false;
    for (const Fragment &f : newer) {
        done = feed(r, f, out);
    }
    CHECK(done);
    for (const Fragment &f : older) {
        CHECK(!feed(r, f, out));
    }
    CHECK(r.counters().stale == older.size());
    CHECK(r.counters().completed == 1);
}

void sequenceWraps() {
    Reassembler r;
    Reassembler::Frame out;
    bool done = false;
    for (const Fragment &f : split(pattern(3000, 7), 0xfffffffeu)) {
        done = feed(r, f, out);
    }
    CHECK(done);
    done = false;
    for (const Fragment &f : split(pattern(3000, 8), 0x00000001u)) {
        done = feed(r, f, out);
    }
    CHECK(done && out.seq == 1);
}

void malformed() {
    Reassembler r;
    Reassembler::Frame out;
    auto f = split(pattern(3000, 9), 1);

    Fragment badIndex = f[0];
    badIndex.header.fragIndex = badIndex.header.fragCount;
    CHECK(!feed(r, badIndex, out));

    Fragment badLength = f[0];
    badLength.payload.pop_back();
    CHECK(!feed(r, badLength, out));

    Fragment badSize = f[0];
    badSize.header.frameSize = 1u << 30;
    CHECK(!feed(r, badSize, out));

    Fragment noDimensions = f[0];
    noDimensions.header.width = 0;
    CHECK(!feed(r, noDimensions, out));

    CHECK(r.counters().malformed == 4);
    CHECK(r.counters().completed == 0);
}

}

int main() {
    inOrder();
    shuffledAndDuplicated();
    newerFrameAbandonsIncomplete();
    olderFrameNeverDisplacesNewer();
    sequenceWraps();
    malformed();
    if (failures == 0) {
        std::puts("reassembler: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
