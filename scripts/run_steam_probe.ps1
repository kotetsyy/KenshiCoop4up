[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ProbeDll,
    [string]$GameRoot = 'D:\SteamLibrary\steamapps\common\Kenshi',
    [ValidateSet('inspect','host','join','loopback')][string]$Role = 'inspect',
    [string]$PeerSteamId = '',
    [ValidateRange(20,180)][int]$Seconds = 145,
    [ValidateRange(1,240)][int]$LauncherTimeoutSec = 90,
    [string]$EvidenceRoot = "$env:LOCALAPPDATA\KenshiCoop\steam-probe-evidence"
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue) { throw 'Close the existing Kenshi session first; this launcher never terminates user-owned games.' }
if (-not (Test-Path -LiteralPath $ProbeDll -PathType Leaf)) { throw "Probe DLL not found: $ProbeDll" }
if (($Role -eq 'join' -or $PeerSteamId) -and $PeerSteamId -notmatch '^[0-9]{17}$') { throw 'JOIN requires the real host SteamID64 (17 decimal digits); HOST has no guest allowlist.' }
$ModRoot = Join-Path $GameRoot 'mods\KenshiCoop'
$Loader = Join-Path $ModRoot 'RE_Kenshi.json'
$InstalledProbe = Join-Path $ModRoot 'KenshiCoopSteamProbe.dll'
$GameExe = Join-Path $GameRoot 'kenshi_x64.exe'
if (-not (Test-Path -LiteralPath $Loader) -or -not (Test-Path -LiteralPath $GameExe)) { throw 'Installed Kenshi and its sanctioned RE_Kenshi plugin loader are required.' }
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$Evidence = Join-Path $EvidenceRoot $Stamp
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Backups = @()
foreach ($File in @($Loader, $InstalledProbe, (Join-Path $GameRoot 'kenshi.cfg'), (Join-Path $GameRoot 'screens.cfg'), (Join-Path $GameRoot 'settings.cfg'))) {
    $Exists = Test-Path -LiteralPath $File -PathType Leaf
    $Backup = Join-Path $Evidence (Split-Path $File -Leaf)
    if ($Exists) { Copy-Item -LiteralPath $File -Destination $Backup }
    $Backups += [PSCustomObject]@{ Path=$File; Backup=$Backup; Existed=$Exists }
}
$ProbeLog = Join-Path $GameRoot 'KenshiCoop_gns_probe.log'
$ProbeLogOffset = if (Test-Path -LiteralPath $ProbeLog) { (Get-Item -LiteralPath $ProbeLog).Length } else { 0 }
$SavedRole = $env:KC_GNS_PROBE_ROLE
$SavedPeer = $env:KC_GNS_PROBE_STEAMID
$Process = $null
$Owned = [PSCustomObject]@{ Process=$null; Processes=@{}; Windows=@{} }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CoopProbeWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct MONITORINFOEX {
        public int Size; public RECT Monitor, Work; public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Device;
    }
    public delegate bool MonitorCallback(IntPtr monitor, IntPtr dc, IntPtr rectangle, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFOEX info);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr window, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rectangle);
    public static bool SameFile(string first, string second) {
        using (var a = System.IO.File.OpenRead(first))
        using (var b = System.IO.File.OpenRead(second))
        using (var sha = System.Security.Cryptography.SHA256.Create()) {
            if (a.Length != b.Length) return false;
            var x = sha.ComputeHash(a); var y = sha.ComputeHash(b);
            for (int i=0; i<x.Length; ++i) if (x[i] != y[i]) return false;
            return true;
        }
    }
}
'@
$script:MonitorTwo = $null
$Callback = [CoopProbeWindow+MonitorCallback]{ param($monitor,$dc,$rectangle,$data)
    $info = New-Object CoopProbeWindow+MONITORINFOEX
    $info.Size = [Runtime.InteropServices.Marshal]::SizeOf($info)
    if ([CoopProbeWindow]::GetMonitorInfoW($monitor,[ref]$info) -and ($info.Flags -band 1) -eq 0 -and -not $script:MonitorTwo) { $script:MonitorTwo=$info }
    return $true
}
[CoopProbeWindow]::EnumDisplayMonitors([IntPtr]::Zero,[IntPtr]::Zero,$Callback,[IntPtr]::Zero) | Out-Null
if (-not $script:MonitorTwo) { throw 'A non-primary monitor is unavailable; no game launch performed.' }
try {
    Copy-Item -LiteralPath $ProbeDll -Destination $InstalledProbe -Force
    [IO.File]::WriteAllText($Loader, "{`n  `"Plugins`": [ `"KenshiCoopSteamProbe.dll`" ]`n}`n", (New-Object Text.UTF8Encoding($false)))
    if ($Role -eq 'inspect') { $env:KC_GNS_PROBE_ROLE=$null; $env:KC_GNS_PROBE_STEAMID=$null }
    else { $env:KC_GNS_PROBE_ROLE=$Role; $env:KC_GNS_PROBE_STEAMID=$PeerSteamId }
    $LauncherWork = $script:MonitorTwo.Work
    $OnOwnedProcess = {
        param($Game)
        $Owned.Processes[$Game.Id]=$Game
        if ($Game.HasExited -or $Game.MainWindowHandle -eq [IntPtr]::Zero -or
            $Owned.Windows.ContainsKey($Game.MainWindowHandle.ToInt64())) { return }
        $Bounds = New-Object CoopProbeWindow+RECT
        if (-not [CoopProbeWindow]::GetWindowRect($Game.MainWindowHandle,[ref]$Bounds) -or
            -not [CoopProbeWindow]::MoveWindow($Game.MainWindowHandle,$LauncherWork.Left+20,$LauncherWork.Top+20,
                $Bounds.Right-$Bounds.Left,$Bounds.Bottom-$Bounds.Top,$true)) { throw 'Cannot place owned graphics launcher on monitor2.' }
        $Owned.Windows[$Game.MainWindowHandle.ToInt64()]=$true
    }.GetNewClosure()
    $BeforeClick = { param($Game) $Owned.Process=$Game }.GetNewClosure()
    $LaunchOutput = & (Join-Path $PSScriptRoot 'start_kenshi.ps1') -ExePath $GameExe -WorkDir $GameRoot -TimeoutSec $LauncherTimeoutSec -OnOwnedProcess $OnOwnedProcess -BeforeLauncherClick $BeforeClick 6>&1
    $LaunchOutput | ForEach-Object { Write-Output "$_" }
    $Process=$Owned.Process
    if (-not $Process) { throw 'Sanctioned launcher did not return an owned actual game process.' }
    $Deadline = (Get-Date).AddSeconds($Seconds)
    $Moved = $false
    $LastWindow = [IntPtr]::Zero
    while ((Get-Date) -lt $Deadline -and -not $Process.HasExited) {
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero -and $Process.MainWindowHandle -ne $LastWindow) {
            $Work = $script:MonitorTwo.Work
            $Width = [Math]::Min(1600, $Work.Right-$Work.Left-40)
            $Height = [Math]::Min(900, $Work.Bottom-$Work.Top-40)
            if (-not [CoopProbeWindow]::MoveWindow($Process.MainWindowHandle,$Work.Left+20,$Work.Top+20,$Width,$Height,$true)) { throw 'Unable to place owned test game on monitor2.' }
            $Bounds = New-Object CoopProbeWindow+RECT
            if (-not [CoopProbeWindow]::GetWindowRect($Process.MainWindowHandle,[ref]$Bounds) -or $Bounds.Left -lt $Work.Left -or $Bounds.Right -gt $Work.Right) { throw 'Monitor2 window-position assertion failed.' }
            [PSCustomObject]@{ PID=$Process.Id; Window=$Process.MainWindowHandle.ToInt64(); Monitor=$script:MonitorTwo.Device; Bounds=$Bounds; Role=$Role } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Evidence 'window.json') -Encoding UTF8
            Write-Output "Owned probe game PID=$($Process.Id), monitor2 placement confirmed. Evidence=$Evidence"
            $Moved=$true
            $LastWindow=$Process.MainWindowHandle
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $Moved) { throw 'Game exited or timed out without a monitor2 window.' }
    if ($Process.HasExited -and $Process.ExitCode -ne 0) { throw "Probe game exited with error $($Process.ExitCode)" }
} finally {
    # Stop the owned process tree before restoring loader or display settings.
    # The bootstrap may exit before its child's launcher becomes ready.
    $Closed = @{}
    while ($true) {
        foreach ($Native in (Get-CimInstance Win32_Process -Filter "Name='kenshi_x64.exe'")) {
            if ($Owned.Processes.ContainsKey([int]$Native.ParentProcessId) -and -not $Owned.Processes.ContainsKey([int]$Native.ProcessId)) {
                $Child = Get-Process -Id $Native.ProcessId -ErrorAction SilentlyContinue
                if ($Child) { $Owned.Processes[$Child.Id]=$Child }
            }
        }
        $Remaining = @($Owned.Processes.Values | Where-Object { -not $Closed.ContainsKey($_.Id) })
        if (-not $Remaining.Count) { break }
        foreach ($Game in $Remaining) {
            if (-not $Game.HasExited) {
                $Game.CloseMainWindow() | Out-Null
                if (-not $Game.WaitForExit(10000)) { $Game.Kill(); $Game.WaitForExit() }
            }
            $Closed[$Game.Id]=$true
        }
    }
    $env:KC_GNS_PROBE_ROLE=$SavedRole
    $env:KC_GNS_PROBE_STEAMID=$SavedPeer
    foreach ($Backup in $Backups) {
        if ($Backup.Existed) {
            Copy-Item -LiteralPath $Backup.Backup -Destination $Backup.Path -Force
            if (-not [CoopProbeWindow]::SameFile($Backup.Backup,$Backup.Path)) { throw "Restore verification failed: $($Backup.Path)" }
        } elseif (Test-Path -LiteralPath $Backup.Path) { Remove-Item -LiteralPath $Backup.Path -Force }
    }
    $Log = Join-Path $GameRoot 'KenshiCoop_gns_probe.log'
    if (Test-Path -LiteralPath $Log) { Copy-Item -LiteralPath $Log -Destination (Join-Path $Evidence 'KenshiCoop_gns_probe.log') }
    Write-Output "Owned process tree closed; original loader/probe/display settings restored and verified. Evidence=$Evidence"
}
if ($Role -eq 'loopback') {
    $Stream = [IO.File]::OpenRead((Join-Path $Evidence 'KenshiCoop_gns_probe.log'))
    try {
        [void]$Stream.Seek($ProbeLogOffset, [IO.SeekOrigin]::Begin)
        $Reader = New-Object IO.StreamReader($Stream)
        try { $FreshLog = $Reader.ReadToEnd() } finally { $Reader.Dispose() }
    } finally { $Stream.Dispose() }
    if ($FreshLog -notmatch "LOOPBACK PASS pid=$($Process.Id) delivered=26 lanes=3 invalidSendRecovery=1 nativeNetworkLoopback=1") {
        throw "Actual Steam production-send loopback regression did not pass. Evidence=$Evidence"
    }
    Write-Output 'PASS actual Steam production-send bytes/lanes/bidirectional ownership and invalid-send recovery; not two-account P2P certification.'
}
