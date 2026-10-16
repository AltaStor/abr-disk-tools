// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// What the tools need from the host, in the two shapes the hosts offer it
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Plat
{
    //
    // Catches the interrupt the host spells its own way -- SIGINT and SIGTERM
    // on POSIX, Ctrl-C, Ctrl-Break and a closed console window on Windows --
    // and sets a flag the copy loops poll.
    //
    // It never ends the process itself. A backup or restore that stops dead
    // leaves work behind that only an unwinding loop puts right: a half
    // written restore target, a virtual disk still attached and its volumes
    // offline, a VM with its methods disabled. A second interrupt does fall
    // through to the host's default handling, so a run wedged inside
    // a hypervisor call can still be killed.
    //
    void installInterruptHandler();

    // Whether an interrupt has been asked for. Polled, never blocking, and
    // silent -- the message belongs to Tool::interrupted(), which knows what
    // the tool asking has to release.
    bool interruptRequested();

    // The process's own arguments as UTF-8, argv[0] first.
    //
    // On POSIX these are main()'s, unchanged. On Windows they are read from
    // the wide command line instead and the ones passed in are ignored:
    // the arguments main() is handed have been through the ANSI code page, which
    // cannot hold every path or VM name. Both tools therefore pass main()'s
    // along and let this decide, so that main() reads the same in both.
    std::vector<std::string> arguments(int argc, char* argv[]);

    // A narrow UTF-8 path as std::filesystem wants it. On Windows that means
    // widening it first: filesystem::path reads a narrow string in the active
    // code page, which is not UTF-8 for every process the tool runs in.
    std::filesystem::path path(const std::string& utf8);

    //
    // What the tool needs from the console and the standard streams, set for
    // the whole run and put back when it ends. On POSIX it does nothing --
    // the streams are already byte streams and a terminal needs no telling
    // about UTF-8 -- but it is still constructed there, so that main() reads
    // the same in every tool.
    //
    class StdioSetup
    {
    public:
        StdioSetup();
        ~StdioSetup();

        StdioSetup(const StdioSetup&) = delete;
        StdioSetup& operator=(const StdioSetup&) = delete;

    private:
        // The console output code page to put back; 0 means there is none to.
        // Spelled unsigned rather than UINT so that one declaration serves
        // both hosts, and unused on the POSIX side.
        [[maybe_unused]] unsigned m_codePage = 0;
    };
}
