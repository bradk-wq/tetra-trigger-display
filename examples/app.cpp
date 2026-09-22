#include "app.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

volatile std::sig_atomic_t keepRunning = 1;
std::chrono::steady_clock::time_point deadline;
bool deadlineSet = false;

void onSignal(int) {
    keepRunning = 0;
}

}

Options parse(int argc, char **argv, std::vector<std::string> &positional) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--device" && i + 1 < argc) {
            options.index = std::atoi(argv[++i]);
        } else if (argument == "--mode" && i + 1 < argc) {
            options.mode = argv[++i];
        } else if (argument == "--quality" && i + 1 < argc) {
            options.quality = std::atoi(argv[++i]);
        } else if (argument == "--free") {
            options.pace = false;
        } else if (argument == "--seconds" && i + 1 < argc) {
            options.seconds = std::strtof(argv[++i], nullptr);
        } else {
            positional.push_back(argument);
        }
    }
    return options;
}

void usage(const char *program, const char *positional, const char *summary) {
    std::printf("%s\n\n", summary);
    std::printf("  %s [options] %s\n\n", program, positional);
    std::printf("  --device N     adapter index, default 0\n");
    std::printf("  --mode WxH     force a mode, default the panel's native one\n");
    std::printf("  --quality N    JPEG quality 1-100, default 85\n");
    std::printf("  --seconds N    stop after N seconds, default run forever\n");
    std::printf("  --free         do not hold frames to the refresh rate\n");
}

tetra::Display start(const Options &options) {
    tetra::Display display(options.index);
    display.setQuality(options.quality);
    runFor(options.seconds);
    display.setPacing(options.pace ? tetra::Pacing::refreshRate
                                   : tetra::Pacing::immediate);

    std::printf("%s (%s), %d MB VRAM, panel %s\n", display.product().c_str(),
                display.projectCode().c_str(), display.vramMegabytes(),
                display.panelAttached() ? "attached" : "absent");

    std::optional<tetra::Mode> chosen;
    if (!options.mode.empty()) {
        int width = 0;
        int height = 0;
        if (std::sscanf(options.mode.c_str(), "%dx%d", &width, &height) != 2) {
            throw tetra::Error("could not parse mode " + options.mode);
        }
        chosen = display.findMode(width, height);
        if (!chosen) {
            throw tetra::Error(options.mode + " is not an available mode");
        }
    } else {
        chosen = display.nativeMode();
        if (!chosen) {
            if (display.modes().empty()) {
                throw tetra::Error("the adapter reported no usable modes");
            }
            chosen = display.modes().front();
        }
    }

    std::printf("setting %s\n", chosen->describe().c_str());
    display.setMode(*chosen);
    display.setPowered(true);
    return display;
}

void catchInterrupts() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
}

void runFor(float seconds) {
    if (seconds > 0.0f) {
        deadline = std::chrono::steady_clock::now()
                   + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<float>(seconds));
        deadlineSet = true;
    }
}

bool running() {
    if (keepRunning == 0) {
        return false;
    }
    return !deadlineSet || std::chrono::steady_clock::now() < deadline;
}

}
