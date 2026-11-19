#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# End-to-end test of nbdkit-abr-plugin without Proxmox VE. abr-nbd-tool backs up a qemu-storage-daemon export as a full
# and two incrementals, nbdkit serves the chain with the plugin, and qemu-img reads it back as a PVE restore does. Each
# image read back is compared with the disk at the point the chain ends.
#
#   test-nbdkit-plugin.sh TOOL PLUGIN SCRATCH_DIR
#
# Checked: every point of the chain, a block zeroed after the full, the extents and block size nbdinfo reports, an
# encrypted chain with its key from a file descriptor and from a file, a read-only repository, nbdkit in the background
# with a pidfile and a relative folder, the version and the exported symbols, and the refusals: a chain that does not
# open stops nbdkit before its pidfile, a wrong or missing key, a chain that starts with an incremental or holds a full
# later on, and parameters that are missing, unknown or wrong.
#
set -euo pipefail

tool=$(realpath "$1")
plugin=$(realpath "$2")
dir=$3

rm -rf "$dir"
mkdir -p "$dir"
mkdir -m 750 "$dir/repo"
cd "$dir"

failures=0
fail() { echo "FAILED: $*"; failures=$((failures + 1)); }
step() { echo "== $*"; }

cleanup() {
    local pidfile
    for pidfile in qsd.pid nbdkit.pid; do
        [ -s "$pidfile" ] && kill "$(cat "$pidfile")" 2>/dev/null || true
    done
    chmod -R u+w repo 2>/dev/null || true
}
trap cleanup EXIT

# The export the backups are taken from, as in the abr-nbd-tool test: stopped, so that qemu-io can change the image.
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

# The disk as a raw image, for comparing what the plugin serves with.
snapshot() { qemu-img convert -f qcow2 -O raw disk.qcow2 "$1"; }

backup() { "$tool" backup --json --folder repo --nbd "$PWD/disk.sock" --export drive-scsi0 "$@"; }

# Serves the parameters with the plugin, read-only as the provider does, and runs a command against the export: nbdkit
# sets $uri for it.
serve() {
    local command=$1
    shift
    nbdkit -r -U - --run "$command" "$plugin" "$@"
}

# Reads the export into a raw image the way PVE's qemu-img reads it in a restore.
read_back() {
    local image=$1
    shift
    serve "qemu-img convert -q -f raw -O raw \"\$uri\" $image" "$@"
}

# The plugin refuses the parameters, and nbdkit reports why.
refused() {
    local what=$1 message=$2
    shift 2
    if serve true "$@" > /dev/null 2> refused.err; then
        fail "$what was served"
    elif ! grep -qF -- "$message" refused.err; then
        fail "$what: expected \"$message\", got: $(cat refused.err)"
    fi
}

key=test-key-0123456789

step "a full and two incrementals of a qemu-storage-daemon export"
# A 256 MiB disk with 6 MiB of data, and a bitmap that starts empty.
qemu-img create -q -f qcow2 disk.qcow2 256M
qemu-io -f qcow2 -c 'write -q -P 0x11 0 4M' -c 'write -q -P 0x22 64M 1M' -c 'write -q -P 0x33 255M 1M' disk.qcow2
qemu-img bitmap --add --granularity 64k disk.qcow2 b0
snapshot full.raw
start
backup --name full > full.json || fail "full backup: $(cat full.json)"

# A block inside the full's data, and one where the full has none.
stop
qemu-io -f qcow2 -c 'write -q -P 0x44 1M 128k' -c 'write -q -P 0x55 128M 64k' disk.qcow2
snapshot inc1.raw
start
backup --name inc1 --type incremental --change-id b0 > inc1.json || fail "first incremental: $(cat inc1.json)"

# The bitmap is cleared, as PVE does after a successful run. Then half a MiB of the full's data is zeroed, which the
# incremental has to store as zeros, and a new block is written.
stop
qemu-img bitmap --clear disk.qcow2 b0
qemu-io -f qcow2 -c 'write -q -z 64M 512k' -c 'write -q -P 0x66 200M 64k' disk.qcow2
snapshot inc2.raw
start
backup --name inc2 --type incremental --change-id b0 > inc2.json || fail "second incremental: $(cat inc2.json)"
grep -q '"raw_data_size": 589824,' inc2.json || fail "the second incremental did not store the zeroed block"

# An encrypted full of the same disk, and an incremental with no changes on top of it.
printf 'key=%s\n\n' "$key" | backup --name efull --stdin-keys > efull.json || fail "encrypted full: $(cat efull.json)"
printf 'key=%s\n\n' "$key" | "$tool" empty --json --folder repo --name einc --parent efull --stdin-keys > einc.json \
    || fail "encrypted empty incremental: $(cat einc.json)"
stop

step "each point of the chain read back through qemu-img"
read_back r-full.raw folder=repo chain=full || fail "reading the full failed"
cmp -s r-full.raw full.raw || fail "the full differs from the disk at the full"
read_back r-inc1.raw folder=repo chain=full,inc1 || fail "reading full,inc1 failed"
cmp -s r-inc1.raw inc1.raw || fail "full,inc1 differs from the disk at the first incremental"
read_back r-inc2.raw folder=repo chain=full,inc1,inc2 || fail "reading full,inc1,inc2 failed"
cmp -s r-inc2.raw inc2.raw || fail "full,inc1,inc2 differs from the disk at the second incremental"

step "extents and block size"
# Data where any backup holds a block, the zeroed block included, and holes that read as zeros elsewhere.
serve 'nbdinfo --map "$uri"' folder=repo chain=full,inc1,inc2 | awk '{print $1, $2, $3}' > map.got
cat > map.want <<'EOF'
0 4194304 0
4194304 62914560 3
67108864 1048576 0
68157440 66060288 3
134217728 65536 0
134283264 75431936 3
209715200 65536 0
209780736 57606144 3
267386880 1048576 0
EOF
diff -u map.want map.got || fail "the extents differ from what the chain holds"
# The full stores 4 MiB blocks, and a read of that size decodes each once.
serve 'nbdinfo --json "$uri"' folder=repo chain=full,inc1,inc2 > info.json
python3 -c 'import json, sys; e = json.load(open(sys.argv[1]))["exports"][0]
sys.exit(not (e["block_size_minimum"] == 1 and e["block_size_preferred"] == 4194304 and e["is_read_only"]))' info.json \
    || fail "block sizes or read-only flag: $(cat info.json)"

step "encrypted chain, the key from a file descriptor and from a file"
read_back r-enc-fd.raw folder=repo chain=efull,einc key=-3 3< <(printf %s "$key") || fail "key from a descriptor"
cmp -s r-enc-fd.raw inc2.raw || fail "the encrypted chain read with the key from a descriptor differs from the disk"
printf %s "$key" > key.txt
read_back r-enc-file.raw folder=repo chain=efull,einc key=+key.txt || fail "key from a file"
cmp -s r-enc-file.raw inc2.raw || fail "the encrypted chain read with the key from a file differs from the disk"

step "read-only repository"
chmod -R a-w repo
read_back r-ro.raw folder=repo chain=full,inc1,inc2 || fail "reading a read-only repository failed"
cmp -s r-ro.raw inc2.raw || fail "the chain read from a read-only repository differs from the disk"
chmod -R u+w repo

step "nbdkit in the background, with a pidfile and a relative folder"
# nbdkit changes to / when it forks, after the plugin has taken the folder as given here.
nbdkit -r -U "$PWD/bg.sock" -P "$PWD/nbdkit.pid" "$plugin" folder=repo chain=full,inc1,inc2
for _ in $(seq 100); do [ -s nbdkit.pid ] && break; sleep 0.1; done
if [ -s nbdkit.pid ]; then
    qemu-img convert -q -f raw -O raw "nbd+unix:///?socket=$PWD/bg.sock" r-bg.raw \
        || fail "reading the background export"
    cmp -s r-bg.raw inc2.raw || fail "the chain served in the background differs from the disk"
    pid=$(cat nbdkit.pid)
    kill "$pid"
    while kill -0 "$pid" 2>/dev/null; do sleep 0.1; done
    rm -f nbdkit.pid
else
    fail "nbdkit in the background wrote no pidfile"
fi

step "a chain that does not open stops nbdkit before its pidfile"
if nbdkit -f -r -U "$PWD/bad.sock" -P "$PWD/bad.pid" "$plugin" folder=repo chain=full,nosuch 2> nosuch.err; then
    fail "nbdkit served a chain with a missing backup"
fi
[ -e bad.pid ] && fail "nbdkit wrote its pidfile although the chain did not open"
grep -qF "failed to open backup 'nosuch'" nosuch.err || fail "missing backup message: $(cat nosuch.err)"

step "refusals"
refused "a wrong key" "the key does not decrypt backup 'efull'" folder=repo chain=efull key=wrong-key
refused "an encrypted chain without its key" "backup 'efull' is encrypted" folder=repo chain=efull
refused "a chain that starts with an incremental" "'inc1' is an incremental backup; a chain starts with a full" \
    folder=repo chain=inc1,inc2
refused "a full later in the chain" "'full' is a full backup" folder=repo chain=inc1,full
refused "no folder" "folder is required" chain=full
refused "no chain" "chain is required" folder=repo
refused "an empty backup name" "chain has an empty backup name" folder=repo chain=full,,inc1
refused "an unknown parameter" "unknown parameter 'name'" folder=repo chain=full name=full
refused "a wrong mode" "mode must be 'file' or 's3'" folder=repo chain=full mode=nfs
refused "an S3 parameter in file mode" "endpoint goes with mode=s3" folder=repo chain=full endpoint=http://s3.local
refused "a bad boolean" "verify-tls takes a boolean" folder=repo chain=full mode=s3 verify-tls=maybe

step "version and exported symbols"
# The provider checks that the plugin and the tool are of the same build.
plugin_version=$(nbdkit "$plugin" --version | awk '$1 == "abr" {print $2}')
tool_version=$("$tool" --version | awk '{print $NF}')
[ -n "$plugin_version" ] && [ "$plugin_version" = "$tool_version" ] \
    || fail "the plugin reports version '$plugin_version', the tool '$tool_version'"
exported=$(nm -D --defined-only "$plugin" | awk '{print $3}' | tr '\n' ' ')
[ "$exported" = "plugin_init " ] || fail "the plugin exports more than plugin_init: $exported"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
cd /
rm -rf "$dir"
echo "All nbdkit-abr-plugin checks passed"
