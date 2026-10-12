# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Norvitex OU
#
# The cluster half of the test: run on a node that does NOT own the cluster
# shared volume, against the work directory that 'Test-Vm.ps1 -Keep' left on
# that volume from the owning node. A repository written on one node has to
# read back the same on another, and virtual disks on the volume have to be
# attachable from a node whose I/O to it may be redirected.
#
# Run it elevated. See doc/testing.md.

param(
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [string]$Tool = (Join-Path $PSScriptRoot 'abr-vhdx-tool.exe'),
    [switch]$Keep
)

. (Join-Path $PSScriptRoot 'hvlib.ps1')

if (-not (Test-Path -LiteralPath $Tool)) { throw "tool not found: $Tool" }
$expectedFile = Join-Path $WorkDir 'expected.json'
if (-not (Test-Path -LiteralPath $expectedFile)) { throw "$expectedFile not found; run Test-Vm.ps1 -Keep -WorkDir $WorkDir on the owning node first" }
$expected = Get-Content -LiteralPath $expectedFile -Raw | ConvertFrom-Json
$store = @('--folder', (Join-Path $WorkDir 'repo'), '--path', 'vm')
$out = Join-Path $WorkDir 'restore'
$disksBefore = @(Get-Disk).Count

"abr-vhdx-tool $(& $Tool --version) on $env:COMPUTERNAME"
Get-ClusterSharedVolumeState -ErrorAction SilentlyContinue | ? { $_.Node -eq $env:COMPUTERNAME } |
    % { "volume $($_.VolumeFriendlyName): $($_.StateInfo) I/O from this node" }

try {
    Assert-Ok "verify the other node's backup" (Invoke-Tool verify @store --name merged)
    $vhdx = Join-Path $out 'other.vhdx'
    Assert-Ok 'restore it to a new VHDX on the volume' (Invoke-Tool restore @store --name merged --vhdx $vhdx)
    Assert-Equal 'restored disk is identical' (Get-VhdContentHash $vhdx) $expected.hash

    Assert-Ok 'back up a VHDX the other node wrote' (Invoke-Tool backup --json @store --name other --vhdx $expected.merged)
    $raw = Join-Path $out 'other.raw'
    Assert-Ok 'restore that to a raw file' (Invoke-Tool restore @store --name other --target $raw)
    Assert-Equal 'restored raw file is identical' (Get-FileContentHash $raw) $expected.hash

    Assert-Equal 'no disk is left attached' @(Get-Disk).Count $disksBefore
}
finally {
    if (-not $Keep) { Remove-Item -Recurse -Force -LiteralPath $WorkDir }
}

Complete-Tests
