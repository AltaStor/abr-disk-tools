// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The POSIX half of the host shim
//
#include "plat.h"

#include <csignal>

namespace {

// Set from a signal handler, so it may only be a plain flag.
volatile sig_atomic_t g_interrupted = 0;

extern "C" void onInterrupt(int)
{
    g_interrupted = 1;
}

}

void Plat::installInterruptHandler()
{
    struct sigaction action = {};

    action.sa_handler = onInterrupt;
    sigemptyset(&action.sa_mask);

    // SA_RESETHAND puts the default disposition back, so a second Ctrl-C still
    // kills the process outright if a hypervisor call is wedged and the copy
    // loop never gets to look at the flag.
    action.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

bool Plat::interruptRequested()
{
    return g_interrupted != 0;
}

std::vector<std::string> Plat::arguments(int argc, char* argv[])
{
    std::vector<std::string> arguments;

    arguments.reserve(static_cast<size_t>(argc < 0 ? 0 : argc));
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    return arguments;
}

std::filesystem::path Plat::path(const std::string& utf8)
{
    // A POSIX path is bytes, and the tool's strings are already those bytes.
    return std::filesystem::path(utf8);
}

// Nothing to set: stdin, stdout and stderr are byte streams here, and a
// terminal needs no telling that what it is being sent is UTF-8.
Plat::StdioSetup::StdioSetup() = default;
Plat::StdioSetup::~StdioSetup() = default;
