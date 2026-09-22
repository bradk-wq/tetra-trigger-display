#pragma once

#include <cstdint>

namespace tetra::protocol {

inline constexpr std::uint16_t vendorId = 0x0711;
inline constexpr std::uint16_t productId = 0x560b;

inline constexpr std::uint8_t directionOut = 0x40;
inline constexpr std::uint8_t directionIn = 0xc0;

inline constexpr std::uint8_t endpointBulkOut = 0x02;
inline constexpr std::uint8_t endpointBulkIn = 0x81;
inline constexpr std::uint8_t endpointInterruptIn = 0x83;

enum Request : std::uint8_t {
    requestSetMonitor = 0x03,
    requestSetCursorPosition = 0x04,
    requestSetCursorState = 0x05,
    requestSetResolution = 0x08,
    requestSetCursorShape = 0x10,
    requestSetDetailTiming = 0x12,
    requestResetHardware = 0x30,
    requestSoftwareReady = 0x31,
    requestGetEdid = 0x80,
    requestGetResolutionCount = 0x84,
    requestGetResolutionTable = 0x85,
    requestResetJpegEngine = 0x86,
    requestGetConnection = 0x87,
    requestGetVramSize = 0x88,
    requestGetTimingTable = 0x89,
    requestGetVersion = 0xb0,
    requestGetFunctionId = 0xb1,
    requestReadRom = 0xb2,
    requestGetSectionData = 0xb3,
};

enum Signature : std::uint32_t {
    signatureDisplay = 0,
    signatureAudio = 3,
    signatureRom = 5,
};

enum VideoCommand : std::uint32_t {
    videoNoOp = 0,
    videoClipPrimary = 1,
    videoClipSecondary = 2,
    videoFlipPrimary = 3,
    videoFlipSecondary = 4,
    videoBlitSourceToMemory = 5,
    videoBlitMemoryToMemory = 6,
    videoFramebufferDirect = 7,
};

enum ColourFormat : std::uint32_t {
    colourYuyv = 0,
    colourYv12 = 4,
    colourNv12 = 6,
    colourRgb32 = 8,
    colourRgb24 = 9,
    colourYuv24 = 10,
    colourYuv444 = 11,
    colourJpeg = 13,
};

inline constexpr std::uint8_t flipFlagResetJpeg = 0x80;

inline constexpr std::uint8_t interruptMaskDisplay = 0x04;
inline constexpr std::uint8_t interruptMaskAudio = 0x20;
inline constexpr std::uint8_t displayEventConnectStatus = 0x01;
inline constexpr std::uint8_t displayEventFenceId = 0x04;
inline constexpr std::uint8_t displayEventJpegDecoderError = 0x08;

#pragma pack(push, 1)

struct BulkCommand {
    std::uint32_t signature;
    std::uint32_t payloadLength;
    std::uint32_t payloadAddress;
    std::uint32_t packetLength;
    std::uint32_t reserved[2];
    std::uint8_t padding[8];
};

struct FlipHeader {
    std::uint32_t command;
    std::uint32_t payloadSize;
    std::uint32_t fenceId;
    std::uint32_t targetFormat;
    std::uint16_t lumaPitch;
    std::uint16_t chromaPitch;
    std::uint32_t lumaAddress;
    std::uint32_t chromaAddress;
    std::uint32_t chromaAddress2;
    std::uint32_t sourceFormat;
    std::uint8_t padding[11];
    std::uint8_t flags;
};

/// Partial update, as the vendor driver does it. Placed purely by destination
/// address; it carries no position fields.
struct DirectHeader {
    std::uint32_t command;
    std::uint32_t payloadSize;
    std::uint32_t fenceId;
    std::uint32_t targetFormat;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t lumaPitch;
    std::uint16_t chromaPitch;
    std::uint32_t lumaAddress;
    std::uint32_t chromaAddress;
    std::uint32_t chromaAddress2;
    std::uint32_t sourceFormat;
    std::uint8_t padding[7];
    std::uint8_t flags;
};

/// Legacy partial update, documented but deliberately unused.
///
/// Its startX and startY are ignored; writes are placed by destination address
/// like DirectHeader, and land at the wrong offset depending on position, with
/// no error reported. The vendor driver never issues this command either.
struct ClipHeader {
    std::uint32_t command;
    std::uint32_t payloadSize;
    std::uint32_t fenceId;
    std::uint32_t targetFormat;
    std::uint16_t startX;
    std::uint16_t startY;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t lumaPitch;
    std::uint16_t chromaPitch;
    std::uint32_t lumaAddress;
    std::uint32_t chromaAddress;
    std::uint32_t chromaAddress2;
    std::uint32_t sourceFormat;
    std::uint8_t padding[3];
    std::uint8_t flags;
};

struct Timing {
    std::uint32_t pixelClockKhz;
    std::uint8_t refreshHz;
    std::uint8_t reserved;
    std::uint16_t horizontalTotal;
    std::uint16_t horizontalActive;
    std::uint16_t horizontalSyncStart;
    std::uint16_t horizontalSyncWidth;
    std::uint16_t verticalTotal;
    std::uint16_t verticalActive;
    std::uint16_t verticalSyncStart;
    std::uint16_t verticalSyncWidth;
    std::uint16_t pllNumerator;
    std::uint16_t pllDenominator;
    std::uint8_t pllDivider;
    std::uint8_t outputSelect;
    std::uint8_t horizontalSyncPolarity;
    std::uint8_t verticalSyncPolarity;
    std::uint8_t reducedBlanking;
    std::uint8_t flags;
};

struct InterruptPacket {
    std::uint8_t functionMask;
    std::uint8_t reserved[3];
    std::uint8_t cpuData[8];
    std::uint32_t displayData;
    std::uint8_t displayReserved[3];
    std::uint8_t displayEvent;
    std::uint8_t networkData[8];
    std::uint8_t audioReserved[4];
    std::uint32_t audioRender;
    std::uint8_t serialData[8];
    std::uint8_t romEvent;
    std::uint8_t romReserved;
    std::uint16_t romFenceId;
    std::uint32_t romProceedSize;
    std::uint8_t trailing[12];
};

#pragma pack(pop)

static_assert(sizeof(BulkCommand) == 32);
static_assert(sizeof(FlipHeader) == 48);
static_assert(sizeof(ClipHeader) == 48);
static_assert(sizeof(DirectHeader) == 48);
static_assert(sizeof(Timing) == 32);
static_assert(sizeof(InterruptPacket) == 64);

inline constexpr std::uint8_t timingFlagCustom = 0x01;

}
