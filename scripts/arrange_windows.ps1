<#
.SYNOPSIS
  Place exact owned HOST/JOIN incarnations on a live non-primary work area.
.DESCRIPTION
  HOST retains slot 0 and JOIN slot 1 even when one window is absent. Every visible
  owned top-level window is validated before movement. Missing or overflowing
  topology and failed native placement are errors, never successful arrangement.
.EXAMPLE
  .\scripts\arrange_windows.ps1 -HostPid $hostGame.Id -HostStartTicks $hostGame.StartTime.ToUniversalTime().Ticks -JoinPid $joinGame.Id -JoinStartTicks $joinGame.StartTime.ToUniversalTime().Ticks
#>
[CmdletBinding()]
param(
    [int]$TimeoutSec=60, [ValidateRange(0,4096)][int]$GapPx=24,
    [int]$HostPid=0, [long]$HostStartTicks=0,
    [int]$JoinPid=0, [long]$JoinStartTicks=0,
    [ValidateSet('non-primary')][string]$Monitor='non-primary',
    [int]$RepeatSec=0, [int]$ClientW=0, [int]$ClientH=0
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'kit_preflight.ps1')
if (($ClientW -gt 0) -ne ($ClientH -gt 0) -or $ClientW -lt 0 -or $ClientH -lt 0) { throw 'Specify both positive client dimensions or neither.' }
$targets=@()
try {
    foreach ($target in @(@{Role='host';Slot=0;Id=$HostPid;Ticks=$HostStartTicks},@{Role='join';Slot=1;Id=$JoinPid;Ticks=$JoinStartTicks})) {
        if ($target.Id -le 0) { continue }
        if ($target.Ticks -le 0) { throw 'Owned placement requires process start-time UTC ticks for each PID.' }
        $p=Get-Process -Id $target.Id -ErrorAction Stop; $null=$p.Handle
        if ($p.HasExited -or $p.StartTime.ToUniversalTime().Ticks -ne $target.Ticks) { $p.Dispose(); throw 'Owned placement process identity changed.' }
        if ($p.ProcessName -ine 'kenshi_x64') { $p.Dispose(); throw 'Only exact owned Kenshi incarnations may be placed.' }
        $target.Process=$p; $targets+=$target
    }
    if (-not $targets.Count) { throw 'Explicit owned HostPid/JoinPid and UTC start ticks required.' }
    if ($HostPid -gt 0 -and $HostPid -eq $JoinPid) { throw 'HOST and JOIN must be distinct owned processes.' }
    Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class WinArrange {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    private delegate bool Callback(IntPtr window,IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback cb,IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll",SetLastError=true)] private static extern bool SetWindowPos(IntPtr window,IntPtr after,int x,int y,int w,int h,uint flags);
    public static IntPtr[] Windows(uint pid) {
        var windows=new List<IntPtr>();
        EnumWindows(delegate(IntPtr window,IntPtr data) { uint owner; GetWindowThreadProcessId(window,out owner);
            if(owner==pid && IsWindowVisible(window) && !IsIconic(window)) windows.Add(window); return true; },IntPtr.Zero);
        return windows.ToArray();
    }
    public static Rect Bounds(IntPtr window,bool client) {
        Rect rect; bool ok=client ? GetClientRect(window,out rect) : GetWindowRect(window,out rect);
        if(!ok || rect.Right<=rect.Left || rect.Bottom<=rect.Top) throw new InvalidOperationException("Owned window bounds unavailable.");
        return rect;
    }
    public static void Check(System.Diagnostics.Process process,IntPtr window) {
        IntPtr held=process.Handle; uint owner; GetWindowThreadProcessId(window,out owner);
        if(process.HasExited || owner!=(uint)process.Id || !IsWindowVisible(window) || IsIconic(window))
            throw new InvalidOperationException("Owned placement window ended or changed.");
    }
    public static void Move(System.Diagnostics.Process process,IntPtr window,int x,int y,int width,int height,bool resize) {
        Check(process,window);
        Rect current=Bounds(window,false);
        resize=resize && (current.Right-current.Left!=width || current.Bottom-current.Top!=height);
        if(!resize && current.Left==x && current.Top==y) return;
        if(!SetWindowPos(window,IntPtr.Zero,x,y,width,height,0x14u | (resize ? 0u : 1u)))
            throw new InvalidOperationException("Native owned-window placement failed.");
    }
}
'@
    function Get-Ordered {
        foreach ($target in $targets) {
            if ($target.Process.HasExited) { continue }
            foreach ($window in [WinArrange]::Windows([uint32]$target.Id)) {
                [WinArrange]::Check($target.Process,$window)
                [pscustomobject]@{Role=$target.Role;Slot=$target.Slot;Process=$target.Process;Hwnd=$window}
            }
        }
    }
    function Set-Placement {
        $work=Get-CoopSecondaryWorkArea
        $windows=@(Get-Ordered)
        if (-not $windows.Count) { throw 'No live owned windows available; no successful placement.' }
        $sizes=@()
        foreach ($window in $windows) {
            $r=[WinArrange]::Bounds($window.Hwnd,$false); $width=$r.Right-$r.Left; $height=$r.Bottom-$r.Top
            if ($ClientW -gt 0) {
                $c=[WinArrange]::Bounds($window.Hwnd,$true)
                $chromeW=$width-($c.Right-$c.Left); $chromeH=$height-($c.Bottom-$c.Top)
                if ($chromeW -lt 0 -or $chromeH -lt 0) { throw 'Unrecognized owned window chrome.' }
                $width=$ClientW+$chromeW; $height=$ClientH+$chromeH
            }
            $sizes+=[pscustomobject]@{Window=$window;Width=$width;Height=$height}
        }
        $slotWidth=($sizes | Measure-Object Width -Maximum).Maximum
        $slotHeight=($sizes | Measure-Object Height -Maximum).Maximum
        $columns=if ($JoinPid -gt 0) {2} else {1}
        # Validate the entire reserved layout before moving any target.
        [void](Get-CoopWindowPlacement -Work $work -Slot 0 -Columns $columns -Width $slotWidth -Height $slotHeight -Gap $GapPx)
        foreach ($size in $sizes) {
            $window=$size.Window
            $placement=Get-CoopWindowPlacement -Work $work -Slot $window.Slot -Columns $columns -Width $slotWidth -Height $slotHeight -Gap $GapPx
            [WinArrange]::Check($window.Process,$window.Hwnd)
            [WinArrange]::Move($window.Process,$window.Hwnd,$placement.X,$placement.Y,$size.Width,$size.Height,($ClientW -gt 0))
        }
    }
    $deadline=[DateTime]::UtcNow.AddSeconds([Math]::Max(0,$TimeoutSec))
    do {
        [void](Get-CoopSecondaryWorkArea)
        $windows=@(Get-Ordered)
        $found=@($windows | ForEach-Object {$_.Process.Id} | Select-Object -Unique)
        if ($found.Count -eq $targets.Count) { break }
        if ([DateTime]::UtcNow -ge $deadline) { break }
        Start-Sleep -Milliseconds 250
    } while ($true)
    Set-Placement
    $until=[DateTime]::UtcNow.AddSeconds([Math]::Max(0,$RepeatSec))
    while ([DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 500; Set-Placement }
    Write-Output 'Arranged exact owned window(s) on the live non-primary work area.'
} finally { foreach ($target in $targets) { $target.Process.Dispose() } }
