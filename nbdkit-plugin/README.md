# nbdkit-abr-plugin

`nbdkit-abr-plugin` is an [nbdkit](https://libguestfs.org/nbdkit.1.html) plugin
that serves a chain of `.abr` backups read-only over NBD, as the disk the chain
restores to. Its main use is Proxmox VE: a restore runs one nbdkit process per
disk with the plugin, attaches it as a kernel NBD device, and PVE's `qemu-img`
reads the disk from there onto any PVE storage. It reads the same repositories
as the tools, on a file share or on S3, and a block comes from the newest
backup that holds it, as in `restore --source-names`.

It runs on Linux. The build is described in the
[top-level README](../README.md): the `linux` preset builds it where nbdkit 1.30
or later is installed (`nbdkit-plugin-dev`), as
`build-linux/lib/nbdkit-abr-plugin.so`. backuplib and the AWS SDK are linked
into it, so on the machine it runs on it needs only nbdkit and the shared
libraries every Proxmox VE node has: libstdc++, OpenSSL (`libssl3`), zlib and
zstd. It exports nothing but nbdkit's entry point, so what it carries cannot
clash with anything else in the process.

The reference is [doc/nbdkit-abr-plugin.md](../doc/nbdkit-abr-plugin.md).

## Running It

```bash
nbdkit -r -U /run/abr/restore.sock nbdkit-abr-plugin.so \
    folder=/mnt/repo path=vm-101 chain=vm-101-scsi0-full,vm-101-scsi0-inc1 key=-3 3<key-pipe
```

serves the disk `vm-101-scsi0-inc1` restores to, and

```bash
nbdkit -r -U - nbdkit-abr-plugin.so folder=/mnt/repo path=vm-101 chain=vm-101-scsi0-full \
    --run 'qemu-img convert -f raw -O raw "$uri" disk.raw'
```

reads a full backup into a raw image. `nbdkit nbdkit-abr-plugin.so --help`
lists the parameters, and `--version` prints the plugin's version next to
nbdkit's, which is the version of the tools built with it.

## Testing

`ctest` runs [test/nbdkit/test-nbdkit-plugin.sh](../test/nbdkit/test-nbdkit-plugin.sh)
where `abr-nbd-tool` is built and `nbdkit`, `nbdinfo`, `qemu-img`, `qemu-io` and
`qemu-storage-daemon` are installed: a full and two incrementals of a
`qemu-storage-daemon` export, each point of the chain served by nbdkit and read
back with `qemu-img`, the extents and block size `nbdinfo` reports, an encrypted
chain with its key from a descriptor and from a file, a read-only repository,
nbdkit in the background, the exported symbols, and the refusals.
