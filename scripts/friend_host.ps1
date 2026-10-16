<#
.SYNOPSIS
  The friend-side HOST runner for a KenshiCoop remote test session. Ships
  INSIDE the host-role remote kit (scripts\make_remote_kit.ps1 -Role host);
  runs from the unzipped kit folder. This is the real-session topology: the
  friend hosts (best experience on their machine), you join and iterate.

.DESCRIPTION
  One command:
    powershell -ExecutionPolicy Bypass -File friend_host.ps1

  Steam P2P (no port forwarding / router setup at all):
    powershell -ExecutionPolicy Bypass -File friend_host.ps1
  Transport comes from kit.json. Give your own SteamID to the joining player
  (they pass it as -HostSteamId); no guest allowlist is required.

  Installs the bundled mod + save into the local Kenshi, adds the Windows
  Firewall rule, asks the router to forward the UDP port automatically via
  UPnP (falling back to the manual port-forward checklist if the router
  refuses), prints this machine's public IP (send it to the joining player),
  launches the game as the HOST, waits for the session to end (scenario
  self-exit, or the friend closes the window in free play), and bundles
  host.log + screenshots into KenshiCoop-results-<stamp>.zip to send back
  for offline judging (analyze_run.ps1).

  -CheckOnly provisions/verifies the permanent firewall admission and tests an
  owned transient router mapping; it installs no runtime and launches no game.

  Scenario timing note: the host's scripted actions ARM only when the joining
  player's stream arrives (peer-ready), so nothing happens before both are
  in-game - the friend just needs to leave the window running.
#>
[CmdletBinding()]
param(
    [string]$KenshiDir = "",
    # Override the kit's default scenario ("" = kit default; "free" = normal
    # co-op play, no scripted scenario, no self-exit).
    [string]$Scenario = "",
    [int]$Port = 0,
    [int]$FreePlayMinutes = 0,
    # Skip the interactive "press Enter when ready" gate (used by the
    # automated dress rehearsal; humans should leave it on).
    [switch]$NoPrompt,
    # Skip the automatic UPnP router mapping (manual port forward only).
    [switch]$NoUpnp,
    # Connectivity preflight: permanently provision/verify the firewall rule,
    # test/release an owned transient UPnP mapping, and inspect the public IP.
    [switch]$CheckOnly,
    # Resume the previous session: load the 'coopresume' save the coordinated
    # save (protocol 31) wrote last session instead of installing the kit's
    # baked save. BOTH sides must resume. Defaults to free play (no scenario).
    [switch]$Resume,
    # Save name the coordinated-save flow writes (must match the join's).
    [string]$UdpProfilePath = $env:KENSHICOOP_UDP_PROFILE_PATH,
    [string]$RunToken = [Guid]::NewGuid().ToString('N'),
    [string]$ResumeSave = "coopresume"
)

$ErrorActionPreference = "Stop"
$kitDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Preflight helpers (RE_Kenshi / Steam / Mark-of-the-Web / windowed checks).
. (Join-Path $kitDir "kit_preflight.ps1")
Invoke-KitUnblock -KitDir $kitDir

# ---- Kit defaults --------------------------------------------------------------
$kit = Get-Content (Join-Path $kitDir "kit.json") -Raw | ConvertFrom-Json
$save = $kit.save
if ($Resume) {
    # The save the coordinated save (protocol 31) wrote last session; resume =
    # both sides load the identical file. Free play unless a scenario is asked
    # for explicitly.
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
$shotAnchor = if ($entry -and $entry.ContainsKey('ShotAnchorHost')) { $entry.ShotAnchorHost } else { 'SCENARIO MEMBER' }
$shotDelay = if ($entry -and $entry.ContainsKey('ShotDelaySec')) { [int]$entry.ShotDelaySec } else { 2 }
$shotWait = if ($entry -and $entry.ContainsKey('ShotWaitSec')) { [int]$entry.ShotWaitSec } else { [int]$prof.JoinAnchorTimeoutSec }
$shotRequired = $entry -and $entry.ContainsKey('ShotAnchorHost')
$requireResult = $null -ne $entry
# The runtime kit selects the transport; HOST does not select guests by ID.
$useSteam = ("$($kit.transport)" -eq 'steam')
# Print YOUR friend code first (read from Steam) so the two of you can swap
# codes right off this screen - no profile-page spelunking.
if ($useSteam -or "$($kit.transport)" -eq "steam") { Show-MySteamId }

# ---- Connectivity helpers ----------------------------------------------------------
. (Join-Path $kitDir "upnp_portmap.ps1")

function Ensure-CoopFirewallRule {
    param([int]$RulePort)
    try {
        $rules = @(Get-NetFirewallRule -DisplayName KenshiCoop -ErrorAction SilentlyContinue | Where-Object { $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' })
        foreach ($rule in $rules) {
            foreach ($filter in @($rule | Get-NetFirewallPortFilter)) {
                if ("$($filter.Protocol)" -in @('UDP','17') -and "$($filter.LocalPort)" -eq "$RulePort") { return $true }
            }
        }
        & netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$RulePort | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Firewall rule creation failed.' }
        $rules = @(Get-NetFirewallRule -DisplayName KenshiCoop -ErrorAction Stop | Where-Object { $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' })
        foreach ($rule in $rules) {
            foreach ($filter in @($rule | Get-NetFirewallPortFilter)) {
                if ("$($filter.Protocol)" -in @('UDP','17') -and "$($filter.LocalPort)" -eq "$RulePort") { return $true }
            }
        }
        throw 'Required enabled inbound UDP firewall rule could not be verified.'
    } catch {
        Write-Warning "Firewall readiness failed: $($_.Exception.Message)"
        return $false
    }
}
function Get-PublicIp {
    try { return (Invoke-WebRequest -Uri "https://api.ipify.org" -UseBasicParsing -TimeoutSec 5).Content.Trim() }
    catch { return "" }
}

# ---- Preflight-only mode (-CheckOnly) ------------------------------------------------
if ($CheckOnly -and $useSteam) {
    Write-Host ""
    Write-Host "=== PRE-SESSION CHECK (Steam P2P transport) ==="
    Write-Host " Steam brokers the connection by SteamID - no firewall rule, router"
    Write-Host " forwarding or public IP needed. Just make sure that on session day:"
    Write-Host "   1. Steam is RUNNING and ONLINE (not offline mode) on both machines."
    Write-Host "   2. You and the other player have exchanged SteamIDs."
    Write-Host " READY."
    exit 0
}
if ($CheckOnly) {
    Write-Host ""
    Write-Host "=== CONNECTIVITY CHECK (firewall provisioning is permanent; no game/runtime install) ==="
    $fwOk = Ensure-CoopFirewallRule $Port
    $publicIp = Get-PublicIp
    if ($publicIp -ne "") { Write-Host "Public IP: $publicIp" }
    else                  { Write-Host "Public IP: (could not detect - use whatismyip.com)" }

    $upnp = $null
    if (-not $NoUpnp) {
        Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
        $upnp = Add-UpnpMapping -Port $Port
    } else {
        Write-Host 'UPnP skipped (-NoUpnp); automatic readiness is unproven. Manual forwarding is required.'
    }
    try {
    $hairpin = "skipped"
    if ($upnp -and $upnp.Ok) {
        Write-Host "UPnP: mapping accepted (UDP $Port -> $($upnp.LanIp))."
        $probeIp = if ($upnp.ExternalIp) { $upnp.ExternalIp } else { $publicIp }
        if ($probeIp -match '^\d') {
            Write-Host "Probing reachability via your own public IP (NAT hairpin) ..."
            $hairpin = Test-UpnpHairpin -Port $Port -ExternalIp $probeIp
        }
    } elseif ($upnp) {
        Write-Host "UPnP: FAILED - $($upnp.Error)"
    }
    } finally { if ($upnp -and ($upnp.Owned -or $upnp.Unresolved)) { Remove-UpnpMapping -Mapping $upnp } }

    $cgnat = ($upnp -and $upnp.Ok -and $upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp)

    Write-Host ""
    Write-Host "=================== CHECK RESULT ==================="
    if ($upnp -and $upnp.Ok -and -not $cgnat -and $fwOk) {
        Write-Host " READY: the session script will open the port automatically."
        if ($hairpin -eq "reachable") {
            Write-Host " (Reachability probe PASSED - strongest possible pre-session signal.)"
        } elseif ($hairpin -eq "inconclusive") {
            Write-Host " (Reachability probe inconclusive - normal; many routers can't self-test.)"
        }
    } elseif ($cgnat) {
        Write-Host " WARNING: your router reports external IP $($upnp.ExternalIp) but the"
        Write-Host " internet sees $publicIp - you appear to be behind a second NAT"
        Write-Host " (carrier-grade NAT). Port forwarding will NOT help; tell the other"
        Write-Host " player so you can arrange a VPN overlay (e.g. Tailscale) instead."
    } else {
        Write-Host " NEEDS MANUAL PORT FORWARD: automatic UPnP did not work."
        Write-Host " Forward UDP $Port to this PC on your router's 'port forwarding'"
        Write-Host " page (or enable UPnP in the router settings and re-run this check)."
    }
    if (-not $fwOk) { Write-Host " Also fix the Windows Firewall rule (see warning above)." }
    Write-Host "===================================================="
    if ($upnp -and $upnp.Ok -and -not $cgnat -and $fwOk) { exit 0 } else { exit 1 }
}

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
$upnp = $null
$outDir = Join-Path $kitDir "results_$RunToken"
$completionFile = Join-Path $kitDir "complete_$RunToken.json"
if ((Test-Path -LiteralPath $outDir) -or (Test-Path -LiteralPath $completionFile)) { throw 'Run token has already been used.' }
New-Item -ItemType Directory -Path $outDir | Out-Null
try {
$runState = New-CoopRunState -GameDirs @($KenshiDir) -SaveNames $ownedSaveNames -PreserveSaves:($Resume -and $Scenario -eq '')
Clear-CoopRunEnvironment -State $runState -PreserveCredentials
Set-CoopPrivateCredentials -Mode host -UseSteam $useSteam -GameDirs @($KenshiDir,$kitDir) -ProfilePath $UdpProfilePath
# ---- Install mod + save ------------------------------------------------------------
$modDst = Join-Path $KenshiDir "mods\KenshiCoop"
New-Item -ItemType Directory -Force -Path $modDst | Out-Null
foreach ($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')) { Copy-Item -LiteralPath (Join-Path "$kitDir\mod" $name) -Destination $modDst -Force }
Write-Host "Mod installed -> $modDst"

$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
New-Item -ItemType Directory -Force -Path $saveRoot | Out-Null
if ($Resume) {
    # No install: the save was written by last session's coordinated save
    # (protocol 31). It must already exist here or there is nothing to resume.
    $found = (Test-Path (Join-Path $saveRoot $save)) -or
             (Test-Path (Join-Path $KenshiDir "save\$save"))
    if (-not $found) {
        throw ("Resume save '$save' not found in '$saveRoot' or '$KenshiDir\save'. " +
               "Run at least one connected session first (any save during it is " +
               "coordinated + shared automatically), or drop -Resume.")
    }
    Write-Host "Resuming on coordinated save '$save' (no kit save installed)."
} else {
    foreach ($destBase in @($saveRoot, (Join-Path $KenshiDir "save"))) {
        if ($destBase -like "*\save" -and -not (Test-Path (Split-Path -Parent $destBase))) { continue }
        New-Item -ItemType Directory -Force -Path $destBase | Out-Null
        $dst = Join-Path $destBase $save
        if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
        Copy-Item -Recurse (Join-Path $kitDir "save\$save") $dst
    }
    Write-Host "Save '$save' installed."
}

# ---- Firewall (local) + router mapping + checklist --------------------------------------
$upnp = $null
$upnpNote = "n/a (steam transport)"
$publicIp = "(n/a - steam transport)"
if ($useSteam) {
    Write-Host ""
    Write-Host "=============== HOST CHECKLIST (STEAM P2P) ==============="
    Write-Host " 1. Steam connects you BY STEAMID - no router setup, no port"
    Write-Host "    forwarding, no public IPs. Keep Steam ONLINE."
    Write-Host " 2. Guests connect using YOUR SteamID; no guest allowlist is required."
    Write-Host " 3. YOUR SteamID is printed after launch - read it to them."
    Write-Host " 4. Leave this window open; the game closes itself when the"
    Write-Host "    test scenario finishes (free play: close the game when done)."
    Write-Host "==========================================================="
    Write-Host ""
} else {
[void](Ensure-CoopFirewallRule $Port)

$upnpNote = "disabled (-NoUpnp)"
if (-not $NoUpnp) {
    Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
    $upnp = Add-UpnpMapping -Port $Port
    $upnpNote = if ($upnp.Ok) { "mapped UDP $Port -> $($upnp.LanIp) (router external IP: $($upnp.ExternalIp))" }
                else          { "failed: $($upnp.Error)" }
}

$publicIp = Get-PublicIp
if ($publicIp -eq "") { $publicIp = "(could not detect - use whatismyip.com)" }
Write-Host ""
Write-Host "=================== HOST CHECKLIST ==================="
if ($null -ne $upnp -and $upnp.Ok) {
    Write-Host " 1. Router: compatible UPnP mapping accepted (UDP $Port -> $($upnp.LanIp))."
    Write-Host "    Only a mapping created by this run is removed at session end."
    if ($upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp) {
        Write-Host "    NOTE: your router reports external IP $($upnp.ExternalIp) but the internet"
        Write-Host "    sees $publicIp - you may be behind a second NAT (carrier-grade NAT)"
        Write-Host "    and the connection may still fail. Manual forwarding won't help there;"
        Write-Host "    a VPN overlay (e.g. Tailscale) is the workaround."
    }
} else {
    if ($null -ne $upnp) { Write-Host " 1. Automatic UPnP mapping FAILED ($($upnp.Error))." }
    else                 { Write-Host " 1. Automatic UPnP mapping skipped (-NoUpnp)." }
    Write-Host "    Router: forward UDP port $Port to this PC (see your router's"
    Write-Host "    'port forwarding' page; this PC's LAN IP is usually 192.168.x.x)."
}
Write-Host " 2. Tell the joining player your public IP: $publicIp"
Write-Host " 3. Leave this window open; the game closes itself when the test"
Write-Host "    scenario finishes (free play: close the game when done)."
Write-Host "======================================================="
Write-Host ""
}
if (-not $NoPrompt) { Read-Host "Press Enter to launch the host" | Out-Null }

# ---- Output + env -----------------------------------------------------------------------
$stamp = $RunToken
$hostLog = Join-Path $outDir "host.log"

if ($entry) { [void](Set-CoopDiagEnv -Entry $entry) }
$env:KENSHICOOP_AUTOCONNECT = '1'
$env:KENSHICOOP_MODE         = "host"
$env:KENSHICOOP_IP           = "0.0.0.0"
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $save
$env:KENSHICOOP_LOG          = $hostLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = if ($entry) { $entry.Setup } else { "" }
$env:KENSHICOOP_TEST_SECONDS = "$seconds"
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Scenario actions arm when the joining player's stream arrives; generous
# fallback for a slow internet connect.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "$($prof.ArmTimeoutMs)"
if ($useSteam) {
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "0"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}
Set-CoopPrivateCredentials -Mode host -UseSteam $useSteam -GameDirs @($KenshiDir,$kitDir) -ProfilePath $UdpProfilePath

$game = $null

    Write-Host "Launching Kenshi as HOST on UDP $Port (save '$save', scenario '$Scenario')"
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $kitDir -ExePath (Join-Path $KenshiDir 'kenshi_x64.exe') -WorkDir $KenshiDir -TimeoutSec $prof.StartTimeoutSec
    $out | ForEach-Object { Write-Host "    $_" }
    $gamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
    if ($gamePid -eq 0) { throw "Kenshi failed to get past the launcher." }
    Write-Host "Game PID: $gamePid"

    $game = $runState.Processes[$gamePid]
    $runDeadline = if ($seconds -gt 0) { Get-CoopRunDeadline -Process $game -Seconds $seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $prof.ArmTimeoutMs -KillGraceSec $killGraceSec } else { [DateTime]::MaxValue }
    if (-not (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $hostLog -Pattern '.' -TimeoutSec 120)) { throw 'Owned game did not load the plugin.' }

    if ($useSteam) {
        Show-MySteamId
        Write-Host 'F2 displays the active native Steam identity/status; share that identity with the joining player.'
        Write-Warning 'Actual two-account Steam P2P remains unverified; this runner does not certify it.'
    }

    if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $hostLog -Pattern "peer connected" -TimeoutSec 300) {
        Write-Host "JOINING PLAYER CONNECTED."
    } elseif ($useSteam) {
        Write-Warning "No connection after 5 min - check that BOTH Steams are online and the"
        Write-Warning "SteamIDs were exchanged correctly (each side peers with the OTHER's id)."
        Write-Warning "Leaving the game up in case they connect late."
    } else {
        Write-Warning "No connection after 5 min - check the port forward + firewall."
        Write-Warning "Leaving the game up in case they connect late."
    }

    # Screenshot once the scenario is live (best-effort evidence for the results zip).
    if ($Scenario -ne "") {
        if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $hostLog -Pattern $shotAnchor -TimeoutSec $shotWait) {
            try {
                if ([DateTime]::UtcNow.AddSeconds($shotDelay) -ge $runDeadline -or $game.HasExited) { throw 'Game/deadline ended before capture.' }
                Start-Sleep -Seconds $shotDelay
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kitDir "screenshot.ps1") `
                    -ProcessId $gamePid -ProcessStartTicks ($game.StartTime.ToUniversalTime().Ticks) -Out (Join-Path $outDir "host.png") -Frames 5 -IntervalMs 16
                if ($LASTEXITCODE -ne 0) { throw 'Screenshot helper failed.' }
            } catch {
                if ($shotRequired) { throw }
                Write-Warning "Screenshot evidence unavailable: $($_.Exception.Message)"
            }
        }
}
Wait-CoopOwnedExit -Process $game -Deadline $runDeadline
if (-not (Test-Path -LiteralPath $hostLog) -or -not (Select-String -LiteralPath $hostLog -Pattern 'gameplay started' -Quiet)) { throw 'No current-run gameplay evidence.' }
if ($requireResult -and (Test-ScenarioResultPass -File $hostLog -Label host -Required $true -Scenario $Scenario) -ne 'PASS') { throw 'Scenario incomplete or failed; exit alone is not success.' }
if ($shotRequired -and -not (Test-Path -LiteralPath (Join-Path $outDir 'host.png'))) { throw 'Required scenario capture is missing.' }

# ---- Bundle results -----------------------------------------------------------------------
@"
role:          host
kit save:      $save
scenario:      $Scenario
transport:     $(if ($useSteam) { "steam" } else { "udp" })
port:          $Port
public ip:     $publicIp
upnp:          $upnpNote
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
        if (Test-Path -LiteralPath $engineLog) { Copy-Item -LiteralPath $engineLog -Destination (Join-Path $outDir 'host_engine.log') }
    } catch { if (-not $failure) { $failure = $_ } }
    try { if ($upnp -and ($upnp.Owned -or $upnp.Unresolved)) { Remove-UpnpMapping -Mapping $upnp } }
    catch { if (-not $failure) { $failure = $_ } }
    finally { try { Restore-CoopRunState $runState; $restored = $true } catch { $failure = $_ } }
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
