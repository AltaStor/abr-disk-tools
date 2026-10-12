// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Command line option parsing
//
#include "cli_options.h"

#include <cstring>
#include <iostream>

Cli::OptionParser::OptionParser(int argc, char* argv[], const char* shortOptions,
                                const LongOption* longOptions)
    : m_argc(argc)
    , m_argv(argv)
    , m_shortOptions(shortOptions)
    , m_longOptions(longOptions)
{}

int Cli::OptionParser::next()
{
    m_argument.clear();

    if (m_cluster) {
        return nextShort();
    }

    if (m_index >= m_argc) {
        return -1;
    }

    const char* arg = m_argv[m_index++];

    // "-" alone is how getopt spells an operand meaning standard input, and
    // nothing here takes operands.
    if (m_optionsEnded || arg[0] != '-' || arg[1] == '\0') {
        std::cerr << "Error: unexpected argument '" << arg << "'\n";
        return '?';
    }

    if (arg[1] == '-') {
        if (arg[2] == '\0') {
            m_optionsEnded = true;
            return next();
        }
        return nextLong(arg + 2);
    }

    m_cluster = arg + 1;
    return nextShort();
}

int Cli::OptionParser::nextLong(const std::string& text)
{
    const size_t equals = text.find('=');
    const std::string name = text.substr(0, equals);

    // An exact match wins outright -- "--source" is also the start of every
    // "--source-..." option -- and otherwise a prefix has to name one option.
    const LongOption* pMatch = nullptr;
    std::string candidates;
    int prefixMatches = 0;

    for (const LongOption* pOption = m_longOptions; pOption->name; ++pOption) {
        if (name == pOption->name) {
            pMatch = pOption;
            prefixMatches = 1;
            break;
        }
        if (!name.empty() && std::strncmp(pOption->name, name.c_str(), name.size()) == 0) {
            pMatch = pOption;
            ++prefixMatches;
            candidates += std::string(" '--") + pOption->name + "'";
        }
    }

    if (!pMatch) {
        std::cerr << "Error: unrecognized option '--" << name << "'\n";
        return '?';
    }
    if (prefixMatches > 1) {
        std::cerr << "Error: option '--" << name << "' is ambiguous; possibilities:" << candidates << "\n";
        return '?';
    }

    if (pMatch->argument == Argument::None) {
        if (equals != std::string::npos) {
            std::cerr << "Error: option '--" << pMatch->name << "' doesn't allow an argument\n";
            return '?';
        }
        return pMatch->value;
    }

    if (equals != std::string::npos) {
        m_argument = text.substr(equals + 1);
    } else if (m_index < m_argc) {
        // Taken whatever it looks like, as getopt does: a password may well
        // start with a dash.
        m_argument = m_argv[m_index++];
    } else {
        std::cerr << "Error: option '--" << pMatch->name << "' requires an argument\n";
        return '?';
    }
    return pMatch->value;
}

int Cli::OptionParser::nextShort()
{
    const char letter = *m_cluster++;
    const char* pSpec = letter == ':' ? nullptr : std::strchr(m_shortOptions, letter);

    if (!pSpec) {
        std::cerr << "Error: invalid option -- '" << letter << "'\n";
        if (*m_cluster == '\0') {
            m_cluster = nullptr;
        }
        return '?';
    }

    if (pSpec[1] == ':') {
        // The rest of the cluster is the argument ("-nweb01"), or else the next
        // word is ("-n web01").
        if (*m_cluster != '\0') {
            m_argument = m_cluster;
        } else if (m_index < m_argc) {
            m_argument = m_argv[m_index++];
        } else {
            m_cluster = nullptr;
            std::cerr << "Error: option requires an argument -- '" << letter << "'\n";
            return '?';
        }
        m_cluster = nullptr;
        return letter;
    }

    if (*m_cluster == '\0') {
        m_cluster = nullptr;
    }
    return letter;
}
