#include <SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app.hpp"

namespace {

/// SDL's packed formats are little-endian words, so ARGB8888 is b, g, r, a in
/// memory. That is tetra::PixelFormat::bgrx, and the alpha byte is ignored.
constexpr tetra::PixelFormat surfaceFormat = tetra::PixelFormat::bgrx;

}

int main(int argc, char **argv) {
    std::vector<std::string> positional;
    app::Options options = app::parse(argc, argv, positional);
    if (options.help) {
        app::usage(argv[0], "",
                   "Render with SDL and present to the panel.\n"
                   "Shows the integration: draw anywhere, hand over pixels.");
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Surface *surface = nullptr;
    SDL_Renderer *renderer = nullptr;
    int status = 0;

    try {
        app::catchInterrupts();
        tetra::Display display = app::start(options);
        display.setPartialUpdates(true);
        const int width = display.width();
        const int height = display.height();

        surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                 SDL_PIXELFORMAT_ARGB8888);
        if (surface == nullptr) {
            throw tetra::Error(std::string("SDL surface failed: ") + SDL_GetError());
        }
        renderer = SDL_CreateSoftwareRenderer(surface);
        if (renderer == nullptr) {
            throw tetra::Error(std::string("SDL renderer failed: ") + SDL_GetError());
        }

        const auto started = std::chrono::steady_clock::now();
        auto reported = started;
        int frames = 0;

        while (app::running()) {
            const float time = std::chrono::duration<float>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();

            SDL_SetRenderDrawColor(renderer, 18, 20, 26, 255);
            SDL_RenderClear(renderer);

            for (int i = 0; i < 12; ++i) {
                const float phase = time * 1.3f + i * 0.5f;
                SDL_Rect box;
                box.w = width / 14;
                box.h = box.w;
                box.x = static_cast<int>((0.5f + 0.42f * std::sin(phase))
                                         * (width - box.w));
                box.y = static_cast<int>((0.5f + 0.42f * std::cos(phase * 0.77f))
                                         * (height - box.h));
                SDL_SetRenderDrawColor(renderer,
                                       static_cast<Uint8>(128 + 127 * std::sin(phase)),
                                       static_cast<Uint8>(128 + 127 * std::sin(phase + 2.1f)),
                                       static_cast<Uint8>(128 + 127 * std::sin(phase + 4.2f)),
                                       255);
                SDL_RenderFillRect(renderer, &box);
            }
            SDL_RenderPresent(renderer);

            const tetra::Surface view(
                static_cast<const std::uint8_t *>(surface->pixels), width,
                height, surface->pitch, surfaceFormat);
            display.present(view);

            ++frames;
            const auto now = std::chrono::steady_clock::now();
            if (now - reported >= std::chrono::seconds(2)) {
                std::printf("%.1f fps  %d regions, %zu bytes last frame\n",
                            frames / std::chrono::duration<float>(now - reported).count(),
                            display.lastPresent().regions,
                            display.lastPresent().encodedBytes);
                std::fflush(stdout);
                frames = 0;
                reported = now;
            }
        }

        std::printf("\nstopping\n");
        display.setPowered(false);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        status = 1;
    }

    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
    }
    if (surface != nullptr) {
        SDL_FreeSurface(surface);
    }
    SDL_Quit();
    return status;
}
