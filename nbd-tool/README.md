# abr-nbd-tool

`abr-nbd-tool` backs up disks exported over NBD into the `.abr` backup format,
and restores backups to plain files and devices. Its main use is Proxmox VE: a
backup provider gets each disk of a VM as an NBD export on a Unix socket, with
a QEMU dirty bitmap for incrementals, and runs the tool once per disk. It has
the same commands, the same storage options and the same `--json` results as
the other tools built from this repository, and reads and writes the same
repositories.

It runs on Linux. The build is described in the
[top-level README](../README.md): the `linux` preset builds it where libnbd 1.18
or later is installed (`libnbd-dev`). The result is one binary, which needs
libnbd's shared library (`libnbd0`, installed on every Proxmox VE node) on the
machine it runs on. There is no launcher.

The command reference is [doc/abr-nbd-tool.md](../doc/abr-nbd-tool.md).

## Running It

```bash
abr-nbd-tool <command> [options]
```

A backup names the export with `--nbd` and `--export`:

```bash
abr-nbd-tool backup --json --folder /mnt/repo --path vm-101 --name vm-101-scsi0-full \
    --nbd /run/qemu-server/101.backup-access.1234//101-nbd.backup-access --export drive-scsi0
```

and an incremental adds the dirty bitmap the export carries:

```bash
abr-nbd-tool backup --json --folder /mnt/repo --path vm-101 --name vm-101-scsi0-inc1 \
    --nbd /run/qemu-server/101.backup-access.1234//101-nbd.backup-access --export drive-scsi0 \
    --type incremental --change-id snapshot-access:abr --discard
```

`--nbd` also takes an NBD URI, such as `nbd://host/disk` or
`nbd+unix:///drive-scsi0?socket=/run/disk.sock`, which names its export itself.

## Commands

```
backup     Create a backup of a disk/volume/NBD export
restore    Restore a disk/volume from a backup
copy       Merge a chain of backups into a new backup
empty      Write an incremental backup that changes nothing
list       List available backups
info       Show information about a backup
verify     Check the integrity of a backup
remove     Delete a backup
retention  Change the immutability a stored backup is held under
meta_save  Store a metadata blob alongside a backup
meta_load  Read back the metadata blob of a backup
```

Options are per command; `abr-nbd-tool <command> --help` is authoritative.
Only `backup` reads an NBD export. `restore` writes a plain file or device: a
Proxmox VE restore reads its chain through nbdkit instead. The other commands
work on the repository alone.

## What It Reads

- **Full backups** read what the export's `base:allocation` status does not
  report as reading zero. An export without allocation status is read whole.
- **Incrementals** read the blocks of the QEMU dirty bitmap named by
  `--change-id`, through the `qemu:dirty-bitmap:<name>` status. An export
  without that bitmap fails the backup rather than turning it into a full.
- Each connection negotiates only one of the two, because QEMU broke a
  connection that negotiated both while the bitmap was clean.
- `--discard` discards each range after it is read. On a Proxmox VE backup
  export this frees the fleecing image early; on an export of a disk itself it
  erases the data, so it is off unless asked for.
- `--bwlimit MIB` limits the read rate to MIB MiB/s.

## Testing

`ctest` runs [test/nbd/test-nbd-tool.sh](../test/nbd/test-nbd-tool.sh) where
`qemu-img`, `qemu-io` and `qemu-storage-daemon` are installed: full,
incremental and empty incremental backups of a `qemu-storage-daemon` export
with a dirty bitmap, an encrypted backup, the restores of the chains compared
with the disk, and the refusals.
