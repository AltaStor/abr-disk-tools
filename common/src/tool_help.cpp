// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The usage text every tool prints the same way
//
#include "tool_help.h"

#include <iostream>

#include "tool_options.h"

void Tool::printMainHelp(const std::string& programName, const Branding& branding)
{
    std::cout << "Usage: " << programName << " <command> [options]\n\n"
              << "Commands:\n"
              << "  backup    Create a backup of a disk/volume/" << branding.diskNoun << "\n"
              << "  restore   Restore a disk/volume/" << branding.diskNoun << " from a backup\n"
              << "  copy      Merge a chain of backups into a new backup\n"
              << "  list      List available backups\n"
              << "  info      Show information about a backup\n"
              << "  verify    Check the integrity of a backup\n"
              << "  remove    Delete a backup\n"
              << "  retention Change the immutability a stored backup is held under\n"
              << "  meta_save Store a metadata blob alongside a backup\n"
              << "  meta_load Read back the metadata blob of a backup\n\n"
              << "Run '" << programName << " <command> --help' for command-specific options.\n"
              << "Pass --json to any command to get its result on stdout as a JSON document.\n";
}

void Tool::printStorageOptions()
{
    std::cout << "Backup storage options:\n"
              << "  -F, --folder FOLDER     Backup storage folder or S3 bucket (required)\n"
              << "  -m, --mode MODE         Backup storage mode: 'file' or 's3' (default: file)\n"
              << "  -p, --path PATH         Subfolder within the storage folder (optional)\n"
              << "      --create-folder     Create the storage folder/bucket if it is missing\n"
              << "      --endpoint URL      S3 endpoint URL\n"
              << "      --region REGION     S3 region\n"
              << "      --access-key KEY    S3 access key\n"
              << "      --secret-key KEY    S3 secret key\n"
              << "      --stdin-keys        Read key=, secret-key=, source-key= and\n"
              << "                          source-secret-key= lines from standard input,\n"
              << "                          up to an empty line, instead of taking them\n"
              << "                          on the command line\n"
              << "      --aws-profile NAME  Shared AWS profile to take credentials from\n"
              << "      --path-style        Use path-style S3 addressing\n"
              << "      --no-verify-tls     Disable TLS verification for S3\n";
}

void Tool::printSourceStorageOptions()
{
    std::cout << "Source storage options:\n"
              << "  Each of these overrides the matching option above for the storage the\n"
              << "  source chain is read from. Whatever is not given is taken from the\n"
              << "  destination, so a copy within one repository needs none of them.\n\n"
              << "      --source-folder FOLDER     Storage folder or S3 bucket of the chain\n"
              << "      --source-mode MODE         Storage mode: 'file' or 's3'\n"
              << "      --source-path PATH         Subfolder holding the chain\n"
              << "      --source-key KEY           Decryption key of the chain\n"
              << "      --source-endpoint URL      S3 endpoint URL\n"
              << "      --source-region REGION     S3 region\n"
              << "      --source-access-key KEY    S3 access key\n"
              << "      --source-secret-key KEY    S3 secret key\n"
              << "      --source-aws-profile NAME  Shared AWS profile to take credentials from\n"
              << "      --source-path-style        Use path-style S3 addressing\n"
              << "      --source-no-verify-tls     Disable TLS verification for S3\n";
}

void Tool::printCopyHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " copy [options]\n\n"
              << "Reads a chain of backups (a full backup and, on top of it, the incrementals\n"
              << "taken since) and writes what they add up to as one new full backup.\n"
              << "A block several of them hold is taken from the last one that changed it,\n"
              << "so the result is the disk as of the end of the chain.\n\n"
              << "The new backup is written with the compression, encryption and\n"
              << "immutability given here rather than the chain's, so this is also how\n"
              << "a backup is recompressed, re-keyed, or moved to another repository.\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Name of the backup to create (required)\n"
              << "      --source-names LIST Comma-separated chain to merge, the full backup\n"
              << "                          first and its incrementals in the order they\n"
              << "                          were taken (required)\n"
              << "  -k, --key KEY           Encryption key for the new backup (optional)\n"
              << "  -c, --compression NUM   Compression level 0-" << kMaxCompression
              << " for the new backup\n"
              << "      --immutable MODE    Hold the new backup in 'governance' or\n"
              << "                          'compliance' mode (S3 only)\n"
              << "      --retention DAYS    How long --immutable holds it, in days\n\n";
    printStorageOptions();
    std::cout << "\n";
    printSourceStorageOptions();
}

void Tool::printListHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " list [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "\n";
    printStorageOptions();
}

void Tool::printInfoHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " info [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name; without it the storage folder's\n"
              << "                          own info is reported (optional)\n\n";
    printStorageOptions();
}

void Tool::printVerifyHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " verify [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -k, --key KEY           Decryption key, to also check that it decrypts\n"
              << "                          the backup; the data checksums are verified\n"
              << "                          without decrypting (optional)\n\n";
    printStorageOptions();
}

void Tool::printRemoveHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " remove [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -f, --force             Remove without asking for confirmation\n\n";
    printStorageOptions();
}

void Tool::printRetentionHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " retention [options]\n\n"
              << "Changes the immutability a stored backup is already held under. The\n"
              << "period is counted from now, so it states what the backup has left rather\n"
              << "than what it was originally given.\n\n"
              << "What can be changed is the storage's call, not this tool's: a compliance\n"
              << "hold may only ever be extended, and a governance one cannot be shortened\n"
              << "without the permission to bypass it. A refusal is reported as it was\n"
              << "given.\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "      --immutable MODE    Hold the backup in 'governance' or 'compliance'\n"
              << "                          mode (required, S3 only)\n"
              << "      --retention DAYS    How long to hold it from now, in days (required)\n\n";
    printStorageOptions();
}

void Tool::printMetaSaveHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " meta_save [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -s, --source PATH       File to read the metadata from ('-' or omitted\n"
              << "                          reads standard input)\n\n";
    printStorageOptions();
}

void Tool::printMetaLoadHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " meta_load [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -t, --target PATH       File to write the metadata to ('-' or omitted\n"
              << "                          writes to standard output)\n\n";
    printStorageOptions();
}
