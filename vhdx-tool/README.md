# abr-vhdx-tool

`abr-vhdx-tool` backs up and restores disks, volumes and Hyper-V virtual disks
(VHD, VHDX, AVHDX) into the `.abr` backup format. It is the data mover the
backup server invokes once per disk, with the same commands, the same options
apart from the hypervisor-specific ones, and the same `--json` results as the
other tools built from this repository; they read and write the same
repositories.

It runs on Windows 10 and Windows Server 2016 or later. The build -- natively
on Windows, or as a cross build on Linux -- is described in the
[top-level README](../README.md). Either way the result is one self-contained
`abr-vhdx-tool.exe`: it depends on nothing but Windows itself, so installing it
on a Hyper-V host means copying that one file. There is no launcher.

The command reference is [doc/abr-vhdx-tool.md](../doc/abr-vhdx-tool.md), and
testing against a real Hyper-V host is [doc/testing.md](../doc/testing.md).

## Running It

```bat
abr-vhdx-tool.exe <command> [options]
```

Commands that read or write a virtual disk (`--vhdx`) attach it through the
Windows virtual disk service, which needs an **elevated (administrator)**
process; so do raw disks and volumes as `--source`/`--target`. Everything else
-- plain files, and every command that only touches the repository -- runs
with the caller's own rights. The tool never asks for elevation itself: run it
from an elevated prompt, as a service account, or over a WinRM session of an
administrator.

Paths, backup names and VM names may be in any language. The tool reads its
command line in UTF-16 and keeps every string in UTF-8, and its manifest sets
the process's code page to UTF-8, which the storage library relies on for the
paths it opens. That part of the manifest takes effect from Windows 10 1903 on;
on older builds, repository paths have to stay within the system code page.

## Commands

```
backup     Create a backup of a disk/volume/virtual disk
restore    Restore a disk/volume/virtual disk from a backup
copy       Merge a chain of backups into a new backup
list       List available backups
info       Show information about a backup
verify     Check the integrity of a backup
remove     Delete a backup
retention  Change the immutability a stored backup is held under
meta_save  Store a metadata blob alongside a backup
meta_load  Read back the metadata blob of a backup
```

Options are per-command; `abr-vhdx-tool.exe <command> --help` is authoritative.
Storage is selected with `--mode file|s3` plus `--folder`, and `--path` for
a subfolder within it. 
Only `backup` and `restore` touch a disk; `copy`, `list`, `info`, `verify`,
`remove`, `retention` and the `meta_*` commands work on any Windows, elevated or not.

See [doc/abr-vhdx-tool.md](../doc/abr-vhdx-tool.md) for the full manual: every
command's options, the Hyper-V backup workflow, immutability and retention,
the `remove` confirmation prompt, and the `--json` result of each command.

One thing surprises callers who script this tool: `remove` asks for a `[y/N]`
confirmation on stdin unless `--force` is given. A subprocess with no stdin
reads an empty answer and aborts.

## JSON Output

Every command takes `--json`, which replaces the report it ends with by a single
JSON object, in the same shapes `abr-vmdk-tool` uses. It covers the result
only. A long `backup`, `restore`, `copy` or `verify` still reports progress
first, as plain `NN%` lines, one per line under `--json`:

```console
> abr-vhdx-tool.exe backup --json --folder D:\Backups --name web01-disk0 --vhdx C:\Hyper-V\web01\disk0.vhdx
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
onward. Every stream ends its lines with a bare LF, as on Linux, and stdin and
stdout are binary, so `meta_save` and `meta_load` carry a blob byte for byte.

A failure still produces one document, carrying the same message that went to
stderr:

```console
> abr-vhdx-tool.exe info --json --folder D:\Backups --name ghost
{
  "command": "info",
  "status": "error",
  "error": "failed to open backup 'ghost'"
}
```

## Virtual Disks and the Hyper-V API

A virtual disk is opened through the Windows virtual disk API (`virtdisk.dll`),
the same service Hyper-V uses for its own disks, and then *attached*: surfaced
as a disk device of the host, which is what the tool reads or writes. The
service does the format work -- VHD, VHDX, and every parent of a differencing
chain of checkpoint AVHDXs -- so the tool reads exactly the sectors the guest
would see and never parses an image itself. 
The attach is read-only for a backup, gets no drive letters, is closed to everyone
but SYSTEM and the Administrators group, and lives only as long as the tool's
handle to it, so a run that is killed outright does not leave a disk attached.

Incremental backups use **resilient change tracking** (RCT), Hyper-V's changed
block tracking: `QueryChangesVirtualDisk` lists what changed since the RCT id
given as `--change-id`. 
A backup of a disk that has tracking on reports the disk's current RCT id
(`rct_id` in the JSON), which is what the next incremental is taken against.

`virtdisk.dll` is bound at run time, from System32 only, rather than linked.
The repository-only commands therefore start on any Windows, and a system
without RCT (Windows 10 before 1607) still takes full backups and restores,
refusing only an incremental.

A running VM holds its current disk open for writing, so the disk to back up
is the one a **checkpoint** of the VM refers to: once the checkpoint exists,
Hyper-V writes to a new AVHDX and leaves that one alone.
Taking the checkpoint, reading its disk paths, and turning it into a reference
point afterwards is the backup server's part, done through Hyper-V's WMI
provider -- just as it takes vSphere snapshots for `abr-vmdk-tool`. 
The [manual](../doc/abr-vhdx-tool.md#the-hyper-v-backup-workflow) walks through it.
