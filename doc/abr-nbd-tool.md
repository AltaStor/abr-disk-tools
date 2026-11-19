# abr-nbd-tool — Command Reference

`abr-nbd-tool` backs up one disk at a time from an NBD export into the `.abr`
backup format, and restores backups to plain files and devices. Its main use
is Proxmox VE: when a backup runs, PVE exports each disk of the VM over NBD on
a Unix socket, with a QEMU dirty bitmap for incrementals, and the backup
provider runs the tool once per disk.

This document is the manual for the NBD side of the tool. The storage options
and the commands that work on the repository alone are the same in every tool
built from this repository; they are described once, in the
[abr-vmdk-tool manual](abr-vmdk-tool.md), and linked from here. For how to
build and install the tool, see the [top-level README](../README.md).

```
abr-nbd-tool <command> [options]
```

Every command accepts `-h`/`--help`, which prints that command's own usage
and exits 0. Running the tool with no arguments, or with `-h`/`--help` before
a command, prints the command list:

```
$ abr-nbd-tool --help
Usage: abr-nbd-tool <command> [options]

Commands:
  backup    Create a backup of a disk/volume/NBD export
  restore   Restore a disk/volume from a backup
  copy      Merge a chain of backups into a new backup
  list      List available backups
  info      Show information about a backup
  verify    Check the integrity of a backup
  remove    Delete a backup
  retention Change the immutability a stored backup is held under
  meta_save Store a metadata blob alongside a backup
  meta_load Read back the metadata blob of a backup

Run 'abr-nbd-tool <command> --help' for command-specific options.
Pass --json to any command to get its result on stdout as a JSON document.
```

`-v`/`--version` prints the tool's version and exits. Options are parsed as in
the other tools: see the [abr-vmdk-tool manual](abr-vmdk-tool.md).

## Contents

- [Storage options](#storage-options)
- [NBD options](#nbd-options)
- [Commands](#commands)
  - [backup](#backup)
  - [restore](#restore)
  - [The other commands](#the-other-commands)
- [JSON output](#json-output)
- [Exit status](#exit-status)
- [Signals](#signals)

## Storage options

`--mode`, `--folder`, `--path`, `--create-folder` and the S3 options select
where backups live, exactly as in the other tools:
[Storage options](abr-vmdk-tool.md#storage-options).

`--stdin-keys` reads the secrets from standard input instead of the command
line, as described under
[Secrets on standard input](abr-vmdk-tool.md#secrets-on-standard-input). The
names this tool takes are `key`, `secret-key`, `source-key` and
`source-secret-key`; it has no password of its own.

## NBD options

`backup` reads an NBD export when `--nbd` is given, and a plain file or device
named by `--source` otherwise. `restore` writes a plain file or device only,
and refuses these options.

| Option | Argument | Meaning |
| --- | --- | --- |
| `--nbd` | `SERVER` | The NBD server to read: a Unix socket path, or an NBD URI such as `nbd+unix:///drive-scsi0?socket=/run/disk.sock` or `nbd://host/disk`. Takes the place of `--source`; giving both is refused. |
| `--export` | `NAME` | The export to open on a socket, such as `drive-scsi0`. Default: the server's default export. A URI names its own export, so `--export` is refused with one. |
| `--bwlimit` | `MIB` | Read at most `MIB` MiB/s, a whole number of at least 1. Default: no limit. |
| `--discard` | — | Discard each range after it is read. See below. |

`--export`, `--bwlimit` and `--discard` go with `--nbd`; any of them without
it is refused.

**What a backup reads.** The tool asks the server for one block status
context per connection:

- A full backup asks for `base:allocation` and reads every block the export
  does not report as reading zero. An export without allocation status is read
  whole.
- An incremental (`--type incremental --change-id BITMAP`) asks for the QEMU
  dirty bitmap `qemu:dirty-bitmap:BITMAP` and reads its dirty blocks. An
  export without that bitmap fails the backup: reading it whole would be a
  full backup recorded as an incremental.

QEMU broke a connection that negotiated both contexts while the bitmap was
clean, which is why there is only ever one.

**Discard.** `--discard` sends an NBD trim for each range once it is read. On
the backup export of a Proxmox VE VM this releases the fleecing image as the
backup goes, which keeps the space a backup needs on the node small. On an
export of a disk itself it **erases the data**, which is why it is off unless
asked for. An export that is read-only or cannot trim gets a warning, and the
backup goes on without discarding; so does an export whose trim fails partway.
A backup that fails after a discard leaves the PVE disk as it was, since only
the fleecing copy is discarded, and the next run reads again from the VM.

**Geometry.** NBD has no sector size, so a backup records 512-byte sectors,
which QEMU addresses its disks in, and the server's preferred block size as
the physical sector size when it lies between 512 and 4096. An export whose
size is not a whole number of 512-byte sectors is refused.

## Commands

### backup

Create a backup of an NBD export, or of a file or device.

```
abr-nbd-tool backup [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required.** |
| `-s`, `--source` | `PATH` | Source file or block device to back up. Required unless `--nbd` is given. |
| `-k`, `--key` | `KEY` | Encryption key. Optional; backups are AES-256-GCM encrypted when a key is given. |
| `-c`, `--compression` | `0`-`22` | Zstd compression level. `0` disables compression. Default: `3`. |
| `--type` | `full` \| `incremental` | `full` (default) reads every block the export does not report as zero. `incremental` reads the blocks of the dirty bitmap named by `--change-id`, and requires `--nbd`. |
| `--change-id` | `BITMAP` | The QEMU dirty bitmap an incremental reads, such as `snapshot-access:abr` on a PVE backup export. Only meaningful, and required, with `--type incremental`. |
| `--immutable`, `--retention`, `--retention-till` | | Immutability on S3, as in [abr-vmdk-tool](abr-vmdk-tool.md#backup). |

Plus the [storage options](#storage-options) and the [NBD options](#nbd-options).

Progress is printed as `NN%` lines, then a report:

```
$ abr-nbd-tool backup --folder repo --path vm-101 --name vm-101-scsi0-full \
      --nbd /run/qemu-server/101-nbd.sock --export drive-scsi0
5%
...
100%
Source:            /run/qemu-server/101-nbd.sock (export drive-scsi0, base:allocation)
Backup:            repo/vm-101/vm-101-scsi0-full
Kind:              full
Raw data size:     70254592 bytes
Compression:       zstd level 3
Stored data size:  2467 bytes
Compression ratio: 3.51151e-05
Encryption:        none
Immutability:      none
```

With `--discard`, the report adds the number of bytes discarded:

```
Discarded:         3145728 bytes
```

An incremental from the bitmap of a PVE backup export, with the key on
standard input:

```console
$ printf 'key=%s\n' "$KEY" |
    abr-nbd-tool backup --stdin-keys --folder repo --path vm-101 --name vm-101-scsi0-inc1 \
    --nbd /run/qemu-server/101-nbd.sock --export drive-scsi0 \
    --type incremental --change-id snapshot-access:abr --discard
```

On failure, anything already written for the backup is discarded, so a failed
run leaves nothing behind in the repository.

### restore

Restore a backup, or a chain of backups, to a plain file or block device.

```
abr-nbd-tool restore [options]
```

| Option | Argument | Meaning |
| --- | --- | --- |
| `-n`, `--name` | `NAME` | Backup name. **Required** unless `--source-names` is given. |
| `--source-names` | `LIST` | Comma-separated chain to restore in one run: the backup to start from first, then the incrementals on top of it in the order they were taken. |
| `-t`, `--target` | `PATH` | Target file or block device. **Required.** |
| `-k`, `--key` | `KEY` | Decryption key, for a backup made with `--key`. |
| `-f`, `--force` | — | Apply an incremental, or a chain that starts with one, onto a target that already holds its parent. |

Plus the [storage options](#storage-options). The NBD options are refused: a
Proxmox VE restore reads its chain through nbdkit, not through this tool.
Chains, `--force` and the geometry checks work as described under
[restore](abr-vmdk-tool.md#restore) in the abr-vmdk-tool manual.

```console
$ abr-nbd-tool restore --folder repo --path vm-101 \
      --source-names vm-101-scsi0-full,vm-101-scsi0-inc1 --target /dev/sdb
```

### The other commands

`copy`, `list`, `info`, `verify`, `remove`, `retention`, `meta_save` and
`meta_load` work on the repository alone and behave exactly as in the other
tools: see [Commands](abr-vmdk-tool.md#copy) in the abr-vmdk-tool manual.

## JSON output

`--json` works as in the other tools ([JSON output](abr-vmdk-tool.md#json-output)):
progress lines first, then one JSON document from the first `{` onward. A
backup of an NBD export adds three fields to the result:

| Field | Meaning |
| --- | --- |
| `export` | The export name given with `--export`, empty for the default export or a URI. |
| `block_status` | The block status context the ranges came from: `base:allocation`, `qemu:dirty-bitmap:<name>`, or empty when the export has no allocation status and was read whole. |
| `discarded_bytes` | With `--discard` only: the bytes discarded after reading. |

```
$ abr-nbd-tool backup --json --folder repo --path vm-101 --name vm-101-scsi0-inc1 \
      --nbd /run/qemu-server/101-nbd.sock --export drive-scsi0 \
      --type incremental --change-id snapshot-access:abr --discard
100%
{
  "command": "backup",
  "status": "ok",
  "source": "/run/qemu-server/101-nbd.sock",
  "backup": "repo/vm-101/vm-101-scsi0-inc1",
  "name": "vm-101-scsi0-inc1",
  "kind": "incremental",
  "change_id": "snapshot-access:abr",
  "export": "drive-scsi0",
  "block_status": "qemu:dirty-bitmap:snapshot-access:abr",
  "discarded_bytes": 3145728,
  "immutability": "none",
  "sectors": 2097152,
  "sector_size": 512,
  "physical_sector_size": 4096,
  "raw_data_size": 3145728,
  "compression": "zstd",
  "compression_level": 3,
  "stored_data_size": 115,
  "compression_ratio": 3.65575e-05,
  "encryption": "none"
}
```

A failure is one document with `status` `error` and the message:

```
$ abr-nbd-tool backup --json --folder repo --name x --nbd /run/qemu-server/101-nbd.sock \
      --export drive-scsi0 --type incremental --change-id nosuch
Error: the NBD export offers no dirty bitmap 'nosuch'
{
  "command": "backup",
  "status": "error",
  "error": "the NBD export offers no dirty bitmap 'nosuch'"
}
```

## Exit status

- `0`: the command succeeded, including `-h`/`--help` and `-v`/`--version`.
- `1`: the command failed: a bad option, an NBD or storage error, a corrupt
  `verify`, or a declined `remove` confirmation.

## Signals

`SIGINT` and `SIGTERM` are caught once during a copy loop: the loop notices
the flag at its next read, prints `Interrupted, closing the NBD connection`,
discards the partial backup and exits. A second signal kills the process at
once. A Proxmox VE task that is stopped sends `SIGTERM` to its whole process
group, which includes the tool.
