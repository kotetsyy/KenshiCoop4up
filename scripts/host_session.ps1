<#
.SYNOPSIS
  Host side of a remote (internet) co-op test session: pre-flight checklist,
  launch the host client with the agreed scenario, collect host.log, and print
  the analyze_run.ps1 command to judge the session once the friend's results
  zip arrives.

.DESCRIPTION
  Pairs with the kit built by scripts\make_remote_kit.ps1 (the friend runs
  friend_join.ps1 from it). The DLL deployed here MUST be the same build as the
  kit's (the protocol-version handshake rejects a mismatch by design - rebuild
  and re-send the kit rather than mixing builds).

  Scenario mode: the host runs the compiled scenario and self-exits like an
  automated run (generous 10-min backstop for slow remote loads). Free play
  (-Scenario free): no scenario, no self-exit; close the window to end.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario coop_presence

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario free -Save squad1

.EXAMPLE
  # Steam P2P transport: no UPnP, port forwarding, public IPs or guest allowlist.
  # Friends join with -HostSteamId set to YOUR SteamID (printed after launch).
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario free -Save squad1 -Transport steam
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    # Save to load (defaults from the scenario manifest; must match the kit's).
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [switch]$SkipBuild,
    [string]$NativeBin = "",
    [int]$FreePlayMinutes = 0,
    # Skip the automatic UPnP router mapping (manual port forward only).
    [switch]$NoUpnp,
    [ValidateSet("udp", "steam")]
    [string]$Transport = "udp"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'kit_preflight.ps1')

$useSteam = ($Transport -eq "steam")

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
$manifest = Get-ScenarioManifest

$isFree = ($Scenario -eq "free" -or $Scenario -eq "")
if ($isFree) { $Scenario = "" }
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }
if ($Save -eq "" -and -not $isFree -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $Save = $manifest.Scenarios[$Scenario].Save
}
if ($Save -eq "" -and -not $isFree) { throw "Pass -Save for an automated scenario." }
$entry = $null
if (-not $isFree) {
    if (-not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }
    $entry = $manifest.Scenarios[$Scenario]
}
$prof = $manifest.Profiles.remote
$seconds = if ($isFree) { $FreePlayMinutes * 60 } elseif ($entry.ContainsKey('Seconds')) { [int]$entry.Seconds } else { 150 }
$killGraceSec = if ($entry -and $entry.ContainsKey('KillGraceSec')) { [int]$entry.KillGraceSec } else { [int]$prof.KillGraceSec }
$requireResult = $null -ne $entry
$HostDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($HostDir)
$upnp = $null

$runState = New-CoopRunState -GameDirs @($HostDir) -SaveNames @($Save,'coopresume') -PreserveSaves:$isFree
try {
Clear-CoopRunEnvironment -State $runState
if (-not $SkipBuild) {
    Write-Host "=== build + deploy ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($HostDir) -NativeBin $NativeBin
    Write-Host ""
    Write-Host "NOTE: if you rebuilt since making the kit, REBUILD THE KIT too"
    Write-Host "      (scripts\make_remote_kit.ps1) - the protocol handshake rejects mixed builds."
}

# ---- Pre-flight checklist -------------------------------------------------------
$upnp = $null
if ($useSteam) {
    Write-Host ""
    Write-Host "=================== REMOTE SESSION PRE-FLIGHT (STEAM P2P) ==================="
    Write-Host " Transport: Steam P2P - no router setup, no port forwarding, no public IPs."
    Write-Host " 1. Both machines: Steam RUNNING and ONLINE (not offline mode)."
    Write-Host " 2. No guest allowlist is required; guests use the host's SteamID."
    Write-Host " 3. Your own SteamID is printed below once the game is up - read it to the"
    Write-Host "    friend; they run: friend_join.ps1 -HostSteamId <your id>"
    Write-Host " 4. Session plan: start with coop_presence, then escalate through the"
    Write-Host "    smoke set (npc_sync, combat_kill, inv_bidir, world_weapon_drop)."
    Write-Host "============================================================================="
    Write-Host ""
} else {
. (Join-Path $scriptDir "upnp_portmap.ps1")

if (-not $NoUpnp) {
    Write-Host ""
    Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
    $upnp = Add-UpnpMapping -Port $Port
}

$publicIp = "(unknown)"
try {
    $publicIp = (Invoke-WebRequest -Uri "https://api.ipify.org" -UseBasicParsing -TimeoutSec 5).Content.Trim()
} catch {}
Write-Host ""
Write-Host "=================== REMOTE SESSION PRE-FLIGHT ==================="
if ($null -ne $upnp -and $upnp.Ok) {
    Write-Host " 1. Router: compatible UPnP mapping accepted (UDP $Port -> $($upnp.LanIp))."
    Write-Host "    Only a mapping created by this run is removed at session end."
    if ($upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp) {
        Write-Host "    NOTE: router external IP $($upnp.ExternalIp) != internet-visible $publicIp -"
        Write-Host "    likely carrier-grade NAT; the connection may fail regardless of forwarding."
    }
} else {
    if ($null -ne $upnp) { Write-Host " 1. Automatic UPnP mapping FAILED ($($upnp.Error))." }
    else                 { Write-Host " 1. Automatic UPnP mapping skipped (-NoUpnp)." }
    Write-Host "    Router: forward UDP port $Port to this machine."
}
Write-Host " 2. Windows Firewall: allow inbound UDP $Port (or kenshi_x64.exe)."
Write-Host "    One-time (admin): netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$Port"
Write-Host " 3. Tell your friend your public IP: $publicIp"
Write-Host "    Friend runs: powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp $publicIp"
Write-Host " 4. Session plan: start with coop_presence, then escalate through the"
Write-Host "    smoke set (npc_sync, combat_kill, inv_bidir, world_weapon_drop)."
Write-Host "=================================================================="
Write-Host ""
}

# ---- Launch host -----------------------------------------------------------------
$stamp  = (Get-Date -Format "yyyyMMdd_HHmmss") + '-' + [Guid]::NewGuid().ToString('N')
$outDir = Join-Path $repoRoot "tools\test-runs\remote_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$hostLog = Join-Path $outDir "host.log"

if ($entry) { [void](Set-CoopDiagEnv -Entry $entry) }
$env:KENSHICOOP_AUTOCONNECT = '1'
$env:KENSHICOOP_MODE         = "host"
$env:KENSHICOOP_IP           = "0.0.0.0"
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $Save
$env:KENSHICOOP_LOG          = $hostLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = if ($entry) { $entry.Setup } else { "" }
$env:KENSHICOOP_TEST_SECONDS = "$seconds"
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Peer-ready arming: the host's scenario clock starts when the friend's stream
# arrives, so scripted actions never fire before the friend is in-game.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "$($prof.ArmTimeoutMs)"
if ($useSteam) {
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "0"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

    Write-Host "Launching HOST (save '$Save', scenario '$Scenario', port $Port) ..."
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath (Join-Path $HostDir 'kenshi_x64.exe') -WorkDir $HostDir -TimeoutSec $prof.StartTimeoutSec -NativeOutput $NativeBin
    $out | ForEach-Object { Write-Host "    $_" }
    $gamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
    if ($gamePid -eq 0) { throw "Host failed to get past the launcher." }
    $game = $runState.Processes[$gamePid]
    $runDeadline = if ($seconds -gt 0) { Get-CoopRunDeadline -Process $game -Seconds $seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $prof.ArmTimeoutMs -KillGraceSec $killGraceSec } else { [DateTime]::MaxValue }

    if ($useSteam) {
        Show-MySteamId
        Write-Host 'F2 displays the active native Steam identity/status; share that identity with the joining player.'
        Write-Warning 'Actual two-account Steam P2P remains unverified; this runner does not certify it.'
    }

    Write-Host ""
    Write-Host "Host is up (PID $gamePid). Waiting for it to exit (scenario self-exit / you close it) ..."
    Wait-CoopOwnedExit -Process $game -Deadline $runDeadline
if (-not (Test-Path -LiteralPath $hostLog) -or -not (Select-String -LiteralPath $hostLog -Pattern 'gameplay started' -Quiet)) { throw 'No current-run gameplay evidence.' }
if ($requireResult -and (Test-ScenarioResultPass -File $hostLog -Label host -Required $true -Scenario $Scenario) -ne 'PASS') { throw 'Scenario incomplete or failed; exit alone is not success.' }
if (Test-Path -LiteralPath (Join-Path $HostDir 'kenshi_info.log')) { Copy-Item -LiteralPath (Join-Path $HostDir 'kenshi_info.log') -Destination (Join-Path $outDir 'host_engine.log') }

Write-Host ""
Write-Host "== Session over =="
Write-Host "  host log: $hostLog"
Write-Host ""
Write-Host "When the friend's results zip arrives, extract join.log into $outDir and judge:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -RunDir $outDir -Scenario $(if ($isFree) { '<scenario>' } else { $Scenario })"
} finally {
    try { if ($upnp -and ($upnp.Owned -or $upnp.Unresolved)) { Remove-UpnpMapping -Mapping $upnp } }
    finally { Restore-CoopRunState $runState }
}
