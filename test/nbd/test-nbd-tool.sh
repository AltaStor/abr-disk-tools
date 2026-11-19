#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# End-to-end test of abr-nbd-tool against a qemu-storage-daemon export with a dirty bitmap, the way Proxmox VE exports
# a disk to a backup provider. Each restore is compared with the export it was taken from.
#
#   test-nbd-tool.sh TOOL SCRATCH_DIR
#
# Checked: a full backup, an incremental from the bitmap, an incremental with no changes, an empty incremental from
# the 'empty' command, an encrypted full with the key on standard input, restores of the chains with --source-names,
# the JSON results, and the refusals of a missing bitmap, an incremental without --nbd and NBD options on a restore.
#
set -euo pipefail

tool=$1
dir=$2

rm -rf "$dir"
mkdir -p "$dir"
mkdir -m 750 "$dir/repo"
cd "$dir"

failures=0
fail() { echo "FAILED: $*"; failures=$((failures + 1)); }
step() { echo "== $*"; }

cleanup() { [ -s qsd.pid ] && kill "$(cat qsd.pid)" 2>/dev/null || true; }
trap cleanup EXIT

# A field of a --json result. Progress lines come first, so the document starts at the first "{".
json() {
    python3 -c "import json, sys; t = open(sys.argv[1]).read(); d = json.loads(t[t.index('{'):]); print(eval(sys.argv[2]))" "$@"
}

# Stops the daemon, so that the image is consistent and qemu-io can change it, and starts it again.
stop() {
    local pid
    pid=$(cat qsd.pid)
    kill "$pid"
    while kill -0 "$pid" 2>/dev/null; do sleep 0.1; done
    rm -f qsd.pid disk.sock
}
start() {
    qemu-storage-daemon --daemonize --pidfile qsd.pid \
        --blockdev driver=file,node-name=file,filename=disk.qcow2 \
        --blockdev driver=qcow2,node-name=disk,file=file \
        --nbd-server addr.type=unix,addr.path=disk.sock \
        --export type=nbd,id=export,node-name=disk,name=drive-scsi0,writable=on,bitmaps.0=b0
}

# The export as a raw image, for comparing a restore with.
snapshot() { qemu-img convert -f qcow2 -O raw disk.qcow2 "$1"; }

backup() { "$tool" backup --json --folder repo --create-folder --nbd "$PWD/disk.sock" --export drive-scsi0 "$@"; }
restore() { "$tool" restore --json --folder repo "$@"; }

# A 256 MiB disk with data, a bitmap that starts empty, and a raw image of it as the full backup will see it.
qemu-img create -q -f qcow2 disk.qcow2 256M
qemu-io -f qcow2 -c 'write -q -P 0x11 0 4M' -c 'write -q -P 0x22 64M 1M' -c 'write -q -P 0x33 255M 1M' disk.qcow2
qemu-img bitmap --add --granularity 64k disk.qcow2 b0
snapshot full.raw
start

step "full backup"
if backup --name full -c 3 > full.json; then
    [ "$(json full.json 'd["kind"]')" = full ] || fail "full kind: $(json full.json 'd["kind"]')"
    [ "$(json full.json 'd["block_status"]')" = base:allocation ] || fail "full block status"
    [ "$(json full.json 'd["export"]')" = drive-scsi0 ] || fail "full export"
    [ "$(json full.json 'd["raw_data_size"]')" = $((6 * 1024 * 1024)) ] \
        || fail "full read $(json full.json 'd["raw_data_size"]') bytes, expected the 6 MiB written"
else
    fail "full backup: $(cat full.json)"
fi

# Changes after the full, recorded by the bitmap.
stop
qemu-io -f qcow2 -c 'write -q -P 0x44 1M 128k' -c 'write -q -P 0x55 128M 64k' disk.qcow2
snapshot inc1.raw
start

step "incremental backup"
if backup --name inc1 --type incremental --change-id b0 > inc1.json; then
    [ "$(json inc1.json 'd["kind"]')" = incremental ] || fail "incremental kind"
    [ "$(json inc1.json 'd["block_status"]')" = qemu:dirty-bitmap:b0 ] || fail "incremental block status"
    [ "$(json inc1.json 'd["raw_data_size"]')" = $((192 * 1024)) ] \
        || fail "incremental read $(json inc1.json 'd["raw_data_size"]') bytes, expected the 192 KiB changed"
else
    fail "incremental backup: $(cat inc1.json)"
fi

# The bitmap is cleared, as PVE does after a successful run, so the next incremental has no changes.
stop
qemu-img bitmap --clear disk.qcow2 b0
start

step "incremental backup with no changes"
if backup --name inc2 --type incremental --change-id b0 > inc2.json; then
    [ "$(json inc2.json 'd["raw_data_size"]')" = 0 ] || fail "empty incremental read data"
else
    fail "empty incremental: $(cat inc2.json)"
fi

# What abr-core writes for a stopped VM that has not run since its last backup, instead of reading it again.
step "empty incremental on top of the chain"
if "$tool" empty --json --folder repo --name inc3 --parent inc2 > inc3.json; then
    [ "$(json inc3.json 'd["kind"]')" = incremental ] || fail "empty kind"
    [ "$(json inc3.json 'd["raw_data_size"]')" = 0 ] || fail "empty incremental holds data"
    [ "$(json inc3.json 'd["sectors"]')" = "$(json inc2.json 'd["sectors"]')" ] || fail "empty incremental geometry"
else
    fail "empty incremental: $(cat inc3.json)"
fi

step "restores"
restore --name full --target r-full.raw > r-full.json || fail "restore of the full: $(cat r-full.json)"
cmp -s r-full.raw full.raw || fail "the restored full differs from the disk at the full"
restore --source-names full,inc1,inc2,inc3 --target r-chain.raw > r-chain.json \
    || fail "chain restore: $(cat r-chain.json)"
cmp -s r-chain.raw inc1.raw || fail "the restored chain differs from the disk"

step "encrypted full with the key on standard input"
if printf 'key=test-key-0123456789\n\n' | backup --name enc --stdin-keys > enc.json; then
    [ "$(json enc.json 'd["encryption"]')" = AES-256-GCM ] || fail "encrypted backup: $(json enc.json 'd["encryption"]')"
    printf 'key=test-key-0123456789\n\n' | restore --name enc --target r-enc.raw --stdin-keys > r-enc.json \
        || fail "encrypted restore: $(cat r-enc.json)"
    cmp -s r-enc.raw inc1.raw || fail "the restored encrypted full differs from the disk"
else
    fail "encrypted backup: $(cat enc.json)"
fi

step "refusals"
if backup --name bad1 --type incremental --change-id nosuch > bad1.json 2> bad1.err; then
    fail "an incremental of a missing bitmap succeeded"
fi
grep -q "offers no dirty bitmap 'nosuch'" bad1.err || fail "missing bitmap message: $(cat bad1.err)"
"$tool" list --json --folder repo > list.json
grep -q '"bad1"' list.json && fail "a refused backup left something in the repository"
if "$tool" backup --folder repo --name bad2 --source full.raw --type incremental --change-id b0 2> bad2.err; then
    fail "an incremental of a plain file succeeded"
fi
grep -q "requires --nbd" bad2.err || fail "incremental without --nbd message: $(cat bad2.err)"
if restore --name full --target bad3.raw --nbd "$PWD/disk.sock" > /dev/null 2> bad3.err; then
    fail "a restore with --nbd succeeded"
fi
grep -q "are for backups" bad3.err || fail "restore with --nbd message: $(cat bad3.err)"
if "$tool" backup --folder repo --name bad4 --source full.raw --discard 2> bad4.err; then
    fail "--discard without --nbd succeeded"
fi
grep -q "go with --nbd" bad4.err || fail "--discard without --nbd message: $(cat bad4.err)"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
cd /
rm -rf "$dir"
echo "All abr-nbd-tool checks passed"
