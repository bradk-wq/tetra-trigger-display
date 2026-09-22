#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace tetra {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DeviceInfo {
    int bus;
    int address;
    std::string product;
};

/// Thin owning wrapper over one Trigger 6 USB device.
///
/// Exposes only the transfers the display path needs; Display layers the
/// protocol on top. Move-only, closes the handle on destruction.
class Device {
public:
    explicit Device(int index = 0);
    ~Device();

    Device(Device &&other) noexcept;
    Device &operator=(Device &&other) noexcept;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    static std::vector<DeviceInfo> enumerate();

    void controlOut(std::uint8_t request, std::uint16_t value,
                    std::uint16_t index, const void *data = nullptr,
                    std::uint16_t length = 0);
    void controlIn(std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, void *data, std::uint16_t length);
    void bulkOut(const void *data, std::size_t length);
    bool readInterrupt(void *data, std::size_t length, unsigned timeoutMs);

    const std::string &product() const { return product_; }

private:
    void close() noexcept;

    libusb_context *context_ = nullptr;
    libusb_device_handle *handle_ = nullptr;
    std::string product_;
    bool claimed_ = false;
};

}
