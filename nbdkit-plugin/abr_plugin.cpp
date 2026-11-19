// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// nbdkit-abr-plugin: serves a chain of .abr backups read-only over NBD
//
// A Proxmox VE restore runs one nbdkit process per disk with this plugin, and qemu-img reads from it the disk the
// chain restores to. The chain is read through ChainReader, so a block comes from the newest backup that holds it, as
// in a restore of the whole chain with the tools.
//
//   nbdkit -r -U SOCKET nbdkit-abr-plugin.so folder=DIR [path=PATH] chain=FULL[,INCREMENTAL...] [key=-FD]
//
#define NBDKIT_API_VERSION 2
// backuplib's restore reads are not thread-safe, so nbdkit passes one request at a time, across all connections.
#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_ALL_REQUESTS
#include <nbdkit-plugin.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "chain.h"
#include "chain_reader.h"
#include "s3_backup.h"
#include "version.h"

namespace
{
    //
    // backuplib and abr-chain-reader report on std::cerr and std::cout. Inside nbdkit each line goes to nbdkit's log
    // instead: std::cerr through nbdkit_error, so that a failed callback says why, and std::cout through nbdkit_debug,
    // which nbdkit shows with -v.
    //
    class LogBuf final : public std::streambuf
    {
    public:
        explicit LogBuf(bool errors) : m_errors(errors) {}

        // A last line without a newline.
        void flushLine()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            emit();
        }

    protected:
        int_type overflow(int_type ch) override
        {
            if (traits_type::eq_int_type(ch, traits_type::eof())) {
                return traits_type::not_eof(ch);
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (ch == '\n') {
                emit();
            } else {
                m_line.push_back(traits_type::to_char_type(ch));
            }
            return ch;
        }

    private:
        void emit()
        {
            // nbdkit labels each line as an error itself, so the "Error: " prefix goes, and so does the indent of
            // a continuation line.
            std::string_view text = m_line;
            if (text.starts_with("Error: ")) {
                text.remove_prefix(7);
            }
            text.remove_prefix(std::min(text.find_first_not_of(' '), text.size()));

            if (!text.empty()) {
                const std::string line(text);
                if (m_errors) {
                    nbdkit_error("%s", line.c_str());
                } else {
                    nbdkit_debug("%s", line.c_str());
                }
            }
            m_line.clear();
        }

        const bool m_errors;
        std::mutex m_mutex;
        std::string m_line;
    };

    // The parameters, as nbdkit passed them.
    struct Config {
        Chain::StorageOptions storage;
        std::vector<std::string> names;
        std::string s3Parameter;
    };

    // What serving needs, from the opening of the chain to cleanup. The members go in reverse order: the chain closes
    // its backups before their storage goes, and the AWS SDK shuts down last.
    struct Server {
        std::unique_ptr<Abr::AwsSdkGuard> aws;
        std::shared_ptr<Abr::IBackupManager> manager;
        Chain::ChainReader reader;
        // What the chain holds data for, merged where it touches, in ascending order.
        std::vector<ByteRange> dataRanges;
        uint64_t size = 0;
        uint32_t preferredBlockSize = 0;
        uint64_t reads = 0;
    };

    // Heap objects, never static ones: nbdkit does not promise to call cleanup, and the AWS SDK must not shut down
    // during static destruction.
    Config* g_config = nullptr;
    Server* g_server = nullptr;
    LogBuf* g_errorLog = nullptr;
    LogBuf* g_debugLog = nullptr;
    std::streambuf* g_savedCerr = nullptr;
    std::streambuf* g_savedCout = nullptr;

    // Runs a callback body, so that no C++ exception reaches nbdkit.
    template <typename Result, typename Body>
    Result guarded(const char* what, Result failure, Body&& body)
    {
        try {
            return body();
        } catch (const std::exception& e) {
            nbdkit_error("%s: %s", what, e.what());
        } catch (...) {
            nbdkit_error("%s: unexpected exception", what);
        }
        nbdkit_set_error(EIO);
        return failure;
    }

    // A secret in any form nbdkit_read_password takes: the value itself, -FD for a file descriptor, +FILE for a file.
    int readSecret(const char* value, std::string& secret)
    {
        char* pSecret = nullptr;
        if (nbdkit_read_password(value, &pSecret) == -1) {
            return -1;
        }
        secret = pSecret;
        explicit_bzero(pSecret, std::strlen(pSecret));
        std::free(pSecret);
        return 0;
    }

    int readBool(const char* key, const char* value, bool& result)
    {
        const int parsed = nbdkit_parse_bool(value);
        if (parsed == -1) {
            nbdkit_error("%s takes a boolean, such as true or false", key);
            return -1;
        }
        result = parsed != 0;
        return 0;
    }

    // Splits NAME[,NAME...] into the names of the chain.
    int readChain(const char* value, std::vector<std::string>& names)
    {
        names.clear();
        const std::string text = value;
        size_t begin = 0;
        while (true) {
            const size_t end = text.find(',', begin);
            const std::string name = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (name.empty()) {
                nbdkit_error("chain has an empty backup name: '%s'", value);
                return -1;
            }
            names.push_back(name);
            if (end == std::string::npos) {
                return 0;
            }
            begin = end + 1;
        }
    }

    // The largest power of two in the largest block the chain stores, within the 512 bytes to 32 MiB that NBD allows.
    // A read of that size and alignment decodes each stored block once.
    uint32_t preferredBlockSize(uint32_t largestBlock)
    {
        return std::bit_floor(std::clamp<uint32_t>(largestBlock, 512, 32 * 1024 * 1024));
    }

    void abrLoad()
    {
        g_config = new Config;
        g_errorLog = new LogBuf(true);
        g_debugLog = new LogBuf(false);
        g_savedCerr = std::cerr.rdbuf(g_errorLog);
        g_savedCout = std::cout.rdbuf(g_debugLog);
    }

    // Puts the streams back before nbdkit unloads the code that writes them.
    void abrUnload()
    {
        g_errorLog->flushLine();
        g_debugLog->flushLine();
        std::cerr.rdbuf(g_savedCerr);
        std::cout.rdbuf(g_savedCout);
    }

    int abrConfig(const char* key, const char* value)
    {
        return guarded("config", -1, [&] {
            Config& config = *g_config;
            Chain::StorageOptions& storage = config.storage;
            const std::string name = key;

            // config_complete refuses S3 parameters without mode=s3, naming the first one given.
            if (config.s3Parameter.empty() &&
                (name == "endpoint" || name == "region" || name == "access-key" || name == "secret-key" ||
                 name == "aws-profile" || name == "path-style" || name == "verify-tls")) {
                config.s3Parameter = name;
            }

            if (name == "folder") {
                storage.folder = value;
            } else if (name == "path") {
                storage.path = value;
            } else if (name == "chain") {
                return readChain(value, config.names);
            } else if (name == "mode") {
                storage.mode = value;
            } else if (name == "key") {
                return readSecret(value, storage.key);
            } else if (name == "endpoint") {
                storage.endpoint = value;
            } else if (name == "region") {
                storage.region = value;
            } else if (name == "access-key") {
                storage.accessKey = value;
            } else if (name == "secret-key") {
                return readSecret(value, storage.secretKey);
            } else if (name == "aws-profile") {
                storage.profile = value;
            } else if (name == "path-style") {
                return readBool(key, value, storage.pathStyle);
            } else if (name == "verify-tls") {
                return readBool(key, value, storage.verifyTls);
            } else {
                nbdkit_error("unknown parameter '%s'", key);
                return -1;
            }
            return 0;
        });
    }

    int abrConfigComplete()
    {
        return guarded("config_complete", -1, [] {
            Config& config = *g_config;
            Chain::StorageOptions& storage = config.storage;

            if (storage.folder.empty()) {
                nbdkit_error("folder is required: the repository folder, or the bucket with mode=s3");
                return -1;
            }
            if (config.names.empty()) {
                nbdkit_error("chain is required: the full backup, then the incrementals on top of it, comma-separated");
                return -1;
            }
            if (storage.mode != "file" && storage.mode != "s3") {
                nbdkit_error("mode must be 'file' or 's3'");
                return -1;
            }
            if (storage.mode == "file" && !config.s3Parameter.empty()) {
                nbdkit_error("%s goes with mode=s3", config.s3Parameter.c_str());
                return -1;
            }

            // nbdkit changes to the root directory when it forks into the background, so the folder is fixed now.
            if (storage.mode == "file") {
                char* pFolder = nbdkit_absolute_path(storage.folder.c_str());
                if (!pFolder) {
                    return -1;
                }
                storage.folder = pFolder;
                std::free(pFolder);
            }
            return 0;
        });
    }

    // Opens the chain once for the whole process.
    int openServer()
    {
        return guarded("opening the backup chain", -1, [] {
            const Config& config = *g_config;
            auto pServer = std::make_unique<Server>();

            if (config.storage.mode == "s3") {
                pServer->aws = std::make_unique<Abr::AwsSdkGuard>();
            }
            pServer->manager = Chain::openManager(config.storage);
            if (!pServer->manager) {
                return -1;
            }

            auto pChain = std::make_unique<Chain::BackupChain>();
            if (!Chain::openChain(*pServer->manager, config.storage, config.names, *pChain)) {
                return -1;
            }

            // The tools apply such a chain with --force onto a disk that holds its parent; here nothing does.
            if (pChain->backups.front()->getInfo().incremental) {
                nbdkit_error("'%s' is an incremental backup; a chain starts with a full backup",
                             config.names.front().c_str());
                return -1;
            }

            pServer->preferredBlockSize = preferredBlockSize(Chain::chainBufferSize(*pChain));
            if (!pServer->reader.open(std::move(pChain)) || !pServer->reader.dataRanges(pServer->dataRanges)) {
                return -1;
            }

            const DiskGeometry& geometry = pServer->reader.geometry();
            pServer->size = geometry.sectors * geometry.sectorSize;
            nbdkit_debug("serving %zu backups, %" PRIu64 " bytes, %zu data ranges", config.names.size(),
                         pServer->size, pServer->dataRanges.size());

            g_server = pServer.release();
            return 0;
        });
    }

    // A chain on a file repository opens before nbdkit forks, writes its pidfile or starts the --run command, so that
    // a failure stops nbdkit before any of them.
    int abrGetReady()
    {
        return g_config->storage.mode == "s3" ? 0 : openServer();
    }

    // The AWS SDK starts threads, which a fork would lose, so a chain on S3 opens after it. A failure there ends nbdkit
    // after its pidfile is written, and in the background it shows only in syslog.
    int abrAfterFork()
    {
        return g_config->storage.mode == "s3" ? openServer() : 0;
    }

    void abrCleanup()
    {
        if (!g_server) {
            return;
        }
        nbdkit_debug("served %" PRIu64 " reads, decoded %" PRIu64 " stored blocks", g_server->reads,
                     g_server->reader.chunkLoads());

        guarded("cleanup", 0, [] {
            delete g_server;
            return 0;
        });
        g_server = nullptr;
    }

    // Every connection reads the one chain, so the handle is the server itself.
    void* abrOpen(int readonly)
    {
        (void)readonly;
        if (!g_server) {
            nbdkit_error("the backup chain is not open");
        }
        return g_server;
    }

    int64_t abrGetSize(void* handle)
    {
        return static_cast<int64_t>(static_cast<Server*>(handle)->size);
    }

    int abrBlockSize(void* handle, uint32_t* pMinimum, uint32_t* pPreferred, uint32_t* pMaximum)
    {
        *pMinimum = 1;
        *pPreferred = static_cast<Server*>(handle)->preferredBlockSize;
        *pMaximum = 0xffffffff;
        return 0;
    }

    int abrPread(void* handle, void* pBuf, uint32_t count, uint64_t offset, uint32_t flags)
    {
        (void)flags;
        return guarded("pread", -1, [&] {
            Server& server = *static_cast<Server*>(handle);
            if (!server.reader.read(offset, count, pBuf)) {
                nbdkit_error("failed to read %" PRIu32 " bytes at offset %" PRIu64, count, offset);
                nbdkit_set_error(EIO);
                return -1;
            }
            ++server.reads;
            return 0;
        });
    }

    int abrCanExtents(void* handle)
    {
        (void)handle;
        return 1;
    }

    // Data where any backup of the chain holds a block, and holes that read as zeros everywhere else.
    int abrExtents(void* handle, uint32_t count, uint64_t offset, uint32_t flags, struct nbdkit_extents* pExtents)
    {
        return guarded("extents", -1, [&] {
            const Server& server = *static_cast<Server*>(handle);
            const std::vector<ByteRange>& ranges = server.dataRanges;
            const uint64_t end = offset + count;
            const uint32_t hole = NBDKIT_EXTENT_HOLE | NBDKIT_EXTENT_ZERO;

            // The first range that ends past the offset.
            auto it = std::upper_bound(ranges.begin(), ranges.end(), offset,
                                       [](uint64_t value, const ByteRange& range) { return value < range.offset; });
            if (it != ranges.begin() && std::prev(it)->offset + std::prev(it)->length > offset) {
                --it;
            }

            uint64_t cursor = offset;
            while (cursor < end) {
                int added;
                if (it == ranges.end()) {
                    added = nbdkit_add_extent(pExtents, cursor, server.size - cursor, hole);
                    cursor = server.size;
                } else if (it->offset > cursor) {
                    added = nbdkit_add_extent(pExtents, cursor, it->offset - cursor, hole);
                    cursor = it->offset;
                } else {
                    added = nbdkit_add_extent(pExtents, cursor, it->offset + it->length - cursor, 0);
                    cursor = it->offset + it->length;
                    ++it;
                }
                if (added == -1) {
                    return -1;
                }
                if (flags & NBDKIT_FLAG_REQ_ONE) {
                    break;
                }
            }
            return 0;
        });
    }

    // can_multi_conn stays at its default, false: a well-behaved client opens one connection, and nbdkit serializes
    // requests across all of them anyway.
    struct nbdkit_plugin g_plugin = {
        .name = "abr",
        .longname = "nbdkit AltaStor backup plugin",
        .version = ABR_DISK_TOOLS_VERSION,
        .description = "Serves a chain of AltaStor .abr backups read-only, as the disk it restores to.",
        .load = abrLoad,
        .unload = abrUnload,
        .config = abrConfig,
        .config_complete = abrConfigComplete,
        .config_help =
            "folder=DIR|BUCKET    (required) The repository folder, or the bucket with mode=s3.\n"
            "path=PATH            Subfolder of the backups in the repository.\n"
            "chain=NAME[,NAME...] (required) The full backup, then the incrementals on top of it,\n"
            "                     in the order they were taken.\n"
            "mode=file|s3         Repository storage (default: file).\n"
            "key=KEY              Decryption key. -FD reads it from file descriptor FD, +FILE from a file.\n"
            "endpoint=URL region=REGION access-key=KEY secret-key=KEY aws-profile=NAME\n"
            "path-style=BOOL verify-tls=BOOL\n"
            "                     S3 settings, with mode=s3. secret-key takes -FD and +FILE as key does.",
        .open = abrOpen,
        .get_size = abrGetSize,
        .errno_is_preserved = 0,
        .pread = abrPread,
        .can_extents = abrCanExtents,
        .extents = abrExtents,
        .get_ready = abrGetReady,
        .after_fork = abrAfterFork,
        .cleanup = abrCleanup,
        .block_size = abrBlockSize,
    };
}

NBDKIT_REGISTER_PLUGIN(g_plugin)
