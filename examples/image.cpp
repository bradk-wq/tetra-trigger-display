#include "image.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "tetra/device.hpp"

namespace image {
namespace {

struct Handle {
    tjhandle value = nullptr;

    explicit Handle(TJINIT mode) : value(tj3Init(mode)) {
        if (value == nullptr) {
            throw tetra::Error("could not create a JPEG decoder");
        }
    }

    ~Handle() {
        if (value != nullptr) {
            tj3Destroy(value);
        }
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

bool endsWithJpeg(const std::string &name) {
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowered.size() > 4
           && (lowered.rfind(".jpg") == lowered.size() - 4
               || lowered.rfind(".jpeg") == lowered.size() - 5);
}

}

std::vector<std::uint8_t> readFile(const std::string &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw tetra::Error("could not open " + path);
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !stream.read(reinterpret_cast<char *>(data.data()), size)) {
        throw tetra::Error("could not read " + path);
    }
    return data;
}

bool jpegSize(const std::uint8_t *data, std::size_t size, int &width,
              int &height) {
    Handle handle(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(handle.value, data, size) != 0) {
        return false;
    }
    width = tj3Get(handle.value, TJPARAM_JPEGWIDTH);
    height = tj3Get(handle.value, TJPARAM_JPEGHEIGHT);
    return width > 0 && height > 0;
}

Rgb decodeJpeg(const std::uint8_t *data, std::size_t size) {
    Handle handle(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(handle.value, data, size) != 0) {
        throw tetra::Error(std::string("JPEG header decode failed: ")
                           + tj3GetErrorStr(handle.value));
    }

    Rgb result;
    result.width = tj3Get(handle.value, TJPARAM_JPEGWIDTH);
    result.height = tj3Get(handle.value, TJPARAM_JPEGHEIGHT);
    if (result.width <= 0 || result.height <= 0) {
        throw tetra::Error("JPEG has no usable dimensions");
    }
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 3);

    if (tj3Decompress8(handle.value, data, size, result.pixels.data(),
                       result.pitch(), TJPF_RGB)
        != 0) {
        throw tetra::Error(std::string("JPEG decode failed: ")
                           + tj3GetErrorStr(handle.value));
    }
    return result;
}

Rgb loadJpeg(const std::string &path) {
    const std::vector<std::uint8_t> data = readFile(path);
    return decodeJpeg(data.data(), data.size());
}

std::vector<std::string> listJpegs(const std::string &directory) {
    std::vector<std::string> found;
    std::error_code error;
    for (const auto &entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file() && endsWithJpeg(entry.path().filename().string())) {
            found.push_back(entry.path().string());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}
