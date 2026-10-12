# abr-vmdk-tool

`abr-vmdk-tool` backs up and restores plain files and VMware virtual disks --
a local VMDK, or a VM's disk on an ESXi host or a vCenter -- into the `.abr`
backup format, through the VMware VDDK. It is the data mover the backup server
invokes once per disk, with the same commands, the same options apart from the
hypervisor-specific ones, and the same `--json` results as the other tools
built from this repository; they read and write the same repositories.

It runs on Linux. The build is described in the
[top-level README](../README.md); the result is a binary and the launcher
that points it at a VDDK installation, `abr-vmdk-tool.sh`.

The command reference is [doc/abr-vmdk-tool.md](../doc/abr-vmdk-tool.md).

## Running It

Run the launcher, not the binary — the binary on its own has no way to find the VDDK:

```bash
abr-vmdk-tool.sh <command> [options]
```

This works the same from an installed prefix or straight out of the build
tree (`build-linux/bin/abr-vmdk-tool.sh`), since the build copies the launcher
beside the binary too.

It finds the bundle at `../vddk` relative to itself — `<prefix>/vddk` in an
installed tree — or at `../../vddk`, which is `vddk/` at the repository root
for a build directory inside a checkout, and exports
an `LD_LIBRARY_PATH` that leads with its `lib64`. There is no option for this:
the search path has to be set before the process starts, so the binary has no
say in it. Three variables override the search, and each is authoritative: if
one is set but does not resolve, the launcher reports it and exits rather than
quietly running something else.

| Variable | Overrides |
| --- | --- |
| `ABR_VMDK_TOOL_BIN` | path to the binary |
| `ABR_VMDK_TOOL_VDDK_DIR` | path to the VDDK root |
| `ABR_VMDK_TOOL_TMPDIR` | scratch directory for the VDDK transport plugins |

## Commands

```
backup     Create a backup of a disk/volume/VMDK
restore    Restore a disk/volume/VMDK from a backup
copy       Merge a chain of backups into a new backup
list       List available backups
info       Show information about a backup
verify     Check the integrity of a backup
remove     Delete a backup
retention  Change the immutability a stored backup is held under
meta_save  Store a metadata blob alongside a backup
meta_load  Read back the metadata blob of a backup
```

Options are per-command; `abr-vmdk-tool.sh <command> --help` is authoritative.
Storage is selected with `--mode file|s3` plus `--folder`, and `--path` for a
subfolder within it. Only `backup`, `restore` and the VMDK-facing paths touch
the VDDK; `copy`, `list`, `info`, `verify`, `remove` and the `meta_*` commands
work without a bundle present.

`copy` reads a full backup and the incrementals taken on top of it and writes
what they add up to as one new full backup, in the same repository or in
another one — which is also how a backup is recompressed, re-keyed, or moved
between a folder and a bucket. The storage it reads from is described by
`--source-<...>` options that each fall back to the destination's.

See [doc/abr-vmdk-tool.md](../doc/abr-vmdk-tool.md) for the full manual: every
command's options, sample invocations, immutability/retention handling, the
`remove` confirmation prompt, and the `--json` result shape for each command.

One thing surprises callers who script this tool: `remove` asks for a `[y/N]`
confirmation on stdin unless `--force` is given. A subprocess with no stdin
reads an empty answer and aborts.

## JSON Output

Every command takes `--json`, which replaces the report it ends with by a single
JSON object. It covers the result only. A long `backup`, `restore` or `verify`
still reports progress first, and those percentages stay plain text — the same
`NN%` the interactive form shows, but one per line and flushed as it happens,
rather than rewritten in place with a carriage return:

```console
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

So a caller reads stdout a line at a time: every line up to and including `100%`
is progress, and the JSON document is everything from the first `{` onward.

Two other things stay off stdout so as not to interleave with that: `remove`
asks for its confirmation on stderr, and `meta_load` hands the blob back as a
`metadata` string field instead of writing it out raw. Human-readable
diagnostics keep going to stderr, and exit codes are unchanged.

```console
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
  "encryption": "AES-256-GCM"
}
```

`command` and `status` are always present. `status` is `ok`, `error`, or
`aborted` — the last only from a `remove` the operator declined. A failure still
produces one document, carrying the same message that went to stderr:

```console
$ abr-vmdk-tool.sh info --json --folder /srv/backups --name ghost
{
  "command": "info",
  "status": "error",
  "error": "failed to open backup 'ghost'"
}
```

`verify` is the one command that reports a failed check as data rather than as a
malfunction: a corrupt backup still yields the full document, with `intact`
false and a `corrupt_ranges` count, alongside the non-zero exit status.

`--help` is exempt and stays plain text.

`meta_load` embeds the stored blob verbatim as a JSON string, so the result is
valid JSON exactly when the blob is valid UTF-8. That holds for the manifests
the backup server stores, which are themselves JSON.

## VDDK and the Library Search Path

The VDDK bundle ships its own `libstdc++`, `libcrypto` and `libcurl`, all older
than what this binary is compiled against, and it `dlopen`s transport plugins
that need a dozen libraries found nowhere but the bundle. Those plugins carry no
RPATH, so `LD_LIBRARY_PATH` is the only search path that reaches them — and
glibc reads that variable once, at process start, which is why the launcher and
not the tool is what sets it. The launcher exports, in order:

- the bundle's `lib64`, when a bundle was found. It leads, so that the plugins
  resolve.
- a vendored `lib/` directory beside the binary, if the package build put one
  there.
- `/usr/local/lib`, where `libcrc32c.so.1` is installed.

The tool then `dlopen`s `libvixDiskLib.so` by bare name and lets the loader pick
— it never takes a path of its own. Whichever installation that turns out to be
is read back with `dlinfo(RTLD_DI_ORIGIN)` and handed to `VixDiskLib_Init`, so
the plugins are looked for in the same bundle the library itself came from.

The bundle leading the search path would normally be a problem, since its older
`libstdc++`/`libcrypto` would be found ahead of the system copies the binary was
compiled against. It is not, because the binary carries a `DT_RPATH` naming the
compiler's runtime directories, and RPATH is searched before `LD_LIBRARY_PATH`
for everything the process loads — the `dlopen`ed plugins included, which is
why the link uses `-Wl,--disable-new-dtags` to get `DT_RPATH` rather than the
default `DT_RUNPATH`.

Those runtime directories are taken from CMake's implicit link directories, but
only the entries under a system prefix are kept. CMake derives that list by test
linking with the configured flags, so a `-L` pointing at a package build's
staging tree would otherwise be embedded in the shipped binary — a path that
does not exist on the target machine, is writable there by anyone who cares to
create it, and is searched ahead of everything else.
