// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// 'backup' and 'restore', and what they need from the format of the disk
//
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "disk_device.h"
#include "tool_json.h"
#include "tool_options.h"

namespace Tool
{
    // The disk 'backup' reads. A format derives from it to report what the disk says about itself.
    struct DiskSource {
        virtual ~DiskSource() = default;

        // Whatever the reader depends on, such as the VDDK session a VMDK reader is only valid in.
        // It is held here rather than in a derived struct on purpose: the members of a derived
        // struct are destroyed before those of its base, and so before the reader, while here
        // the reader is declared last and so is closed first.
        std::shared_ptr<void> session;
        std::unique_ptr<IDiskReader> reader;

        // Shown in parentheses after the source path, when there is something to show.
        virtual std::string description() const { return {}; }

        // What the format knows about the disk beyond what every backup reports,
        // such as the change tracking id the next incremental is taken against.
        virtual void addDetails(JsonObject& json) const { (void)json; }
        virtual void printDetails() const {}
    };

    // The disk 'restore' writes, with whatever the writer depends on held the same way.
    struct DiskTarget {
        virtual ~DiskTarget() = default;

        std::shared_ptr<void> session;
        std::unique_ptr<IDiskWriter> writer;

        // Set by a format that can create the target, to whether this restore did.
        std::optional<bool> created;
    };

    // What 'backup' and 'restore' need from the format for one run: the command
    // line parsed with the format's own options on top of the shared ones, and
    // the disks opened from what it named.
    // The shared commands get by with the shared half of the parse, and so with
    // Context::parseSharedOptions; these two are the ones that need the rest.
    //
    class DiskTool
    {
    public:
        virtual ~DiskTool() = default;

        // The whole command line, the format's options included.
        virtual bool parseOptions(int argc, char* argv[]) = 0;

        // The shared half of what parseOptions filled in.
        virtual const CommonOptions& options() const = 0;

        virtual void printBackupHelp(const std::string& programName) const = 0;
        virtual void printRestoreHelp(const std::string& programName) const = 0;

        // The format's checks on its own options, for a backup or a restore.
        virtual bool checkOptions(bool forRestore) const = 0;

        // The option that names a disk of the format, "--vmdk" or "--vhdx", and the disk
        // it named, empty when the run reads or writes a plain file or device instead.
        virtual const char* diskOption() const = 0;
        virtual const std::string& disk() const = 0;

        virtual std::unique_ptr<DiskSource> openSource() = 0;

        // image is the geometry of the backup, which a disk that is not there yet is created with.
        virtual std::unique_ptr<DiskTarget> openTarget(bool keepExisting, const DiskGeometry& image) = 0;
    };

    int runBackup(const std::string& programName, int argc, char* argv[]);
    int runRestore(const std::string& programName, int argc, char* argv[]);
}
