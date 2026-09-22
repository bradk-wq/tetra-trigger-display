#pragma once

#include <string>
#include <vector>

#include "tetra/display.hpp"

namespace app {

struct Options {
    int index = 0;
    std::string mode;
    int quality = 85;
    float seconds = 0.0f;
    bool pace = true;
    bool help = false;
};

/// Parse the flags every example shares, leaving anything else in positional.
Options parse(int argc, char **argv, std::vector<std::string> &positional);

void usage(const char *program, const char *positional, const char *summary);

/// Open the adapter, pick a mode and power the panel on.
tetra::Display start(const Options &options);

/// Install SIGINT and SIGTERM handlers so loops can exit cleanly.
void catchInterrupts();

/// Stop running() after this many seconds. Zero means run until interrupted.
void runFor(float seconds);
bool running();

}
