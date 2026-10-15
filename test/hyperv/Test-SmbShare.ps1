# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Norvitex OU
#
# Backs up every disk of a Hyper-V VM to a repository on an SMB share, and
# checks what landed there: a full and an incremental backup of each disk from
# the backup server's checkpoint workflow, verify read over the network, and a
# restore from the share compared, by SHA-256, with what Hyper-V itself reads
# from the checkpoint's disk.
#
# Test-Vm.ps1 covers the tool on a local repository; this covers the one thing
# it does not, a UNC --folder: backuplib creating the repository level by level
# on a share, writing and renaming its files there, and reading them back.
#
# Run it elevated on the Hyper-V host that owns the VM. See doc/testing.md.
#
# Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the
# ANSI code page.

param(
    [Parameter(Mandatory = $true)][string]$VmName,
    # The share, \\server\share. The repository goes into a new directory on
    # it, named after this host and the time, which is removed at the end.
    [Parameter(Mandatory = $true)][string]$Share,
    # An account for the share. Without one the share must already be
    # reachable from this session; over WinRM it usually is not, since the
    # session's credentials do not travel a second hop.
    [string]$ShareUser,
    # A SecureString, never plain text, so that the password is in no script
    # text a log could record: hv.py run --secret SharePassword=VAR sends one
    # over WinRM, and on the host (Read-Host -AsSecureString) makes one.
    [securestring]$SharePassword,
    # Encrypts the backups with this key, which reaches the tool on its
    # standard input (--stdin-keys), never on its command line.
    [securestring]$BackupKey,
    # Restored disks go here; it must not exist yet. Needs room for the
    # virtual size of the VM's largest disk.
    [string]$WorkDir = (Join-Path $PSScriptRoot 'work-smb'),
    [string]$Tool = (Join-Path $PSScriptRoot 'abr-vhdx-tool.exe'),
    # Leave the repository on the share and the work directory in place.
    [switch]$Keep,
    # Leave the two reference points the test makes on the VM.
    [switch]$KeepReferencePoints
)

. (Join-Path $PSScriptRoot 'hvlib.ps1')

$Share = $Share.TrimEnd('\')
if (-not (Test-Path -LiteralPath $Tool)) { throw "tool not found: $Tool" }
if (Test-Path -LiteralPath $WorkDir) { throw "$WorkDir exists; remove it or pass another -WorkDir" }

if ($ShareUser -and -not $SharePassword) { throw '-ShareUser needs -SharePassword' }

# A connection made here is this session's own and is dropped at the end; one
# that was already there is left alone. Not net use, which takes the password
# on its command line, and not New-SmbMapping, which needs an interactive
# logon to make a mapping without a drive letter.
$mapped = $false
if ($ShareUser) {
    try { [SmbConnection]::Add($Share, $ShareUser, $SharePassword) }
    catch { throw "connecting to $Share as $ShareUser failed: $($_.Exception.GetBaseException().Message)" }
    $mapped = $true
}

# Every call goes through --stdin-keys, with or without a key, so that the
# path a backup server takes is the one tested.
$secrets = if ($BackupKey) { @{ key = $BackupKey } } else { @{} }

$runDir = $null
$snap = $null
$before = @(Get-ReferencePoints $VmName | % { $_.InstanceID })
$disksBefore = @(Get-Disk).Count
$running = (Get-VM -Name $VmName).State -eq 'Running'

"abr-vhdx-tool $(& $Tool --version) on $env:COMPUTERNAME, VM '$VmName' ($(if ($running) { 'running' } else { 'not running' }))$(if ($BackupKey) { ', encrypted' })"

try {
    # --- the share -------------------------------------------------------------
    $runDir = Join-Path $Share ("abr-test-$env:COMPUTERNAME-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory $runDir | Out-Null
    "repository: $runDir"
    $probe = Join-Path $runDir 'probe.txt'
    Set-Content -LiteralPath $probe -Value $env:COMPUTERNAME
    Assert-Equal 'the share is writable and reads back' (Get-Content -LiteralPath $probe) $env:COMPUTERNAME
    Remove-Item -LiteralPath $probe
    New-Item -ItemType Directory -Force $WorkDir | Out-Null

    # The repository itself is left for the tool to create, through
    # --create-folder, since that walk up a UNC path is part of what is tested.
    $repo = Join-Path $runDir 'repo'
    $store = @('--folder', $repo, '--path', $VmName)

    # --- full backup of every disk of a checkpoint ------------------------------
    $snap = New-RecoveryCheckpoint $VmName
    $disks = @(Get-CheckpointDisks $snap)
    if (-not $disks) { throw "the checkpoint of '$VmName' has no virtual disks" }
    $rct = @{}; $hashFull = @{}; $hashInc = @{}
    for ($i = 0; $i -lt $disks.Count; $i++) {
        "disk ${i}: $($disks[$i])"
        $r = Invoke-ToolSecret $secrets backup --json @store --create-folder --name "disk$i-full" --vhdx $disks[$i]
        Assert-Ok "disk ${i}: full backup to the share" $r
        $rct[$i] = $r.Json.rct_id
        Assert-That "disk ${i}: full backup reports the RCT id" ([bool]$rct[$i]) $r.Text
        $hashFull[$i] = Get-VhdContentHash $disks[$i]
    }
    ConvertTo-ReferencePoint $snap; $snap = $null

    # --- incremental of every disk against it -----------------------------------
    $snap = New-RecoveryCheckpoint $VmName
    $disks = @(Get-CheckpointDisks $snap)
    for ($i = 0; $i -lt $disks.Count; $i++) {
        if (-not $rct[$i]) { "SKIP  disk ${i}: incremental, the full backup failed"; continue }
        $r = Invoke-ToolSecret $secrets backup --json @store --name "disk$i-inc" --vhdx $disks[$i] --type incremental --change-id $rct[$i]
        Assert-Ok "disk ${i}: incremental backup to the share" $r
        $hashInc[$i] = Get-VhdContentHash $disks[$i]
    }
    ConvertTo-ReferencePoint $snap; $snap = $null

    # --- what is on the share -----------------------------------------------------
    $files = @(Get-ChildItem -LiteralPath $repo -Recurse -File)
    $bytes = ($files | Measure-Object -Property Length -Sum).Sum
    "on the share: $($files.Count) files, $([math]::Round($bytes / 1MB, 1)) MB"
    Assert-That 'the repository is on the share' ($files.Count -gt 0) "nothing under $repo"
    $list = Invoke-ToolSecret $secrets list @store
    Assert-Ok 'list the repository on the share' $list
    for ($i = 0; $i -lt $disks.Count; $i++) {
        foreach ($name in "disk$i-full", "disk$i-inc") {
            if ($name -like '*-inc' -and -not $hashInc[$i]) { continue }
            Assert-That "list shows $name" ($list.Text -match [regex]::Escape($name)) $list.Text
            Assert-Ok "verify $name over SMB" (Invoke-ToolSecret $secrets verify @store --name $name)
        }
    }

    # Were the key to be lost on its way down the pipe, every backup above
    # would quietly be an unencrypted one, and every check still pass.
    if ($BackupKey) {
        $r = Invoke-Tool restore @store --name 'disk0-full' --vhdx (Join-Path $WorkDir 'nokey.vhdx')
        Assert-Refused 'the backups are encrypted: no restore without the key' $r 'is encrypted'
    }

    # --- restore from the share ---------------------------------------------------
    # One disk at a time, so that the work directory needs room for one.
    for ($i = 0; $i -lt $disks.Count; $i++) {
        if (-not $hashFull[$i]) { continue }
        $vhdx = Join-Path $WorkDir "disk$i.vhdx"
        Assert-Ok "disk ${i}: restore full from the share" (Invoke-ToolSecret $secrets restore @store --name "disk$i-full" --vhdx $vhdx)
        Assert-Equal "disk ${i}: restored full is identical" (Get-VhdContentHash $vhdx) $hashFull[$i]
        if ($hashInc[$i]) {
            Assert-Ok "disk ${i}: restore incremental from the share" (Invoke-ToolSecret $secrets restore @store --name "disk$i-inc" --vhdx $vhdx --force)
            Assert-Equal "disk ${i}: full + incremental is identical" (Get-VhdContentHash $vhdx) $hashInc[$i]
        }
        Remove-Item -LiteralPath $vhdx
    }

    Assert-Equal 'no disk is left attached' @(Get-Disk).Count $disksBefore
}
finally {
    # A recovery checkpoint left by a failure half way would keep the VM
    # writing to an AVHDX for good; converting merges it back.
    if ($snap) { ConvertTo-ReferencePoint $snap }
    if (-not $KeepReferencePoints) {
        Get-ReferencePoints $VmName | ? { $before -notcontains $_.InstanceID } | % { Remove-ReferencePoint $_ }
    }
    if (-not $Keep) {
        if ($runDir -and (Test-Path -LiteralPath $runDir)) { Remove-Item -Recurse -Force -LiteralPath $runDir }
        if (Test-Path -LiteralPath $WorkDir) { Remove-Item -Recurse -Force -LiteralPath $WorkDir }
    } elseif ($runDir) {
        "kept: $runDir"
    }
    if ($mapped) { [SmbConnection]::Remove($Share) }
}

Complete-Tests
