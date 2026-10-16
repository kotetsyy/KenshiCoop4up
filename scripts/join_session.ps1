<#
.SYNOPSIS
  Your side of the real remote session (friend-HOSTS topology): launch the
  local JOIN client at the friend's public IP with remote-profile timeouts,
  collect join.log + screenshots, and print the analyze_run.ps1 command to
  judge once the friend's results zip arrives.

.DESCRIPTION
  Pairs with the host-role kit (make_remote_kit.ps1 -Role host -> the friend
  runs friend_host.ps1). Build lockstep matters: the DLL deployed locally must
  be the same build as the kit's (the protocol handshake rejects mixed builds).

  Scenario mode runs the compiled scenario and self-exits; free play
  (-Scenario free) keeps running until you close the window.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostIp 203.0.113.7

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostIp 203.0.113.7 -Scenario free -Save squad1

.EXAMPLE
  # Steam P2P transport: no IPs or port forwarding. -HostSteamId is the host's
  # steamid64 (or short friend code); HOST needs no guest allowlist.
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostSteamId 76561198000000000 -Scenario free -Save squad1
#>
[CmdletBinding()]
param(
    [string]$HostIp = "",
    # Steam P2P transport: the HOST's steamid64 (or short friend code). When set,
    # the connection is brokered by Steam (NAT punch / Valve relay) and -HostIp
    # is ignored entirely.
    [string]$HostSteamId = "",
    [string]$Scenario = "coop_presence",
    # Save to load (defaults from the scenario manifest; must match the kit's).
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [switch]$SkipBuild,
    [string]$NativeBin = "",
    [int]$FreePlayMinutes = 0
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'kit_preflight.ps1')

# Short Steam friend codes are steamid64 minus the account-universe base.
function ConvertTo-SteamId64([string]$id) {
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}

$useSteam = ($HostSteamId -ne "")
if (-not $useSteam -and $HostIp -eq "") { throw "Pass -HostIp <ip> (UDP) or -HostSteamId <steamid64> (Steam P2P)." }

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
$shotAnchor = if ($entry -and $entry.ContainsKey('ShotAnchorJoin')) { $entry.ShotAnchorJoin } else { 'SCENARIO RECV' }
$shotDelay = if ($entry -and $entry.ContainsKey('ShotDelaySec')) { [int]$entry.ShotDelaySec } else { 2 }
$shotWait = if ($entry -and $entry.ContainsKey('ShotWaitSec')) { [int]$entry.ShotWaitSec } else { [int]$prof.JoinAnchorTimeoutSec }
$shotRequired = $entry -and $entry.ContainsKey('ShotAnchorJoin')
$requireResult = $null -ne $entry
$JoinDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($JoinDir)

$runState = New-CoopRunState -GameDirs @($JoinDir) -SaveNames @($Save,'coopresume') -PreserveSaves:$isFree
try {
Clear-CoopRunEnvironment -State $runState -PreserveCredentials
Set-CoopPrivateCredentials -Mode join -UseSteam $useSteam -GameDirs @($JoinDir)
if (-not $SkipBuild) {
    Write-Host "=== build + deploy ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($JoinDir) -NativeBin $NativeBin
    Write-Host ""
    Write-Host "NOTE: if you rebuilt since sending the kit, REBUILD + RESEND the kit"
    Write-Host "      (make_remote_kit.ps1 -Role host) - the handshake rejects mixed builds."
}

$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe" }

$prof = $manifest.Profiles["remote"]
$stamp  = (Get-Date -Format "yyyyMMdd_HHmmss") + '-' + [Guid]::NewGuid().ToString('N')
$outDir = Join-Path $repoRoot "tools\test-runs\session_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$joinLog = Join-Path $outDir "join.log"
if ($entry) { [void](Set-CoopDiagEnv -Entry $entry) }
$env:KENSHICOOP_AUTOCONNECT = '1'

$env:KENSHICOOP_MODE         = "join"
$env:KENSHICOOP_IP           = $HostIp
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $Save
$env:KENSHICOOP_LOG          = $joinLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = ""
$env:KENSHICOOP_TEST_SECONDS = "$seconds"
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
$env:KENSHICOOP_ARM_TIMEOUT_MS     = "$($prof.ArmTimeoutMs)"
if ($useSteam) {
    $hostId64 = ConvertTo-SteamId64 $HostSteamId
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$hostId64"
    Write-Host "Transport: STEAM P2P -> host SteamID $hostId64 (no IP / port forwarding)."
    Write-Host "The host uses -Transport steam; no guest allowlist is required."
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}
Set-CoopPrivateCredentials -Mode join -UseSteam $useSteam -GameDirs @($JoinDir)

$target = if ($useSteam) { "steam:$(ConvertTo-SteamId64 $HostSteamId)" } else { "${HostIp}:$Port" }
Write-Host "Launching JOIN -> $target (save '$Save', scenario '$Scenario') ..."
$out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath $joinExe -WorkDir $JoinDir -TimeoutSec $prof.StartTimeoutSec -NativeOutput $NativeBin
$out | ForEach-Object { Write-Host "    $_" }
$gamePid = 0
$line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
if ($gamePid -eq 0) { throw "Join failed to get past the launcher." }

$game = $runState.Processes[$gamePid]
$runDeadline = if ($seconds -gt 0) { Get-CoopRunDeadline -Process $game -Seconds $seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $prof.ArmTimeoutMs -KillGraceSec $killGraceSec } else { [DateTime]::MaxValue }

if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern "peer connected" -TimeoutSec 300) {
    Write-Host "CONNECTED to the host."
} elseif ($useSteam) {
    Write-Warning "No connection after 5 min - is the host listening with -Transport steam and Steam online on both sides?"
} else {
    Write-Warning "No connection after 5 min - is the host up + port-forwarded?"
}

if ($Scenario -ne "") {
    if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern $shotAnchor -TimeoutSec $shotWait) {
        try {
            if ([DateTime]::UtcNow.AddSeconds($shotDelay) -ge $runDeadline -or $game.HasExited) { throw 'Game/deadline ended before capture.' }
            Start-Sleep -Seconds $shotDelay
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") `
                -ProcessId $gamePid -ProcessStartTicks ($game.StartTime.ToUniversalTime().Ticks) -Out (Join-Path $outDir "join.png") -Frames 5 -IntervalMs 16
            if ($LASTEXITCODE -ne 0) { throw 'Screenshot helper failed.' }
        } catch {
            if ($shotRequired) { throw }
            Write-Warning "Screenshot evidence unavailable: $($_.Exception.Message)"
        }
    }
}
Wait-CoopOwnedExit -Process $game -Deadline $runDeadline
if (-not (Test-Path -LiteralPath $joinLog) -or -not (Select-String -LiteralPath $joinLog -Pattern 'gameplay started' -Quiet)) { throw 'No current-run gameplay evidence.' }
if ($requireResult -and (Test-ScenarioResultPass -File $joinLog -Label join -Required $true -Scenario $Scenario) -ne 'PASS') { throw 'Scenario incomplete or failed; exit alone is not success.' }
if ($shotRequired -and -not (Test-Path -LiteralPath (Join-Path $outDir 'join.png'))) { throw 'Required scenario capture is missing.' }
if (Test-Path -LiteralPath (Join-Path $JoinDir 'kenshi_info.log')) { Copy-Item -LiteralPath (Join-Path $JoinDir 'kenshi_info.log') -Destination (Join-Path $outDir 'join_engine.log') }

Write-Host ""
Write-Host "== Session over =="
Write-Host "  join log: $joinLog"
Write-Host ""
Write-Host "When the friend's results zip arrives, extract host.log into $outDir and judge:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -RunDir $outDir -Scenario $(if ($isFree) { '<scenario>' } else { $Scenario })"
} finally {
    Restore-CoopRunState $runState
}
