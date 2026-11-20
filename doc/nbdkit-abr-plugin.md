# nbdkit-abr-plugin — Reference

`nbdkit-abr-plugin` serves a chain of `.abr` backups read-only over NBD, as the
disk the chain restores to. A Proxmox VE restore runs one nbdkit process per
disk with it, and PVE's `qemu-img` reads the disk through a kernel NBD device.
The tools back the disks up; see the [abr-nbd-tool manual](abr-nbd-tool.md).
For how to build and install the plugin, see the
[top-level README](../README.md).

```
nbdkit [nbdkit options] nbdkit-abr-plugin.so folder=DIR chain=FULL[,INCREMENTAL...] [parameters]
```

nbdkit also finds the plugin by its name, `abr`, when it is installed in
nbdkit's plugin directory. `nbdkit nbdkit-abr-plugin.so --help` lists the
parameters, and `--version` prints the plugin's version, `abr <version>`, next
to nbdkit's: the version of the tools built with it.

## Contents

- [Parameters](#parameters)
- [Secrets](#secrets)
- [What it serves](#what-it-serves)
- [Opening the chain and errors](#opening-the-chain-and-errors)
- [Proxmox VE restores](#proxmox-ve-restores)
- [Examples](#examples)

## Parameters

| Parameter | Value | Meaning |
| --- | --- | --- |
| `folder` | `DIR` or `BUCKET` | The repository folder, or the bucket with `mode=s3`. **Required.** A relative folder is taken from the directory nbdkit starts in. |
| `path` | `PATH` | The subfolder of the backups in the repository, as the tools' `--path`. |
| `chain` | `NAME[,NAME...]` | The backups to serve: a full backup, then the incrementals taken on top of it, in the order they were taken, as the tools' `--source-names`. **Required.** |
| `mode` | `file` \| `s3` | Where the repository is. Default: `file`. |
| `key` | `KEY` | The decryption key, for a chain made with a key. See [Secrets](#secrets). |
| `endpoint` | `URL` | S3 endpoint. |
| `region` | `REGION` | S3 region. |
| `access-key` | `KEY` | S3 access key. |
| `secret-key` | `KEY` | S3 secret key. See [Secrets](#secrets). |
| `aws-profile` | `NAME` | A profile of the AWS configuration files, in place of the keys. |
| `path-style` | boolean | Path-style S3 addressing, as `--path-style`. Default: false. |
| `verify-tls` | boolean | Whether to verify the endpoint's certificate. Default: true; false is `--no-verify-tls`. |
| `stall-timeout` | `SECONDS` | How long a read goes on trying while the S3 storage does not answer, as the tools' `--stall-timeout`. Default: 0, which fails within seconds. A client that waits less, such as a kernel NBD device with a request timeout, gives up first. |

The S3 parameters go with `mode=s3`; any of them in file mode is refused. A
boolean is anything nbdkit reads as one: `true`, `false`, `on`, `off`, `1`,
`0`. An unknown parameter is refused.

## Secrets

`key` and `secret-key` take nbdkit's forms for passwords:

| Form | Meaning |
| --- | --- |
| `key=SECRET` | The secret itself. Other users of the machine can see it in the process list. |
| `key=-FD` | Read it from file descriptor `FD`, inherited from the process that starts nbdkit, which then closes it. Not 0, 1 or 2. |
| `key=+FILE` | Read it from `FILE`. |
| `key=-` | Ask for it, when standard input is a terminal. |

A Proxmox VE provider passes `-FD` with a pipe, so the key is in no file and on
no command line. nbdkit drops a trailing newline from what it reads.

## What it serves

- **The disk the chain restores to.** A block comes from the newest backup that
  holds it, and a block no backup holds reads as zeros: the same disk as
  `restore --source-names` writes. The size is the size of the backed-up disk.
- **Only whole chains.** The first backup must be a full backup and the others
  incrementals of the same disk; a chain that starts with an incremental is
  refused, since nothing under it holds its parent. The order of the
  incrementals is taken as given, as by the tools.
- **Read-only.** The plugin has no write operations, and the provider runs
  nbdkit with `-r` as well. It writes nothing to the repository, so a
  read-only mount of a share is enough.
- **Block status.** Data where any backup of the chain holds a block, and holes
  that read as zeros everywhere else. A block an incremental changed to zeros
  is data. Clients that read block status, such as `qemu-img` reading an NBD
  URI, `nbdcopy` and `nbdinfo --map`, skip the holes; a kernel NBD device does
  not pass block status on.
- **Block size.** Minimum 1 byte, so any read is served. The preferred size is
  the largest block the chain stores, rounded down to a power of two, 4 MiB by
  default: a read of that size and alignment decodes each stored block once.
  No maximum beyond nbdkit's own.
- **One request at a time.** backuplib's reads are not thread-safe, so the
  plugin uses nbdkit's `serialize_all_requests` thread model, with the chain
  opened once per process. It does not offer multi-conn. The last decoded block
  of each backup is kept, so small sequential reads cost one decode per stored
  block.

## Opening the chain and errors

The chain is opened, and its key checked, once, before nbdkit serves anything:

- **On a file repository,** before nbdkit forks into the background, writes
  its pidfile or starts its `--run` command. A chain that does not open stops
  nbdkit with exit status 1, before any of them.
- **On S3,** after the fork, because the AWS SDK starts threads that a fork
  would lose. A chain that does not open still stops nbdkit with exit status 1,
  but its pidfile has been written by then, and in the background the error
  shows only in syslog. A caller that waits for the pidfile should also check
  that nbdkit is still running; a connection to its socket fails once it has
  exited.

Errors go through nbdkit's log, each line as an nbdkit error: on standard error
in the foreground (`-f`, or with `--run`), and to syslog in the background. A
failed read is an I/O error to the client. With `-v`, nbdkit also shows the
plugin's debug lines, among them the size of the chain when it is opened and,
at exit, how many reads it served and how many stored blocks it decoded.

## Proxmox VE restores

The provider starts one nbdkit per disk in the foreground, on a Unix socket,
with the repository settings and the chain from the restore file and the key
on a pipe:

```
nbdkit -f -r -U <socket> -P <pidfile> --exit-with-parent nbdkit-abr-plugin.so \
    folder=<folder> path=<path> chain=<full>,<incremental>,... key=-<fd>
```

It then attaches the socket read-only to a free kernel NBD device and hands
PVE the device, since qemu-server does not take an NBD URI as a restore source:

```
nbd-client -u <socket> /dev/nbdN -readonly
```

On an SMB repository, the provider starts nbdkit in a private mount namespace
with a read-only kernel mount of the share, so the plugin sees an ordinary
folder.

## Examples

Read a chain into a raw image, as a restore does:

```console
$ nbdkit -r -U - nbdkit-abr-plugin.so folder=/mnt/repo path=vm-101 \
      chain=vm-101-scsi0-full,vm-101-scsi0-inc1,vm-101-scsi0-inc2 \
      --run 'qemu-img convert -f raw -O raw "$uri" scsi0.raw'
```

Show where an encrypted chain on S3 holds data, with the keys from files:

```console
$ nbdkit -r -U - nbdkit-abr-plugin.so mode=s3 folder=abr-backups path=pve/101 \
      endpoint=https://s3.example.com region=us-east-1 path-style=true \
      access-key=ABRACCESSKEY secret-key=+/run/abr/secret key=+/run/abr/key \
      chain=vm-101-scsi0-full,vm-101-scsi0-inc1 --run 'nbdinfo --map "$uri"'
         0     4194304    0  data
   4194304    62914560    3  hole,zero
  ...
```

Serve a chain in the background until it is stopped:

```console
$ nbdkit -r -U /run/abr/scsi0.sock -P /run/abr/scsi0.pid nbdkit-abr-plugin.so \
      folder=/mnt/repo path=vm-101 chain=vm-101-scsi0-full
$ qemu-img info nbd+unix:///?socket=/run/abr/scsi0.sock
$ kill "$(cat /run/abr/scsi0.pid)"
```
