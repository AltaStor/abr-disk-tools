# abr-vhdx-tool — Command Reference

`abr-vhdx-tool` moves one disk at a time between a source (a local file, a
disk or volume, or a Hyper-V virtual disk reached through the Windows virtual
disk service) and the `.abr` backup format, in either direction. It is the
Windows counterpart of `abr-vmdk-tool`: the commands, the storage options and
the `--json` results are the same, and so is the repository format — a backup
written by either tool is read by the other.

This document is the manual for the commands and their options. For how to
build and install the tool, see the [top-level README](../README.md).

```
abr-vhdx-tool.exe <command> [options]
```

Every command accepts `-h`/`--help`, which prints that command's own usage and
exits 0. Running the tool with no arguments, or with `-h`/`--help` before a
command, prints the command list instead:

```
> abr-vhdx-tool.exe --help
Usage: abr-vhdx-tool <command> [options]

Commands:
  backup    Create a backup of a disk/volume/virtual disk
  restore   Restore a disk/volume/virtual disk from a backup
  copy      Merge a chain of backups into a new backup
  list      List available backups
  info      Show information about a backup
  verify    Check the integrity of a backup
  remove    Delete a backup
  retention Change the immutability a stored backup is held under
  meta_save Store a metadata blob alongside a backup
  meta_load Read back the metadata blob of a backup

Run 'abr-vhdx-tool <command> --help' for command-specific options.
Pass --json to any command to get its result on stdout as a JSON document.
```

`abr-vhdx-tool.exe -v` / `--version` prints the tool's version and exits: the
full version of the package it came in (`26.11~pre+31.g6a11c8981539`, say), or
`1.0.0` for a build that was not given one.

Options are parsed the way `getopt_long` parses them on Linux: `--name value`
or `--name=value`, `-n value` or `-nvalue`, flags bundled as `-fh`, and any
unambiguous prefix of a long option. An argument that is not an option is
refused: no command takes one, so a stray word is a mistake rather than an
operand. `abr-vmdk-tool` parses them identically — both tools use the same
parser.

## Contents

- [Storage options](#storage-options)
- [Virtual disk (Hyper-V) options](#virtual-disk-hyper-v-options)
- [The Hyper-V backup workflow](#the-hyper-v-backup-workflow)
- [Commands](#commands)
  - [backup](#backup)
  - [restore](#restore)
  - [copy](#copy)
  - [list](#list)
  - [info](#info)
  - [verify](#verify)
  - [remove](#remove)
  - [retention](#retention)
  - [meta_save](#meta_save)
  - [meta_load](#meta_load)
- [JSON output](#json-output)
- [Exit status](#exit-status)
- [Interrupting a run](#interrupting-a-run)
- [Differences from abr-vmdk-tool](#differences-from-abr-vmdk-tool)

## Storage options

These apply to every command; they select where backups live. `copy`, `list`,
`info`, `verify`, `remove`, `retention` and the `meta_*` commands act on
storage alone and never touch a disk. `backup` and `restore` also need one side
of a disk, described under [Virtual disk (Hyper-V)
options](#virtual-disk-hyper-v-options).

`copy` reads and writes storage at the same time, so it takes a second set of
these, prefixed `--source-` and described under [copy](#copy).

| Option | Argument | Meaning |
| --- | --- | --- |
| `-F`, `--folder` | `FOLDER` | Backup storage folder (file mode) or S3 bucket (s3 mode). **Required.** A local path (`D:\Backups`) or a share (`\\nas\backups`). |
| `-m`, `--mode` | `file` \| `s3` | Storage backend. Default: `file`. |
| `-p`, `--path` | `PATH` | Subfolder within the storage folder/bucket. Optional; groups backups (e.g. per VM) under one folder. |
| `--create-folder` | — | Create the storage folder, or the S3 bucket, if it does not already exist. |
| `--endpoint` | `URL` | S3 endpoint URL. S3 mode only; omit for AWS's default endpoint. |
| `--region` | `REGION` | S3 region. |
| `--access-key` | `KEY` | S3 access key. Omit to fall back to the AWS SDK's normal credential resolution (environment, shared config, instance role, …). |
| `--secret-key` | `KEY` | S3 secret key. Paired with `--access-key`. |
| `--stdin-keys` | — | Read `--key`, `--secret-key`, `--source-key` and `--source-secret-key` from standard input instead of the command line. See [Secrets on standard input](#secrets-on-standard-input). |
| `--aws-profile` | `NAME` | Named profile to take credentials from, instead of explicit keys. |
| `--path-style` | — | Use path-style S3 addressing (`https://endpoint/bucket/key`) instead of virtual-hosted style. Needed for most S3-compatible object stores. |
| `--no-verify-tls` | — | Disable TLS certificate verification for S3 requests. For endpoints with a self-signed or otherwise unverifiable certificate; avoid it against anything reachable over an untrusted network. |

`--mode` must be exactly `file` or `s3`; anything else is rejected before any
storage is touched. In `file` mode the S3-only options above are accepted but
have no effect.

A backup's full location, as printed in reports and JSON, is
`folder[/path]/name`, with a forward slash between the parts whatever the
folder itself is spelled with.

### Secrets on standard input

Every process on the host can read another process's command line, and audit
logs record it. A secret given as `--key`, `--secret-key`, `--source-key` or
`--source-secret-key` is therefore exposed for the whole run. With
`--stdin-keys`, the tool reads them from standard input instead, one per
line as `name=value`:

```powershell
PS> $secret = "key=$key`nsecret-key=$s3Secret`n"
PS> $secret | abr-vhdx-tool.exe backup --stdin-keys --mode s3 --folder backups --access-key AK --name web01-disk0 --vhdx $disk
```

- The names are the option names without the dashes: `key`, `secret-key`,
  `source-key`, `source-secret-key`. The value is everything after the first
  `=`, spaces included. PowerShell ends its lines with `\r\n`; the `\r` is dropped.
  Windows PowerShell 5.1 pipes text to a program in ASCII, turning every
  other character into `?`. For a secret with non-ASCII characters, set
  `$global:OutputEncoding = New-Object Text.UTF8Encoding $false` first. It
  has to be the global variable: one set inside a function or script is
  ignored.
- Input ends at an empty line or at end of file. Whatever follows the empty
  line is left for the command: the `y` that [remove](#remove) asks for, or
  the data [meta_save](#meta_save) reads from standard input.
- A secret given both on the command line and on standard input is refused, so
  are an unknown name, a name given twice and an empty value. The error names
  the line's secret but never shows its value. A line that does not look like
  `name=value` is not shown at all, since it may be a bare password.
- With `--help`, nothing is read.

The access key (`--access-key`) is an identifier rather than a secret and
stays on the command line.

Folder, path and backup names may use any characters Windows allows in a file
name, in any language (on Windows 10 1903 and later — see the
[tool's README](../vhdx-tool/README.md#running-it)).

## Virtual disk (Hyper-V) options

Shared by `backup` and `restore`. Passing `--vhdx` switches the disk side of
the command (source for `backup`, target for `restore`) from a plain file,
disk or volume to a virtual disk reached through the Windows virtual disk
service — the one Hyper-V itself uses.

| Option | Argument | Meaning |
| --- | --- | --- |
| `--vhdx` | `PATH` | VHD, VHDX or AVHDX to read (backup) or write (restore), in place of `--source`/`--target`. A local path, a cluster shared volume path, or an SMB share Hyper-V can use. |

`--vhdx` and `--source`/`--target` are alternatives; giving both is an error.

The virtual disk is opened, then *attached*: surfaced as a disk device of the
host, which is what the tool reads or writes. That is what makes a VHDX, a VHD,
and a differencing chain of checkpoint AVHDXs all look like the plain run of
sectors the guest sees. It also means:

- **An elevated (administrator) process is needed**, for attaching a disk and
  for opening the device it becomes. The tool checks up front and says so, and
  never asks for elevation itself.
- The attach is **read-only for a backup**, gets **no drive letters**, is
  closed to everyone but SYSTEM and the Administrators group, and lasts only
  as long as the tool's handle to it. A run that is killed outright still
  leaves no disk attached behind it.
- **A disk a running VM is writing to cannot be opened.** Back up a
  [checkpoint](#the-hyper-v-backup-workflow) instead.

The format is taken from the name — `.vhdx` and `.avhdx` are VHDX, `.vhd` and
`.avhd` are VHD — and, for any other name, from the file's own signature.

A plain `--source` or `--target` may also name a disk or a volume, like
`\\.\PhysicalDrive2` or `\\.\E:`, which needs an elevated process too. A disk
restored to is taken offline for the restore and put back online afterwards;
a volume is locked and dismounted, which fails while files are open on it.

## The Hyper-V backup workflow

The tool moves the data of one disk; the backup server decides which disk, and
when. For a VM that is running, that means working with checkpoints through
Hyper-V's WMI provider (`root\virtualization\v2`), exactly as it works with
vSphere snapshots for `abr-vmdk-tool`:

1. **Take a checkpoint** of the VM for the backup —
   `Msvm_VirtualSystemSnapshotService.CreateSnapshot` with the recovery
   (backup) checkpoint type, application-consistent where the guest supports
   it. From then on Hyper-V writes the VM's changes to new AVHDX files, and
   the disks the checkpoint refers to stay as they were.
2. **Find the checkpoint's disks**: the `HostResource` paths of the
   `Msvm_StorageAllocationSettingData` instances that belong to the
   checkpoint's settings. For a VM without earlier checkpoints these are the
   VM's own `.vhdx` files; otherwise they are the AVHDXs that were current when
   the checkpoint was taken.
3. **Back each one up** with `abr-vhdx-tool.exe backup --vhdx <path>` — a full
   backup, or an incremental with `--type incremental --change-id <id>` against
   the id recorded for that disk at the previous backup. The result reports
   the disk's current RCT id as `rct_id`; record it for the next incremental.
4. **Turn the checkpoint into a reference point** (`ConvertToReferencePoint`),
   which merges it back into the VM's disks while keeping the change tracking
   position it marked. Discarding the checkpoint instead works too, but then
   the next backup has no position to be incremental against.

Resilient change tracking (RCT) is Hyper-V's changed block tracking. It
belongs to the disk: an id is only good for the disk that issued it, and only
for as long as its tracking lasts. A disk that was moved, compacted, merged by
hand or replaced starts over, and an incremental against an id it no longer
knows is refused — take a full backup to start a new chain.

A VM that is off needs no checkpoint: its disks can be backed up directly.

## Commands

### backup

Create a backup of a disk, volume, or virtual disk.

```
abr-vhdx-tool.exe backup [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-s`, `--source` | `PATH` | Source file, disk or volume to back up. Required unless `--vhdx` is given. |
| `-k`, `--key` | `KEY` | Encryption key. Optional; omit for an unencrypted backup. Backups are AES-256-GCM encrypted when a key is given. |
| `-c`, `--compression` | `0`-`22` | Zstd compression level. `0` disables compression. Default: `3`. |
| `--type` | `full` \| `incremental` | `full` (default) copies the whole disk. `incremental` copies only the blocks changed since `--change-id`; it requires `--vhdx`, since it relies on Hyper-V resilient change tracking (RCT) — there is no equivalent for a plain file, so this is rejected rather than silently falling back to a full copy. |
| `--change-id` | `ID` | The RCT id an incremental backup is taken against: the `rct_id` the previous backup of the same disk reported. Only meaningful — and required — with `--type incremental`. |
| `--immutable` | `none` \| `governance` \| `compliance` | Hold the backup under S3 Object Lock so it cannot be deleted or overwritten before its retention expires. `governance` allows a user with special permissions to remove the lock; `compliance` allows no one, including the storage account root, to shorten or remove it. S3 storage only. |
| `--retention` | `DAYS` | How long `--immutable` holds the backup, in days. `--immutable` requires either this or `--retention-till`, and neither is accepted without it, since a mode with no period holds nothing and a period with no mode enforces nothing. |
| `--retention-till` | `DATE` | Hold the backup until this moment instead of for a number of days: a date and time in UTC, `YYYY-MM-DDTHH:MM:SSZ` (a space may take the place of the `T`, and the `Z` may be left out). Seconds are required, and a moment already passed is refused. Mutually exclusive with `--retention`. |

Plus the [storage options](#storage-options) and, when backing up a virtual
disk, the [virtual disk options](#virtual-disk-hyper-v-options).

A full backup reads the whole disk. The virtual disk service has no call that
lists which blocks a disk has allocated, so the unused parts of a dynamic disk
are read too — as zeros, which backuplib drops rather than stores. They cost
reading time, not repository space.

Progress is reported as it copies: `NN%` lines, rewritten in place
interactively or one per line under `--json` (see [JSON
output](#json-output)). On success, a human-readable report follows (or a
JSON object, see below):

```
> abr-vhdx-tool.exe backup --folder D:\Backups --path web01 --name disk0-full ^
    --vhdx "C:\Hyper-V\web01\Virtual Hard Disks\disk0.vhdx" -c 3
Virtual disk 'C:\Hyper-V\web01\Virtual Hard Disks\disk0.vhdx' attached read-only as \\.\PhysicalDrive3
2%
...
100%
Source:            C:\Hyper-V\web01\Virtual Hard Disks\disk0.vhdx (VHDX, dynamic)
Backup:            D:\Backups/web01/disk0-full
Kind:              full
RCT id:            rctX:4ab2f3a0:6c1e:4b85:9e1b:25b4d8a1c3f7:00000001
Raw data size:     8471179264 bytes
Compression:       zstd level 3
Stored data size:  3016784896 bytes
Compression ratio: 0.356124
Encryption:        none
Immutability:      none
```

(The `Virtual disk … attached` line goes to stderr, with the other notes the
disk layer writes.)

An incremental backup of the checkpoint taken for the next run:

```
> abr-vhdx-tool.exe backup --folder D:\Backups --path web01 --name disk0-inc1 ^
    --vhdx "C:\Hyper-V\web01\Virtual Hard Disks\disk0_6B2E1A34-....avhdx" ^
    --type incremental --change-id rctX:4ab2f3a0:6c1e:4b85:9e1b:25b4d8a1c3f7:00000001
```

On failure, anything already written for the backup is discarded — a failed
run leaves nothing behind at the target location. If that cleanup itself
fails, the tool says so and tells you to remove the partial backup with the
`remove` command rather than pretending the run succeeded.

### restore

Restore a disk, volume, or virtual disk from a backup.

```
abr-vhdx-tool.exe restore [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-t`, `--target` | `PATH` | Target file, disk or volume to restore to. Required unless `--vhdx` is given. |
| `-k`, `--key` | `KEY` | Decryption key. Required if the backup was made with `--key`; omit for an unencrypted backup. A missing or wrong key is refused before the target is opened, so no virtual disk is created for it. |
| `-f`, `--force` | — | Apply an incremental backup onto a target that already holds its parent. |

Plus the [storage options](#storage-options) and, when restoring to a virtual
disk, the [virtual disk options](#virtual-disk-hyper-v-options).

A `--vhdx` target that **does not exist is created**: a dynamic VHDX (or a VHD,
when the name ends in `.vhd`) of exactly the backup's size and sector sizes,
ready to be attached to a VM. Any other name is refused, since Hyper-V would
not attach it. If the restore then fails or is interrupted, the half-written
disk is removed again. A VHD holds 512-byte sectors only, so a backup of a
4K-sector disk has to go to a VHDX.

A `--vhdx` target that **exists** is written in place, like a VMDK is by
`abr-vmdk-tool`: only the ranges the backup holds are written, and the rest of
the disk is left as it was. That is right for a disk that is new and empty,
and wrong for one that still holds something else — restore to a new path when
in doubt. Its VM has to be off.

An incremental backup holds only the blocks that changed since its parent —
restoring it alone leaves every other block untouched (an empty disk, if the
target started empty). To apply one:

1. Restore the full backup to the target first.
2. Restore the incremental to the *same* target with `-f`/`--force`, which
   permits writing onto a target that already holds data instead of the tool
   refusing as a safety check.

Restoring to an existing file target without `--force` empties it first; with
`--force` (applying an incremental) the existing content is kept and only the
ranges the incremental covers are overwritten. A file target is written
sparse, so the parts the backup does not hold take no space.

Before restoring, the tool checks that a fixed-geometry target (a disk, a
volume, or an existing virtual disk) actually matches the backup's sector size
and sector count, and refuses rather than writing a mismatched image. A plain
file target has its size set to match the backup.

```
> abr-vhdx-tool.exe restore --folder D:\Backups --path web01 --name disk0-full ^
    --vhdx "C:\Hyper-V\web01-restored\disk0.vhdx"
Created dynamic VHDX 'C:\Hyper-V\web01-restored\disk0.vhdx' of 42949672960 bytes
Virtual disk 'C:\Hyper-V\web01-restored\disk0.vhdx' attached as \\.\PhysicalDrive4
100%
Restored 'D:\Backups/web01/disk0-full' to new virtual disk 'C:\Hyper-V\web01-restored\disk0.vhdx'
```

The disk a restore writes to is taken offline while it is written, so that
Windows neither mounts nor writes to the file systems appearing on it, and is
detached still offline.

### copy

Merge a chain of backups — a full backup and the incrementals taken on top of
it — into one new full backup.

```
abr-vhdx-tool.exe copy [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Name of the backup to create. **Required.** |
| `--source-names` | `LIST` | Comma-separated chain to merge: the full backup first, then its incrementals in the order they were taken. **Required.** |
| `-k`, `--key` | `KEY` | Encryption key for the new backup. Optional, and unrelated to the chain's own key — that one is `--source-key`. |
| `-c`, `--compression` | `0`–`22` | Compression level for the new backup. |
| `--immutable` | `none` \| `governance` \| `compliance` | Hold the new backup under S3 Object Lock, as for [backup](#backup). |
| `--retention` | `DAYS` | How long `--immutable` holds it, in days. |
| `--retention-till` | `DATE` | Hold it until this moment in UTC instead, as for [backup](#backup). Mutually exclusive with `--retention`. |

Plus the [storage options](#storage-options) for the destination, and the
source storage options below for the chain.

The result is a full backup of the disk as of the end of the chain: where
several backups hold the same block, it is taken from the last one that
changed it. Nothing of the chain is read twice and nothing is written to it —
`copy` only reads it — so the originals are left exactly as they were and can
be removed afterwards, once the copy has been verified, to collapse a long
chain into a single backup.

What the copy is written *as* comes from the options given here rather than
from the chain, which makes this also the way to recompress a backup, change
its encryption key, or move it between a folder and a bucket:

```
> abr-vhdx-tool.exe copy --folder D:\Backups --path web01 ^
    --source-names disk0-full,disk0-inc1,disk0-inc2 --name disk0-week12 --compression 6
100%
Source:            D:\Backups/web01 (disk0-full, disk0-inc1, disk0-inc2)
Backup:            D:\Backups/web01/disk0-week12
Kind:              full, merged from 3 backup(s)
Raw data size:     8471179264 bytes
Compression:       zstd level 6
Stored data size:  2874138624 bytes
Compression ratio: 0.339286
Encryption:        none
Immutability:      none
```

#### Source storage options

The chain is read through storage of its own, described by these. Each mirrors
a [storage option](#storage-options) and falls back to it when it is not given,
so a copy within one repository needs none of them and a copy between two needs
only what actually differs.

| Option | Argument | Meaning |
| --- | --- | --- |
| `--source-folder` | `FOLDER` | Storage folder or S3 bucket holding the chain. |
| `--source-mode` | `file` \| `s3` | Storage backend of the chain. |
| `--source-path` | `PATH` | Subfolder holding the chain. |
| `--source-key` | `KEY` | Decryption key of the chain, tried on every encrypted backup of it before the copy starts. |
| `--source-endpoint` | `URL` | S3 endpoint URL. |
| `--source-region` | `REGION` | S3 region. |
| `--source-access-key` | `KEY` | S3 access key. |
| `--source-secret-key` | `KEY` | S3 secret key. |
| `--source-aws-profile` | `NAME` | Named profile to take credentials from. |
| `--source-path-style` | — | Use path-style S3 addressing. |
| `--source-no-verify-tls` | — | Disable TLS verification for S3. |

Note that `--create-folder` is never inherited: the chain has to already be
where it is said to be, and only the destination is ever created.

**What is checked, and what is not.** The format keeps no link from an
incremental back to its parent, so the order `--source-names` gives *is* the
chain — pass them out of order and the copy will hold the wrong blocks, with
nothing to warn you. What the tool does check, and refuses on:

- the first backup must be full, and every one after it incremental;
- all of them must describe the same disk (same sector size and sector count);
- the new backup may not be one of the chain, when the destination storage is
  the same one the chain is read from — that would overwrite what is being
  read.

A metadata blob stored with `meta_save` belongs to the backup it was stored
with and is not carried over; run `meta_save` against the copy if it needs one.

An interrupted copy discards what it had written, exactly as `backup` does,
and leaves the chain untouched.

### list

List the backups in a storage folder (or a subfolder of it).

```
abr-vhdx-tool.exe list [options]
```

Takes only the [storage options](#storage-options) — no `--name`. `--path`
narrows the listing to one subfolder.

```
> abr-vhdx-tool.exe list --folder D:\Backups --path web01
Backups in 'D:\Backups/web01':
  disk0-full
  disk0-inc1
  disk1-full
```

### info

Show information about a backup, or — without `--name` — about the storage
folder itself.

```
abr-vhdx-tool.exe info [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. Optional: omitted, `info` reports the storage folder's own properties instead of a specific backup. |

Plus the [storage options](#storage-options).

Without `--name`, the report covers the folder alone — the strongest
immutability mode it can offer at all, and the mode (and retention) it
currently applies by default to new backups. With `--name`, it covers that one
backup: its geometry, compression, encryption, and the immutability it is
actually held under.

```
> abr-vhdx-tool.exe info --folder D:\Backups --path web01 --name disk0-full
Backup:            D:\Backups/web01/disk0-full
Kind:              full
Sectors:           83886080
Sector size:       512 bytes
Physical sector:   4096 bytes
Raw data size:     8471179264 bytes
Compression:       zstd level 3
Stored data size:  3016784896 bytes
Compression ratio: 0.356124
Encryption:        none
Max immutability:  none
Immutability:      none, the backup can be deleted
```

### verify

Check the integrity of a backup by re-reading and checksumming every stored
data range, without necessarily decrypting it.

```
abr-vhdx-tool.exe verify [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-k`, `--key` | `KEY` | Decryption key. Optional — given, it is tried on the backup's first data block, and a key that does not decrypt it fails the verify; the data checksums are verified without decrypting, key or no key. |

Plus the [storage options](#storage-options).

Every data range is checked — `verify` does not stop at the first corrupt
one, so a single run reports the full extent of any damage:

```
> abr-vhdx-tool.exe verify --folder D:\Backups --path web01 --name disk0-full
100%
Backup 'D:\Backups/web01/disk0-full' is intact: 42 data ranges (8471179264 bytes) verified
```

A corrupt backup is reported as a *result*, not treated as a malfunction: the
command still exits non-zero, but `--json` still gets one complete result
document (`intact: false`, `corrupt_ranges` set) rather than an error — see
[JSON output](#json-output).

### remove

Delete a backup.

```
abr-vhdx-tool.exe remove [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-f`, `--force` | — | Skip the confirmation prompt. |

Plus the [storage options](#storage-options).

Without `--force`, `remove` asks for confirmation on stdin:

```
> abr-vhdx-tool.exe remove --folder D:\Backups --path web01 --name disk0-inc1
Remove backup 'D:\Backups/web01/disk0-inc1'? [y/N] y
Removed backup 'D:\Backups/web01/disk0-inc1'
```

Anything other than `y`/`Y` aborts with exit status 1 and prints `Aborted.`
(or, under `--json`, a result document with `"status": "aborted"`).

**This is what surprises callers who script the tool:** a subprocess with no
stdin attached reads an empty line immediately, which is not `y`/`Y`, so the
removal is aborted — not performed, and not an error either from the exit
status alone. Always pass `--force` when scripting `remove` unless you are
prepared to feed the prompt an answer.

A backup held under S3 Object Lock cannot be removed before its retention
expires; the storage rejects the deletion and the tool reports that as a
failure. See [retention](#retention) for changing how long that hold lasts.

### retention

Change the immutability a backup already in storage is held under, to extend
a hold that is running out or to raise it to a stronger mode. S3 storage only.

```
abr-vhdx-tool.exe retention [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `--immutable` | `governance` \| `compliance` | Mode to hold the backup in. **Required.** Unlike `backup --immutable`, `none` is not accepted here: Object Lock has no way to release an object that is already held. |
| `--retention` | `DAYS` | How long to hold the backup **from now**, in days. This or `--retention-till` is **required**. |
| `--retention-till` | `DATE` | Hold the backup until this moment, in UTC as `YYYY-MM-DDTHH:MM:SSZ`, as for [backup](#backup). Mutually exclusive with `--retention`. |

Plus the [storage options](#storage-options).

The period is counted from the moment the command runs, so it states what the
backup has left rather than what it was originally given. `--retention-till`
names the moment the hold ends instead, so every disk of a VM can be given the
same date. All three objects a backup is made of (`.abrd`, `.abri` and `.abrm`) are put under the same
retention.

**What the storage will accept is its call, not the tool's.** Extending a hold
works in either mode. Shortening or lifting one does not: a compliance hold can
never be shortened by anyone, and a governance hold only by a caller with the
permission to bypass it, which this command does not ask for. Raising
`governance` to `compliance` is permitted by AWS S3, but S3-compatible targets
vary. A refusal leaves the backup as it was, and the command exits 1 after
printing the storage's own error for each object it could not update.

```
> abr-vhdx-tool.exe retention --mode s3 --folder backups --name web01-disk0 ^
    --immutable compliance --retention 200
Backup 'backups/web01-disk0' is now held in compliance mode, retained for 200 day(s)
```

Under `--json` a hold given with `--retention-till` is reported as a
`retention_till` string, `YYYY-MM-DDTHH:MM:SSZ`, in place of `retention_days`,
here and in the results of `backup` and `copy`.

### meta_save

Store a metadata blob alongside a backup — for example the JSON manifest a
backup server keeps about the VM and its disk. The blob is opaque to the tool;
it is stored and returned byte for byte.

```
abr-vhdx-tool.exe meta_save [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-s`, `--source` | `PATH` | File to read the metadata from. `-` or omitted reads standard input. |

Plus the [storage options](#storage-options).

Standard input is read in binary mode, so line endings, a 0x1A byte or a NUL
arrive as they were sent.

```
> abr-vhdx-tool.exe meta_save --folder D:\Backups --path web01 --name disk0-full --source web01.json
Saved metadata for 'D:\Backups/web01/disk0-full'
```

### meta_load

Read back the metadata blob stored by `meta_save`.

```
abr-vhdx-tool.exe meta_load [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-t`, `--target` | `PATH` | File to write the metadata to. `-` or omitted writes to standard output. |

Plus the [storage options](#storage-options).

Without `--json`, a blob written to stdout is the *only* thing on stdout —
nothing else is printed around it, and stdout is binary, so the output can be
piped or redirected as it is. Under `--json`, the blob instead comes back as a
`metadata` string field of the result document (see below), since raw
metadata on stdout would otherwise collide with the JSON document.

## JSON output

Every command accepts `--json`, which replaces the human-readable report a
command normally ends with by a single JSON object on stdout. The shapes are
those of `abr-vmdk-tool`, with the two additions noted below. It covers only
the *result*; a long `backup`, `restore`, `copy` or `verify` still prints
progress first, and those percentages stay plain text — the same `NN%` lines
the interactive form shows, except one per line and flushed immediately,
rather than rewritten in place with a carriage return.

So a caller reads stdout a line at a time: every line up to and including
`100%` is progress, and the JSON document is everything from the first `{`
onward. Lines end in a bare LF on every stream.

Two things are kept off stdout so they never interleave with the JSON
document:

- `remove` asks its `[y/N]` confirmation on stderr instead of stdout when
  `--json` is given (see [remove](#remove)).
- `meta_load` writing to stdout hands the blob back as a `metadata` string
  field of the result document, instead of writing it raw.

Human-readable diagnostics still go to stderr, and exit codes are unchanged
by `--json`.

`command` and `status` are always present. `status` is one of:

- `ok` — the command succeeded.
- `error` — the command failed; an `error` string field carries the message
  (the same one that went to stderr).
- `aborted` — only from a `remove` the operator declined at the confirmation
  prompt.

A failure still produces exactly one document, option errors included:

```
> abr-vhdx-tool.exe info --json --folder D:\Backups --name ghost
{
  "command": "info",
  "status": "error",
  "error": "failed to open backup 'ghost'"
}
```

`backup` of a virtual disk with resilient change tracking on also reports the
disk's RCT id, as `rct_id` — the `--change-id` of the next incremental:

```
> abr-vhdx-tool.exe backup --json --folder D:\Backups --path web01 --name disk0-inc1 ^
    --vhdx "C:\Hyper-V\web01\Virtual Hard Disks\disk0_6B2E1A34-....avhdx" ^
    --type incremental --change-id rctX:4ab2f3a0:6c1e:4b85:9e1b:25b4d8a1c3f7:00000001
{
  "command": "backup",
  "status": "ok",
  "source": "C:\\Hyper-V\\web01\\Virtual Hard Disks\\disk0_6B2E1A34-....avhdx",
  "backup": "D:\\Backups/web01/disk0-inc1",
  "name": "disk0-inc1",
  "kind": "incremental",
  "change_id": "rctX:4ab2f3a0:6c1e:4b85:9e1b:25b4d8a1c3f7:00000001",
  "rct_id": "rctX:4ab2f3a0:6c1e:4b85:9e1b:25b4d8a1c3f7:00000002",
  "immutability": "none",
  "sectors": 83886080,
  "sector_size": 512,
  "physical_sector_size": 4096,
  "raw_data_size": 318767104,
  "compression": "zstd",
  "compression_level": 3,
  "stored_data_size": 102760448,
  "compression_ratio": 0.322368,
  "encryption": "none"
}
```

`restore` to a `--vhdx` target also reports whether the disk was created by
this run, as `created`:

```
{
  "command": "restore",
  "status": "ok",
  "backup": "D:\\Backups/web01/disk0-full",
  "name": "disk0-full",
  "target": "C:\\Hyper-V\\web01-restored\\disk0.vhdx",
  "kind": "full",
  "created": true,
  "decrypted": false
}
```

`verify` is the one command that reports a failed check as data rather than
as a malfunction: a corrupt backup still yields the full result document,
with `"intact": false` and a `corrupt_ranges` count, alongside the non-zero
exit status:

```
{
  "command": "verify",
  "status": "error",
  "backup": "D:\\Backups/web01/disk0-full",
  "name": "disk0-full",
  "intact": false,
  "data_ranges": 42,
  "corrupt_ranges": 2,
  "bytes_verified": 8471179264,
  "error": "backup is corrupt: 2 of 42 data ranges failed verification"
}
```

`info`, `list`, `copy`, `retention`, `remove`, `meta_save` and `meta_load`
report exactly as `abr-vmdk-tool` does; see its manual for each shape.

`meta_load` embeds the stored blob verbatim as a JSON string, so the result is
valid JSON exactly when the blob itself is valid UTF-8 — true of the JSON
manifests a backup server typically stores with `meta_save`.

`--help` is exempt from `--json` and always prints plain text, on the
reasoning that a caller asking for the manual is not asking for a machine
format.

## Exit status

- `0` — the command succeeded (including `-h`/`--help` and `-v`/`--version`).
- `1` — the command failed: a bad option, a storage or virtual disk error, a
  corrupt `verify`, or a declined `remove` confirmation.

## Interrupting a run

Ctrl+C and Ctrl+Break (or `GenerateConsoleCtrlEvent` from a parent process,
the Windows counterparts of `SIGINT` and `SIGTERM`) are caught once, during a
`backup`, `restore`, `copy`, or `verify` copy loop: the loop notices at its
next iteration, prints `Interrupted, releasing the disk`, and unwinds
normally — a partial backup is discarded, a virtual disk this run created is
removed, a disk taken offline is put back, and an attached virtual disk is
detached. A second Ctrl+C, or one that arrives while nothing is polling (a
call that is blocked), falls through to the default handling and ends the
process at once.

Closing the console window asks for the same unwinding, but Windows waits only
a few seconds for it. A process ended outright — Task Manager,
`TerminateProcess`, a WinRM session torn down — gets no say at all; even then
no virtual disk stays attached, since the attachment ends with the process.

## Differences from abr-vmdk-tool

The commands, the storage options and the JSON shapes are the same. What
differs is what has to:

| abr-vmdk-tool | abr-vhdx-tool | Why |
| --- | --- | --- |
| `--vmdk PATH` | `--vhdx PATH` | The disk side: a VMDK through VDDK there, a VHD/VHDX/AVHDX through the Windows virtual disk service here. |
| `--host`, `--user`, `--password`, `--port`, `--thumbprint`, `--nfc-port`, `--transport` | — | The tool runs on the Hyper-V host (or reaches the disk over SMB), so there is no remote connection to describe. |
| `--vm`, `--snapshot` | — | A checkpoint's disk is a file of its own, named with `--vhdx`; there is no connection it has to be reached through. |
| `--query-chunk N` | — | A VDDK allocated-block query setting; Hyper-V has no such query. |
| `--change-id` is a CBT change id | `--change-id` is an RCT id | Each hypervisor's own changed block tracking. |
| — | `rct_id` in the `backup` result | The id the next incremental is taken against. |
| — | `created` in the `restore` result | A missing `--vhdx` target is created. |
| `abr-vmdk-tool.sh` launcher | none | Nothing has to be put on a search path. |
