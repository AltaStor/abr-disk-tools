// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The --json result document, and the stderr filter it needs
//
#pragma once

#include <iostream>
#include <string>
#include <vector>

#include "abr_backup.h"
#include "tool_options.h"

namespace Tool
{
    // ---------------------------------------------------------------------------
    // JSON output
    //
    // --json replaces the human-readable report a command ends with by a single
    // JSON document. It covers the result only: the progress percentages a long
    // copy emits stay plain "NN%" lines ahead of it, one per line. The shapes are
    // flat and fixed, so a small writer here is cheaper than taking on a JSON
    // library for them.
    // ---------------------------------------------------------------------------

    // Set once a command has written its result document, so that run() knows
    // not to follow a failure with a second one. JsonObject's constructor is
    // the only thing that sets it.
    bool jsonEmitted();
    void setJsonEmitted();

    std::string jsonEscape(const std::string& value);

    // Writes one flat JSON object to stdout, a field per line and as the fields are
    // added; the closing brace goes out when it is destroyed, so nothing else may
    // be written to stdout while one is alive.
    class JsonObject
    {
    public:
        JsonObject(const std::string& command, const std::string& status)
        {
            std::cout << "{";
            setJsonEmitted();
            addString("command", command);
            addString("status", status);
        }

        ~JsonObject() { std::cout << "\n}\n" << std::flush; }

        JsonObject(const JsonObject&) = delete;
        JsonObject& operator=(const JsonObject&) = delete;

        void addString(const std::string& name, const std::string& value)
        {
            key(name) << '"' << jsonEscape(value) << '"';
        }

        void addNumber(const std::string& name, uint64_t value) { key(name) << value; }

        void addReal(const std::string& name, double value) { key(name) << value; }

        void addBool(const std::string& name, bool value) { key(name) << (value ? "true" : "false"); }

        void addStrings(const std::string& name, const std::vector<std::string>& values)
        {
            std::ostream& out = key(name);

            if (values.empty()) {
                out << "[]";
                return;
            }

            for (size_t i = 0; i < values.size(); ++i) {
                out << (i == 0 ? "[" : ",") << "\n    \"" << jsonEscape(values[i]) << '"';
            }
            out << "\n  ]";
        }

    private:
        std::ostream& key(const std::string& name)
        {
            std::cout << (m_empty ? "\n" : ",\n") << "  \"" << jsonEscape(name) << "\": ";
            m_empty = false;
            return std::cout;
        }

        bool m_empty = true;
    };

    // The block of backup properties that 'backup' and 'info' both report.
    void addBackupInfo(JsonObject& json, const Abr::BackupInfo& info);

    // What restore applied: the backup and its name, or the storage and the chain read from it,
    // reported the way 'copy' reports its chain.
    void addRestoredFrom(JsonObject& json, const CommonOptions& options);

    // The block of folder properties that 'list' and 'info' both report.
    void addFolderInfo(JsonObject& json, const Abr::BackupFolderInfo& folder);

    // The immutability a stored backup is actually held under. The storage
    // reports it when the backup is opened, since the backup itself keeps no
    // record of it.
    void addBackupImmutability(JsonObject& json, const Abr::BackupInfo& info,
                               const Abr::BackupFolderInfo& folder);

    std::string backupImmutability(const Abr::BackupInfo& info);
    std::string folderImmutability(const Abr::BackupFolderInfo& folder);

    // The immutability a command was asked to hold a backup in, as 'backup', 'copy' and
    // 'retention' report it: the period in days, or the moment it ends, whichever was given.
    void addRequestedImmutability(JsonObject& json, const CommonOptions& options);
    std::string requestedImmutability(const CommonOptions& options);
    std::string requestedRetention(const CommonOptions& options);

    // In --json mode the result document is the only thing a caller parses, so
    // a command that fails still has to report through it.
    // Every diagnostic in the tool and in the disk layers under it goes to std::cerr
    // behind an "Error: " prefix; this filter keeps the first such line while passing
    // everything -- warnings and the disk layers' notes included -- on to the real stderr.
    class ErrorCollector : public std::streambuf
    {
    public:
        explicit ErrorCollector(std::ostream& stream)
            : m_stream(stream)
            , m_pTarget(stream.rdbuf(this))
        {}

        ~ErrorCollector() override { m_stream.rdbuf(m_pTarget); }

        ErrorCollector(const ErrorCollector&) = delete;
        ErrorCollector& operator=(const ErrorCollector&) = delete;

        const std::string& firstError() const { return m_firstError; }

    protected:
        int overflow(int ch) override
        {
            if (ch == traits_type::eof()) {
                return traits_type::not_eof(ch);
            }

            char c = traits_type::to_char_type(ch);
            if (c == '\n') {
                takeLine();
            } else {
                m_line += c;
            }
            return m_pTarget->sputc(c);
        }

        int sync() override { return m_pTarget->pubsync(); }

    private:
        void takeLine()
        {
            static const std::string kPrefix = "Error: ";

            if (m_firstError.empty() && m_line.compare(0, kPrefix.size(), kPrefix) == 0) {
                m_firstError = m_line.substr(kPrefix.size());
            }
            m_line.clear();
        }

        std::ostream& m_stream;
        std::streambuf* m_pTarget;
        std::string m_line;
        std::string m_firstError;
    };

    // Spotted ahead of the command, so that a failure is still reported as JSON
    // when it happens before -- or instead of -- the command's own option
    // parsing.
    bool jsonRequested(int argc, char* argv[]);
}
