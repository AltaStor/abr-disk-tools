// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The commands that touch no backend, and the scaffolding around all of them
//
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "tool_help.h"
#include "tool_options.h"

namespace Tool
{
    class DiskTool; // tool_disk.h
    //
    // What the shared commands need to know about the tool running them.
    //
    // Reached through context() rather than passed as a parameter, so that
    // the command bodies stayed a verbatim move out of each tool -- and because
    // interrupted(), buried inside the copy loops, needs the branding too and
    // would otherwise force a signature change on those as well.
    //
    struct Context {
        Branding branding;

        // The tool's own parse, narrowed to the half these commands read. It
        // parses the whole command line, this tool's options included, and
        // hands back only the shared part: a bad value in the tool's half is
        // still an error on a command that never looks at it.
        std::function<bool(int argc, char* argv[], CommonOptions& options)> parseSharedOptions;

        // A fresh instance of the format's half of 'backup' and 'restore', one per run.
        std::function<std::unique_ptr<DiskTool>()> diskTool;
    };

    // Installed by run() before any command sees an argument, and valid until
    // run() returns. Aborts with a message if a command is reached without it.
    const Context& context();

    // Returned by a dispatch handed a command name it does not know; run()
    // then reports it and prints the main help.
    constexpr int kUnknownCommand = -1;

    using Dispatch = std::function<int(const std::string& programName,
                                       const std::string& command,
                                       int argc, char* argv[])>;

    // Everything main() does around the command itself: the console and the standard
    // streams, the UTF-8 argument vector, the AWS SDK lifetime, the interrupt handler,
    // the program name, -h/--help and -v/--version, the stderr filter --json needs,
    // the try/catch a storage library makes necessary, and the failure document
    // a command that died owes its caller.
    //
    int run(int argc, char* argv[], const Context& context, const Dispatch& dispatch);

    // The name the tool was started by, without its directory or ".exe", for
    // the usage lines.
    std::string programNameOf(const std::string& invokedAs, const char* fallback);

    int runCopy(const std::string& programName, int argc, char* argv[]);
    int runList(const std::string& programName, int argc, char* argv[]);
    int runInfo(const std::string& programName, int argc, char* argv[]);
    int runVerify(const std::string& programName, int argc, char* argv[]);
    int runRemove(const std::string& programName, int argc, char* argv[]);
    int runRetention(const std::string& programName, int argc, char* argv[]);
    int runMetaSave(const std::string& programName, int argc, char* argv[]);
    int runMetaLoad(const std::string& programName, int argc, char* argv[]);
}
