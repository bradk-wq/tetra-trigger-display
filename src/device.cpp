#include "tetra/device.hpp"

#include <libusb.h>

#include <utility>

#include "tetra/protocol.hpp"

namespace tetra {
namespace {

constexpr unsigned controlTimeoutMs = 3000;
constexpr unsigned bulkTimeoutMs = 5000;

std::string describe(int code) {
    return std::string(libusb_error_name(code));
}

std::string hex(std::uint8_t value) {
    static const char digits[] = "0123456789abcdef";
    return std::string("0x") + digits[(value >> 4) & 0xf] + digits[value & 0xf];
}

std::string readProduct(libusb_device_handle *handle,
                        const libusb_device_descriptor &descriptor) {
    if (descriptor.iProduct == 0) {
        return {};
    }
    unsigned char text[256] = {0};
    int length = libusb_get_string_descriptor_ascii(handle, descriptor.iProduct,
                                                    text, sizeof(text));
    if (length <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<char *>(text),
                       static_cast<std::size_t>(length));
}

}

Device::Device(int index) {
    if (int code = libusb_init(&context_); code != 0) {
        throw Error("libusb_init failed: " + describe(code));
    }

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
        close();
        throw Error("libusb_get_device_list failed: "
                    + describe(static_cast<int>(count)));
    }

    int seen = 0;
    int failure = 0;
    for (ssize_t i = 0; i < count && handle_ == nullptr; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(list[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != protocol::vendorId
            || descriptor.idProduct != protocol::productId) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        failure = libusb_open(list[i], &handle_);
        if (failure != 0) {
            handle_ = nullptr;
        }
    }
    libusb_free_device_list(list, 1);

    if (handle_ == nullptr) {
        close();
        if (seen == 0) {
            throw Error("no Trigger 6 adapter found");
        }
        if (failure != 0) {
            throw Error("libusb_open failed: " + describe(failure));
        }
        throw Error("adapter index " + std::to_string(index) + " out of range");
    }

    libusb_device_descriptor descriptor{};
    libusb_get_device_descriptor(libusb_get_device(handle_), &descriptor);
    product_ = readProduct(handle_, descriptor);

    int configuration = 0;
    if (libusb_get_configuration(handle_, &configuration) != 0
        || configuration != 1) {
        libusb_set_configuration(handle_, 1);
    }

    if (int code = libusb_claim_interface(handle_, 0); code != 0) {
        close();
        throw Error("could not claim interface 0: " + describe(code));
    }
    claimed_ = true;
}

Device::~Device() {
    close();
}

Device::Device(Device &&other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      product_(std::move(other.product_)),
      claimed_(std::exchange(other.claimed_, false)) {}

Device &Device::operator=(Device &&other) noexcept {
    if (this != &other) {
        close();
        context_ = std::exchange(other.context_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        product_ = std::move(other.product_);
        claimed_ = std::exchange(other.claimed_, false);
    }
    return *this;
}

void Device::close() noexcept {
    if (handle_ != nullptr) {
        if (claimed_) {
            libusb_release_interface(handle_, 0);
            claimed_ = false;
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (context_ != nullptr) {
        libusb_exit(context_);
        context_ = nullptr;
    }
}

std::vector<DeviceInfo> Device::enumerate() {
    libusb_context *context = nullptr;
    if (libusb_init(&context) != 0) {
        return {};
    }

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(context, &list);
    std::vector<DeviceInfo> found;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(list[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != protocol::vendorId
            || descriptor.idProduct != protocol::productId) {
            continue;
        }
        DeviceInfo info;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        libusb_device_handle *handle = nullptr;
        if (libusb_open(list[i], &handle) == 0) {
            info.product = readProduct(handle, descriptor);
            libusb_close(handle);
        }
        found.push_back(std::move(info));
    }
    if (count >= 0) {
        libusb_free_device_list(list, 1);
    }
    libusb_exit(context);
    return found;
}

void Device::controlOut(std::uint8_t request, std::uint16_t value,
                        std::uint16_t index, const void *data,
                        std::uint16_t length) {
    int code = libusb_control_transfer(
        handle_, protocol::directionOut, request, value, index,
        const_cast<unsigned char *>(static_cast<const unsigned char *>(data)),
        length, controlTimeoutMs);
    if (code < 0) {
        throw Error("control out " + hex(request) + " failed: " + describe(code));
    }
}

void Device::controlIn(std::uint8_t request, std::uint16_t value,
                       std::uint16_t index, void *data, std::uint16_t length) {
    int code = libusb_control_transfer(handle_, protocol::directionIn, request,
                                       value, index,
                                       static_cast<unsigned char *>(data),
                                       length, controlTimeoutMs);
    if (code < 0) {
        throw Error("control in " + hex(request) + " failed: " + describe(code));
    }
    if (code != length) {
        throw Error("control in " + hex(request) + " returned "
                    + std::to_string(code) + " of " + std::to_string(length)
                    + " bytes");
    }
}

void Device::bulkOut(const void *data, std::size_t length) {
    int transferred = 0;
    int code = libusb_bulk_transfer(
        handle_, protocol::endpointBulkOut,
        const_cast<unsigned char *>(static_cast<const unsigned char *>(data)),
        static_cast<int>(length), &transferred, bulkTimeoutMs);
    if (code != 0) {
        throw Error("bulk out failed: " + describe(code));
    }
    if (static_cast<std::size_t>(transferred) != length) {
        throw Error("bulk out short write: " + std::to_string(transferred)
                    + " of " + std::to_string(length) + " bytes");
    }
}

bool Device::readInterrupt(void *data, std::size_t length, unsigned timeoutMs) {
    int transferred = 0;
    int code = libusb_interrupt_transfer(handle_, protocol::endpointInterruptIn,
                                         static_cast<unsigned char *>(data),
                                         static_cast<int>(length), &transferred,
                                         timeoutMs);
    // Status is advisory. A short timeout surfaces as ERROR_IO on macOS rather
    // than ERROR_TIMEOUT, and reading this endpoint often enough to cancel
    // transfers can halt it, which recovers with a clear.
    if (code == LIBUSB_ERROR_TIMEOUT || code == LIBUSB_ERROR_IO) {
        return false;
    }
    if (code == LIBUSB_ERROR_PIPE) {
        libusb_clear_halt(handle_, protocol::endpointInterruptIn);
        return false;
    }
    if (code != 0) {
        throw Error("interrupt in failed: " + describe(code));
    }
    return transferred > 0;
}

}
