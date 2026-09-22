#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "app.hpp"
#include "draw.hpp"

namespace {

constexpr draw::Colour bars[] = {
    {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255}, {255, 0, 0},   {0, 0, 255},
};

void centreText(draw::Canvas &canvas, float centreX, float centreY, float scale,
                draw::Colour colour, const std::string &value) {
    canvas.text(centreX - canvas.textWidth(scale, value) * 0.5f,
                centreY - canvas.textHeight(scale) * 0.5f, scale, colour, value);
}

void castellations(draw::Canvas &canvas, float thickness) {
    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());
    const float horizontalStep = width / 16.0f;
    for (int i = 0; i < 16; ++i) {
        const draw::Colour colour = (i % 2) ? draw::white : draw::black;
        canvas.rect(i * horizontalStep, 0.0f, horizontalStep, thickness, colour);
        canvas.rect(i * horizontalStep, height - thickness, horizontalStep,
                    thickness, colour);
    }
    const float verticalStep = height / 12.0f;
    for (int i = 0; i < 12; ++i) {
        const draw::Colour colour = (i % 2) ? draw::black : draw::white;
        canvas.rect(0.0f, i * verticalStep, thickness, verticalStep, colour);
        canvas.rect(width - thickness, i * verticalStep, thickness, verticalStep,
                    colour);
    }
}

void graticule(draw::Canvas &canvas, float thickness, float pitch, float line) {
    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());
    for (float x = thickness + pitch; x < width - thickness; x += pitch) {
        canvas.rect(x - line * 0.5f, thickness, line, height - thickness * 2,
                    draw::white);
    }
    for (float y = thickness + pitch; y < height - thickness; y += pitch) {
        canvas.rect(thickness, y - line * 0.5f, width - thickness * 2, line,
                    draw::white);
    }
}

void gratings(draw::Canvas &canvas, float x, float y, float width, float height,
              const float *pitches, int count) {
    const float span = width / count;
    for (int i = 0; i < count; ++i) {
        const float left = x + i * span;
        canvas.rect(left, y, span, height, draw::black);
        for (float bar = left; bar < left + span - pitches[i];
             bar += pitches[i] * 2.0f) {
            canvas.rect(bar, y, pitches[i], height, draw::white);
        }
    }
}

void greyscale(draw::Canvas &canvas, float x, float y, float width,
               float height) {
    constexpr int steps = 8;
    const float span = width / steps;
    for (int i = 0; i < steps; ++i) {
        const auto level =
            static_cast<std::uint8_t>(255 * i / (steps - 1));
        canvas.rect(x + i * span, y, span, height, {level, level, level});
    }
}

void colourbars(draw::Canvas &canvas, float x, float y, float width,
                float height) {
    constexpr int count = sizeof(bars) / sizeof(bars[0]);
    const float span = width / count;
    for (int i = 0; i < count; ++i) {
        canvas.rect(x + i * span, y, span, height, bars[i]);
    }
}

void render(draw::Canvas &canvas, const std::string &label) {
    const float width = static_cast<float>(canvas.width());
    const float height = static_cast<float>(canvas.height());
    const float thickness = height / 24.0f;
    const float line = std::max(1.0f, height / 400.0f);
    const float centreX = width * 0.5f;
    const float centreY = height * 0.5f;
    const float radius = (height * 0.5f - thickness) * 0.94f;

    canvas.clear(draw::grey);
    graticule(canvas, thickness, height / 12.0f, line);
    canvas.ring(centreX, centreY, radius, line * 3.0f, draw::white);

    const float boxWidth = width * 0.34f;
    const float boxHeight = height * 0.30f;
    const float boxLeft = centreX - boxWidth * 0.5f;
    const float boxTop = centreY - boxHeight * 0.5f;
    canvas.rect(boxLeft, boxTop, boxWidth, boxHeight, draw::black);
    canvas.rect(boxLeft - line, boxTop - line, boxWidth + line * 2, line, draw::white);
    canvas.rect(boxLeft - line, boxTop + boxHeight, boxWidth + line * 2, line, draw::white);
    canvas.rect(boxLeft - line, boxTop, line, boxHeight, draw::white);
    canvas.rect(boxLeft + boxWidth, boxTop, line, boxHeight, draw::white);
    for (int i = 1; i < 3; ++i) {
        canvas.rect(boxLeft + boxWidth * i / 3.0f, boxTop, line, boxHeight,
                    draw::white);
        canvas.rect(boxLeft, boxTop + boxHeight * i / 3.0f, boxWidth, line,
                    draw::white);
    }
    centreText(canvas, centreX, centreY, boxHeight * 0.30f / 32.0f, draw::white,
               "MBP");

    const float strip = height * 0.09f;
    colourbars(canvas, boxLeft, boxTop + boxHeight + height * 0.03f, boxWidth,
               strip);
    greyscale(canvas, boxLeft, boxTop - height * 0.03f - strip, boxWidth, strip);

    const float gratingWidth = width * 0.16f;
    const float rising[] = {line, line * 2.0f, line * 3.0f, line * 4.0f};
    const float falling[] = {line * 4.0f, line * 3.0f, line * 2.0f, line};
    gratings(canvas, boxLeft - width * 0.03f - gratingWidth,
             centreY - strip * 0.5f, gratingWidth, strip, rising, 4);
    gratings(canvas, boxLeft + boxWidth + width * 0.03f, centreY - strip * 0.5f,
             gratingWidth, strip, falling, 4);

    centreText(canvas, centreX, centreY - radius * 0.78f, height * 0.055f / 32.0f,
               draw::white, "TETRA");
    centreText(canvas, centreX, centreY + radius * 0.78f, height * 0.045f / 32.0f,
               draw::white, label);

    castellations(canvas, thickness);
}

}

namespace {

std::string takeSavePath(std::vector<std::string> &positional) {
    auto flag = std::find(positional.begin(), positional.end(), "--save");
    if (flag == positional.end() || flag + 1 == positional.end()) {
        return {};
    }
    const std::string path = *(flag + 1);
    positional.erase(flag, flag + 2);
    return path;
}

}

int main(int argc, char **argv) {
    std::vector<std::string> positional;
    app::Options options = app::parse(argc, argv, positional);
    const std::string savePath = takeSavePath(positional);
    const bool fill = std::find(positional.begin(), positional.end(), "--fill")
                      != positional.end();
    if (options.help) {
        app::usage(argv[0], "[--fill] [--save FILE.jpg]",
                   "Show a BBC Test Card F style pattern.\n"
                   "Drawn 4:3 and pillarboxed unless --fill is given.");
        return 0;
    }

    try {
        tetra::Display display = app::start(options);
        const int width = display.width();
        const int height = display.height();

        draw::Canvas screen(width, height);
        screen.clear(draw::black);

        const std::string label =
            std::to_string(width) + " x " + std::to_string(height);
        if (fill) {
            render(screen, label);
        } else {
            const int cardWidth = std::min(width, height * 4 / 3);
            const int cardHeight = cardWidth * 3 / 4;
            draw::Canvas card(cardWidth, cardHeight);
            render(card, label);
            screen.blit(card.pixels(), cardWidth, cardHeight, card.pitch(),
                        static_cast<float>((width - cardWidth) / 2),
                        static_cast<float>((height - cardHeight) / 2),
                        static_cast<float>(cardWidth),
                        static_cast<float>(cardHeight));
        }

        if (!savePath.empty()) {
            tetra::Encoder encoder;
            encoder.setQuality(options.quality);
            const tetra::EncodedImage encoded = encoder.encode(screen.surface());
            std::ofstream file(savePath, std::ios::binary);
            file.write(reinterpret_cast<const char *>(encoded.data),
                       static_cast<std::streamsize>(encoded.size));
            std::printf("wrote %s\n", savePath.c_str());
        }

        display.present(screen.surface());
        const tetra::Fence fence = display.poll(500);
        std::printf("presented %dx%d, fence %s%s\n", width, height,
                    fence.seen ? "ok" : "not seen",
                    fence.decoderError ? ", decoder error" : "");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
