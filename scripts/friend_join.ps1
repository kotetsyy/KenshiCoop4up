<#
.SYNOPSIS
  The friend-side runner for a KenshiCoop remote test session. Ships INSIDE the
  remote kit (scripts\make_remote_kit.ps1); runs from the unzipped kit folder.

.DESCRIPTION
  One command:
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <host public IP>

  Steam P2P (no IPs, no port forwarding - the host tells you their SteamID):
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their steamid64>
  The host does not need your SteamID in advance; no guest allowlist is required.

  Installs the bundled mod + save into the local Kenshi, launches the game as
  the JOIN client pointed at the host, lets the agreed scenario run to its own
  self-exit (remote profile: generous timeouts), captures screenshots at the
  scenario anchor, and bundles log + screenshots + a small system report into
  KenshiCoop-results-<stamp>.zip for sending back.

  RESUMING a previous session (-Resume): after the first session, every save
  the HOST makes is streamed to you in-band (coordinated save, protocol 31)
  and committed as 'coopresume' in your save folder - no manual save copy
  needed ever again. -Resume skips the kit save install and loads that
  transferred save; the host resumes with the same name on their side:
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <ip> -Resume

  Judged later on the host side by scripts\analyze_run.ps1 - clock alignment
  between the two machines comes from the plugin's wire time-sync (CLOCKSYNC),
  so the friend's wall clock does NOT need to match the host's.
#>
[CmdletBinding()]
param(
    [string]$HostIp = "",
    # Steam P2P transport: the HOST's steamid64 (or short friend code). No IPs or
    # port forwarding needed; Steam brokers the connection. The host needs YOUR
    # SteamID on their side too.
    [string]$HostSteamId = "",
    # Kenshi install dir (auto-detected from the default Steam path when empty).
    [string]$KenshiDir = "",
    # Override the kit's default scenario ("" = kit default; "free" = no scenario,
    # normal co-op play with no self-exit).
    [string]$Scenario = "",
    [int]$Port = 0,
    # How long a free-play session runs before self-exit (0 = never).
    [int]$FreePlayMinutes = 0,
    # Resume the previous session: load the 'coopresume' save the host's
    # coordinated save streamed to this machine (protocol 31) instead of
    # installing the kit's baked save. Defaults to free play (no scenario).
    [switch]$Resume,
    # Save name the coordinated-save flow writes (must match the host's).
    [string[]]$OwnedPeerTokens = @(),
    [string]$RunToken = [Guid]::NewGuid().ToString('N'),
    [string]$ResumeSave = "coopresume"
)

$ErrorActionPreference = "Stop"
$kitDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Short Steam friend codes are steamid64 minus the account-universe base.
function ConvertTo-SteamId64([string]$id) {
    $id = "$id".Trim()
    if ($id -notmatch '^\d{1,17}$') {
        throw ("'$id' is not a Steam ID. Use the other player's friend code (Steam > " +
               "Friends > Add a Friend - the number at the top) or their 17-digit " +
               "SteamID64 (steamid.io converts profile URLs).")
    }
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}

# Preflight helpers (RE_Kenshi / Steam / Mark-of-the-Web / windowed checks).
. (Join-Path $kitDir "kit_preflight.ps1")
Invoke-KitUnblock -KitDir $kitDir

# ---- Kit defaults --------------------------------------------------------------
$kit = Get-Content (Join-Path $kitDir "kit.json") -Raw | ConvertFrom-Json
$save = $kit.save
if ($Resume) {
    # The save the host's coordinated save (protocol 31) streamed here last
    # session; resume = both sides load the identical file. Free play unless
    # a scenario is asked for explicitly.
    $save = $ResumeSave
    if ($Scenario -eq "") { $Scenario = "free" }
}
if ($Scenario -eq "") { $Scenario = $kit.scenario }
if ($Scenario -eq "free") { $Scenario = "" }
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }
if ($Port -eq 0) { $Port = [int]$kit.port }
if ($RunToken -notmatch '^[a-fA-F0-9]{32}$') { throw 'RunToken must be a GUID in N format.' }
Import-Module (Join-Path $kitDir 'CoopHarness.psm1') -Force
Import-Module (Join-Path $kitDir 'CoopOracles.psm1') -Force
$manifest = Import-PowerShellDataFile (Join-Path $kitDir 'scenarios.psd1')
$entry = $null
if ($Scenario) {
    if (-not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }
    $entry = $manifest.Scenarios[$Scenario]
    if ($Scenario -ne $kit.scenario -and $entry.Save -ne $save) { throw 'Scenario override requires a different fixture; assemble a matching kit.' }
}
$prof = $manifest.Profiles.remote
$seconds = if (-not $Scenario) { $FreePlayMinutes * 60 } elseif ($entry.ContainsKey('Seconds')) { [int]$entry.Seconds } else { 150 }
$killGraceSec = if ($entry -and $entry.ContainsKey('KillGraceSec')) { [int]$entry.KillGraceSec } else { [int]$prof.KillGraceSec }
$shotAnchor = if ($entry -and $entry.ContainsKey('ShotAnchorJoin')) { $entry.ShotAnchorJoin } else { 'SCENARIO RECV' }
$shotDelay = if ($entry -and $entry.ContainsKey('ShotDelaySec')) { [int]$entry.ShotDelaySec } else { 2 }
$shotWait = if ($entry -and $entry.ContainsKey('ShotWaitSec')) { [int]$entry.ShotWaitSec } else { [int]$prof.JoinAnchorTimeoutSec }
function Assert-CoopSharedFixture {
    param([string]$KitSave, [string]$SharedSave)
    foreach ($root in @($KitSave,$SharedSave)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'Parent-owned shared fixture is missing.' }
        Assert-CoopOwnedPath -Path $root -Recurse
        $items = @((Get-Item -LiteralPath $root -Force)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)
        if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Shared fixture identity must not traverse reparse points.' }
    }
    $expected = @(Get-ChildItem -LiteralPath $KitSave -Recurse -File -Force)
    $actual = @(Get-ChildItem -LiteralPath $SharedSave -Recurse -File -Force)
    if ($expected.Count -ne $actual.Count -or -not $expected.Count) { throw 'Parent-owned shared fixture file catalog differs from the kit.' }
    foreach ($file in $expected) {
        $relative = $file.FullName.Substring($KitSave.TrimEnd('\').Length).TrimStart('\')
        $target = Join-Path $SharedSave $relative
        if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) {
            throw "Parent-owned shared fixture differs from the kit: $relative"
        }
    }
}
$shotRequired = $entry -and $entry.ContainsKey('ShotAnchorJoin')
$requireResult = $null -ne $entry
# The kit can pre-bake the Steam transport (kit.json: transport + hostSteamId).
if ($HostSteamId -eq "" -and $kit.PSObject.Properties["hostSteamId"] -and "$($kit.hostSteamId)" -ne "" -and "$($kit.hostSteamId)" -ne "0") {
    $HostSteamId = "$($kit.hostSteamId)"
}
$useSteam = ($HostSteamId -ne "")
if ("$($kit.transport)" -eq 'steam' -and -not $useSteam) { throw 'Steam kit requires an explicit host Steam ID; no UDP fallback.' }
if ("$($kit.transport)" -eq 'udp' -and $useSteam) { throw 'UDP kit cannot switch to Steam.' }
# Print YOUR friend code first (read from Steam) so the two of you can swap
# codes right off this screen - the host needs YOURS as much as you need
# theirs. Shown even if the launch then stops for a missing host id.
if ($useSteam -or "$($kit.transport)" -eq "steam") { Show-MySteamId }
if (-not $useSteam -and $HostIp -eq "") { throw "Pass -HostIp <ip> (UDP) or -HostSteamId <steamid64> (Steam P2P)." }
# Validate the ID up front (clear error now beats a dead session later).
if ($useSteam) { [void](ConvertTo-SteamId64 $HostSteamId) }

# ---- Locate Kenshi ---------------------------------------------------------------
if ($KenshiDir -eq "") {
    foreach ($cand in @(
        "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
        "C:\Program Files\Steam\steamapps\common\Kenshi")) {
        if (Test-Path (Join-Path $cand "kenshi_x64.exe")) { $KenshiDir = $cand; break }
    }
}
if ($KenshiDir -eq "" -or -not (Test-Path (Join-Path $KenshiDir "kenshi_x64.exe"))) {
    throw "Kenshi not found. Pass -KenshiDir 'C:\path\to\Kenshi'."
}
$KenshiDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($KenshiDir)
Write-Host "Kenshi install: $KenshiDir"

Test-CoopPrereqs -KenshiDir $KenshiDir -UseSteam $useSteam

Assert-CoopKit -KitDir $kitDir -Metadata $kit
if (-not $Resume -and -not (Test-Path -LiteralPath (Join-Path $kitDir "save\$save\quick.save"))) { throw 'Complete fixture save required before installation.' }
$ownedSaveNames = if ($Scenario -ne '') { @($save,'coopresume') } elseif (-not $Resume) { @($save) } else { @() }
$runState = $null
$restored = $false
$failure = $null
$outDir = Join-Path $kitDir "results_$RunToken"
$completionFile = Join-Path $kitDir "complete_$RunToken.json"
if ((Test-Path -LiteralPath $outDir) -or (Test-Path -LiteralPath $completionFile)) { throw 'Run token has already been used.' }
New-Item -ItemType Directory -Path $outDir | Out-Null
try {
$runState = New-CoopRunState -GameDirs @($KenshiDir) -SaveNames $ownedSaveNames -PreserveSaves:($Resume -and $Scenario -eq '') -AllowedPeerTokens $OwnedPeerTokens -PreserveUserSaveRoot:($OwnedPeerTokens.Count -gt 0)
Clear-CoopRunEnvironment -State $runState
# ---- Install mod + save ------------------------------------------------------------
$modDst = Join-Path $KenshiDir "mods\KenshiCoop"
New-Item -ItemType Directory -Force -Path $modDst | Out-Null
foreach ($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')) { Copy-Item -LiteralPath (Join-Path "$kitDir\mod" $name) -Destination $modDst -Force }
Write-Host "Mod installed -> $modDst"

$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
New-Item -ItemType Directory -Force -Path $saveRoot | Out-Null
if ($Resume) {
    # No install: the save was DELIVERED by the host's coordinated save last
    # session (streamed in-band, CRC-verified, committed atomically). It must
    # already exist here or there is nothing to resume.
    $found = (Test-Path (Join-Path $saveRoot $save)) -or
             (Test-Path (Join-Path $KenshiDir "save\$save"))
    if (-not $found) {
        throw ("Resume save '$save' not found in '$saveRoot' or '$KenshiDir\save'. " +
               "Run at least one connected session first (the host's save is " +
               "streamed to this machine automatically), or drop -Resume.")
    }
    Write-Host "Resuming on transferred save '$save' (no kit save installed)."
} else {
    $saveDst = Join-Path $saveRoot $save
    $kitSave = Join-Path $kitDir "save\$save"
    if ($OwnedPeerTokens.Count) {
        # The outer owning rehearsal snapshots the shared root. Do not replace
        # or restore it while the exempt host may still be using it.
        Assert-CoopSharedFixture -KitSave $kitSave -SharedSave $saveDst
    } else {
        if (Test-Path -LiteralPath $saveDst) { Remove-Item -LiteralPath $saveDst -Recurse -Force }
        Copy-Item -LiteralPath $kitSave -Destination $saveDst -Recurse
    }
    $installSaveRoot = Join-Path $KenshiDir "save"
    New-Item -ItemType Directory -Path $installSaveRoot -Force | Out-Null
    $dst2 = Join-Path $installSaveRoot $save
    if (Test-Path -LiteralPath $dst2) { Remove-Item -LiteralPath $dst2 -Recurse -Force }
    Copy-Item -LiteralPath $kitSave -Destination $dst2 -Recurse
    Set-CoopSaveLocation -GameDir $KenshiDir -UserLocation 0
    Write-Host "Save '$save' installed."
}

# ---- Output folder ------------------------------------------------------------------
$stamp = $RunToken
$joinLog = Join-Path $outDir "join.log"

# ---- Env + launch --------------------------------------------------------------------
if ($entry) { [void](Set-CoopDiagEnv -Entry $entry) }
$env:KENSHICOOP_AUTOCONNECT = '1'
$env:KENSHICOOP_MODE         = "join"
$env:KENSHICOOP_IP           = $HostIp
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $save
$env:KENSHICOOP_LOG          = $joinLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = ""
$env:KENSHICOOP_TEST_SECONDS = "$seconds"
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Peer-ready arming: the scenario clock starts when the host's stream arrives;
# generous fallback for a slow internet connect.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "$($prof.ArmTimeoutMs)"
if ($useSteam) {
    $hostId64 = ConvertTo-SteamId64 $HostSteamId
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$hostId64"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

$target = if ($useSteam) { "steam:$(ConvertTo-SteamId64 $HostSteamId)" } else { "${HostIp}:$Port" }
Write-Host ""
if ($useSteam) {
    Write-Host "Transport: STEAM P2P (no IP / port forwarding). Make sure Steam is ONLINE"
    Write-Host "and that the host launched with YOUR SteamID as their peer."
}
Write-Host "Launching Kenshi as JOIN -> $target  (save '$save', scenario '$Scenario')"
$out = Start-CoopOwnedGame -State $runState -ScriptDir $kitDir -ExePath (Join-Path $KenshiDir 'kenshi_x64.exe') -WorkDir $KenshiDir -TimeoutSec $prof.StartTimeoutSec
$out | ForEach-Object { Write-Host "    $_" }
$gamePid = 0
$line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
if ($gamePid -eq 0) { throw "Kenshi failed to get past the launcher." }
Write-Host "Game PID: $gamePid"

$game = $runState.Processes[$gamePid]
$runDeadline = if ($seconds -gt 0) { Get-CoopRunDeadline -Process $game -Seconds $seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $prof.ArmTimeoutMs -KillGraceSec $killGraceSec } else { [DateTime]::MaxValue }
if (-not (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern '.' -TimeoutSec 120)) { throw 'Owned game did not load the plugin.' }

# ---- Monitor: connection, gameplay, scenario anchor, screenshots ------------------------
if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern "peer connected" -TimeoutSec 120) {
    Write-Host "CONNECTED to the host."
} elseif ($useSteam) {
    Write-Warning "No connection after 120 s. Check that BOTH Steams are online and that"
    Write-Warning "the host used YOUR SteamID as their peer. Results still collected."
} else {
    Write-Warning "No connection after 120 s. Check the host's port forwarding / firewall."
    Write-Warning "Leaving the game up in case it connects late; results still collected."
}
if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern "gameplay started" -TimeoutSec 240) {
    Write-Host "In-game."
}
if ($Scenario -ne "") {
    if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern $shotAnchor -TimeoutSec $shotWait) {
        Write-Host "Scenario anchor observed; capturing screenshots."
        try {
            if ([DateTime]::UtcNow.AddSeconds($shotDelay) -ge $runDeadline -or $game.HasExited) { throw 'Game/deadline ended before capture.' }
            Start-Sleep -Seconds $shotDelay
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kitDir "screenshot.ps1") `
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

# ---- Bundle results -----------------------------------------------------------------------
@"
kit save:      $save
scenario:      $Scenario
transport:     $(if ($useSteam) { "steam (host steamid $(ConvertTo-SteamId64 $HostSteamId))" } else { "udp" })
host ip:       $HostIp
port:          $Port
machine:       $env:COMPUTERNAME
os:            $([System.Environment]::OSVersion.VersionString)
utc offset:    $((Get-TimeZone).BaseUtcOffset)
run stamp:     $stamp
"@ | Set-Content (Join-Path $outDir "session_info.txt") -Encoding UTF8

} catch {
    $failure = $_
} finally {
    try {
        $engineLog = Join-Path $KenshiDir 'kenshi_info.log'
        if (Test-Path -LiteralPath $engineLog) { Copy-Item -LiteralPath $engineLog -Destination (Join-Path $outDir 'join_engine.log') }
    } catch { if (-not $failure) { $failure = $_ } }
    try { Restore-CoopRunState $runState; $restored = $true } catch { $failure = $_ }
}
$zip = Join-Path $kitDir "KenshiCoop-results-$RunToken.zip"
if (-not $failure) {
    try { Compress-Archive -Path "$outDir\*" -DestinationPath $zip }
    catch { $failure = $_ }
}
$status = @{ runToken=$RunToken; success=($null -eq $failure); restored=$restored; zip=$(if (-not $failure) { Split-Path -Leaf $zip } else { $null }); error=$(if ($failure) { "$failure" } else { $null }) }
$status | ConvertTo-Json | Set-Content -LiteralPath "$completionFile.tmp" -Encoding UTF8
Move-Item -LiteralPath "$completionFile.tmp" -Destination $completionFile
if ($failure) { throw $failure }
Write-Host "Results bundled after restoration: $zip"
