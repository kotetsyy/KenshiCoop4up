<#
.SYNOPSIS
  Capture a single window to a PNG, by process id (preferred) or window-title match.

.DESCRIPTION
  Used by the KenshiCoop test runner so an agent can visually evaluate the host
  and join clients. Capture is OS-level (no game code), so it works even when the
  window is unfocused or overlapped by the other client.

  Primary path: Win32 PrintWindow with PW_RENDERFULLCONTENT, which grabs a
  window's real client content without requiring it to be foreground. If that
  comes back blank (some D3D windows), it falls back to a BitBlt of the on-screen
  window rectangle.

  Both Kenshi clients are the same kenshi_x64.exe image, so -ProcessId is the
  reliable discriminator between host and join.

.PARAMETER ProcessId
  PID of the target process, paired with ProcessStartTicks to reject PID reuse.
  The largest visible top-level window owned by that exact process is captured.

.PARAMETER ProcessStartTicks
  Process.StartTime.ToUniversalTime().Ticks for the owned target.

.PARAMETER WindowTitle
  Substring to match against window titles (used only when -ProcessId is absent).

.PARAMETER Out
  Output PNG path. Parent directory is created if needed.

.EXAMPLE
  $p = Get-Process -Id 12345
  .\scripts\screenshot.ps1 -ProcessId $p.Id -ProcessStartTicks $p.StartTime.ToUniversalTime().Ticks -Out host.png
#>
[CmdletBinding()]
param(
    [int]$ProcessId = 0,
    [long]$ProcessStartTicks = 0,
    [string]$WindowTitle = "",
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Frames = 1,
    [int]$IntervalMs = 16
)

$ErrorActionPreference = "Stop"
if ($ProcessId -gt 0) {
    if ($ProcessStartTicks -le 0) { throw 'PID capture requires the owned process start-time UTC ticks.' }
    $script:CaptureProcess=Get-Process -Id $ProcessId -ErrorAction Stop
    $null=$script:CaptureProcess.Handle
    if ($script:CaptureProcess.HasExited -or $script:CaptureProcess.StartTime.ToUniversalTime().Ticks -ne $ProcessStartTicks) { throw 'Screenshot process identity changed.' }
}

$references=if ($PSVersionTable.PSVersion.Major -ge 6) {
    @('System.Drawing.Common','System.Drawing.Primitives','System.Diagnostics.Process','System.Threading.Thread','System.Runtime.InteropServices','System.ComponentModel.Primitives')
} else { @('System','System.Core','System.Drawing') }
if ($PSVersionTable.PSVersion.Major -ge 6) {
    $references+=@('System.Private.Windows.GdiPlus','System.Private.Windows.Core') |
        Where-Object { Test-Path -LiteralPath (Join-Path $PSHOME "$_.dll") -PathType Leaf }
}
Add-Type -ReferencedAssemblies $references -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class WinCap
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr hdc, int x, int y, int w, int h, IntPtr src, int sx, int sy, int rop);

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);

    public const uint PW_RENDERFULLCONTENT = 2;
    public const int  SRCCOPY = 0x00CC0020;

    // Make this process DPI-aware so GetClientRect / PrintWindow operate in real
    // (physical) pixels. Without this, on a scaled display (e.g. 175%) the client
    // rect comes back in logical pixels (1280 -> ~731), the bitmap is undersized,
    // and PrintWindow renders only the top-left of the real frame - a crop. Must
    // run before any window/DC calls. Prefer per-monitor-v2; fall back to legacy.
    public static void EnableDpiAwareness()
    {
        try { if (SetProcessDpiAwarenessContext((IntPtr)(-4)) != IntPtr.Zero) return; } catch {}
        try { SetProcessDPIAware(); } catch {}
    }

    // Largest visible, non-minimized top-level window owned by 'pid'.
    public static IntPtr FindMainWindow(uint pid)
    {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            if (!IsWindowVisible(h) || IsIconic(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    // Returns true if the bitmap has any non-black pixel (sampled), i.e. not blank.
    public static bool LooksNonBlank(Bitmap bmp)
    {
        int stepX = Math.Max(1, bmp.Width / 40);
        int stepY = Math.Max(1, bmp.Height / 40);
        for (int y = 0; y < bmp.Height; y += stepY)
            for (int x = 0; x < bmp.Width; x += stepX)
            {
                Color c = bmp.GetPixel(x, y);
                if (c.R > 8 || c.G > 8 || c.B > 8) return true;
            }
        return false;
    }

    // Capture window client content; PrintWindow first, BitBlt fallback.
    public static Bitmap Capture(IntPtr hWnd)
    {
        RECT cr;
        if (!GetClientRect(hWnd, out cr)) throw new InvalidOperationException("No capture client bounds.");
        int w = cr.Right - cr.Left, h = cr.Bottom - cr.Top;
        if (w <= 0 || h <= 0) throw new InvalidOperationException("Empty capture client.");
        Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        try {
            bool ok;
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                try { ok = PrintWindow(hWnd, hdc, PW_RENDERFULLCONTENT | 1); }
                finally { g.ReleaseHdc(hdc); }
            }
            if (ok && LooksNonBlank(bmp)) { Bitmap result=bmp; bmp=null; return result; }
        } finally { if (bmp!=null) bmp.Dispose(); }
        bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        try {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                try {
                    IntPtr src = GetDC(hWnd);
                    if (src==IntPtr.Zero) throw new InvalidOperationException("Capture source DC unavailable.");
                    try {
                        if (!BitBlt(hdc, 0, 0, w, h, src, 0, 0, SRCCOPY))
                            throw new InvalidOperationException("Capture GDI copy failed.");
                    } finally { ReleaseDC(hWnd, src); }
                } finally { g.ReleaseHdc(hdc); }
            }
            Bitmap result=bmp; bmp=null; return result;
        } finally { if (bmp!=null) bmp.Dispose(); }
    }

    // Capture a short burst of 'frames' bitmaps spaced ~intervalMs apart. Frames
    // are held in memory so PNG encoding (done by the caller afterwards) doesn't
    // bloat the inter-frame timing. A Stopwatch paces each capture toward its
    // target offset; we only sleep when ahead of schedule, so if a single
    // PrintWindow costs more than intervalMs the frames just space out naturally
    // rather than being captured artificially fast.
    public static Bitmap[] CaptureSequence(IntPtr hWnd, System.Diagnostics.Process process, int frames, int intervalMs)
    {
        if (frames < 1) frames = 1;
        Bitmap[] outp = new Bitmap[frames];
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try {
            for (int i = 0; i < frames; i++) {
                IntPtr held=process.Handle; uint owner;
                GetWindowThreadProcessId(hWnd,out owner);
                if (process.HasExited || owner!=(uint)process.Id || !IsWindowVisible(hWnd) || IsIconic(hWnd))
                    throw new InvalidOperationException("Capture window identity is no longer visible/owned.");
                long target = (long)i * intervalMs;
                long now = sw.ElapsedMilliseconds;
                if (now < target) System.Threading.Thread.Sleep((int)(target - now));
                GetWindowThreadProcessId(hWnd,out owner);
                if(process.HasExited || owner!=(uint)process.Id) throw new InvalidOperationException("Capture identity ended between frames.");
                outp[i] = Capture(hWnd);
            }
            return outp;
        } catch { foreach (Bitmap frame in outp) if(frame!=null) frame.Dispose(); throw; }
    }
}
"@

function Resolve-Hwnd {
    if ($ProcessId -gt 0) {
        if ($script:CaptureProcess.HasExited) { throw 'Screenshot process identity ended.' }
    } elseif ($WindowTitle -ne '') {
        $matches=@(Get-Process | Where-Object {
            $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle.IndexOf($WindowTitle,[StringComparison]::OrdinalIgnoreCase) -ge 0
        })
        if ($matches.Count -ne 1) { throw "Expected one window matching '$WindowTitle', found $($matches.Count). Use an exact process identity." }
        $script:CaptureProcess=$matches[0]
        $null=$script:CaptureProcess.Handle
    } else { throw 'Provide an exact process identity or an unambiguous WindowTitle.' }
    $h=[WinCap]::FindMainWindow([uint32]$script:CaptureProcess.Id)
    if ($h -eq [IntPtr]::Zero) { throw "No visible window found for owned PID $($script:CaptureProcess.Id)." }
    return $h
}

function Assert-CaptureTarget {
    [uint32]$owner=0
    [void][WinCap]::GetWindowThreadProcessId($hwnd,[ref]$owner)
    if ($script:CaptureProcess.HasExited -or $owner -ne $script:CaptureProcess.Id -or
        -not [WinCap]::IsWindowVisible($hwnd) -or [WinCap]::IsIconic($hwnd)) {
        throw 'Owned screenshot process/window ended or changed.'
    }
}

# Must happen before any GetClientRect/GetWindowRect/PrintWindow calls.
[WinCap]::EnableDpiAwareness()

$hwnd = Resolve-Hwnd

$Out=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

if ($Frames -lt 1) { $Frames = 1 }

Assert-CaptureTarget
$bmps = [WinCap]::CaptureSequence($hwnd, $script:CaptureProcess, $Frames, $IntervalMs)
try {
    Assert-CaptureTarget
    if ($Frames -le 1) {
        $bmp = $bmps[0]
        if ($null -eq $bmp) { throw 'Capture failed (zero-size window).' }
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Host "Saved screenshot: $Out"
    } else {
        $dirp = [IO.Path]::GetDirectoryName($Out)
        $base = [IO.Path]::GetFileNameWithoutExtension($Out)
        $ext  = [IO.Path]::GetExtension($Out)
        for ($i = 0; $i -lt $bmps.Length; $i++) {
            $bmp = $bmps[$i]
            if ($null -eq $bmp) { throw "Frame $($i + 1) capture failed (zero-size window)." }
            $framePath = Join-Path $dirp ("{0}_{1}{2}" -f $base, ($i + 1), $ext)
            $bmp.Save($framePath, [System.Drawing.Imaging.ImageFormat]::Png)
            if ($i -eq 0) { $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png) }
            Write-Host "Saved frame: $framePath"
        }
    }
} finally {
    foreach ($bmp in $bmps) { if ($null -ne $bmp) { $bmp.Dispose() } }
}
