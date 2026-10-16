# start_kenshi.ps1 - launch a Kenshi instance and get it past the launcher.
#
# Kenshi's startup shows a Win32/MFC graphics launcher (Video settings / Mods /
# Changelog, with an OK button) and blocks there until OK is clicked. RE_Kenshi
# and our plugin only load AFTER that click. The root kenshi_x64.exe is just a
# loader that relaunches the real game as a separate process named "Kenshi_x64",
# so we also have to discover that real process (a different PID).
#
# Ownership is established from held process handles and parent lifetimes.
# GAMEPID is emitted only after OK closes and the real game window exists.
#
# Env vars (KENSHICOOP_*) must be set by the caller before invoking; they are
# inherited by the launched process and propagate to the relaunched game.

param(
    [string]$ExePath = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\kenshi_x64.exe",
    [string]$WorkDir = "",
    [int]$TimeoutSec = 90,
    [scriptblock]$BeforeLauncherClick = $null,
    [scriptblock]$OnOwnedProcess = $null
)

$ErrorActionPreference = "Stop"
if (-not $WorkDir) { $WorkDir = Split-Path $ExePath }
$ExePath=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExePath)
$WorkDir=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkDir)
if (Test-Path -LiteralPath (Join-Path $WorkDir 'mods\KenshiCoop\.kenshicoop-deploy.pending')) { throw 'Pending runtime deployment requires recovery before launch.' }

if (-not ('KenLauncher' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class KenLauncher {
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Auto, SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out UIntPtr result);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint BM_CLICK = 0x00F5;
  static IntPtr found;
  public static IntPtr FindButton(IntPtr top, string text) {
    found = IntPtr.Zero;
    EnumChildWindows(top, (h,p) => {
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "Button") {
        var t = new StringBuilder(64); GetWindowText(h, t, 64);
        // Kenshi's accept control ID is stable across launcher languages.
        if (t.ToString() == text || (text == "OK" && GetDlgCtrlID(h) == 1003)) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static bool ClickButton(IntPtr top, string text, System.Diagnostics.Process process, uint timeout) {
    IntPtr held=process.Handle;
    uint topOwner; GetWindowThreadProcessId(top,out topOwner);
    if(process.HasExited || topOwner!=(uint)process.Id) return false;
    IntPtr b = FindButton(top, text);
    uint owner; GetWindowThreadProcessId(b,out owner);
    if (b == IntPtr.Zero || owner!=(uint)process.Id || process.HasExited || timeout==0) return false;
    UIntPtr result;
    return SendMessageTimeout(b, BM_CLICK, IntPtr.Zero, IntPtr.Zero, 2, timeout, out result)!=IntPtr.Zero;
  }
}
"@
}

Write-Host ("Launching: " + $ExePath)
$bootstrap = Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir -PassThru
$owned=@{$bootstrap.Id=$bootstrap}
try {
$null=$bootstrap.Handle
if ($OnOwnedProcess) { & $OnOwnedProcess $bootstrap | Out-Null }
$deadline=$bootstrap.StartTime.ToUniversalTime().AddSeconds($TimeoutSec)

function Get-OwnedKenshiProcesses {
    foreach ($native in (Get-CimInstance Win32_Process -Filter "Name='kenshi_x64.exe'")) {
        $id=[int]$native.ProcessId
        $parent=$owned[[int]$native.ParentProcessId]
        if (-not $parent) { continue }
        $child=Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $child) { continue }
        try {
            $null=$child.Handle
            if ($child.StartTime -ge $parent.StartTime -and
                (-not $parent.HasExited -or $child.StartTime -le $parent.ExitTime)) {
                if (-not $owned.ContainsKey($id) -or $owned[$id].HasExited) {
                    $owned[$id]=$child
                }
            }
        } catch {
            if (-not $child.HasExited) { throw 'Cannot verify owned Kenshi child lifetime.' }
        }
    }
    $live=@($owned.Values | Where-Object { -not $_.HasExited -and $_.ProcessName -ieq 'kenshi_x64' })
    foreach ($process in $live) {
        $process.Refresh()
        if ($OnOwnedProcess) { & $OnOwnedProcess $process | Out-Null }
    }
    return $live
}

$launcher=$null
while ([DateTime]::UtcNow -lt $deadline) {
    foreach ($candidate in @(Get-OwnedKenshiProcesses)) {
        if ($candidate.MainWindowHandle -ne [IntPtr]::Zero -and
            [KenLauncher]::FindButton($candidate.MainWindowHandle,'OK') -ne [IntPtr]::Zero) {
            $launcher=$candidate
            break
        }
    }
    if ($launcher) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $launcher) { throw 'Timed out waiting for the owned Kenshi launcher.' }
if ($BeforeLauncherClick) { & $BeforeLauncherClick $launcher | Out-Null }
$launcherWindow=$launcher.MainWindowHandle
[uint32]$windowOwner=0
[void][KenLauncher]::GetWindowThreadProcessId($launcherWindow,[ref]$windowOwner)
if ($launcher.HasExited -or $windowOwner -ne $launcher.Id) { throw 'Owned launcher window ended or changed.' }
Write-Host ("Launcher up: PID=" + $launcher.Id + " '" + $launcher.MainWindowTitle + "' - clicking OK")
$remainingMs=[Math]::Min([uint32]::MaxValue,[Math]::Floor(($deadline-[DateTime]::UtcNow).TotalMilliseconds))
if ($remainingMs -le 0 -or -not [KenLauncher]::ClickButton($launcherWindow,'OK',$launcher,[uint32]$remainingMs)) { throw 'Owned launcher OK click failed or timed out.' }

$game=$null
while ([DateTime]::UtcNow -lt $deadline) {
    $nativeExe=Join-Path $WorkDir 'RE_Kenshi\kenshi_x64.exe'
    $expected=if (Test-Path -LiteralPath $nativeExe -PathType Leaf) { $nativeExe } else { $ExePath }
    foreach ($candidate in @(Get-OwnedKenshiProcesses)) {
        if ($candidate.Path -ieq $expected -and $candidate.MainWindowHandle -ne [IntPtr]::Zero -and
            [KenLauncher]::FindButton($candidate.MainWindowHandle,'OK') -eq [IntPtr]::Zero) {
            $gameWindow=$candidate.MainWindowHandle
            [uint32]$gameOwner=0
            [void][KenLauncher]::GetWindowThreadProcessId($gameWindow,[ref]$gameOwner)
            if ($candidate.HasExited -or $gameOwner -ne $candidate.Id) { continue }
            $game=$candidate
            break
        }
    }
    if ($game) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $game) { throw 'Owned launcher accepted OK, but the actual game window did not become ready.' }
Write-Output ("GAMEPID=" + $game.Id)
} catch {
    $launchFailure=$_
    if (-not $OnOwnedProcess) {
        # Standalone failure owns only the held launch tree, never name-based games.
        try { [void](Get-OwnedKenshiProcesses) } catch { Write-Warning "Owned child discovery failed: $_" }
        foreach ($process in @($owned.Values)) {
            try {
                if (-not $process.HasExited) {
                    [void]$process.CloseMainWindow()
                    if (-not $process.WaitForExit(1000)) {
                        $process.Kill()
                        if (-not $process.WaitForExit(10000)) { throw 'Launched process did not close.' }
                    }
                }
            } catch { Write-Warning "Owned launcher cleanup failed: $_" }
        }
    }
    throw $launchFailure
}
