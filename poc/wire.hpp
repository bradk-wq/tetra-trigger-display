#pragma once

// Wire format for the wireless Tetra proof of concept.
//
// One UDP datagram per message, little-endian, no byte swapping (x86 and
// arm64 are the only targets). Frames are fragmented to fit a Wi-Fi MTU;
// there is no retransmission, because a late frame is worth less than the
// next one. See docs/wireless-touch-investigation.md.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the wire format is little-endian and is not byte-swapped"
#endif

namespace tetra::wireless {

inline constexpr std::uint32_t magic = 0x31465754;  // "TWF1" in memory
inline constexpr std::uint16_t defaultPort = 47800;

/// Every fragment except the last carries exactly this many JPEG bytes.
inline constexpr std::size_t fragmentPayload = 1200;
inline constexpr std::size_t maxFragments = 4096;
inline constexpr std::size_t maxFrameBytes = 4u * 1024u * 1024u;

enum class Type : std::uint8_t {
    frame = 1,  // sender -> receiver: one fragment of one JPEG frame
    ack = 2,    // receiver -> sender: a frame reached the panel
    touch = 3,  // receiver -> sender: one contact changed
};

#pragma pack(push, 1)

struct Header {
    std::uint32_t magic;
    std::uint8_t type;
    std::uint8_t flags;
    std::uint16_t reserved;
};

struct FrameHeader {
    Header header;
    std::uint32_t seq;
    std::uint64_t sendNs;  // sender's clock; echoed back, never compared
    std::uint32_t frameSize;
    std::uint16_t fragIndex;
    std::uint16_t fragCount;
    std::uint16_t width;
    std::uint16_t height;
};

struct Ack {
    Header header;
    std::uint32_t seq;
    std::uint64_t echoSendNs;
    std::uint32_t holdUs;     // frame complete -> presentJpeg returned
    std::uint32_t presented;  // running counters, for the sender's log
    std::uint32_t superseded;
    std::uint32_t abandoned;
    std::uint32_t faults;
};

enum TouchState : std::uint8_t { touchUp = 0, touchDown = 1, touchMove = 2 };

struct Touch {
    Header header;
    std::uint32_t seq;
    std::uint8_t contact;
    std::uint8_t state;
    std::uint16_t x;  // device units; divide by xMax for 0..1
    std::uint16_t y;
    std::uint16_t xMax;
    std::uint16_t yMax;
    std::uint16_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 8);
static_assert(sizeof(FrameHeader) == 32);
static_assert(sizeof(Ack) == 40);
static_assert(sizeof(Touch) == 24);

inline Header makeHeader(Type type) {
    return Header{magic, static_cast<std::uint8_t>(type), 0, 0};
}

/// Copy a message out of a datagram, rejecting short or foreign ones.
template <typename T>
bool parse(const std::uint8_t *data, std::size_t size, T &out) {
    if (size < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data, sizeof(T));
    return out.header.magic == magic;
}

inline bool peekType(const std::uint8_t *data, std::size_t size, Type &type) {
    Header header;
    if (size < sizeof(Header)) {
        return false;
    }
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != magic) {
        return false;
    }
    type = static_cast<Type>(header.type);
    return true;
}

}
