// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The --json result document
//
#include "tool_json.h"

#include <sstream>

// Abr::immutabilityModeName lives here.
// It kept out of tool_json.h on purpose:
// s3_backup.h pulls in <aws/core/Aws.h> and <aws/s3/S3Client.h>,
// and the library's public headers stay AWS-free so that
// its consumers' other translation units do too.
#include "s3_backup.h"

namespace {
    bool g_jsonEmitted = false;
}

bool Tool::jsonEmitted()
{
    return g_jsonEmitted;
}

void Tool::setJsonEmitted()
{
    g_jsonEmitted = true;
}

std::string Tool::jsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (unsigned char c : value) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b";  break;
            case '\f': escaped += "\\f";  break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:
                if (c < 0x20) {
                    char unicode[7];
                    std::snprintf(unicode, sizeof(unicode), "\\u%04x", c);
                    escaped += unicode;
                } else {
                    // Bytes above 0x7f go through untouched, so the result is
                    // valid JSON exactly when the input was valid UTF-8.
                    escaped += static_cast<char>(c);
                }
        }
    }
    return escaped;
}

// The block of backup properties that 'backup' and 'info' both report.
void Tool::addBackupInfo(JsonObject& json, const Abr::BackupInfo& info)
{
    json.addNumber("sectors", info.sectors);
    json.addNumber("sector_size", info.sectorSize);
    json.addNumber("physical_sector_size", info.phySectorSize);
    json.addNumber("raw_data_size", info.orig_data_size);
    json.addString("compression", info.compression ? "zstd" : "none");
    json.addNumber("compression_level", info.compression);

    if (info.compression) {
        json.addNumber("stored_data_size", info.stored_data_size);
        if (info.orig_data_size > 0) {
            json.addReal("compression_ratio",
                         static_cast<double>(info.stored_data_size) / info.orig_data_size);
        }
    }

    json.addString("encryption", info.encryption ? "AES-256-GCM" : "none");
}

// The immutability of the storage folder, which 'info' reports both on its own
// and next to a backup. A backup keeps no record of its own retention -- it is
// the folder that holds it immutable -- so this is what the immutability of
// a stored backup comes down to.
void Tool::addFolderInfo(JsonObject& json, const Abr::BackupFolderInfo& folder)
{
    json.addString("immutability_supported",
                   Abr::immutabilityModeName(folder.m_immutabilitySupported));
    json.addString("immutability", Abr::immutabilityModeName(folder.m_immutabilityCurrent));

    if (folder.m_immutabilityCurrent != Abr::ImmutabilityMode::NONE) {
        json.addNumber("retention_days", static_cast<uint64_t>(folder.m_immutabilityDays));
    }
}

// The immutability a stored backup is actually held under. The storage reports
// it when the backup is opened, since the backup itself keeps no record of it.
void Tool::addBackupImmutability(JsonObject& json, const Abr::BackupInfo& info,
                                 const Abr::BackupFolderInfo& folder)
{
    json.addString("immutability_supported",
                   Abr::immutabilityModeName(folder.m_immutabilitySupported));
    json.addString("immutability", Abr::immutabilityModeName(info.m_immutabilityMode));

    if (info.m_immutabilityMode != Abr::ImmutabilityMode::NONE) {
        json.addNumber("retention_days", static_cast<uint64_t>(info.m_immutabilityDays));
    }
}

std::string Tool::backupImmutability(const Abr::BackupInfo& info)
{
    if (info.m_immutabilityMode == Abr::ImmutabilityMode::NONE) {
        return "none, the backup can be deleted";
    }
    return std::string(Abr::immutabilityModeName(info.m_immutabilityMode))
        + ", held for another " + std::to_string(info.m_immutabilityDays) + " day(s)";
}

std::string Tool::folderImmutability(const Abr::BackupFolderInfo& folder)
{
    if (folder.m_immutabilityCurrent == Abr::ImmutabilityMode::NONE) {
        return "none, new backups stay deletable";
    }
    return std::string(Abr::immutabilityModeName(folder.m_immutabilityCurrent))
        + ", new backups retained for " + std::to_string(folder.m_immutabilityDays) + " day(s)";
}

void Tool::addRequestedImmutability(JsonObject& json, const CommonOptions& options)
{
    json.addString("immutability", Abr::immutabilityModeName(options.immutability));

    if (options.immutability == Abr::ImmutabilityMode::NONE) {
        return;
    }
    if (options.retentionTill != 0) {
        json.addString("retention_till", formatRetentionTill(options.retentionTill));
    } else {
        json.addNumber("retention_days", static_cast<uint64_t>(options.retentionDays));
    }
}

std::string Tool::requestedImmutability(const CommonOptions& options)
{
    if (options.immutability == Abr::ImmutabilityMode::NONE) {
        return "none";
    }
    return std::string(Abr::immutabilityModeName(options.immutability)) + ", " + requestedRetention(options);
}

std::string Tool::requestedRetention(const CommonOptions& options)
{
    if (options.retentionTill != 0) {
        return "retained until " + formatRetentionTill(options.retentionTill);
    }
    return "retained for " + std::to_string(options.retentionDays) + " day(s)";
}

// Spotted ahead of the command, so that a failure is still reported as JSON
// when it happens before (or instead of) the command's own option parsing.
bool Tool::jsonRequested(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            return true;
        }
    }
    return false;
}