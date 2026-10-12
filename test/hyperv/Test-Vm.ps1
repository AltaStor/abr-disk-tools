# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Norvitex OU
#
# End-to-end test of abr-vhdx-tool against one disk of a Hyper-V VM, which may
# be running: the backup server's checkpoint workflow, a full and an
# incremental backup, every way of restoring them, and the refusals. Every
# restore is compared, by SHA-256, with what Hyper-V itself reads from the
# checkpoint's disk.
#
# Run it elevated on the Hyper-V host that owns the VM. See doc/testing.md.
#
# Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the
# ANSI code page.

param(
    [Parameter(Mandatory = $true)][string]$VmName,
    # Repository and restored disks go here; it must not exist yet. Needs room
    # for about five times the data on the disk.
    [string]$WorkDir = (Join-Path $PSScriptRoot 'work'),
    [string]$Tool = (Join-Path $PSScriptRoot 'abr-vhdx-tool.exe'),
    # Which of the VM's disks, in the order the checkpoint lists them.
    [int]$DiskIndex = 0,
    # Leave WorkDir in place, for Test-OtherNode.ps1 or for a look around.
    [switch]$Keep,
    # Leave the two reference points the test makes on the VM.
    [switch]$KeepReferencePoints
)

. (Join-Path $PSScriptRoot 'hvlib.ps1')

if (-not (Test-Path -LiteralPath $Tool)) { throw "tool not found: $Tool" }
if (Test-Path -LiteralPath $WorkDir) { throw "$WorkDir exists; remove it or pass another -WorkDir" }
$repo = Join-Path $WorkDir 'repo'
$out = Join-Path $WorkDir 'restore'
New-Item -ItemType Directory -Force $repo, $out | Out-Null

$store = @('--folder', $repo, '--path', 'vm')
$running = (Get-VM -Name $VmName).State -eq 'Running'
$before = @(Get-ReferencePoints $VmName | % { $_.InstanceID })
$disksBefore = @(Get-Disk).Count
$snap = $null

"abr-vhdx-tool $(& $Tool --version) on $env:COMPUTERNAME, VM '$VmName' ($(if ($running) { 'running' } else { 'not running' }))"

try {
    # --- full backup of a checkpoint's disk -------------------------------
    $snap = New-RecoveryCheckpoint $VmName
    $disk = @(Get-CheckpointDisks $snap)[$DiskIndex]
    "checkpoint disk: $disk"
    $full = Invoke-Tool backup --json @store --name full --vhdx $disk
    Assert-Ok 'full backup of a checkpoint disk' $full
    $rct = $full.Json.rct_id
    Assert-That 'full backup reports the RCT id' ([bool]$rct) $full.Text
    $hashFull = Get-VhdContentHash $disk
    ConvertTo-ReferencePoint $snap; $snap = $null
    $ids = @(Get-ReferencePoints $VmName | % { $_.ResilientChangeTrackingIdentifiers })
    Assert-That "rct_id is the reference point's" ($ids -contains $rct) "rct_id $rct`nreference points: $ids"

    # --- incremental against it --------------------------------------------
    $snap = New-RecoveryCheckpoint $VmName
    $disk = @(Get-CheckpointDisks $snap)[$DiskIndex]
    $inc = Invoke-Tool backup --json @store --name inc --vhdx $disk --type incremental --change-id $rct
    Assert-Ok 'incremental backup' $inc
    $hashInc = Get-VhdContentHash $disk
    $r = Invoke-Tool backup --json @store --name bogus --vhdx $disk --type incremental `
        --change-id 'rctX:deadbeef:1db9:4c87:9761:58470a221046:00000001'
    Assert-Refused 'incremental against an unknown change id' $r
    ConvertTo-ReferencePoint $snap; $snap = $null
    $r = Invoke-Tool list @store
    Assert-That 'a refused backup leaves nothing behind' ($r.Exit -eq 0 -and $r.Text -notmatch 'bogus') $r.Text

    # --- verify and restore --------------------------------------------------
    Assert-Ok 'verify full' (Invoke-Tool verify @store --name full)
    Assert-Ok 'verify incremental' (Invoke-Tool verify @store --name inc)

    $chain = Join-Path $out 'chain.vhdx'
    Assert-Ok 'restore full to a new VHDX' (Invoke-Tool restore @store --name full --vhdx $chain)
    Assert-Equal 'restored full is identical' (Get-VhdContentHash $chain) $hashFull
    Assert-Refused 'incremental restore without --force' (Invoke-Tool restore @store --name inc --vhdx $chain)
    Assert-Ok 'incremental restore with --force' (Invoke-Tool restore @store --name inc --vhdx $chain --force)
    Assert-Equal 'full + incremental is identical' (Get-VhdContentHash $chain) $hashInc

    # --- copy: the chain merged into one backup ------------------------------
    Assert-Ok 'copy full,inc into one backup' (Invoke-Tool copy --json @store --source-names 'full,inc' --name merged)
    Assert-Ok 'verify merged' (Invoke-Tool verify @store --name merged)
    $merged = Join-Path $out 'merged.vhdx'
    Assert-Ok 'restore merged' (Invoke-Tool restore @store --name merged --vhdx $merged)
    Assert-Equal 'restored merged is identical' (Get-VhdContentHash $merged) $hashInc

    # --- the other kinds of target ---------------------------------------------
    $vhd = Join-Path $out 'merged.vhd'
    Assert-Ok 'restore to a VHD' (Invoke-Tool restore @store --name merged --vhdx $vhd)
    Assert-Equal 'restored VHD is identical' (Get-VhdContentHash $vhd) $hashInc
    $raw = Join-Path $out 'merged.raw'
    Assert-Ok 'restore to a raw file' (Invoke-Tool restore @store --name merged --target $raw)
    Assert-Equal 'restored raw file is identical' (Get-FileContentHash $raw) $hashInc
    Remove-Item -LiteralPath $raw, $chain

    # --- encryption, no compression, and a non-ASCII path ----------------------
    Assert-Ok 'encrypted, uncompressed backup of a VHD' (Invoke-Tool backup @store --name enc --vhdx $vhd -k s3cret -c 0)
    Assert-Ok 'verify with the right key' (Invoke-Tool verify @store --name enc -k s3cret)
    Assert-Ok 'verify without a key' (Invoke-Tool verify @store --name enc)
    Assert-Refused 'verify with a wrong key' (Invoke-Tool verify @store --name enc -k wrong) 'does not decrypt'
    # "Proverka diska\disk.vhdx" in Cyrillic, spelled in code points to keep this file ASCII.
    $uniDir = Join-Path $out (-join ([char[]](0x41F, 0x440, 0x43E, 0x432, 0x435, 0x440, 0x43A, 0x430, 0x20, 0x434, 0x438, 0x441, 0x43A, 0x430)))
    $uni = Join-Path $uniDir ((-join ([char[]](0x434, 0x438, 0x441, 0x43A))) + '.vhdx')
    New-Item -ItemType Directory -Force $uniDir | Out-Null
    Assert-Ok 'restore encrypted to a non-ASCII path' (Invoke-Tool restore @store --name enc -k s3cret --vhdx $uni)
    Assert-Equal 'restored encrypted is identical' (Get-VhdContentHash $uni) $hashInc
    $wk = Join-Path $out 'wrongkey.vhdx'
    Assert-Refused 'restore with a wrong key' (Invoke-Tool restore --json @store --name enc -k wrong --vhdx $wk) 'does not decrypt'
    Assert-Refused 'restore without the key' (Invoke-Tool restore @store --name enc --vhdx $wk) 'is encrypted'
    Assert-That 'a refused key creates no virtual disk' (-not (Test-Path -LiteralPath $wk))
    Assert-Refused 'copy with a wrong source key' (Invoke-Tool copy @store --source-names enc --source-key wrong --name cp) 'does not decrypt'
    Assert-Ok 'copy with the right source key' (Invoke-Tool copy @store --source-names enc --source-key s3cret --name cp)
    Remove-Item -Recurse -LiteralPath $uniDir, $vhd

    # --- refusals ----------------------------------------------------------------
    if ($running) {
        $live = @(Get-VMHardDiskDrive -VMName $VmName)[$DiskIndex].Path
        Assert-Refused "backup of a running VM's current disk" (Invoke-Tool backup @store --name live --vhdx $live) 'running virtual machine|in use'
        Assert-Refused "restore onto a running VM's current disk" (Invoke-Tool restore @store --name merged --vhdx $live --force)
    } else {
        "SKIP  the in-use refusals: '$VmName' is not running"
    }
    Assert-Refused 'incremental of a disk without change tracking' (Invoke-Tool backup @store --name norct --vhdx $merged `
            --type incremental --change-id 'rctX:00000000:0000:0000:0000:000000000000:00000001')
    $small = Join-Path $out 'small.vhdx'
    New-VHD -Path $small -SizeBytes 16MB -Dynamic | Out-Null
    Assert-Refused 'restore to an existing disk of another size' (Invoke-Tool restore @store --name merged --vhdx $small)
    $img = Join-Path $out 'x.img'
    Assert-Refused 'restore --vhdx to a name that is no virtual disk' (Invoke-Tool restore @store --name merged --vhdx $img)
    Assert-That 'and nothing is created for it' (-not (Test-Path -LiteralPath $img))
    Assert-Refused 'backup of a missing disk' (Invoke-Tool backup @store --name nope --vhdx (Join-Path $out 'nope.vhdx'))

    Assert-Equal 'no disk is left attached' @(Get-Disk).Count $disksBefore

    # What Test-OtherNode.ps1 checks from another cluster node.
    @{ hash = $hashInc; merged = $merged } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $WorkDir 'expected.json')
}
finally {
    # A recovery checkpoint left by a failure half way would keep the VM
    # writing to an AVHDX for good; converting merges it back.
    if ($snap) { ConvertTo-ReferencePoint $snap }
    if (-not $KeepReferencePoints) {
        Get-ReferencePoints $VmName | ? { $before -notcontains $_.InstanceID } | % { Remove-ReferencePoint $_ }
    }
    if (-not $Keep) { Remove-Item -Recurse -Force -LiteralPath $WorkDir }
}

Complete-Tests
