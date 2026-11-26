# Changelog

All notable changes to `abr-vmdk-tool` and `abr-vhdx-tool` are recorded here.

A release is a tag, `v<year>.<month>` and then `v<year>.<month>.<n>`, on its
release branch (`v26.09.x` for 26.09); fixes reach a release branch from `main` by
cherry-pick.

## [v26.11] - 2026-11-26

`abr-vmdk-tool` reaches VMware disks over the vSphere SOAP API as well as through the VDDK.
Both tools restore a chain of backups in one run, copy incrementals, hold a backup under Object Lock until a date and
read their secrets from standard input.

### Added

- `abr-vmdk-tool --api soap`: VMware disks over the vSphere SOAP API and HTTPS,
  through vsphere-soap-lib, with no VDDK on the host. Backups of powered-off
  and running VMs, at a `--snapshot` or at a snapshot of the tool's own, full
  or incremental by changed block tracking, reporting `next_change_id` for the
  next incremental. Restores of a whole disk, a full backup or a chain that
  starts with one, overwriting a VM's disk in place or creating a new one,
  attached to a VM or placed at a datastore path. `--quiesce`, `--cluster` and
  `--resource-pool` go with it. `--api vddk` stays the default where the
  tool is built with the VDDK, and the tool now builds with either or both
  (`ABR_DISK_WITH_VDDK`, `ABR_DISK_WITH_SOAP`).
  A disk created on a VM goes on a controller with a free unit, SCSI first on a VM without disks.
- `abr-vmdk-tool --vddk-config FILE`: a configuration file handed to the VDDK when it starts (`VixDiskLib_InitEx`), for
  its transport and logging settings.
  A file that cannot be read is an error rather than a silent fall back to the VDDK's defaults, and `--api soap`
  refuses the option.
- Without `--vddk-config`, `abr-vmdk-tool` hands the VDDK a configuration of its own.
  It switches on writes over NBD and HotAdd for LibreVDDK, which keeps them off by default, and sets `tmpDirectory` to
  `/var/log/altastor-backup/vddk`, or to the directory `ABR_VMDK_TOOL_VDDK_TMPDIR` names, when that directory exists.
  The VDDK's logs and temporary files are then kept there; the tool never creates the directory.
- `ABR_VMDK_TOOL_VDDK_LIB`, read by the launcher: the VDDK library file itself, under whatever name it has, in place
  of a VDDK root.
  Its directory leads the library search path and the tool loads exactly that file.
  Setting it together with `ABR_VMDK_TOOL_VDDK_DIR` is refused.

- `restore --source-names LIST`, in both tools: restore a chain of backups in
  one run in place of `--name`, the backup to start from first and the
  incrementals on top of it in order, checked as `copy` checks it. Each block is
  written once, as the last backup that changed it holds it. A chain that
  starts with an incremental needs `--force`, like a single one. Under `--json`
  a chain is reported with `source`, `source_names` and `merged_from` in place
  of `backup` and `name`. The `--source-<...>` storage options are now refused
  by `restore` rather than ignored.
- `copy` of a single backup, full or incremental, and of a chain that starts
  with an incremental, in both tools. The new backup is of the same kind as the
  first one of the chain; several incrementals merge into one incremental of
  the first one's parent, which `restore --force` applies like any other. A
  second repository, and a disk restored from it, can so be kept up to date
  one new backup at a time rather than by copying the whole disk again. Under
  `--json`, `kind` reports `incremental` for such a copy.
- `--retention-till DATE`, in `backup`, `copy` and `retention` of both tools:
  hold a backup under S3 Object Lock until a moment given in UTC, as
  `YYYY-MM-DDTHH:MM:SSZ`, rather than for `--retention` days counted from the
  moment the backup is created. Every disk of a VM can so be held until the
  same date, however long each one takes to back up. The two options are
  mutually exclusive; a date already passed is refused. Under `--json` the hold
  is reported as `retention_till` in place of `retention_days`. `--ret` is no
  longer an unambiguous prefix of `--retention`.
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
- `abr-backup-lib/doc/abr-backup-format.md` describes the `.abr` format field by field: the index and the data file,
  chunking and zero blocks, encryption, the integrity checks and the commit protocol.
- `NOTICE` and `THIRD-PARTY-NOTICES.md`: the attribution of the tools, and the notices and licenses of the third-party
  software each tool has built in. `cmake --install` puts them in `share/doc/abr-disk-tools` along with `LICENSE`.
- The READMEs and the manual name LibreVDDK, a free and open-source drop-in replacement for VMware's VDDK library, and
  describe how `abr-vmdk-tool` is built and run with it.

### Fixed

- `retention` on a name that holds only metadata, such as a VM manifest stored
  with `meta_save` next to the backups of its disks, held the `.abrm` but then
  failed with NoSuchKey on the `.abrd` and `.abri` it never had. The index now
  decides what a name holds: with it, `.abrd` and `.abri` are held as before;
  without it, the `.abrm` alone is. A name that holds neither, or an index
  without its data, is reported as such.
- A header, index or chunk whose stored CRC32C was 0 was read unchecked, since 0 used to mean "not checksummed": a
  zeroed field, or a chunk whose real checksum is 0, passed `restore`, `copy` and `verify`.
  Every CRC32C is now checked whatever its value.

### Changed

- `abr-vhdx-tool.exe` is built against the Universal C Runtime (UCRT) rather
  than `msvcrt.dll`. The cross build uses `x86_64-w64-mingw32ucrt-g++`
  (Debian's `g++-mingw-w64-ucrt64`) by default, and `ABR_MINGW_TRIPLE` names
  another UCRT toolchain; a native build needs the MSYS2 UCRT64 environment.
  A non-UCRT compiler is refused at configure time.
- An incremental over the VDDK asks vSphere what changed since `--change-id` through the SOAP API
  (`QueryChangedDiskAreas`), and reads the data through the VDDK as before.
  It takes a tool built with the SOAP API and a disk on a host; a local VMDK has no incrementals.
- A backup whose records point into a data file other than its own, which no version writes, is refused, and the
  tools no longer build on a big-endian host, where the index would not match the format.
  Backups written by earlier versions restore unchanged, encrypted ones included.
- The copyright lines of the sources and of the Windows version resource read Norvitex OÜ (AltaStor.com).
- The `abr-disk-lib` submodule comes from a new repository, whose history starts in November 2026.
  The `abr-disk-lib` commits that the 26.09 tags pin are not in it.

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
