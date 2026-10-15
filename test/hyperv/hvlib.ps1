# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Norvitex OU
#
# Shared by the Test-*.ps1 scripts: what a backup server does through Hyper-V's
# WMI provider around a run of the tool, an independent way to read a virtual
# disk's content, and the pass/fail bookkeeping.
#
# Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the
# ANSI code page.

$ErrorActionPreference = 'Stop'
$ns = 'root\virtualization\v2'

function Wait-Job2($r, $what) {
    if ($r.ReturnValue -eq 4096) {
        $job = [wmi]$r.Job
        while ($job.JobState -in 2,3,4) { Start-Sleep -Milliseconds 300; $job = [wmi]$r.Job }
        if ($job.JobState -ne 7) { throw "$what failed: $($job.ErrorDescription) ($($job.ErrorCode))" }
    } elseif ($r.ReturnValue -ne 0) { throw "$what failed: $($r.ReturnValue)" }
}

# The host is an Msvm_ComputerSystem too, and may well have the VM's name.
function Get-Vm2($name) {
    $vm = Get-WmiObject -Namespace $ns -Class Msvm_ComputerSystem -Filter "ElementName='$name' AND Caption='Virtual Machine'"
    if (-not $vm) { throw "no virtual machine '$name' on $env:COMPUTERNAME (a clustered VM is only visible on the node that owns it)" }
    $vm
}

# A recovery (backup) checkpoint, application consistent where the guest allows.
function New-RecoveryCheckpoint($vmName) {
    $vm = Get-Vm2 $vmName
    $svc = Get-WmiObject -Namespace $ns -Class Msvm_VirtualSystemSnapshotService
    $cls = [wmiclass]"\\.\root\virtualization\v2:Msvm_VirtualSystemSnapshotSettingData"
    $s = $cls.CreateInstance(); $s.ConsistencyLevel = 1; $s.IgnoreNonSnapshottableDisks = $true
    $in = $svc.GetMethodParameters("CreateSnapshot")
    $in.AffectedSystem = $vm.__PATH
    $in.SnapshotSettings = $s.GetText(1)
    $in.SnapshotType = [uint16]32768
    $r = $svc.InvokeMethod("CreateSnapshot", $in, $null)
    Wait-Job2 $r "CreateSnapshot"
    Get-WmiObject -Namespace $ns -Query "ASSOCIATORS OF {$($vm.__PATH)} WHERE AssocClass=Msvm_SnapshotOfVirtualSystem" |
        ? { $_.VirtualSystemType -eq 'Microsoft:Hyper-V:Snapshot:Recovery' } |
        sort CreationTime | select -Last 1
}

function Get-CheckpointDisks($snap) {
    Get-WmiObject -Namespace $ns -Query "ASSOCIATORS OF {$($snap.__PATH)} WHERE ResultClass=Msvm_StorageAllocationSettingData" |
        ? { $_.ResourceType -eq 31 } | % { $_.HostResource[0] }
}

function ConvertTo-ReferencePoint($snap) {
    $svc = Get-WmiObject -Namespace $ns -Class Msvm_VirtualSystemSnapshotService
    $in = $svc.GetMethodParameters("ConvertToReferencePoint")
    $in.AffectedSnapshot = $snap.__PATH
    $r = $svc.InvokeMethod("ConvertToReferencePoint", $in, $null)
    Wait-Job2 $r "ConvertToReferencePoint"
}

function Get-ReferencePoints($vmName) {
    $vm = Get-Vm2 $vmName
    Get-WmiObject -Namespace $ns -Query "ASSOCIATORS OF {$($vm.__PATH)} WHERE ResultClass=Msvm_VirtualSystemReferencePoint"
}

function Remove-ReferencePoint($rp) {
    $svc = Get-WmiObject -Namespace $ns -Class Msvm_VirtualSystemReferencePointService
    $in = $svc.GetMethodParameters("DestroyReferencePoint")
    $in.AffectedReferencePoint = $rp.__PATH
    $r = $svc.InvokeMethod("DestroyReferencePoint", $in, $null)
    Wait-Job2 $r "DestroyReferencePoint"
}

# SHA-256 of a disk device or a file, read raw.
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Security.Cryptography; using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;
public static class RawHash {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
    public static string Hash(string path, long length) {
        var h = CreateFile(path, 0x80000000, 7, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("open " + path + ": " + Marshal.GetLastWin32Error());
        using (var fs = new FileStream(h, FileAccess.Read, 1 << 20, false))
        using (var sha = SHA256.Create()) {
            var buf = new byte[4 << 20]; long left = length;
            while (left > 0) {
                int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (n <= 0) throw new IOException("short read at " + (length - left));
                sha.TransformBlock(buf, 0, n, null, 0); left -= n;
            }
            sha.TransformFinalBlock(buf, 0, 0);
            return BitConverter.ToString(sha.Hash).Replace("-", "");
        }
    }
}
'@

# Hash of what a virtual disk holds, read through Hyper-V's own attach rather
# than the tool's, so that the two can disagree.
function Get-VhdContentHash($path) {
    $m = Mount-VHD -Path $path -ReadOnly -NoDriveLetter -Passthru
    try {
        $d = $m | Get-Disk
        [RawHash]::Hash("\\.\PhysicalDrive$($d.Number)", $m.Size)
    } finally { Dismount-VHD -Path $path }
}

function Get-FileContentHash($path) { [RawHash]::Hash($path, (Get-Item -LiteralPath $path).Length) }

# Runs the tool and returns its exit code, its output without the progress
# lines, and the --json document if there was one. The tool's stderr arrives
# as error records, which must not stop the script: a failure is often what a
# test is after.
function Invoke-Tool { Invoke-ToolWith $args $null }

# The same with secrets: Invoke-ToolSecret @{ key = $k } restore ... hands them
# to the tool on standard input, behind --stdin-keys, instead of on its
# command line, which every process on the host can read. A value may be a
# string or a SecureString; it is decrypted only to be written to the pipe.
#
# A plain function taking the table as its first argument rather than a
# declared parameter: PowerShell would bind the tool's own -s or --source-key
# to any parameter whose name they abbreviate.
function Invoke-ToolSecret {
    $secrets = $args[0]
    $lines = @($secrets.GetEnumerator() | sort Name | % { '{0}={1}' -f $_.Name, (ConvertTo-PlainText $_.Value) })
    Invoke-ToolWith (@($args | select -Skip 1) + '--stdin-keys') ($lines + '')
}

function Invoke-ToolWith([object[]]$ToolArgs, [string[]]$Stdin) {
    $ErrorActionPreference = 'Continue'
    if ($null -ne $Stdin) {
        # Windows PowerShell pipes to a native program in ASCII, which turns
        # every non-ASCII character of a key into '?' -- and consistently, so
        # a backup and a restore both given the key this way still agree. It
        # takes the encoding from the global $OutputEncoding only: one set in
        # this function's scope is ignored.
        $saved = $global:OutputEncoding
        $global:OutputEncoding = New-Object Text.UTF8Encoding $false
        try { $out = $Stdin | & $script:Tool @ToolArgs 2>&1 }
        finally { $global:OutputEncoding = $saved }
    } else {
        $out = & $script:Tool @ToolArgs 2>&1
    }
    $lines = @($out | % { "$_" } | ? { $_ -notmatch '^\d+%$' })
    $result = [pscustomobject]@{ Exit = $LASTEXITCODE; Lines = $lines; Text = ($lines -join "`n"); Json = $null }
    if ($result.Text -match '(?s)\{.*\}') { try { $result.Json = $Matches[0] | ConvertFrom-Json } catch { } }
    $result
}

function ConvertTo-PlainText($value) {
    if ($value -isnot [securestring]) { return [string]$value }
    $p = [Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($value)
    try { [Runtime.InteropServices.Marshal]::PtrToStringUni($p) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($p) }
}

# A connection to an SMB share under an account of its own, made the way net use makes it
# but without net use's command line: the password goes from the SecureString straight to
# WNetAddConnection2 in unmanaged memory, and is zeroed after. Like net use's, the connection
# belongs to this logon session, so the tool, started from here, reaches the share through it.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security;

public static class SmbConnection {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    class NetResource {
        public int Scope, Type = 1 /* RESOURCETYPE_DISK */, DisplayType, Usage;
        public string LocalName, RemoteName, Comment, Provider;
    }
    [DllImport("mpr.dll", CharSet = CharSet.Unicode, EntryPoint = "WNetAddConnection2W")]
    static extern int WNetAddConnection2(NetResource resource, IntPtr password, string user, int flags);
    [DllImport("mpr.dll", CharSet = CharSet.Unicode, EntryPoint = "WNetCancelConnection2W")]
    static extern int WNetCancelConnection2(string name, int flags, bool force);

    public static void Add(string share, string user, SecureString password) {
        IntPtr p = Marshal.SecureStringToGlobalAllocUnicode(password);
        try {
            int e = WNetAddConnection2(new NetResource { RemoteName = share }, p, user, 0);
            if (e != 0) throw new Win32Exception(e);
        } finally { Marshal.ZeroFreeGlobalAllocUnicode(p); }
    }
    public static void Remove(string share) { WNetCancelConnection2(share, 0, true); }
}
'@

$script:Passed = 0
$script:Failed = 0

function Assert-That($name, [bool]$ok, $detail) {
    if ($ok) { $script:Passed++; "PASS  $name" }
    else { $script:Failed++; "FAIL  $name"; if ($detail) { "$detail".Split("`n") | % { "        $_" } } }
}

function Assert-Ok($name, $r) { Assert-That $name ($r.Exit -eq 0) "exit $($r.Exit)`n$($r.Text)" }

# A refusal has to be a clean one: exit code 1 and a message behind 'Error: ',
# optionally a particular message.
function Assert-Refused($name, $r, $pattern = 'Error: ') {
    Assert-That $name ($r.Exit -eq 1 -and $r.Text -match $pattern) "exit $($r.Exit), expected 1 and /$pattern/`n$($r.Text)"
}

function Assert-Equal($name, $actual, $expected) {
    Assert-That $name ($actual -eq $expected) "got      $actual`nexpected $expected"
}

function Complete-Tests {
    "RESULT: $script:Passed passed, $script:Failed failed"
    if ($script:Failed) { throw "$script:Failed test(s) failed" }
}
