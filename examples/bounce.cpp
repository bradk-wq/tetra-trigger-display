#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app.hpp"
#include "draw.hpp"
#include "logo.hpp"

namespace {

constexpr draw::Colour palette[] = {
    {235, 64, 52},  {247, 148, 29}, {252, 214, 46},
    {66, 186, 110}, {56, 160, 235}, {141, 94, 224},
    {236, 92, 168},
};
constexpr int paletteSize = sizeof(palette) / sizeof(palette[0]);

constexpr draw::Colour background{14, 16, 20};

tetra::Rect boxFor(float x, float y, float width, float height) {
    return tetra::Rect{static_cast<int>(std::floor(x)),
                       static_cast<int>(std::floor(y)),
                       static_cast<int>(std::ceil(width)) + 1,
                       static_cast<int>(std::ceil(height)) + 1};
}

bool overlaps(const tetra::Rect &a, const tetra::Rect &b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom()
           && b.y < a.bottom();
}

}

int main(int argc, char **argv) {
    std::vector<std::string> positional;
    app::Options options = app::parse(argc, argv, positional);
    const bool full = std::find(positional.begin(), positional.end(), "--full")
                      != positional.end();
    const bool partial = !full;
    if (options.help) {
        app::usage(argv[0], "[--full]",
                   "Bounce the Mobile Pixels logo around the panel.\n"
                   "Sends only the regions that moved unless --full is given.");
        return 0;
    }

    try {
        app::catchInterrupts();
        tetra::Display display = app::start(options);
        display.setPartialUpdates(partial);

        const auto width = static_cast<float>(display.width());
        const auto height = static_cast<float>(display.height());

        std::vector<draw::Polygon> polygons;
        polygons.reserve(logo::contourCount);
        for (int i = 0; i < logo::contourCount; ++i) {
            polygons.push_back({logo::contours[i].xy, logo::contours[i].count});
        }

        const float logoWidth = width * 0.28f;
        const float logoHeight = logoWidth * logo::aspect;
        float x = width * 0.2f;
        float y = height * 0.3f;
        float velocityX = width * 0.22f;
        float velocityY = height * 0.19f;
        int colour = 0;

        draw::Canvas canvas(display.width(), display.height());
        canvas.clear(background);
        canvas.fill(polygons.data(), static_cast<int>(polygons.size()),
                    logoWidth, x, y, palette[colour]);
        display.presentFull(canvas.surface());

        auto previous = std::chrono::steady_clock::now();
        auto reported = previous;
        int frames = 0;
        std::size_t bytes = 0;
        int regions = 0;

        while (app::running()) {
            const auto now = std::chrono::steady_clock::now();
            const float elapsed =
                std::chrono::duration<float>(now - previous).count();
            previous = now;

            const tetra::Rect before = boxFor(x, y, logoWidth, logoHeight);

            x += velocityX * elapsed;
            y += velocityY * elapsed;

            bool hit = false;
            if (x < 0.0f) {
                x = 0.0f;
                velocityX = -velocityX;
                hit = true;
            } else if (x + logoWidth > width) {
                x = width - logoWidth;
                velocityX = -velocityX;
                hit = true;
            }
            if (y < 0.0f) {
                y = 0.0f;
                velocityY = -velocityY;
                hit = true;
            } else if (y + logoHeight > height) {
                y = height - logoHeight;
                velocityY = -velocityY;
                hit = true;
            }
            if (hit) {
                colour = (colour + 1) % paletteSize;
            }

            canvas.clear(background);
            canvas.fill(polygons.data(), static_cast<int>(polygons.size()),
                        logoWidth, x, y, palette[colour]);

            if (!partial) {
                display.present(canvas.surface());
            } else {
                const tetra::Rect after = boxFor(x, y, logoWidth, logoHeight);
                // Two separate updates beat one union whenever the logo has
                // moved clear of where it was.
                if (overlaps(before, after)) {
                    const tetra::Rect merged = before.unionWith(after);
                    display.present(canvas.surface(), merged);
                } else {
                    const tetra::Rect both[] = {before, after};
                    display.present(canvas.surface(), both, 2);
                }
            }

            bytes += display.lastPresent().encodedBytes;
            regions += display.lastPresent().regions;
            ++frames;

            if (now - reported >= std::chrono::seconds(2)) {
                const float seconds =
                    std::chrono::duration<float>(now - reported).count();
                std::printf("%.1f fps  %.1f KB/frame  %.1f regions/frame\n",
                            frames / seconds,
                            bytes / 1024.0 / std::max(frames, 1),
                            static_cast<double>(regions) / std::max(frames, 1));
                std::fflush(stdout);
                frames = 0;
                bytes = 0;
                regions = 0;
                reported = now;
            }
        }

        std::printf("\nstopping\n");
        display.setPowered(false);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
