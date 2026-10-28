# abr-vmdk-tool — Command Reference

`abr-vmdk-tool` moves one disk at a time between a source (a local file, a
block device, or a VMware VMDK reached through VDDK) and the `.abr` backup
format, in either direction. 
This document is the manual for the commands and their options. 
For how to build, install and run the tool, see the
[top-level README](../README.md); everything below assumes the launcher,
`abr-vmdk-tool.sh`, is what you invoke — the raw binary has no way to find the
VDDK on its own.

```
abr-vmdk-tool.sh <command> [options]
```

Every command accepts `-h`/`--help`, which prints that command's own usage and
exits 0. Running the launcher with no arguments, or with `-h`/`--help` before
a command, prints the command list instead.

Options are parsed the way `getopt_long` parses them: `--name value` or
`--name=value`, `-n value` or `-nvalue`, flags bundled as `-fh`, and any
unambiguous prefix of a long option. An argument that is not an option is
refused: no command takes one, so a stray word is a mistake rather than an
operand. `abr-vhdx-tool` parses them identically — both tools use the same
parser.

```
$ abr-vmdk-tool.sh --help
Usage: abr-vmdk-tool <command> [options]

Commands:
  backup    Create a backup of a disk/volume/VMDK
  restore   Restore a disk/volume/VMDK from a backup
  copy      Merge a chain of backups into a new backup
  list      List available backups
  info      Show information about a backup
  verify    Check the integrity of a backup
  remove    Delete a backup
  retention Change the immutability a stored backup is held under
  meta_save Store a metadata blob alongside a backup
  meta_load Read back the metadata blob of a backup

Run 'abr-vmdk-tool <command> --help' for command-specific options.
Pass --json to any command to get its result on stdout as a JSON document.
```

`abr-vmdk-tool.sh -v` / `--version` prints the tool's version and exits: the
full version of the package it came in (`26.11~pre+31.g6a11c8981539`, say), or
`1.0.0` for a build that was not given one.

## Contents

- [Storage options](#storage-options)
- [VMDK (VDDK) options](#vmdk-vddk-options)
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
- [Signals](#signals)

## Storage options

These apply to every command; they select where backups live. `copy`, `list`,
`info`, `verify`, `remove`, `retention` and the `meta_*` commands act on storage
alone and never touch the VDDK. `backup` and `restore` also need one side of a
disk, described under [VMDK (VDDK) options](#vmdk-vddk-options).

`copy` reads and writes storage at the same time, so it takes a second set of
these, prefixed `--source-` and described under [copy](#copy).

| Option | Argument | Meaning |
| --- | --- | --- |
| `-F`, `--folder` | `FOLDER` | Backup storage folder (file mode) or S3 bucket (s3 mode). **Required.** |
| `-m`, `--mode` | `file` \| `s3` | Storage backend. Default: `file`. |
| `-p`, `--path` | `PATH` | Subfolder within the storage folder/bucket. Optional; groups backups (e.g. per VM) under one folder. |
| `--create-folder` | — | Create the storage folder, or the S3 bucket, if it does not already exist. |
| `--endpoint` | `URL` | S3 endpoint URL. S3 mode only; omit for AWS's default endpoint. |
| `--region` | `REGION` | S3 region. |
| `--access-key` | `KEY` | S3 access key. Omit to fall back to the AWS SDK's normal credential resolution (environment, shared config, instance role, …). |
| `--secret-key` | `KEY` | S3 secret key. Paired with `--access-key`. |
| `--stdin-keys` | — | Read `--key`, `--secret-key`, `--source-key`, `--source-secret-key` and `--password` from standard input instead of the command line. See [Secrets on standard input](#secrets-on-standard-input). |
| `--aws-profile` | `NAME` | Named profile to take credentials from, instead of explicit keys. |
| `--path-style` | — | Use path-style S3 addressing (`https://endpoint/bucket/key`) instead of virtual-hosted style. Needed for most S3-compatible object stores. |
| `--no-verify-tls` | — | Disable TLS certificate verification for S3 requests. For endpoints with a self-signed or otherwise unverifiable certificate; avoid it against anything reachable over an untrusted network. |

`--mode` must be exactly `file` or `s3`; anything else is rejected before any
storage is touched. In `file` mode the S3-only options above are accepted but
have no effect.

A backup's full location, as printed in reports and JSON, is
`folder[/path]/name`.

### Secrets on standard input

Every process on the host can read another process's command line, and audit
logs record it. A secret given as `--key`, `--secret-key`, `--source-key`,
`--source-secret-key` or `--password` (the vCenter/ESXi password, see
[VMDK (VDDK) options](#vmdk-vddk-options)) is therefore exposed for the whole
run. With
`--stdin-keys`, the tool reads them from standard input instead, one per
line as `name=value`:

```console
$ printf 'key=%s\nsecret-key=%s\n' "$KEY" "$S3_SECRET" |
    abr-vmdk-tool backup --stdin-keys --mode s3 --folder backups --access-key AK --name web01-disk0 ...
```

- The names are the option names without the dashes: `key`, `secret-key`,
  `source-key`, `source-secret-key`, `password`. The value is everything after the first
  `=`, spaces included. A trailing `\r` is dropped, so a line ending in `\r\n` works too.
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

## VMDK (VDDK) options

Shared by `backup` and `restore`. Passing `--vmdk` switches the disk side of
the command (source for `backup`, target for `restore`) from a plain file to
a VMDK reached through VDDK. Without `--host`, `--vmdk` still goes through
VDDK but opens the path as a local VMDK file rather than one on a vSphere
host — useful for VMDKs already exported to local disk. With `--host`, the
options below describe the vCenter/ESXi connection.

| Option | Argument | Meaning |
| --- | --- | --- |
| `--vmdk` | `PATH` | VMDK to read (backup) or write (restore), in place of `--source`/`--target`. Path syntax follows VDDK: a datastore path like `[datastore1] vm/vm.vmdk` for a host-backed disk, or a local filesystem path when `--host` is omitted. |
| `--host` | `HOST` | ESXi or vCenter host to connect to. Omit to open `--vmdk` as a local file instead. |
| `--user` | `NAME` | User name on the host. Required with `--host`. |
| `--password` | `PASS` | Password on the host. Required with `--host`, here or as `password=` with [`--stdin-keys`](#secrets-on-standard-input). |
| `--vm` | `SPEC` | Managed object reference of the VM, e.g. `vm-1234`. Required with `--host`. |
| `--thumbprint` | `PRINT` | SSL certificate thumbprint of the host, for hosts with a certificate that won't otherwise validate. |
| `--port` | `PORT` | Host port. Default: `443`. |
| `--nfc-port` | `PORT` | NFC port on the ESXi host used for the actual data transfer. Default: `902`. |
| `--transport` | `MODES` | Colon-separated VDDK transport mode list, tried in order, e.g. `nbdssl:nbd:hotadd:san`. Left to VDDK's own default when omitted. |
| `--snapshot` | `MOREF` | Managed object reference of the VM snapshot to read from/write to, instead of the VM's current point. |

`--host` requires `--user`, a password and `--vm` together; the tool checks
this before connecting and reports whichever is missing rather than failing
deep inside VDDK.

## Commands

### backup

Create a backup of a disk, volume, or VMDK.

```
abr-vmdk-tool.sh backup [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-s`, `--source` | `PATH` | Source file or block device to back up. Required unless `--vmdk` is given. |
| `-k`, `--key` | `KEY` | Encryption key. Optional; omit for an unencrypted backup. Backups are AES-256-GCM encrypted when a key is given. |
| `-c`, `--compression` | `0`-`22` | Zstd compression level. `0` disables compression. Default: `3`. Anything that is not a whole number in that range is refused. |
| `--type` | `full` \| `incremental` | `full` (default) copies every allocated block. `incremental` copies only the blocks changed since `--change-id`; it requires `--vmdk`, since it relies on VMware changed block tracking (CBT) — there is no equivalent for a plain file, so this is rejected rather than silently falling back to a full copy. |
| `--change-id` | `ID` | The change id an incremental backup is taken against (the CBT id of the point the previous backup was taken from). Only meaningful — and required — with `--type incremental`. |
| `--query-chunk` | `N` | Allocated-block query granularity, in sectors. Tunes how VDDK batches its `QueryAllocatedBlocks` calls. Default: `2048`. |
| `--immutable` | `none` \| `governance` \| `compliance` | Hold the backup under S3 Object Lock so it cannot be deleted or overwritten before its retention expires. `governance` allows a user with special permissions to remove the lock; `compliance` allows no one, including the storage account root, to shorten or remove it. S3 storage only. |
| `--retention` | `DAYS` | How long `--immutable` holds the backup, in days. `--immutable` requires either this or `--retention-till`, and neither is accepted without it, since a mode with no period holds nothing and a period with no mode enforces nothing. |
| `--retention-till` | `DATE` | Hold the backup until this moment instead of for a number of days: a date and time in UTC, `YYYY-MM-DDTHH:MM:SSZ` (a space may take the place of the `T`, and the `Z` may be left out). Seconds are required, and a moment already passed is refused. Mutually exclusive with `--retention`. |

Plus the [storage options](#storage-options) and, when backing up a VMDK, the
[VMDK options](#vmdk-vddk-options).

Whether the storage can actually deliver the requested immutability mode and
period is decided by the bucket, not by this tool — see `info` for what a
folder currently supports and applies.

Progress is reported as it copies: `NN%` lines, rewritten in place
interactively or one per line under `--json` (see [JSON
output](#json-output)). On success, a human-readable report follows (or a
JSON object, see below):

```
$ abr-vmdk-tool.sh backup --folder /srv/backups --name web01-sda --source /dev/sda -c 3
2%
...
100%
Source:            /dev/sda
Backup:            /srv/backups/web01-sda
Kind:              full
Raw data size:     8471179264 bytes
Compression:       zstd level 3
Stored data size:  3016784896 bytes
Compression ratio: 0.356124
Encryption:        none
Immutability:      none
```

An incremental backup:

```
$ printf 'password=%s\n' "$VC_PASSWORD" |
    abr-vmdk-tool.sh backup --folder /srv/backups --name web01-sda-inc1 \
    --vmdk '[datastore1] web01/web01.vmdk' --host vc.example.com \
    --user backup --stdin-keys --vm vm-1234 \
    --type incremental --change-id 52/1234
```

On failure, anything already written for the backup is discarded — a failed
run leaves nothing behind at the target location. If that cleanup itself
fails, the tool says so and tells you to remove the partial backup with the
`remove` command rather than pretending the run succeeded.

### restore

Restore a disk, volume, or VMDK from a backup.

```
abr-vmdk-tool.sh restore [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required** unless `--source-names` is given. |
| `--source-names` | `LIST` | Comma-separated chain to restore in one run in place of `--name`: the backup to start from first, full or incremental, then the incrementals taken on top of it in the order they were taken (see below). |
| `-t`, `--target` | `PATH` | Target file or block device to restore to. Required unless `--vmdk` is given. |
| `-k`, `--key` | `KEY` | Decryption key. Required if the backup was made with `--key`; omit for an unencrypted backup. A missing or wrong key is refused before the target is opened. |
| `-f`, `--force` | — | Apply an incremental backup, or a chain that starts with one, onto a target that already holds its parent (see below). |

Plus the [storage options](#storage-options) and, when restoring to a VMDK,
the [VMDK options](#vmdk-vddk-options).

An incremental backup holds only the blocks that changed since its parent —
restoring it alone leaves every other block untouched (an empty disk, if the
target started empty). To apply one:

1. Restore the full backup to the target first.
2. Restore the incremental to the *same* target with `-f`/`--force`, which
   permits writing onto a target that already holds data instead of the tool
   refusing as a safety check.

The target is then kept up to date by applying every incremental taken
afterwards the same way, one at a time and in the order they were taken, or
several at once.

`--source-names` restores a chain in one run, in place of `--name`: the backup
to start from first, then the incrementals taken on top of it in the order
they were taken. It is the same list, checked the same way, as [copy](#copy)
takes. Each block is written once, as the last backup that changed it holds
it, so the target goes straight to the state at the end of the chain without
being written over once per backup. A chain that starts with a full backup
restores a whole disk; one that starts with an incremental is applied with
`--force`, like a single incremental, onto a target that holds the parent of
its first backup. The chain is read from the storage the
[storage options](#storage-options) describe, and decrypted with `--key`; the
`--source-<...>` storage options belong to `copy` and are refused here. Under
`--json` a chain is reported as `copy` reports one, with `source`,
`source_names` and `merged_from` in place of `backup` and `name`.

```console
$ abr-vmdk-tool.sh restore --folder /srv/backups \
      --source-names web01-sda,web01-sda-inc1,web01-sda-inc2 --target /dev/sdb
100%
Restored '/srv/backups' (web01-sda, web01-sda-inc1, web01-sda-inc2) to '/dev/sdb'
```

Restoring to an existing file target without `--force` truncates it first;
with `--force` (applying an incremental) the existing content is kept and
only the ranges the incremental covers are overwritten.

Before restoring, the tool checks that a fixed-geometry target (a block
device, or a VMDK with an existing layout) actually matches the backup's
sector size and sector count, and refuses rather than writing a mismatched
image. A plain file target with no existing geometry has its size set to
match the backup.

```
$ abr-vmdk-tool.sh restore --folder /srv/backups --name web01-sda --target /dev/sdb
100%
Restored '/srv/backups/web01-sda' to '/dev/sdb'
```

### copy

Merge a chain of backups — a full backup and the incrementals taken on top of
it — into one new full backup. A chain may also start with an incremental, or
be a single backup of either kind, which is how another repository is kept up
to date one new backup at a time.

```
abr-vmdk-tool.sh copy [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Name of the backup to create. **Required.** |
| `--source-names` | `LIST` | Comma-separated chain to merge: the backup to start from first, full or incremental, then the incrementals taken on top of it in the order they were taken. **Required.** |
| `-k`, `--key` | `KEY` | Encryption key for the new backup. Optional, and unrelated to the chain's own key — that one is `--source-key`. |
| `-c`, `--compression` | `0`–`22` | Compression level for the new backup. Default: `3`. |
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

A chain that starts with an incremental is not a whole disk, and the result is
an incremental too: an incremental of the same parent as the first backup of
the chain. Applied with `restore --force` onto a target that holds that parent,
it leaves the disk that applying each backup of the chain in turn would have
left. With a single backup in `--source-names` the copy is that backup as it
is, full or incremental, so a second repository is kept up to date by copying
every new backup there as it is taken, rather than the whole disk again:

```console
$ abr-vmdk-tool.sh copy --folder /mnt/offsite \
      --source-folder /srv/backups --source-names web01-sda-inc3 --name web01-sda-inc3
100%
Source:            /srv/backups (web01-sda-inc3)
Backup:            /mnt/offsite/web01-sda-inc3
Kind:              incremental, merged from 1 backup(s)
Raw data size:     402653184 bytes
Compression:       zstd level 3
Stored data size:  136314880 bytes
Compression ratio: 0.338542
Encryption:        none
Immutability:      none
```

What the copy is written *as* comes from the options given here rather than
from the chain, which makes this also the way to recompress a backup, change
its encryption key, or move it between a folder and a bucket:

```console
$ abr-vmdk-tool.sh copy --folder /srv/backups \
      --source-names web01-sda,web01-sda-inc1,web01-sda-inc2 \
      --name web01-sda-week12 --compression 6
100%
Source:            /srv/backups (web01-sda, web01-sda-inc1, web01-sda-inc2)
Backup:            /srv/backups/web01-sda-week12
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

```console
$ abr-vmdk-tool.sh copy --mode s3 --folder archive --endpoint https://s3.example.com \
      --path-style --access-key AK --secret-key SK \
      --source-mode file --source-folder /srv/backups \
      --source-names web01-sda,web01-sda-inc1 \
      --name web01-sda-2026-03 --immutable compliance --retention 365
```

**What is checked, and what is not.** The format keeps no link from an
incremental back to its parent, so the order `--source-names` gives *is* the
chain — pass them out of order and the copy will hold the wrong blocks, with
nothing to warn you. What the tool does check, and refuses on:

- the first backup may be full or incremental, but every one after it must
  be incremental;
- all of them must describe the same disk (same sector size and sector count);
- the new backup may not be one of the chain, when the destination storage is
  the same one the chain is read from — that would overwrite what is being
  read.

A metadata blob stored with `meta_save` belongs to the backup it was stored
with and is not carried over; run `meta_save` against the copy if it needs one.

An interrupted copy (Ctrl-C, `SIGTERM`) discards what it had written, exactly
as `backup` does, and leaves the chain untouched.

### list

List the backups in a storage folder (or a subfolder of it).

```
abr-vmdk-tool.sh list [options]
```

Takes only the [storage options](#storage-options) — no `--name`. `--path`
narrows the listing to one subfolder.

```
$ abr-vmdk-tool.sh list --folder /srv/backups
Backups in '/srv/backups':
  web01-sda
  web01-sda-inc1
  db02-sdb
```

### info

Show information about a backup, or — without `--name` — about the storage
folder itself.

```
abr-vmdk-tool.sh info [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. Optional: omitted, `info` reports the storage folder's own properties instead of a specific backup. |

Plus the [storage options](#storage-options).

Without `--name`, the report covers the folder alone — the strongest
immutability mode it can offer at all, and the mode (and retention) it
currently applies by default to new backups:

```
$ abr-vmdk-tool.sh info --folder s3://backups --mode s3
Folder:            s3://backups
Max immutability:  compliance
Immutability:      none, new backups stay deletable
```

With `--name`, the report covers that one backup: its geometry, compression,
encryption, and the immutability it is actually held under (which can differ
from the folder's own default when the backup requested its own mode/period
via `backup --immutable`/`--retention`):

```
$ abr-vmdk-tool.sh info --folder /srv/backups --name web01-sda
Backup:            /srv/backups/web01-sda
Kind:              full
Sectors:           16777216
Sector size:       512 bytes
Physical sector:   512 bytes
Raw data size:     8471179264 bytes
Compression:       zstd level 3
Stored data size:  3016784896 bytes
Compression ratio: 0.356124
Encryption:        none
Max immutability:  compliance
Immutability:      none, the backup can be deleted
```

### verify

Check the integrity of a backup by re-reading and checksumming every stored
data range, without necessarily decrypting it.

```
abr-vmdk-tool.sh verify [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-k`, `--key` | `KEY` | Decryption key. Optional — given, it is tried on the backup's first data block, and a key that does not decrypt it fails the verify; the data checksums are verified without decrypting, key or no key. |

Plus the [storage options](#storage-options).

Every data range is checked — `verify` does not stop at the first corrupt
one, so a single run reports the full extent of any damage:

```
$ abr-vmdk-tool.sh verify --folder /srv/backups --name web01-sda
100%
Backup '/srv/backups/web01-sda' is intact: 42 data ranges (8471179264 bytes) verified
```

```
$ abr-vmdk-tool.sh verify --folder /srv/backups --name web01-sda
100%
Backup '/srv/backups/web01-sda' is CORRUPT: 2 of 42 data ranges failed verification
```

A corrupt backup is reported as a *result*, not treated as a malfunction: the
command still exits non-zero, but `--json` still gets one complete result
document (`intact: false`, `corrupt_ranges` set) rather than an error — see
[JSON output](#json-output).

### remove

Delete a backup.

```
abr-vmdk-tool.sh remove [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-f`, `--force` | — | Skip the confirmation prompt. |

Plus the [storage options](#storage-options).

Without `--force`, `remove` asks for confirmation on stdin:

```
$ abr-vmdk-tool.sh remove --folder /srv/backups --name web01-sda-inc1
Remove backup '/srv/backups/web01-sda-inc1'? [y/N] y
Removed backup '/srv/backups/web01-sda-inc1'
```

Anything other than `y`/`Y` aborts with exit status 1 and prints `Aborted.`
(or, under `--json`, a result document with `"status": "aborted"`).

**This is what surprises callers who script the tool:** if you run `remove`
from a subprocess with no stdin attached, `std::cin` reads an empty line
immediately, which is not `y`/`Y`, so the removal is silently aborted — not
performed and not an error either, from the exit status alone. Always pass
`--force` when scripting `remove` unless you are prepared to feed the prompt
an answer.

A backup held under S3 Object Lock (`governance` or `compliance`, see
`backup --immutable`) cannot be removed before its retention expires; the
storage rejects the deletion and the tool reports that as a failure. See
[retention](#retention) for changing how long that hold lasts.

### retention

Change the immutability a backup already in storage is held under, to extend
a hold that is running out or to raise it to a stronger mode. S3 storage only.

```
abr-vmdk-tool.sh retention [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `--immutable` | `governance` \| `compliance` | Mode to hold the backup in. **Required.** Unlike `backup --immutable`, `none` is not accepted here: Object Lock has no way to release an object that is already held. |
| `--retention` | `DAYS` | How long to hold the backup **from now**, in days. This or `--retention-till` is **required**. |
| `--retention-till` | `DATE` | Hold the backup until this moment, in UTC as `YYYY-MM-DDTHH:MM:SSZ`, as for [backup](#backup). Mutually exclusive with `--retention`. |

Plus the [storage options](#storage-options).

The period is counted from the moment the command runs, so it states what the
backup has left rather than what it was originally given. Extending a backup
created with `--retention 10` four days ago to `--retention 30` leaves it held
for 30 more days, not 26. `--retention-till` names the moment the hold ends
instead, so the same command run twice, or run for every disk of a VM, puts the
same date on all of them.

All three objects a backup is made of (`.abrd`, `.abri` and `.abrm`) are put
under the same retention, since a backup is only held as firmly as its least
protected object. The metadata blob is optional, so a backup that never had a
`meta_save` simply has nothing to hold under that key.

A name may also hold only metadata, with no backup under it, such as a manifest
of a whole VM that `meta_save` stores next to the backups of its disks.
`retention` holds the `.abrm` of such a name on its own. The index decides
which of the two a name is: with `.abri` there, `.abrd` has to be there as well,
and a name that holds neither a backup nor metadata is an error.

```
$ abr-vmdk-tool.sh retention --mode s3 --folder backups --name web01-sda \
      --immutable compliance --retention 200
Backup 'backups/web01-sda' is now held in compliance mode, retained for 200 day(s)

$ abr-vmdk-tool.sh retention --mode s3 --folder backups --name web01-sda \
      --immutable compliance --retention-till 2027-06-30T00:00:00Z
Backup 'backups/web01-sda' is now held in compliance mode, retained until 2027-06-30T00:00:00Z
```

**What the storage will accept is its call, not the tool's.** The tool checks
only that the mode and the period make sense together and that the bucket
supports the mode at all; the request itself is passed to the storage, and a
refusal is reported as it was given. In practice:

- Extending a hold works in either mode.
- Shortening or lifting one does not. A compliance hold can never be shortened
  by anyone; a governance hold can only be shortened by a caller with the
  permission to bypass it, which this command does not ask for.
- Raising `governance` to `compliance` is permitted by AWS S3, but S3-compatible
  targets vary — MinIO, for one, refuses any change of mode on an object that is
  already held.

A refusal leaves the backup as it was, and the command exits 1 after printing
the storage's own error for each object it could not update.

### meta_save

Store a metadata blob alongside a backup — for example the JSON manifest a
backup server keeps about the disk. The blob is opaque to the tool; it is
stored and returned byte-for-byte.
The name does not have to hold a backup: metadata may also be stored on its own,
and [retention](#retention) holds it under Object Lock the same way.

```
abr-vmdk-tool.sh meta_save [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-s`, `--source` | `PATH` | File to read the metadata from. `-` or omitted reads standard input. |

Plus the [storage options](#storage-options).

```
$ echo '{"vm":"web01","disk":"sda"}' | abr-vmdk-tool.sh meta_save --folder /srv/backups --name web01-sda
Saved metadata for '/srv/backups/web01-sda'
```

### meta_load

Read back the metadata blob stored by `meta_save`.

```
abr-vmdk-tool.sh meta_load [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-t`, `--target` | `PATH` | File to write the metadata to. `-` or omitted writes to standard output. |

Plus the [storage options](#storage-options).

```
$ abr-vmdk-tool.sh meta_load --folder /srv/backups --name web01-sda
{"vm":"web01","disk":"sda"}
```

Without `--json`, a blob written to stdout is the *only* thing on stdout —
nothing else is printed around it, so the output can be piped or redirected
directly. Under `--json`, the blob instead comes back as a `metadata` string
field of the result document (see below), since raw metadata on stdout would
otherwise collide with the JSON document.

## JSON output

Every command accepts `--json`, which replaces the human-readable report a
command normally ends with by a single JSON object on stdout. It covers only
the *result*; a long `backup`, `restore`, or `verify` still prints progress
first, and those percentages stay plain text — the same `NN%` lines the
interactive form shows, except one per line and flushed immediately, rather
than rewritten in place with a carriage return:

```
$ abr-vmdk-tool.sh backup --json --folder /srv/backups --name web01-sda --source /dev/sda
2%
4%
...
100%
{
  "command": "backup",
  "status": "ok",
  ...
}
```

So a caller reads stdout a line at a time: every line up to and including
`100%` is progress, and the JSON document is everything from the first `{`
onward.

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

A failure still produces exactly one document:

```
$ abr-vmdk-tool.sh info --json --folder /srv/backups --name ghost
{
  "command": "info",
  "status": "error",
  "error": "failed to open backup 'ghost'"
}
```

`verify` is the one command that reports a failed check as data rather than
as a malfunction: a corrupt backup still yields the full result document,
with `"intact": false` and a `corrupt_ranges` count, alongside the non-zero
exit status:

```
$ abr-vmdk-tool.sh verify --json --folder /srv/backups --name web01-sda
{
  "command": "verify",
  "status": "error",
  "backup": "/srv/backups/web01-sda",
  "name": "web01-sda",
  "intact": false,
  "data_ranges": 42,
  "corrupt_ranges": 2,
  "bytes_verified": 8471179264,
  "error": "backup is corrupt: 2 of 42 data ranges failed verification"
}
```

`info`, called on a backup, reports both the backup's own properties and its
immutability:

```
$ abr-vmdk-tool.sh info --json --folder /srv/backups --name web01-sda
{
  "command": "info",
  "status": "ok",
  "backup": "/srv/backups/web01-sda",
  "name": "web01-sda",
  "kind": "full",
  "sectors": 41943040,
  "sector_size": 512,
  "physical_sector_size": 512,
  "raw_data_size": 8471179264,
  "compression": "zstd",
  "compression_level": 3,
  "stored_data_size": 3016784896,
  "compression_ratio": 0.356124,
  "encryption": "AES-256-GCM",
  "immutability_supported": "compliance",
  "immutability": "none"
}
```

`retention` reports the mode and the period the backup is held under once the
update has gone through:

```
$ abr-vmdk-tool.sh retention --json --mode s3 --folder backups --name web01-sda \
      --immutable compliance --retention 200
{
  "command": "retention",
  "status": "ok",
  "backup": "backups/web01-sda",
  "name": "web01-sda",
  "immutability": "compliance",
  "retention_days": 200
}
```

A hold given with `--retention-till` is reported as a `retention_till` string,
`YYYY-MM-DDTHH:MM:SSZ`, in place of `retention_days`. `backup` and `copy` report
the hold they were asked for the same way.

`copy` reports the chain it merged alongside the properties of the backup it
wrote; `kind` is `incremental` when the chain starts with one:

```console
$ abr-vmdk-tool.sh copy --json --folder /srv/backups \
      --source-names web01-sda,web01-sda-inc1 --name web01-sda-week12
{
  "command": "copy",
  "status": "ok",
  "source": "/srv/backups",
  "source_names": [
    "web01-sda",
    "web01-sda-inc1"
  ],
  "merged_from": 2,
  "backup": "/srv/backups/web01-sda-week12",
  "name": "web01-sda-week12",
  "kind": "full",
  "immutability": "none",
  "sectors": 41943040,
  "sector_size": 512,
  "physical_sector_size": 512,
  "raw_data_size": 8471179264,
  "compression": "none",
  "compression_level": 0,
  "encryption": "none"
}
```

`meta_load` embeds the stored blob verbatim as a JSON string, so the result is
valid JSON exactly when the blob itself is valid UTF-8 — true of the JSON
manifests a backup server typically stores with `meta_save`.

`--help` is exempt from `--json` and always prints plain text, on the
reasoning that a caller asking for the manual is not asking for a machine
format.

## Exit status

- `0` — the command succeeded (including `-h`/`--help` and `-v`/`--version`).
- `1` — the command failed: a bad option, a storage or VDDK error, a corrupt
  `verify`, a declined `remove` confirmation, or an exception that escaped a
  storage library (reported behind `Error: ` like any other failure).

## Signals

`SIGINT` (Ctrl-C) and `SIGTERM` are caught once, during a `backup`, `restore`,
`copy`, or `verify` copy loop: the loop notices the flag at its next iteration,
prints `Interrupted, releasing the disk and the VM`, and unwinds normally so
a VDDK session gets released cleanly rather than the process being killed out
from under it (which would leave the VM's disks marked busy). A second
Ctrl-C, or one that arrives while nothing is polling the flag (a VDDK call
that's currently blocked), falls through to the default disposition and kills
the process immediately.
