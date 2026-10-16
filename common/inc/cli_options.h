// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Command line option parsing
//
#pragma once

#include <string>

namespace Cli
{
    enum class Argument { None, Required };

    struct LongOption {
        const char* name; // null ends the table
        Argument argument;
        int value;
    };

    //
    // Walks a command's arguments the way getopt_long does, so that the tool
    // takes the same command lines on Windows as abr-vmdk-tool does on Linux:
    // "--name value" and "--name=value", "-n value" and "-nvalue", flags
    // bundled as "-fh", and any unambiguous prefix of a long option.
    // MSVC has no getopt_long, and MinGW's reports its errors in a shape the tool's
    // --json failure document cannot pick up.
    //
    // It differs in two places, both on purpose. An argument that is not
    // an option is refused instead of being set aside, since no command takes one
    // and a stray word is a mistake, not an operand. And every complaint goes
    // to std::cerr behind "Error: ", where main() finds it for the JSON report.
    //
    class OptionParser
    {
    public:
        // argv[0] is the command and is skipped. Short options use getopt's
        // syntax: each letter, followed by ':' when it takes an argument.
        OptionParser(int argc, char* argv[], const char* shortOptions,
                     const LongOption* longOptions);

        // The value of the next option; -1 once the arguments are used up, or
        // '?' for one that could not be parsed, already reported by then.
        int next();

        // The argument of the option next() returned last, if it takes one.
        const std::string& argument() const { return m_argument; }

    private:
        int nextLong(const std::string& text);
        int nextShort();

        int m_argc;
        char** m_argv;
        const char* m_shortOptions;
        const LongOption* m_longOptions;

        int m_index = 1;
        const char* m_cluster = nullptr; // the rest of a "-abc" being taken apart
        bool m_optionsEnded = false;     // after "--"
        std::string m_argument;
    };
}
