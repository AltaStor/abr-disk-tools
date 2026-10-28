# abr-vhdx-tool — Testing on Hyper-V

The scripts in [test/hyperv/](../test/hyperv) run `abr-vhdx-tool.exe` against a real Hyper-V
host and a real virtual machine. They do what the backup server does around
the tool — take a recovery checkpoint, find its disks, turn it into a
reference point — and check every restored disk, by SHA-256, against what
Hyper-V itself reads from the checkpoint. This is the part of the tool that
cannot be tested anywhere else: Wine has no virtual disk service, and
backuplib's own tests never touch a disk.

Use a host and a VM set aside for testing. The scripts do not write to the
VM's disks, but they do take checkpoints of it; see
[What the test does to the VM](#what-the-test-does-to-the-vm).

## Contents

- [What is in test/hyperv/](#what-is-in-testhyperv)
- [Requirements](#requirements)
- [Running on the host](#running-on-the-host)
- [Running from another machine](#running-from-another-machine)
- [Testing on a cluster](#testing-on-a-cluster)
- [Testing a repository on an SMB share](#testing-a-repository-on-an-smb-share)
- [What is tested](#what-is-tested)
- [Reading the output](#reading-the-output)
- [What the test does to the VM](#what-the-test-does-to-the-vm)
- [Troubleshooting](#troubleshooting)

## What is in test/hyperv/

| File | Runs on | Purpose |
| --- | --- | --- |
| `Test-Vm.ps1` | the Hyper-V host | The test: one disk of one VM, through backup, restore, copy and the refusals. |
| `Test-SmbShare.ps1` | the Hyper-V host | Every disk of one VM backed up to a repository on an SMB share, verified there and restored from it. |
| `Test-OtherNode.ps1` | a second cluster node | Reads back, from another node, what `Test-Vm.ps1` left on a cluster shared volume. |
| `hvlib.ps1` | — | Shared by the three: the WMI calls, the content hash, the pass/fail bookkeeping. |
| `hv.py` | Linux, macOS or Windows | Copies the tool and the scripts to a host and runs them there over WinRM. Optional. |
| `requirements.txt` | — | What `hv.py` needs (`pypsrp`). |

## Requirements

On the Hyper-V host:

- Windows Server 2016 or later (or Windows 10 1607 or later) with the Hyper-V
  role and its PowerShell module.
- An account in the local Administrators group. `--vhdx` needs an elevated
  token.
- A test VM of configuration version 8.0 or later, with at least one VHDX
  disk. It may be running or off; running is the better test. Earlier
  versions have no resilient change tracking, so the incremental part fails.
- Free space for about five times the data on the tested disk, in the work
  directory (by default `work` next to the scripts).

The run reads the whole virtual size of the disk about ten times, to hash it,
so its duration follows the disk's size and not how full it is. A 10 GB disk
takes on the order of a quarter of an hour; pick a small one.

## Running on the host

Copy `abr-vhdx-tool.exe` and the three `.ps1` files into one directory on the
host, open an **elevated** PowerShell there, and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Test-Vm.ps1 -VmName test-vm-01
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `-VmName` | required | The VM to test against. |
| `-WorkDir` | `work` next to the script | Where the repository and the restored disks go. Must not exist yet; removed at the end. |
| `-Tool` | `abr-vhdx-tool.exe` next to the script | The executable to test. |
| `-DiskIndex` | `0` | Which of the VM's disks, in the order the checkpoint lists them. |
| `-Keep` | off | Leave the work directory in place. |
| `-KeepReferencePoints` | off | Leave the two reference points the test makes on the VM. |

The exit code is 0 when everything passed and 1 otherwise.

## Running from another machine

`hv.py` does the same over PowerShell remoting, so that a build made on Linux
can be tested without logging on to the host. It needs Python 3 and one
package:

```bash
python3 -m venv ~/.venvs/hv && . ~/.venvs/hv/bin/activate
pip install -r test/hyperv/requirements.txt
```

The host needs WinRM enabled, which it is by default on Windows Server (and
`Enable-PSRemoting` turns on elsewhere), and reachable on port 5985.

The account is taken from the environment, so that it stays out of the shell
history and out of the process list:

| Variable | Meaning |
| --- | --- |
| `HV_USER` | `DOMAIN\user`, an administrator of the host. Required. |
| `HV_PASSWORD` | Its password. When not set, `hv.py` asks for it. |
| `HV_AUTH` | `ntlm` (default), `kerberos`, `negotiate`, `credssp` or `basic`. |
| `HV_SSL` | `1` to use WinRM over HTTPS (port 5986). |
| `HV_DIR` | Directory on the host. Default `C:\abr-test`. |

Over plain HTTP, NTLM and Kerberos still encrypt the session; `basic` does
not, and should only be used together with `HV_SSL=1`.

Then deploy and run:

```bash
export HV_USER='LAB\Administrator'

# Copies build-windows-cross/bin/abr-vhdx-tool.exe and test/hyperv/*.ps1 to C:\abr-test.
test/hyperv/hv.py 192.0.2.10 deploy

test/hyperv/hv.py 192.0.2.10 run Test-Vm.ps1 -VmName test-vm-01
```

A session opened this way for an administrator is already elevated.

An unstripped cross build is large (tens of megabytes of debug information)
and slow to copy over WinRM. Strip a copy and deploy that:

```bash
x86_64-w64-mingw32ucrt-strip -o /tmp/abr-vhdx-tool.exe build-windows-cross/bin/abr-vhdx-tool.exe
test/hyperv/hv.py 192.0.2.10 deploy --exe /tmp/abr-vhdx-tool.exe
```

`hv.py` prints the script's output when the script ends, not while it runs,
and exits 1 if the script failed. The other verbs are for looking around:

```bash
test/hyperv/hv.py HOST ps 'Get-VM | ft Name,State'      # run PowerShell; "-" reads it from stdin
test/hyperv/hv.py HOST put local.file 'C:\abr-test\remote.file'
test/hyperv/hv.py HOST get 'C:\abr-test\remote.file' local.file
```

A script's own secrets go with `run --secret PARAMETER=VARIABLE`, given
before the script's name. The value of the local variable `VARIABLE` becomes
the script's `-PARAMETER` as a SecureString. When the variable is not set,
`hv.py` asks for the value. The value travels as a PSRP secure string,
encrypted with a key of the session on top of WinRM's own encryption, and is
never part of the script text the host runs. Script text is what PowerShell
logs (event 4104), and Windows PowerShell 5.1 logs any script it finds
suspicious even when no logging policy is set.

## Testing on a cluster

A clustered VM is tested like any other, with two things to mind:

- Run `Test-Vm.ps1` on the node that currently **owns the VM**. Hyper-V's WMI
  provider only shows a VM on its owner, and the script says so when it does
  not find it. `Get-ClusterGroup` shows the owner.
- Connect to the nodes by their own addresses, not the cluster's.

To also test across nodes, put the work directory on the cluster shared volume
and keep it, then read it back from a node that does not own the volume
(`Get-ClusterSharedVolume` shows the owner):

```bash
# On the node that owns the VM:
test/hyperv/hv.py NODE-A run Test-Vm.ps1 -VmName clustered-vm-01 \
    -WorkDir 'C:\ClusterStorage\Volume1\abr-test' -Keep

# On another node:
test/hyperv/hv.py NODE-B run Test-OtherNode.ps1 -WorkDir 'C:\ClusterStorage\Volume1\abr-test'
```

`Test-OtherNode.ps1` verifies and restores, on the shared volume, the backup
the first node wrote; backs up a VHDX the first node restored there; and
compares both results with the hash the first node recorded. It prints
whether this node's I/O to the volume is direct or redirected, and removes the
work directory at the end unless given `-Keep`.

## Testing a repository on an SMB share

`Test-Vm.ps1` keeps its repository on a local disk. `Test-SmbShare.ps1` puts
it on a share (`--folder \\server\share\...`). backuplib then creates the
repository folder by folder over a UNC path, and writes, renames and reads
back its files through the SMB redirector:

```bash
test/hyperv/hv.py 192.0.2.10 run --secret SharePassword=SMB_PASSWORD --secret BackupKey=BACKUP_KEY \
    Test-SmbShare.ps1 -VmName test-vm-01 -Share '\\192.0.2.20\share' -ShareUser test
```

It makes a recovery checkpoint and backs up **every** disk of the VM to the
share. It turns the checkpoint into a reference point, then takes a second
checkpoint and backs up an incremental of each disk against the first one.
Then it checks that the repository is on the share and that `list` shows
every backup, and runs `verify` on each backup over the network. Last, it
restores each disk's chain from the share to a local VHDX and compares it,
by SHA-256, with the checkpoint's disk after the full backup and again after
the incremental. With `-BackupKey`, the backups are encrypted, and a restore
without the key must be refused. That proves the key really reached the
tool.

Every call to the tool goes through `--stdin-keys`, with a key or
without, since that is the path a backup server takes.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `-VmName` | required | The VM to back up. |
| `-Share` | required | `\\server\share`. The repository goes into a new `abr-test-<host>-<time>` directory on it. |
| `-ShareUser` | none | Account for the share. The script connects to the share and disconnects at the end. Without an account, the session must already reach the share. Over WinRM it usually cannot, because the logon credentials do not travel a second hop. |
| `-SharePassword` | none | Its password, as a SecureString. Required with `-ShareUser`. |
| `-BackupKey` | none | Encryption key for the backups, as a SecureString. |
| `-WorkDir` | `work-smb` next to the script | Where the restored disks go, one at a time. Must not exist yet. |
| `-Tool` | `abr-vhdx-tool.exe` next to the script | The executable to test. |
| `-Keep` | off | Leave the repository on the share and the work directory in place. |
| `-KeepReferencePoints` | off | Leave the two reference points the test makes on the VM. |

Neither secret is ever in plain text where something could record it.
`hv.py run --secret` delivers both as SecureStrings, and run on the host they
come from `Read-Host -AsSecureString`. The script hands the password to
`WNetAddConnection2`, the call `net use` makes, from unmanaged memory that is
zeroed afterwards, so no command line carries it. The backup key reaches the
tool on its standard input, behind `--stdin-keys`; see `abr-vhdx-tool`'s
manual. Neither appears in the script text that PowerShell may log, nor in any
environment variable.

A share of a standalone Samba server takes the bare user name. If Windows
already holds a connection to the same server under another account, the
connection fails with error 1219, "multiple connections to a server". Disconnect that connection first, or address the
server by another name or address.

## What is tested

`Test-Vm.ps1`, in order:

1. **Full backup** of the disk of a new recovery checkpoint, while the VM
   keeps running. The `rct_id` the tool reports must be one of the ids of the
   reference point the checkpoint is then converted to.
2. **Incremental backup** of a second checkpoint's disk, against that id. An
   incremental against an id the disk does not know is refused and leaves no
   backup behind.
3. **Verify** of both, then **restore**: the full to a new VHDX, the
   incremental on top of it — refused without `--force`, applied with it. The
   VHDX must hash the same as the checkpoint's disk at each step.
4. **Copy** of the chain into one backup, verified and restored.
5. Restore to a **VHD** and to a **raw file**.
6. An **encrypted, uncompressed** backup, its key on standard input
   (`--stdin-keys`) like every key in the test; restore of it to a path
   with non-ASCII characters. The key ends in a non-ASCII character, and a
   `verify` given the same key with `-k` on the command line must accept it.
   Both ways must deliver the same bytes. A wrong or missing key is refused by
   `verify`, `restore` and `copy` before any target is created. So are a key
   given both ways at once and an unknown name on standard input.
7. **Refusals**: the running VM's current disk as a backup source and as a
   restore target (skipped when the VM is off), an incremental of a disk
   without change tracking, an existing target of another size, `--vhdx` with
   a name that is no virtual disk, a missing source. Each must exit 1 with a
   message behind `Error: `.
8. **No disk left attached** to the host afterwards.

Not covered: S3 storage, disk and volume devices as source or target,
`remove`, `retention` and the `meta_*` commands. None of these depends on
Hyper-V; they are the same code as in `abr-vmdk-tool` and are exercised by
backuplib's tests.

## Reading the output

```
abr-vhdx-tool 1.0.0 on HV-01, VM 'test-vm-01' (running)
checkpoint disk: D:\Hyper-V\test-vm-01\test-vm-01_6A0C….avhdx
PASS  full backup of a checkpoint disk
PASS  full backup reports the RCT id
…
RESULT: 40 passed, 0 failed
```

A failed check prints `FAIL`, followed by the tool's exit code and output, or
the two hashes that differ. The run continues past a failed check, since the
later ones often narrow the cause down; it stops at a PowerShell error, such
as a checkpoint that could not be taken. Either way the cleanup below still
runs.

## What the test does to the VM

- It takes two **recovery checkpoints** and converts each into a reference
  point, which merges the checkpoint back into the VM's disks. While a
  checkpoint exists the VM writes to an extra AVHDX; if the script is stopped
  half way, it converts the open checkpoint on its way out.
- Taking a recovery checkpoint **turns resilient change tracking on** for the
  VM's disks if it was off: an `.rct` and an `.mrt` file appear next to each
  disk and stay. This is what any Hyper-V backup product does.
- It removes the **reference points** it made, and only those; ones that were
  there before are left alone. `-KeepReferencePoints` keeps them.
- Existing checkpoints of the VM are not touched.
- The guest is not shut down, paused or written to. With integration services
  running, the checkpoint is application consistent, so the guest sees a VSS
  backup.

On the host, the work directory is removed at the end. The tool and the
scripts stay where they were copied.

## Troubleshooting

**`no virtual machine '…' on HOST`** — the name is wrong, or the VM is
clustered and owned by another node.

**`CreateSnapshot failed: 32773`** (invalid parameter) from your own WMI code —
the host is an `Msvm_ComputerSystem` too. A query by `ElementName` alone
returns both when the host and a VM share a name; `hvlib.ps1` filters on
`Caption='Virtual Machine'`.

**`…\work exists`** — an earlier run was given `-Keep`, or was killed. Remove
the directory or pass another `-WorkDir`.

**`Error: '…' has to be attached to be read or written, which needs an elevated (administrator) process`** — the PowerShell
window is not elevated. Over WinRM this does not happen for an administrator.

**The incremental is refused on a VM that was just created or moved** — change
tracking starts with the first reference point; the test takes its full backup
first for that reason. A VM of configuration version below 8.0 has none.

**Non-ASCII names show up garbled over `hv.py`** — PowerShell decodes the
tool's output in the host's OEM code page before sending it on. The files are
named correctly; the check in the test goes by the path, not by the text.

**`hv.py` fails to connect** — check that port 5985 is reachable and that the
user name carries its domain. A host outside a domain may need
`HV_AUTH=basic` with `HV_SSL=1`, or `ntlm` with the local account spelled
`HOST\user`.

## Proving a change to common/ changed nothing

Most of the CLI lives in `common/`, shared by every tool. A change
there is a change to all of them, and `test/hyperv/` covers only the part that
is *not* shared. Two cheaper checks cover the rest, and they are what the
original extraction was verified with.

**The textual check.** When code is only being moved, the strongest evidence is
that it is literally the same text. For each function that moved, pull it out of
the old file and out of the new one and diff them:

```bash
git show <before>:vhdx-tool/disk_tool.cpp > /tmp/before.cpp
# extract the same function from both, strip leading whitespace and blank
# lines, and diff. Every surviving difference is a finding.
```

Normalising away indentation and a known rename is legitimate; normalising away
anything else is hiding a change.

**The before/after capture.** Build the tool before and after, then run the
same matrix through both and diff the output. Capture stdout, stderr and the
exit status separately, and a merged `2>&1` run as well — the merged one is the
only thing that catches a change in how the two streams interleave.

The matrix that matters:

- every help and version path: no arguments, `-h`, `--help`, `-v`,
  `--version`, `--version --json`, and `<cmd> --help` for all ten commands;
- every option error, with and without `--json`: an unknown option, an
  ambiguous prefix, a bad `-c`, `--type`, `--immutable`, `--retention`, `--retention-till`,
  `--source-names`, a missing required option for each command, and a stray
  non-option argument;
- a round trip against a `file` repository: back up a fixed input at a couple of
  compression levels and with `-k`, then `info`, `list`, `verify`, a two-backup
  `copy`, `meta_save`/`meta_load` through both a file and stdio, `remove` with
  `y`, with `n` and with empty input, and finally `restore` compared byte for
  byte against the input.

Normalise the program name and any path, timestamp or backup id before diffing,
and diff the **raw text** rather than parsed JSON: `JsonObject` emits fields in
insertion order and this manual documents that order, so a reordering is a
change even though the JSON would still compare equal.

A `file` repository must be mode `0750` or stricter or backuplib refuses it.

Under Wine the help and error matrices run in full and the round trip runs for
the `file` backend; nothing involving `--vhdx` does, since Wine's `virtdisk.dll`
is mostly stubs and its processes are not elevated. Run `Test-Vm.ps1` against a
real host once afterwards: `runBackup` and `runRestore` are in `common/` too, so
a shared-code change reaches the virtual disk path as well as `Tool::run`'s
scaffolding and `Plat::StdioSetup`. For `abr-vmdk-tool`, the round trip
through a local VMDK made with `qemu-img` covers the VDDK side of it; capture
it with `ABR_VMDK_TOOL_VDDK_DIR` set, and filter out the VDDK's own timestamped
log lines before diffing.
