# Shared preflight helpers for the friend kit scripts (friend_host.ps1 /
# friend_join.ps1 dot-source this). Every check exists because a real user hit
# the failure: RE_Kenshi missing = the plugin silently never loads; Steam
# closed = the P2P transport can't broker; Mark-of-the-Web on a downloaded
# zip = scripts/DLL blocked; fullscreen = alt-tab pain and unreliable
# screenshots. Fail fast with the exact fix instead of a silent dead session.

# PS7 parents can leave a PS5.1 child searching incompatible builtin modules.
# Use this engine's builtins first while preserving external module paths.
$coopEngineModules=Join-Path $PSHOME 'Modules'
$coopEnginePrefix=$coopEngineModules+[IO.Path]::PathSeparator
if($env:PSModulePath -ine $coopEngineModules -and -not($env:PSModulePath -and $env:PSModulePath.StartsWith($coopEnginePrefix,[StringComparison]::OrdinalIgnoreCase))){
    $env:PSModulePath=$coopEnginePrefix+$env:PSModulePath
}
Import-Module (Join-Path $coopEngineModules 'Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop

function Invoke-KitUnblock {
    # Strip Mark-of-the-Web from everything in the kit (zips downloaded from
    # the internet tag every extracted file; a tagged DLL can be refused by
    # the loader). Harmless when nothing is tagged.
    param([string]$KitDir)
    try {
        Get-ChildItem -Path $KitDir -Recurse -File -ErrorAction SilentlyContinue |
            Unblock-File -ErrorAction SilentlyContinue
    } catch {}
}

function Test-CoopPrereqs {
    # Throws (stopping the script) only for problems that guarantee a dead
    # session; anything survivable is a warning.
    param(
        [string]$KenshiDir,
        [bool]$UseSteam
    )
    Write-Host "Checking your setup ..."

    # RE_Kenshi: its loader DLL sits in the Kenshi install root. Without it
    # the co-op plugin is never loaded and the game runs vanilla.
    $rek = (Test-Path (Join-Path $KenshiDir "RE_Kenshi.dll")) -or
           (Test-Path (Join-Path $KenshiDir "RE_Kenshi")) -or
           (Test-Path (Join-Path $KenshiDir "dinput8.dll"))
    if (-not $rek) {
        throw ("RE_Kenshi not found in '$KenshiDir'. It is required (it loads the " +
               "co-op plugin into the game). Install it from " +
               "https://www.nexusmods.com/kenshi/mods/847 and run this again.")
    }
    Write-Host "  [ok] RE_Kenshi detected."

    if ($UseSteam) {
        $steam = Get-Process -Name steam -ErrorAction SilentlyContinue
        if ($null -eq $steam) {
            throw ("Steam is not running. The connection goes through Steam, so start " +
                   "Steam, log in (online, not offline mode), and run this again.")
        }
        Write-Host "  [ok] Steam is running (make sure you are ONLINE, not in offline mode)."
    }

    # Windowed mode: only warn - the session still works fullscreen, it is
    # just awkward (alt-tab to read this window) and screenshots can fail.
    # Kenshi's launcher only writes a fullscreen line when it is enabled, so
    # no line = windowed = fine.
    $cfg = Join-Path $KenshiDir "settings.cfg"
    if (Test-Path $cfg) {
        $fs = Select-String -Path $cfg -Pattern '^\s*full\s*screen\s*=\s*1' -ErrorAction SilentlyContinue |
              Select-Object -First 1
        if ($null -ne $fs) {
            Write-Warning "Kenshi appears to be set to FULL SCREEN. Windowed mode is strongly"
            Write-Warning "recommended: launch Kenshi once, Options > Video > un-check Full Screen."
        } else {
            Write-Host "  [ok] Kenshi video mode looks windowed."
        }
    }
}

function Get-MySteamId {
    # The logged-in user's 32-bit account id IS the Steam friend code.
    # Registry (live while Steam runs) first; fall back to the most recent
    # login in Steam's loginusers.vdf (works with Steam closed). Returns
    # @{ FriendCode; SteamId64 } or $null if neither source pans out.
    try {
        $a = (Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction SilentlyContinue).ActiveUser
        if ($a -and $a -ne 0) {
            return @{ FriendCode = [uint64]$a; SteamId64 = [uint64]$a + 76561197960265728 }
        }
    } catch {}
    try {
        $steamPath = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
        if ($steamPath) {
            $vdf = Join-Path $steamPath "config\loginusers.vdf"
            if (Test-Path $vdf) {
                $raw = Get-Content $vdf -Raw
                $m = [regex]::Match($raw, '"(\d{17})"\s*\{[^}]*"MostRecent"\s*"1"',
                                    [System.Text.RegularExpressions.RegexOptions]::Singleline)
                if ($m.Success) {
                    $id64 = [uint64]$m.Groups[1].Value
                    return @{ FriendCode = $id64 - 76561197960265728; SteamId64 = $id64 }
                }
            }
        }
    } catch {}
    return $null
}

function Show-MySteamId {
    # Solves the ID-exchange chicken-and-egg: print YOUR code before anyone
    # has to type the other player's, so both players can just read their
    # code off this screen to each other.
    $me = Get-MySteamId
    if ($null -ne $me) {
        Write-Host ""
        Write-Host (">>> YOUR Steam friend code: $($me.FriendCode)") -ForegroundColor Green
        Write-Host ">>> Send it to the other player - they enter it on their side." -ForegroundColor Green
        Write-Host ""
    } else {
        Write-Host ""
        Write-Host "(Could not detect your Steam friend code - find it in Steam under"
        Write-Host " Friends > Add a Friend, and send it to the other player.)"
        Write-Host ""
    }
}

function Wait-PluginLoaded {
    # The co-op plugin creates its log file the moment RE_Kenshi loads it
    # (before any menu/save). If the game is up but the log never appears,
    # RE_Kenshi did not load the plugin - tell the user exactly where to look
    # instead of letting them stare at a vanilla game.
    param(
        [string]$LogPath,
        [string]$KenshiDir,
        [int]$TimeoutSec = 120
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $LogPath) {
            Write-Host "  [ok] Co-op plugin loaded."
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning "The co-op plugin has not started (no log after $TimeoutSec s)."
    Write-Warning "The game may be running WITHOUT co-op. Check that RE_Kenshi is working:"
    Write-Warning "  $KenshiDir\RE_Kenshi_log.txt should mention 'KenshiCoop'."
    Write-Warning "If RE_Kenshi is missing, install it: https://www.nexusmods.com/kenshi/mods/847"
    return $false
}

# Private harness helpers. Runtime assets and credential material have separate
# roots: neither a host signing profile nor a client invitation belongs in a kit.
function Assert-CoopRuntime {
    param([string]$ModDir)
    $names = @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')
    foreach ($name in $names) {
        $path = Join-Path $ModDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
            throw "Complete native runtime required; missing/empty $name."
        }
    }
    $loader = Get-Content -LiteralPath (Join-Path $ModDir 'RE_Kenshi.json') -Raw | ConvertFrom-Json
    if (@($loader.Plugins).Count -ne 1 -or $loader.Plugins[0] -ne 'KenshiCoop.dll') {
        throw 'RE_Kenshi.json must load only KenshiCoop.dll.'
    }
}

function Assert-CoopPrivatePath {
    param([string]$Path, [string[]]$GameDirs)
    if (-not $Path -or $Path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)') { throw 'Credential path must be an absolute file path.' }
    $full = [IO.Path]::GetFullPath($Path)
    $ancestor = $full
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Credential paths must not traverse reparse points.'
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    foreach ($dir in (@($GameDirs) + @((Join-Path $env:LOCALAPPDATA 'kenshi\save')))) {
        if (-not $dir) { continue }
        $root = [IO.Path]::GetFullPath($dir).TrimEnd('\') + '\'
        if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Private credentials must be outside game and kit trees.'
        }
    }
    return $full
}

function Set-CoopPrivateCredentials {
    param([string]$Mode, [bool]$UseSteam, [string[]]$GameDirs, [string]$ProfilePath = $env:KENSHICOOP_UDP_PROFILE_PATH, [string]$InviteFile = $env:KENSHICOOP_UDP_INVITE_FILE)
    $env:KENSHICOOP_UDP_PROFILE_PATH = $null
    $env:KENSHICOOP_UDP_INVITE_FILE = $null
    if ($UseSteam) {
        if ($Mode -eq 'host') {
            $env:KENSHICOOP_STEAM_PEER = '0'
        }
        return
    }
    if ($Mode -eq 'host') {
        if (-not $ProfilePath) { $ProfilePath = Join-Path $env:LOCALAPPDATA 'KenshiCoop\private\udp-host.credential' }
        $env:KENSHICOOP_UDP_PROFILE_PATH = Assert-CoopPrivatePath $ProfilePath $GameDirs
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $env:KENSHICOOP_UDP_PROFILE_PATH) | Out-Null
    } else {
        $env:KENSHICOOP_UDP_INVITE_FILE = Assert-CoopPrivatePath $InviteFile $GameDirs
        if (-not (Test-Path -LiteralPath $env:KENSHICOOP_UDP_INVITE_FILE -PathType Leaf)) { throw 'Import this client''s private UDP invitation file before joining.' }
        $size = (Get-Item -LiteralPath $env:KENSHICOOP_UDP_INVITE_FILE).Length
        if ($size -lt 6 -or $size -ge 4096) { throw 'Invalid invitation file size.' }
        $file = Get-Item -LiteralPath $env:KENSHICOOP_UDP_INVITE_FILE
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Invitation file cannot be a reparse point.' }
        $allowed = @([Security.Principal.WindowsIdentity]::GetCurrent().User.Value,'S-1-5-18')
        foreach ($rule in (Get-Acl -LiteralPath $file.FullName).Access) {
            if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin $allowed) {
                throw 'Invitation file must grant access only to this Windows user and SYSTEM.'
            }
        }
    }
}

function New-CoopUdpCredentials {
    param([string]$NativeBin, [string]$ProfilePath, [string]$InviteFile, [string[]]$GameDirs)
    # Validate BOTH paths before directory mutation or the real provider call.
    $profile = Assert-CoopPrivatePath $ProfilePath $GameDirs
    $invite = Assert-CoopPrivatePath $InviteFile $GameDirs
    if (Test-Path -LiteralPath $invite) { throw 'A new private invitation file is required.' }
    $tool = Join-Path $NativeBin 'transport_credentials.exe'
    $provider = Join-Path $NativeBin 'KenshiCoopNetUDP.dll'
    foreach ($file in @($tool,$provider)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Complete private native provisioning inputs required.' }
    }
    foreach ($dir in @((Split-Path -Parent $profile),(Split-Path -Parent $invite)) | Select-Object -Unique) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    & $tool --udp $provider --profile $profile --invite-file $invite | ForEach-Object { Write-Host "$_" }
    if ($LASTEXITCODE -ne 0) { throw 'Native credential provisioning failed.' }
    return @{ Profile=$profile; Invite=$invite }
}

function New-CoopLocalCredentials {
    param([string]$NativeBin, [string[]]$GameDirs)
    $root = Join-Path $env:LOCALAPPDATA ('KenshiCoop\private\harness-' + [Guid]::NewGuid().ToString('N'))
    try {
        $result = New-CoopUdpCredentials -NativeBin $NativeBin -ProfilePath (Join-Path $root 'host.credential') -InviteFile (Join-Path $root 'client.invite') -GameDirs $GameDirs
        return @{ Root=$root; Profile=$result.Profile; Invite=$result.Invite }
    } catch {
        if (Test-Path -LiteralPath $root) {
            Assert-CoopOwnedPath -Path $root -Recurse
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        throw
    }
}

function Get-CoopWindowPlacement {
    param($Work,[int]$Slot,[int]$Columns,[int]$Width,[int]$Height,[int]$Gap=20)
    if (-not $Work -or $Columns -lt 1 -or $Columns -gt 2 -or $Slot -lt 0 -or $Slot -ge $Columns -or $Width -le 0 -or $Height -le 0 -or $Gap -lt 0) { throw 'Invalid owned window layout.' }
    $total=$Columns*$Width+($Columns-1)*$Gap
    if ($total -gt $Work.Width -or $Height -gt $Work.Height) { throw 'Complete owned window layout does not fit the non-primary work area.' }
    return @{X=$Work.Left+[int][Math]::Floor(($Work.Width-$total)/2)+$Slot*($Width+$Gap);
        Y=$Work.Top+[int][Math]::Floor(($Work.Height-$Height)/2);Width=$Width;Height=$Height}
}

function Get-CoopOwnedDirectorySlot {
    param([string[]]$GameDirs,[string]$WorkDir,[int]$PeerCount)
    $owned=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkDir).TrimEnd('\')
    for ($index=0; $index -lt $GameDirs.Count; $index++) {
        if ($GameDirs[$index] -ieq $owned) { return $index+$PeerCount }
    }
    throw 'Owned snapshot required for this game directory.'
}

function Assert-CoopPeerInstallationDisjoint {
    param([string]$GameDir,[string]$PeerExecutable)
    if (-not $PeerExecutable) { throw 'Exempt peer installation cannot be established.' }
    $peerDir=Split-Path -Parent $PeerExecutable
    if ((Split-Path -Leaf $peerDir) -ieq 'RE_Kenshi') { $peerDir=Split-Path -Parent $peerDir }
    $peerDir=[IO.Path]::GetFullPath($peerDir).TrimEnd('\')
    $dir=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GameDir).TrimEnd('\')
    if ($dir -ieq $peerDir -or $dir.StartsWith($peerDir+'\',[StringComparison]::OrdinalIgnoreCase) -or
        $peerDir.StartsWith($dir+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Owned installation overlaps an exempt peer installation.' }
}

function Place-CoopOwnedWindows {
    param([Diagnostics.Process]$Process,[int]$Slot,[int]$Columns)
    $work=Get-CoopSecondaryWorkArea
    $placement=Get-CoopWindowPlacement -Work $work -Slot $Slot -Columns $Columns `
        -Width ([Math]::Min(1600,[Math]::Floor(($work.Width-20*($Columns+1))/$Columns))) -Height ([Math]::Min(900,$work.Height-40))
    [CoopHarnessWindow]::PlaceOwned($Process,$work,$placement.X,$placement.Y,$placement.Width,$placement.Height)
}

function Get-CoopSecondaryWorkArea {
    if (-not ('CoopHarnessWindow' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CoopHarnessWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct Area {
        public int Left, Top, Right, Bottom;
        public int Width { get { return Right-Left; } }
        public int Height { get { return Bottom-Top; } }
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo { public int Size; public Area Monitor, Work; public uint Flags; }
    private delegate bool MonitorCallback(IntPtr monitor,IntPtr dc,ref Area bounds,IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr dc,IntPtr clip,MonitorCallback callback,IntPtr data);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(IntPtr monitor,ref MonitorInfo info);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool MoveWindow(IntPtr window,int x,int y,int width,int height,bool redraw);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback,IntPtr data);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window,out Area rect);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    private delegate bool WindowCallback(IntPtr window,IntPtr data);
    public static void PlaceOwned(System.Diagnostics.Process process,Area work,int x,int y,int width,int height) {
        IntPtr held=process.Handle;
        if(process.HasExited) return;
        if(width<=0 || height<=0 || x<work.Left || y<work.Top || x+width>work.Right || y+height>work.Bottom)
            throw new InvalidOperationException("Owned window does not fit secondary work area.");
        Exception failure=null;
        EnumWindows(delegate(IntPtr window,IntPtr data) {
            try {
                uint owner; GetWindowThreadProcessId(window,out owner);
                if(owner!=(uint)process.Id || !IsWindowVisible(window) || IsIconic(window)) return true;
                GetWindowThreadProcessId(window,out owner);
                if(process.HasExited || owner!=(uint)process.Id) throw new InvalidOperationException("Owned window identity ended or changed.");
                Area current;
                if(!GetWindowRect(window,out current)) throw new InvalidOperationException("Owned window bounds unavailable.");
                if(current.Left==x && current.Top==y && current.Width==width && current.Height==height) return true;
                if(!MoveWindow(window,x,y,width,height,true)) throw new InvalidOperationException("Owned-window placement failed.");
                return true;
            } catch(Exception error) { failure=error; return false; }
        },IntPtr.Zero);
        if(failure!=null) throw failure;
    }
    private static readonly object QueryLock=new object();
    private static readonly int InfoSize=Marshal.SizeOf(typeof(MonitorInfo));
    private static readonly MonitorCallback Callback=FindSecondary;
    private static bool Found;
    private static Area Work;
    private static bool FindSecondary(IntPtr monitor,IntPtr dc,ref Area bounds,IntPtr data) {
        MonitorInfo info=new MonitorInfo(); info.Size=InfoSize;
        if(GetMonitorInfo(monitor,ref info) && (info.Flags & 1)==0) {
            Work=info.Work; Found=true; return false;
        }
        return true;
    }
    public static Area SecondaryWorkArea() {
        lock(QueryLock) {
            try { SetProcessDpiAwarenessContext((IntPtr)(-4)); } catch { SetProcessDPIAware(); }
            Found=false;
            EnumDisplayMonitors(IntPtr.Zero,IntPtr.Zero,Callback,IntPtr.Zero);
            if(!Found) throw new InvalidOperationException("A live non-primary monitor is required.");
            return Work;
        }
    }
}
'@
    }
    return [CoopHarnessWindow]::SecondaryWorkArea()
}

function Get-CoopManualGameDirs {
    param([string]$HostDir, [string]$JoinDir, [bool]$NoJoin)
    if ($NoJoin) { return @($HostDir) }
    return @($HostDir,$JoinDir)
}


function Clear-CoopRunEnvironment {
    param($State, [switch]$PreserveCredentials, [string[]]$PreserveVariables = @())
    if (-not $State) { throw 'An environment snapshot is required before clearing the run.' }
    foreach ($variable in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'KENSHICOOP_*' })) {
        [Environment]::SetEnvironmentVariable($variable.Name,[NullString]::Value,'Process')
    }
    $names=@($PreserveVariables) -ne $null
    if ($PreserveCredentials) { $names+=@('KENSHICOOP_UDP_PROFILE_PATH','KENSHICOOP_UDP_INVITE_FILE') }
    foreach ($name in $names) {
        if ($State.Environment.ContainsKey($name)) {
            [Environment]::SetEnvironmentVariable($name,[string]$State.Environment[$name],'Process')
        }
    }
}

function Assert-CoopOwnedPath {
    param([string]$Path, [switch]$Recurse)
    $full=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $ancestor=$full; $leaf=$true
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item=Get-Item -LiteralPath $ancestor -Force -ErrorAction Stop
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Owned paths must not traverse reparse points.' }
            if (-not $leaf -and -not $item.PSIsContainer) { throw 'Owned path ancestors must be directories.' }
        }
        $leaf=$false; $ancestor=[IO.Path]::GetDirectoryName($ancestor)
    }
    if ($Recurse -and (Test-Path -LiteralPath $full -PathType Container)) {
        foreach ($child in Get-ChildItem -LiteralPath $full -Force -ErrorAction Stop) {
            Assert-CoopOwnedPath -Path $child.FullName -Recurse
        }
    }
}

function Add-CoopRunBackup {
    param($State, [string]$Path)
    $ErrorActionPreference='Stop'
    if (-not $State -or -not $Path) { throw 'An owned snapshot and exact path are required.' }
    $Path=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    if (@($State.Backups | Where-Object { $_.Path -ieq $Path }).Count) { return }
    $backup=Join-Path $State.Root "$($State.Backups.Count)"
    Assert-CoopOwnedPath -Path $Path -Recurse
    Assert-CoopOwnedPath -Path $backup -Recurse
    $exists=Test-Path -LiteralPath $Path
    $directory=$false
    if ($exists) {
        $directory=(Get-Item -LiteralPath $Path -Force).PSIsContainer
        Copy-Item -LiteralPath $Path -Destination $backup -Recurse -Force -ErrorAction Stop
    }
    $State.Backups+=@{Path=$Path;Backup=$backup;Existed=$exists;Directory=$directory}
}

function Wait-CoopOwnedExit {
    param([Diagnostics.Process]$Process, [DateTime]$Deadline)
    $ErrorActionPreference='Stop'
    if (-not $Process) { throw 'A held owned process is required.' }
    $null=$Process.Handle
    if (-not $Process.HasExited) {
        if ($Deadline -eq [DateTime]::MaxValue) {
            Wait-Process -InputObject $Process -ErrorAction Stop
        } else {
            $remaining=[Math]::Ceiling(($Deadline-[DateTime]::UtcNow).TotalSeconds)
            try {
                if ($remaining -le 0) { throw 'Owned run deadline already expired.' }
                Wait-Process -InputObject $Process -Timeout ([int][Math]::Min([int]::MaxValue,$remaining)) -ErrorAction Stop
            } catch {
                if (-not $Process.HasExited) {
                    $Process.Kill()
                    if (-not $Process.WaitForExit(10000)) { throw "Owned process $($Process.Id) did not close after timeout." }
                    throw "Owned process $($Process.Id) exceeded its run deadline."
                }
            }
        }
    }
    if ($Deadline -ne [DateTime]::MaxValue -and $Process.ExitTime.ToUniversalTime() -gt $Deadline) {
        throw "Owned process $($Process.Id) exited after its run deadline."
    }
    if ($Process.ExitCode -ne 0) { throw "Owned process $($Process.Id) exited with code $($Process.ExitCode)." }
}

function Wait-CoopOwnedLogLine {
    param([Diagnostics.Process]$Process, [DateTime]$Deadline, [string]$File,
          [string]$Pattern, [int]$TimeoutSec)
    $ErrorActionPreference='Stop'
    if (-not $Process) { throw 'A held owned process is required for log observation.' }
    $null=$Process.Handle
    $until=[DateTime]::UtcNow.AddSeconds([Math]::Max(0,$TimeoutSec))
    if ($Deadline -lt $until) { $until=$Deadline }
    do {
        if ((Test-Path -LiteralPath $File -PathType Leaf) -and
            (Select-String -LiteralPath $File -Pattern $Pattern -Encoding UTF8 -Quiet)) { return $true }
        if ($Process.HasExited -or [DateTime]::UtcNow -ge $until) { return $false }
        Start-Sleep -Milliseconds 500
    } while ($true)
}

function Get-CoopRunDeadline {
    param([Diagnostics.Process]$Process, [int]$Seconds, [int]$StartTimeoutSec,
          [int]$ArmTimeoutMs = 0, [int]$KillGraceSec = 30)
    if (-not $Process) { throw 'An owned process is required for the run deadline.' }
    return $Process.StartTime.ToUniversalTime().AddSeconds(
        [Math]::Max(0,$StartTimeoutSec) + [Math]::Max(0,$ArmTimeoutMs)/1000.0 +
        [Math]::Max(0,$Seconds) + [Math]::Max(0,$KillGraceSec))
}

function New-CoopRunState {
    param([string[]]$GameDirs, [string[]]$SaveNames = @(), [switch]$PreserveSaves,
          [string[]]$AllowedPeerTokens = @(), [switch]$PreserveUserSaveRoot)
    $ErrorActionPreference='Stop'
    foreach ($name in $SaveNames) {
        if ($name -and ($name -match '[\\/:]' -or $name -eq '.' -or $name -eq '..')) { throw 'Invalid owned fixture save name.' }
    }
    $peers=@{}
    foreach ($token in $AllowedPeerTokens) {
        if ($token -notmatch '^([1-9][0-9]*):([1-9][0-9]*)$') { throw 'A peer exemption requires PID:start-time-UTC-ticks.' }
        $peers[[int]$Matches[1]]=[long]$Matches[2]
    }
    $livePeers=@()
    foreach ($game in @(Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue)) {
        $null=$game.Handle
        if (-not $peers.ContainsKey($game.Id) -or $game.StartTime.ToUniversalTime().Ticks -ne $peers[$game.Id]) {
            throw 'A user-owned Kenshi session exists; close it yourself before this harness run.'
        }
        if ($game.HasExited) { throw 'A peer exemption ended during admission.' }
        $livePeers+=$game
    }
    if ($livePeers.Count -ne $peers.Count) { throw 'A peer exemption is stale or does not identify a live Kenshi process.' }
    if ($PreserveUserSaveRoot -and -not $livePeers.Count) { throw 'Shared user-save ownership requires a live exempt peer and its outer snapshot.' }
    $GameDirs=@($GameDirs | Where-Object { $_ } | ForEach-Object {
        $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($_).TrimEnd('\')
    } | Select-Object -Unique)
    foreach ($dir in $GameDirs) {
        Assert-CoopOwnedPath -Path $dir
        foreach ($peer in $livePeers) {
            Assert-CoopPeerInstallationDisjoint -GameDir $dir -PeerExecutable $peer.Path
        }
    }
    if ($GameDirs.Count+$livePeers.Count -gt 2) { throw 'At most two owned/exempt installation slots are supported.' }
    $screen = @{ WorkingArea=(Get-CoopSecondaryWorkArea) }
    $root = Join-Path $env:LOCALAPPDATA ('KenshiCoop\private\restore-' + [Guid]::NewGuid().ToString('N'))
    Assert-CoopOwnedPath -Path $root
    New-Item -ItemType Directory -Path $root | Out-Null
    $state = @{ Root=$root; Processes=@{}; Backups=@(); Screen=$screen; Environment=@{}; GameDirs=$GameDirs;
        Peers=$livePeers; WindowSlots=@{}; CleanupOwned=$false; ExpectedWindows=[Math]::Max(1,$GameDirs.Count+$livePeers.Count) }
    Get-ChildItem Env: | Where-Object { $_.Name -like 'KENSHICOOP_*' } | ForEach-Object { $state.Environment[$_.Name]=$_.Value }
    $paths = @()
    foreach ($dir in $GameDirs | Select-Object -Unique) {
        if (-not $dir) { continue }
        foreach ($name in @('settings.cfg','kenshi.cfg','controls.cfg','data\mods.cfg','RE_Kenshi.json')) { $paths += Join-Path $dir $name }
        foreach ($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json','coop_config.json')) {
            $paths += Join-Path $dir "mods\KenshiCoop\$name"
        }
    }
    if (-not $PreserveSaves) {
        foreach ($name in $SaveNames | Select-Object -Unique) {
            if (-not $name) { continue }
            if (-not $PreserveUserSaveRoot) { $paths += Join-Path $env:LOCALAPPDATA "kenshi\save\$name" }
            foreach ($dir in $GameDirs) { if ($dir) { $paths += Join-Path $dir "save\$name" } }
        }
    }
    try {
        foreach ($path in $paths | Select-Object -Unique) { Add-CoopRunBackup -State $state -Path $path }
    } catch {
        Assert-CoopOwnedPath -Path $root -Recurse
        Remove-Item -LiteralPath $root -Recurse -Force
        throw
    }
    return $state
}

function Start-CoopOwnedGame {
    param($State, [string]$ScriptDir, [string]$ExePath, [string]$WorkDir, [int]$TimeoutSec, [string]$NativeOutput = '')
    if (Test-Path -LiteralPath (Join-Path $WorkDir 'mods\KenshiCoop\.kenshicoop-deploy.pending')) { throw 'Pending runtime deployment requires recovery before launch.' }
    Assert-CoopRuntime (Join-Path $WorkDir 'mods\KenshiCoop')
    if (Test-Path -LiteralPath (Join-Path $ScriptDir 'NativeBuild.ps1')) {
        . (Join-Path $ScriptDir 'RuntimeDistribution.ps1') -Configuration Harness -NativeBin $NativeOutput
        $expected = Get-CoopRuntimeFiles -Configuration Harness -NativeBin $NativeOutput
        foreach ($name in $expected.Keys) {
            if ((Get-FileHash -LiteralPath (Join-Path $WorkDir "mods\KenshiCoop\$name") -Algorithm SHA256).Hash -cne
                (Get-FileHash -LiteralPath $expected[$name] -Algorithm SHA256).Hash) {
                throw "Installed native runtime does not match the pinned Harness inputs: $name"
            }
        }
    }
    $ownedSlot=Get-CoopOwnedDirectorySlot -GameDirs $State.GameDirs -WorkDir $WorkDir -PeerCount $State.Peers.Count
    $windowSetter = Join-Path $ScriptDir 'set_video_mode.ps1'
    if (-not (Test-Path -LiteralPath $windowSetter -PathType Leaf)) { throw 'Required windowed prelaunch helper is missing.' }
    $workArea = Get-CoopSecondaryWorkArea
    $columns = $State.ExpectedWindows
    $renderWidth = [Math]::Min(1600,[Math]::Floor(($workArea.Width-20*($columns+1))/$columns))
    $renderHeight = [Math]::Min(900,$workArea.Height-40)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $windowSetter -Width $renderWidth -Height $renderHeight -HostDir $WorkDir -JoinDir $WorkDir
    if ($LASTEXITCODE -ne 0) { throw 'Windowed non-primary launch preparation failed.' }
    $oldWatcher = $State.Watcher
    # Both launcher callbacks run synchronously before this function returns.
    # A dynamic-module closure loses helpers defined in the calling script.
    $callback = {
        param($game)
        if ($game.HasExited -or $game.ProcessName -ine 'kenshi_x64') { return }
        $startTicks=$game.StartTime.ToUniversalTime().Ticks
        $previousSlot=$State.WindowSlots[$game.Id]
        if (-not $previousSlot -or $previousSlot.StartTicks -ne $startTicks) {
            $State.WindowSlots[$game.Id]=@{StartTicks=$startTicks;Slot=$ownedSlot}
        }
        $State.Processes[$game.Id] = $game
        # Resolve live topology even before a launcher has a window handle.
        $work = [CoopHarnessWindow]::SecondaryWorkArea()
        if ($game.HasExited) { return }
        # Hold the original kernel handle so PID reuse cannot change ownership.
        $null = $game.Handle
        Place-CoopOwnedWindows -Process $game -Slot $State.WindowSlots[$game.Id].Slot -Columns $State.ExpectedWindows
    }
    & (Join-Path $ScriptDir 'start_kenshi.ps1') -ExePath $ExePath -WorkDir $WorkDir -TimeoutSec $TimeoutSec -OnOwnedProcess $callback -BeforeLauncherClick $callback 6>&1
    foreach ($game in @($State.Processes.Values)) { & $callback $game }
    # Kenshi recenters at world load. Pin only owned PID incarnations.
    $targets = @($State.Processes.Values | Where-Object { -not $_.HasExited -and $_.ProcessName -ieq 'kenshi_x64' })
    if ($targets.Count) {
        $ids = ($targets | ForEach-Object { "$($_.Id)='$($_.StartTime.ToUniversalTime().Ticks)'" }) -join ';'
        $slots = ($targets | ForEach-Object { "$($_.Id)='$($State.WindowSlots[$_.Id].Slot)'" }) -join ';'
        $readyName = 'Local\KenshiCoop.Pin.' + [Guid]::NewGuid().ToString('N')
        $ready = New-Object Threading.EventWaitHandle($false,[Threading.EventResetMode]::ManualReset,$readyName)
        $pin = @"
`$ErrorActionPreference='Stop'
. '$( (Join-Path $ScriptDir 'kit_preflight.ps1').Replace("'","''") )'
`$ready=[Threading.EventWaitHandle]::OpenExisting('$readyName')
`$targets=@{$ids}
`$slots=@{$slots}
`$held=@{}
function Stop-Owned {
    foreach(`$candidate in @(`$held.Values)) {
        if(-not `$candidate.HasExited) { `$candidate.Kill() }
    }
}
try {
    [void](Get-CoopSecondaryWorkArea)
    foreach(`$id in @(`$targets.Keys)) {
        `$p=Get-Process -Id `$id -ErrorAction SilentlyContinue
        if(-not `$p) { `$targets.Remove(`$id); continue }
        `$null=`$p.Handle
        if(`$p.HasExited -or `$p.StartTime.ToUniversalTime().Ticks -ne [long]`$targets[`$id]) { `$p.Dispose(); `$targets.Remove(`$id); continue }
        `$held[`$id]=`$p
    }
    while(`$targets.Count) {
        `$w=Get-CoopSecondaryWorkArea
        `$columns=$($State.ExpectedWindows)
        foreach(`$id in @(`$targets.Keys)){
            `$p=`$held[`$id]
            if(`$p.HasExited) { `$p.Dispose(); `$held.Remove(`$id); `$targets.Remove(`$id); continue }
            Place-CoopOwnedWindows -Process `$p -Slot ([int]`$slots[`$id]) -Columns `$columns
        }
        [void]`$ready.Set()
        Start-Sleep -Milliseconds 500
    }
} catch { Stop-Owned; exit 1 } finally { foreach(`$p in @(`$held.Values)) { `$p.Dispose() }; `$ready.Dispose() }
"@
        try {
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($pin))
            $watcher = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-EncodedCommand',$encoded)
            $State.Processes[$watcher.Id]=$watcher
            if (-not $ready.WaitOne(15000)) { throw 'Replacement non-primary watcher did not become active.' }
            $State.Watcher=$watcher
            if ($oldWatcher -and -not $oldWatcher.HasExited) { $oldWatcher.Kill(); $oldWatcher.WaitForExit() }
        } finally { $ready.Dispose() }
    }
}

function Restore-CoopRunState {
    param($State)
    $ErrorActionPreference='Stop'
    if (-not $State) { return }
    $State.CleanupOwned=$false
    $failures=@(); $closed=@{}
    try {
        while ($true) {
            try {
                if ($State.Processes.Count) {
                    foreach ($native in (Get-CimInstance Win32_Process)) {
                        if ($State.Processes.ContainsKey([int]$native.ParentProcessId) -and -not $State.Processes.ContainsKey([int]$native.ProcessId)) {
                            $parent=$State.Processes[[int]$native.ParentProcessId]
                            $child=Get-Process -Id $native.ProcessId -ErrorAction SilentlyContinue
                            if ($child) {
                                try {
                                    $null=$child.Handle
                                    if (-not $child.HasExited -and $child.StartTime -ge $parent.StartTime -and
                                        (-not $parent.HasExited -or $child.StartTime -le $parent.ExitTime)) { $State.Processes[$child.Id]=$child }
                                } catch { $failures+='owned child tracking' }
                            }
                        }
                    }
                }
            } catch { $failures+='owned tree discovery' }
            $remaining=@($State.Processes.Keys | Where-Object { -not $closed.ContainsKey($_) })
            if (-not $remaining.Count) { break }
            foreach ($id in $remaining) {
                $game=$State.Processes[$id]
                try {
                    $null=$game.Handle
                    if (-not $game.HasExited) {
                        $game.CloseMainWindow() | Out-Null
                        if (-not $game.WaitForExit(10000)) {
                            $game.Kill()
                            if (-not $game.WaitForExit(10000)) { throw 'Owned process did not close.' }
                        }
                    }
                    if (-not $game.HasExited) { throw 'Owned process remains live.' }
                } catch { $failures+='owned process cleanup' }
                $closed[$id]=$true
            }
        }
        if ($failures.Count) {
            throw "Owned process cleanup failed; no runtime/config/save files restored. Preserved backups: $($State.Root)"
        }
        $State.CleanupOwned=$true
        foreach ($entry in $State.Backups) {
            $releaseWait=[Diagnostics.Stopwatch]::StartNew()
            while ($true) {
                try {
                    Assert-CoopOwnedPath -Path $entry.Path -Recurse
                    if ($entry.Existed) {
                        Assert-CoopOwnedPath -Path $entry.Backup -Recurse
                        $source=Get-Item -LiteralPath $entry.Backup -Force -ErrorAction Stop
                        if ($entry.ContainsKey('Directory') -and $source.PSIsContainer -ne $entry.Directory) { throw 'Backup type changed.' }
                        # Establish readable recovery data before deleting the current destination.
                        $files=if ($source.PSIsContainer) { @(Get-ChildItem -LiteralPath $entry.Backup -Recurse -File -Force) } else { @($source) }
                        foreach ($file in $files) {
                            $stream=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
                            try { $buffer=New-Object byte[] 65536; while ($stream.Read($buffer,0,$buffer.Length) -gt 0) {} }
                            finally { $stream.Dispose() }
                        }
                    }
                    Assert-CoopOwnedPath -Path $entry.Path -Recurse
                    if (Test-Path -LiteralPath $entry.Path) { Remove-Item -LiteralPath $entry.Path -Recurse -Force }
                    if ($entry.Existed) {
                        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $entry.Path) | Out-Null
                        Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Recurse -Force
                    }
                    break
                } catch {
                    # Owned processes are closed, but a native image/file scan can
                    # still deny deletion briefly. Never discard recovery data.
                    $code=$_.Exception.HResult -band 0xFFFF
                    if ($code -notin @(5,32,33) -or $releaseWait.ElapsedMilliseconds -ge 5000) {
                        $failures+="$($entry.Path): $($_.Exception.Message)"; break
                    }
                    Start-Sleep -Milliseconds 50
                }
            }
        }
        if ($failures.Count) { throw "Restore failed; preserved backups in $($State.Root): $($failures -join ', ')" }
        Assert-CoopOwnedPath -Path $State.Root -Recurse
        Remove-Item -LiteralPath $State.Root -Recurse -Force
    } finally {
        foreach ($variable in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'KENSHICOOP_*' })) {
            [Environment]::SetEnvironmentVariable($variable.Name,[NullString]::Value,'Process')
        }
        foreach ($name in $State.Environment.Keys) { [Environment]::SetEnvironmentVariable($name,[string]$State.Environment[$name],'Process') }
    }
}

function Restore-CoopHarnessSession {
    param($State, [string]$CredentialRoot = '', $Proxy = $null)
    try {
        if ($null -ne $Proxy -and -not $Proxy.HasExited) { $Proxy.Kill(); $Proxy.WaitForExit() }
    } finally {
        try { Restore-CoopRunState $State }
        finally {
            if ($State -and $State.CleanupOwned -and $CredentialRoot -and (Test-Path -LiteralPath $CredentialRoot)) {
                Assert-CoopOwnedPath -Path $CredentialRoot -Recurse
                Remove-Item -LiteralPath $CredentialRoot -Recurse -Force -ErrorAction Stop
            }
        }
    }
}


function Get-CoopHarnessRuntime {
    param([string]$RepoRoot, [string]$NativeBin = '')
    . (Join-Path $RepoRoot 'scripts\RuntimeDistribution.ps1') -Configuration Harness -NativeBin $NativeBin
    $files = Get-CoopRuntimeFiles -Configuration Harness -NativeBin $NativeBin
    return @($files.Values)
}

function Set-CoopSaveLocation {
    param([string]$GameDir, [ValidateSet(0,1)][int]$UserLocation)
    $settings=Join-Path $GameDir 'settings.cfg'
    Assert-CoopOwnedPath -Path $settings
    $encoding=[Text.Encoding]::GetEncoding(28591)
    $text=$encoding.GetString([IO.File]::ReadAllBytes($settings))
    if ($text.Contains([string][char]0)) { throw 'Unrecognized save settings encoding; no bytes changed.' }
    $pattern='(?m)^((?:\xEF\xBB\xBF)?[ \t]*User save location[ \t]*=[ \t]*)[01]([ \t]*\r?)$'
    if ([regex]::Matches($text,$pattern).Count -ne 1) { throw 'Native save location must have one recognized settings.cfg key.' }
    $selected=[regex]::Replace($text,$pattern,('${1}'+$UserLocation+'${2}'))
    if ($selected -cne $text) { [IO.File]::WriteAllBytes($settings,$encoding.GetBytes($selected)) }
}

function Sync-CoopNamedSave {
    param([string]$Save, [string]$HostDir, [string]$JoinDir)
    if (-not $Save -or $Save -match '[\\/:]' -or $Save -eq '.' -or $Save -eq '..') { throw 'Sync requires one named save.' }
    $settings = Join-Path $HostDir 'settings.cfg'
    $location = @(Get-Content -LiteralPath $settings | Where-Object { $_ -match '^\s*User save location\s*=\s*[01]\s*$' })
    if ($location.Count -ne 1) { throw 'Host native save location cannot be established from settings.cfg.' }
    $hostSaveRoot = if ($location[0] -match '=\s*1\s*$') {
        Join-Path $env:LOCALAPPDATA 'kenshi\save'
    } else {
        Join-Path $HostDir 'save'
    }
    $source = Join-Path $hostSaveRoot $Save
    if (-not (Test-Path -LiteralPath (Join-Path $source 'quick.save'))) { throw 'Host named save is missing.' }
    $destination = Join-Path $JoinDir "save\$Save"
    $source=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($source).TrimEnd('\')
    $destination=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($destination).TrimEnd('\')
    Assert-CoopOwnedPath -Path $source -Recurse
    Assert-CoopOwnedPath -Path $destination -Recurse
    if ($source -ine $destination) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    }
    Set-CoopSaveLocation -GameDir $JoinDir -UserLocation 0
}

function Install-CoopHarnessRuntime {
    param([string]$RepoRoot, [string[]]$GameDirs, [string]$NativeBin = '')
    . (Join-Path $RepoRoot 'scripts\RuntimeDistribution.ps1') -Configuration Harness -NativeBin $NativeBin
    $files = Get-CoopRuntimeFiles -Configuration Harness -NativeBin $NativeBin
    Install-CoopRuntime -Files $files -Installations $GameDirs
}

function Assert-CoopKit {
    param([string]$KitDir, $Metadata)
    $mod = Join-Path $KitDir 'mod'
    Assert-CoopRuntime $mod
    $names = @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')
    if (@(Get-ChildItem -LiteralPath $mod -Force).Count -ne 6) { throw 'Private kit mod directory must contain exactly six runtime files.' }
    foreach ($name in $names) {
        $pin = $Metadata.runtimeSha256.PSObject.Properties[$name]
        if (-not $pin -or "$($pin.Value)" -notmatch '^[0-9a-fA-F]{64}$' -or
            (Get-FileHash -LiteralPath (Join-Path $mod $name) -Algorithm SHA256).Hash -ine "$($pin.Value)") { throw "Private kit runtime hash mismatch: $name" }
    }
}
