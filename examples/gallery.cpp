#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#include "app.hpp"
#include "draw.hpp"
#include "image.hpp"

namespace {

float takeDelay(std::vector<std::string> &positional, float fallback) {
    auto flag = std::find(positional.begin(), positional.end(), "--delay");
    if (flag == positional.end() || flag + 1 == positional.end()) {
        return fallback;
    }
    const float value = std::strtof((flag + 1)->c_str(), nullptr);
    positional.erase(flag, flag + 2);
    return value > 0.0f ? value : fallback;
}

void drawCaption(draw::Canvas &canvas, const std::string &text, int index,
                 int total) {
    const float scale = canvas.height() * 0.035f / 32.0f;
    const std::string line =
        std::to_string(index + 1) + "/" + std::to_string(total) + "  " + text;
    const float margin = canvas.height() * 0.025f;
    const float boxHeight = canvas.textHeight(scale) + margin;
    canvas.rect(0.0f, canvas.height() - boxHeight,
                static_cast<float>(canvas.width()), boxHeight, {0, 0, 0});
    canvas.text(margin * 0.5f, canvas.height() - boxHeight + margin * 0.35f,
                scale, draw::white, line);
}

}

int main(int argc, char **argv) {
    std::vector<std::string> positional;
    app::Options options = app::parse(argc, argv, positional);
    const float delay = takeDelay(positional, 4.0f);
    if (options.help || positional.empty()) {
        app::usage(argv[0], "[--delay SECONDS] DIRECTORY",
                   "Cycle through the JPEGs in a directory, letterboxed.");
        return options.help ? 0 : 2;
    }

    try {
        const std::vector<std::string> files = image::listJpegs(positional[0]);
        if (files.empty()) {
            std::fprintf(stderr, "no JPEGs in %s\n", positional[0].c_str());
            return 1;
        }

        app::catchInterrupts();
        tetra::Display display = app::start(options);
        const int width = display.width();
        const int height = display.height();
        draw::Canvas canvas(width, height);

        std::printf("%zu images, %.1fs each\n", files.size(), delay);

        std::size_t index = 0;
        while (app::running()) {
            const std::string &path = files[index];
            const std::string name =
                std::filesystem::path(path).filename().string();
            try {
                const std::vector<std::uint8_t> encoded = image::readFile(path);
                int sourceWidth = 0;
                int sourceHeight = 0;

                if (image::jpegSize(encoded.data(), encoded.size(), sourceWidth,
                                    sourceHeight)
                    && sourceWidth == width && sourceHeight == height) {
                    display.presentJpeg(encoded.data(), encoded.size());
                    std::printf("%s  %dx%d (sent as-is)\n", name.c_str(),
                                sourceWidth, sourceHeight);
                } else {
                    const image::Rgb picture =
                        image::decodeJpeg(encoded.data(), encoded.size());
                    const float scale =
                        std::min(static_cast<float>(width) / picture.width,
                                 static_cast<float>(height) / picture.height);
                    const float drawWidth = picture.width * scale;
                    const float drawHeight = picture.height * scale;
                    canvas.clear(draw::black);
                    canvas.blit(picture.pixels.data(), picture.width,
                                picture.height, picture.pitch(),
                                (width - drawWidth) * 0.5f,
                                (height - drawHeight) * 0.5f, drawWidth,
                                drawHeight);
                    drawCaption(canvas, name, static_cast<int>(index),
                                static_cast<int>(files.size()));
                    display.present(canvas.surface());
                    std::printf("%s  %dx%d -> %.0fx%.0f\n", name.c_str(),
                                picture.width, picture.height, drawWidth,
                                drawHeight);
                }
                std::fflush(stdout);
                const tetra::Fence fence = display.poll(200);
                if (fence.decoderError) {
                    std::fprintf(stderr, "  decoder error on %s\n", name.c_str());
                }
            } catch (const std::exception &error) {
                std::fprintf(stderr, "skipping %s: %s\n", name.c_str(),
                             error.what());
            }

            const auto until = std::chrono::steady_clock::now()
                               + std::chrono::duration<float>(delay);
            while (app::running() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            index = (index + 1) % files.size();
        }

        std::printf("\nstopping\n");
        display.setPowered(false);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
