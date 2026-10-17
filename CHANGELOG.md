# Changelog

All notable changes to `abr-vmdk-tool` and `abr-vhdx-tool` are recorded here.

A release is a tag, `v<year>.<month>` and then `v<year>.<month>.<n>`, on its
release branch (`v26.09.x` for 26.09); fixes reach a release branch from `main` by
cherry-pick.

## [v26.11-pre] - Unreleased

On `main`, not in any release yet.

### Added

- `--stdin-keys`, in both tools: read the secrets from standard input as
  `name=value` lines rather than from the command line, where every process on
  the host can read them. It takes `key`, `secret-key`, `source-key` and
  `source-secret-key`, and in `abr-vmdk-tool` also `password`, the vCenter or
  ESXi password. Input ends at an empty line or at EOF; the rest of stdin is
  left for `remove`'s confirmation and `meta_save`'s data. A trailing `\r` is
  dropped, for PowerShell and Windows consoles. An unknown name, a name given
  twice, an empty value, and a secret given both on the command line and on
  stdin are refused. Error messages name the secret but never show its value.
- `test/hyperv/Test-SmbShare.ps1`: backs up every disk of a VM, full and
  incremental, to a repository on an SMB share, verifies it there and checks
  the restores by SHA-256, with and without encryption. The share is
  connected from a `SecureString`, so its password is on no command line.
- `hv.py run --secret PARAM=VAR`, which passes a local variable to a test
  script as a `SecureString` parameter rather than in the script text, which
  PowerShell 5.1 may log. The Hyper-V test scripts now pass every key through
  `--stdin-keys`.

### Changed

- `abr-vhdx-tool.exe` is built against the Universal C Runtime (UCRT) rather
  than `msvcrt.dll`. The cross build uses `x86_64-w64-mingw32ucrt-g++`
  (Debian's `g++-mingw-w64-ucrt64`) by default, and `ABR_MINGW_TRIPLE` names
  another UCRT toolchain; a native build needs the MSYS2 UCRT64 environment.
  A non-UCRT compiler is refused at configure time.

## [v26.09.2] - 2026-10-15

Three changes from `main`, cherry-picked onto `v26.09.x`.

### Added

- `ABR_VERSION`, numeric with up to four parts of 0..65535, sets the project
  version and the `FILEVERSION`/`PRODUCTVERSION` of every `.exe`;
  `ABR_VERSION_STRING`, any text, is what `--version` prints and the version
  resource's strings. Both are checked before `project()`.

### Fixed

- `abr-vmdk-tool` exited 0 from every backup or restore that failed after its
  disk was opened: an `atexit` handler that libvmacore registers ends the
  process with a hard-coded 0, overriding `main`'s return value. A restore
  from a backup with a corrupted data file reported the CRC32C mismatch and
  `"status": "error"`, yet a caller going by the exit status took it as a
  success. The tool now flushes its streams and leaves through `_exit()` with
  the status the command returned.
- Backups against a standalone ESXi host failed with "The command is not
  supported on remote objects".

### Security

- MinGW builds are hardened for every target: `-fstack-protector-strong`,
  `_FORTIFY_SOURCE=2` outside Debug unless already given, and explicit
  `--dynamicbase`, `--nxcompat` and `--high-entropy-va`, so a package build
  can drop the distribution's ELF-only flags safely.

## [v26.09.1] - 2026-10-13

The same commit as 26.09; no changes.

## [v26.09] - 2026-10-13

The first release from this repository, which joins `abr-vmdk-tools`,
`abr-vhdx-tools` and their shared `abr-tools-lib` (now `common/`) into one
tree that builds one tool per virtual disk format:

- `abr-vmdk-tool`, for Linux: plain files and VMware VMDKs, local or on an
  ESXi host or a vCenter, through the VDDK.
- `abr-vhdx-tool`, for Windows 10 / Server 2016 and later: plain files, disks
  and volumes, and Hyper-V VHD, VHDX and AVHDX through the Windows virtual
  disk service.

Both have the same commands (`backup`, `restore`, `copy`, `list`, `info`,
`verify`, `remove`, `retention`, `meta_save`, `meta_load`), the same options
apart from their hypervisor's own, the same `--json` results, and read and
write the same `.abr` repositories on file and S3 storage.

### Added

- `ABR_TARGET` (`linux`, `windows`, `windows-cross`, `auto`) chooses what a
  build is for, and the `linux`, `windows` and `windows-cross` presets build
  each in a directory of its own.
- `ABR_ENABLED_TOOLS` names the tools to build (`vmdk`, or `vmdk,vhdx`), in
  place of the per-tool `ABR_BUILD_<FMT>_TOOL` switches, which still apply
  when it is empty. Naming a tool the target cannot build, or one that does
  not exist, stops the configure.
- `VDDK_DIR` defaults to `../vddk`, beside the repository checkout.
