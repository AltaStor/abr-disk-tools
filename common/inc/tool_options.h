// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The options every command shares, and the parsing every tool does the same way
//
#pragma once

#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <vector>

#include "abr_backup.h"
#include "cli_options.h"

namespace Tool
{
    constexpr uint32_t kDefaultChunkSize = 4 * 1024 * 1024;
    constexpr const char* kDefaultMode = "file";
    constexpr int kDefaultCompression = 3;

    // The highest level backuplib accepts, which is zstd's own.
    constexpr int kMaxCompression = 22;

    //
    // The values the library's own long-only options use.
    //
    // A tool's own start at kOptToolFirst, so a row added to either table can
    // never collide with a row in the other, and the library can grow a new
    // option without renumbering a tool's.
    //
    enum Option {
        kOptEndpoint = 256,
        kOptRegion,
        kOptAccessKey,
        kOptSecretKey,
        kOptProfile,
        kOptPathStyle,
        kOptCreateFolder,
        kOptNoVerifyTls,
        kOptImmutable,
        kOptRetention,
        kOptChangeId,
        kOptType,
        kOptJson,

        // The chain 'copy' merges, and the storage it is read from. Each of the
        // storage options mirrors a destination one and falls back to it, so a
        // copy inside one repository needs none of them and a copy between two
        // needs only what actually differs.
        kOptSourceNames,
        kOptSourceMode,
        kOptSourceFolder,
        kOptSourcePath,
        kOptSourceKey,
        kOptSourceEndpoint,
        kOptSourceRegion,
        kOptSourceAccessKey,
        kOptSourceSecretKey,
        kOptSourceProfile,
        kOptSourcePathStyle,
        kOptSourceNoVerifyTls,

        kOptStdinKeys,

        // Reserved: the library will never use a value at or above this, and
        // leaves room below it to add one. A tool's enum starts here.
        kOptToolFirst = 512,
    };

    struct StorageOptions {
        std::string mode = kDefaultMode;
        std::string folder;
        std::string path;
        std::string name;
        std::string key;
        int compression = kDefaultCompression;

        std::string endpoint;
        std::string region;
        std::string accessKey;
        std::string secretKey;
        std::string profile;
        bool pathStyle = false;
        bool verifyTls = true;
        bool createFolder = false;
    };

    // What the --source-<...> options overrode, and nothing else. A field left
    // unset is taken from the destination storage, which is what lets 'copy'
    // read a chain out of the same repository it writes to without being told
    // twice.
    struct SourceStorageOptions {
        std::optional<std::string> mode;
        std::optional<std::string> folder;
        std::optional<std::string> path;
        std::optional<std::string> key;
        std::optional<std::string> endpoint;
        std::optional<std::string> region;
        std::optional<std::string> accessKey;
        std::optional<std::string> secretKey;
        std::optional<std::string> profile;
        std::optional<bool> pathStyle;
        std::optional<bool> verifyTls;
    };

    //
    // Named CommonOptions, not Options, and that is not cosmetic: each tool
    // derives its own "struct Options : Tool::CommonOptions" and keeps it at
    // global scope, where a "using namespace Tool;" is in effect. Two types
    // called Options, one here and one there, would make the unqualified name
    // ambiguous rather than shadowed -- ill-formed at every use site.
    //
    struct CommonOptions {
        StorageOptions storage;
        SourceStorageOptions sourceStorage;

        // The chain 'copy' merges, in the order it has to be applied: the full
        // backup first, then each incremental on top of the one before it.
        std::vector<std::string> sourceNames;

        std::string source;
        std::string target;
        std::string changeId;
        std::string type = "full";

        // What the backup asks to be held in. It is written on the objects of
        // that one backup as its own Object Lock retention, on top of whatever
        // default rule the bucket applies to everything it holds, so it needs S3.
        Abr::ImmutabilityMode immutability = Abr::ImmutabilityMode::NONE;
        int retentionDays = 0;

        bool force = false;
        bool help = false;
        bool json = false;

        // The secrets come on standard input rather than on the command line,
        // which every process on the host can read. See readStdinKeys.
        bool stdinKeys = false;

        bool incremental() const { return type == "incremental" || type == "inc"; }
    };

    // All ten short options are shared; a tool needing a letter of its own
    // appends to this when it builds its parser.
    inline constexpr const char* kShortOptions = "hF:m:p:n:s:t:k:c:f";

    //
    // The table Cli::OptionParser wants is one null-terminated array, and its
    // prefix matching makes the ORDER observable: "--t" names four options in
    // abr-vmdk-tool, and the ambiguity message lists them in table order. So
    // the library hands out its rows in the two blocks they occupy today --
    // the shared options before a tool's, the --source-<...> ones after --
    // rather than as one block a tool would have to append to.
    //
    extern const Cli::LongOption kHeadLongOptions[];
    extern const Cli::LongOption kTailLongOptions[];

    //
    // kHeadLongOptions, then the tool's own rows, then kTailLongOptions, as the
    // single array the parser takes. Joined at run time rather than by a macro
    // so that both halves stay plain, commentable C++ rows.
    //
    class OptionTable
    {
    public:
        // own is the tool's own null-terminated rows, or nullptr for none.
        explicit OptionTable(const Cli::LongOption* own);

        const Cli::LongOption* rows() const { return m_rows.data(); }

    private:
        std::vector<Cli::LongOption> m_rows;
    };

    // Called for a value the library does not know -- a tool's own row, which
    // is to say a value at or above kOptToolFirst. Returns false for an
    // argument it rejects, having said why on std::cerr behind "Error: ".
    using ToolOptionHandler = std::function<bool(int value, const std::string& argument)>;

    //
    // A secret of a tool's own that --stdin-keys takes as well, such as
    // abr-vmdk-tool's --password. name is the tool's long option without the
    // dashes; value is the field that option fills, which is how a secret given
    // on the command line too is caught: it is no longer empty by the time
    // standard input is read.
    //
    struct StdinKey {
        const char* name;
        std::string* value;
    };
    using StdinKeys = std::vector<StdinKey>;

    //
    // Fills the options every command shares, handing a tool's own to
    // setToolOption. '?' from the parser is refused here, before the hook: the
    // parser has already reported it, and handing a failure to the tool to
    // reject again would read as if it were a value the tool did not know.
    //
    // setToolOption may be empty, for a parse that wants the shared half only.
    // toolKeys are the tool's own secrets for --stdin-keys, beside the shared
    // ones.
    //
    bool parseSharedOptions(int argc, char* argv[], const Cli::LongOption* longOptions,
                            CommonOptions& options, const ToolOptionHandler& setToolOption,
                            const StdinKeys& toolKeys = {});

    //
    // What --stdin-keys reads: "name=value" lines, one per secret, up to an
    // empty line or the end of the input. The names are the long options the
    // values would otherwise be given with -- key, secret-key, source-key,
    // source-secret-key, then the tool's own toolKeys. Whatever follows the
    // empty line is left on the stream for the command, so that remove can
    // still read its answer and meta_save its data from the same standard
    // input.
    //
    // Refuses an unknown name, a name given twice, an empty value, and a
    // secret also given on the command line: either would leave the tool to
    // guess which value was meant, and an empty key would quietly make an
    // unencrypted backup.
    //
    bool readStdinKeys(std::istream& in, CommonOptions& options, const StdinKeys& toolKeys = {});

    bool parseImmutabilityMode(const std::string& text, Abr::ImmutabilityMode& mode);
    bool parseRetentionDays(const std::string& text, int& days);
    bool parseCompression(const std::string& text, int& level);
    bool parseBackupNames(const std::string& text, std::vector<std::string>& names);

    bool checkStorageOptions(const StorageOptions& storage);
    bool checkSourceStorageOptions(const StorageOptions& source);
    bool checkImmutabilityOptions(const CommonOptions& options);

    //
    // Whether --type names a kind this run can actually take. diskOption is the
    // tool's own disk option ("--vhdx", "--vmdk") and diskGiven whether it was
    // named: an incremental rests on the hypervisor's own changed block
    // tracking, which a plain file has no equivalent of, and silently falling
    // back to a full copy would be a trap.
    //
    bool checkBackupType(const CommonOptions& options, const char* diskOption, bool diskGiven);

    // The destination storage with every --source-<...> override laid over it.
    StorageOptions sourceStorage(const CommonOptions& options);
}
