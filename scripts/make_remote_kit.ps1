<#
.SYNOPSIS
  Package a private HOST or JOIN harness kit with the matched six-file native
  runtime, named fixture save and interactive runner. This private kit is not
  the six-runtime-file playable distribution ZIP. Credentials never enter it.

.DESCRIPTION
  Kit contents (unique dist\private-harness-kit-*\, zipped to its own private kit):
    mod\KenshiCoop.dll        - matched core/UI/native Steam/native UDP runtime
    mod\KenshiCoopUI.dll      - required native F2 controls
    mod\KenshiCoopNetSteam.dll / mod\KenshiCoopNetUDP.dll - real gameplay providers
    mod\RE_Kenshi.json        - loads core only (providers/UI are loaded by core)
    mod\KenshiCoop.mod        - canonical fixture mod-list placeholder
    save\<name>\...           - the fixture save (both clients MUST load the identical
                                save: entity identity is resolve-by-hand)
    friend_join.ps1           - the friend's one-command runner
    start_kenshi.ps1          - launcher helper (clicks through Kenshi's OK dialog)
    screenshot.ps1            - window capture helper (for the results bundle)
    upnp_portmap.ps1          - UPnP router-mapping helpers (host-role kits only)
    README.txt                - friend-facing instructions + prerequisites

  Run a DRESS REHEARSAL first (kit installed into the local Kenshi-Join install,
  pointed at 127.0.0.1, ideally with run_test's -Wan proxy in the path) so the
  internet session only changes the network, not the workflow.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1 -SkipBuild

.EXAMPLE
  # Steam P2P kit (no port forwarding / router setup): bake YOUR steamid64 in as
  # the friend-host's peer so their command stays one line.
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1 -Transport steam -MySteamId 76561198000000000
#>
[CmdletBinding()]
param(
    # Which role the FRIEND plays. 'host' is the real-session topology (friend
    # hosts for the best experience on their machine; you join and iterate) and
    # bundles friend_host.ps1; 'join' bundles friend_join.ps1.
    [ValidateSet("host", "join")]
    [string]$Role = "host",
    # Fixture save to bundle (must exist in %LOCALAPPDATA%\kenshi\save).
    [string]$Save = "squad1",
    # Default scenario the friend script runs (any manifest scenario, or "" = free play).
    [string]$Scenario = "coop_presence",
    [int]$Port = 27800,
    [string]$NativeBin = "",
    [switch]$SkipBuild,
    # Transport baked into kit.json: "udp" (default; IP + port forwarding) or
    # "steam" (connect by SteamID: NAT punch + Valve relay, no router setup).
    [ValidateSet("udp", "steam")]
    [string]$Transport = "udp",
    # Optional HOST SteamID baked into a JOIN kit; HOST kits need no guest ID.
    [string]$MySteamId = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'NativeBuild.ps1')
. (Join-Path $scriptDir 'kit_preflight.ps1')
$manifest = Import-PowerShellDataFile (Join-Path $scriptDir 'scenarios.psd1')
if ($Scenario -eq 'free') { $Scenario = '' }
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }
if ($Scenario -and -not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }

if (-not $SkipBuild) {
    Write-Host "=== build plugin ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

. (Join-Path $scriptDir 'RuntimeDistribution.ps1') -Configuration Harness -NativeBin $NativeBin
$runtime = Get-CoopRuntimeFiles -Configuration Harness -NativeBin $NativeBin

if (-not $Save -or $Save -match '[\\/:]' -or $Save -eq '.' -or $Save -eq '..') { throw 'One named fixture save is required.' }
$saveSrc = Join-Path $env:LOCALAPPDATA "kenshi\save\$Save"
if (-not (Test-Path -LiteralPath (Join-Path $saveSrc 'quick.save'))) { throw 'A complete named fixture quick.save is required before assembly.' }
$friendScript = if ($Role -eq 'host') { 'friend_host.ps1' } else { 'friend_join.ps1' }
$requiredScripts = @($friendScript,'start_kenshi.ps1','set_video_mode.ps1','screenshot.ps1','kit_preflight.ps1','CoopHarness.psm1','CoopOracles.psm1','scenarios.psd1',$(if ($Role -eq 'host') { 'HOST.cmd' } else { 'JOIN.cmd' }))
if ($Role -eq 'host') { $requiredScripts += 'upnp_portmap.ps1' }
foreach ($file in $requiredScripts) { if (-not (Test-Path -LiteralPath (Join-Path $scriptDir $file) -PathType Leaf)) { throw "Missing private harness component: $file" } }
if (-not (Test-Path -LiteralPath (Join-Path $scriptDir 'oracles\CoreChecks.ps1') -PathType Leaf)) { throw 'Current native terminal oracle closure is required.' }
$kitId = (Get-Date -Format 'yyyyMMdd_HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
$kitDir = Join-Path $repoRoot "dist\private-harness-kit-$kitId"
New-Item -ItemType Directory -Path "$kitDir\mod" | Out-Null

Write-Host "=== assembling kit ==="
foreach ($name in $runtime.Keys) { Copy-Item -LiteralPath $runtime[$name] -Destination (Join-Path "$kitDir\mod" $name) }
Assert-CoopRuntime "$kitDir\mod"

New-Item -ItemType Directory -Force -Path "$kitDir\save" | Out-Null
Copy-Item -Recurse $saveSrc "$kitDir\save\$Save"

Copy-Item (Join-Path $scriptDir $friendScript)      "$kitDir\$friendScript"
Copy-Item (Join-Path $scriptDir "start_kenshi.ps1") "$kitDir\start_kenshi.ps1"
Copy-Item -LiteralPath (Join-Path $scriptDir 'set_video_mode.ps1') -Destination (Join-Path $kitDir 'set_video_mode.ps1')
Copy-Item (Join-Path $scriptDir "screenshot.ps1")   "$kitDir\screenshot.ps1"
# Preflight helpers (RE_Kenshi / Steam / Mark-of-the-Web checks), dot-sourced
# by the friend script.
Copy-Item (Join-Path $scriptDir "kit_preflight.ps1") "$kitDir\kit_preflight.ps1"
Copy-Item -LiteralPath (Join-Path $scriptDir 'CoopHarness.psm1') -Destination (Join-Path $kitDir 'CoopHarness.psm1')
Copy-Item -LiteralPath (Join-Path $scriptDir 'scenarios.psd1') -Destination (Join-Path $kitDir 'scenarios.psd1')
Copy-Item -LiteralPath (Join-Path $scriptDir 'CoopOracles.psm1') -Destination (Join-Path $kitDir 'CoopOracles.psm1')
Copy-Item -LiteralPath (Join-Path $scriptDir 'oracles') -Destination (Join-Path $kitDir 'oracles') -Recurse
# Double-click launcher matching the kit's role: prompts for the other
# player's Steam code and runs the friend script (no PowerShell knowledge
# needed).
$launcher = if ($Role -eq "host") { "HOST.cmd" } else { "JOIN.cmd" }
Copy-Item (Join-Path $scriptDir $launcher) "$kitDir\$launcher"
if ($Role -eq "host") {
    # UPnP router-mapping helpers (dot-sourced by friend_host.ps1).
    Copy-Item (Join-Path $scriptDir "upnp_portmap.ps1") "$kitDir\upnp_portmap.ps1"
}

# Kit defaults consumed by the friend script (keeps the friend's command minimal).
# Steam JOIN kits can remember the host endpoint; HOST kits admit authenticated guests.
function ConvertTo-SteamId64([string]$id) {
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}
$myId64 = if ($MySteamId -ne "") { "$(ConvertTo-SteamId64 $MySteamId)" } else { "" }
$kitMeta = @{
    role      = $Role
    save      = $Save
    scenario  = $Scenario
    port      = $Port
    transport = $Transport
}
$selectedEntry = if ($Scenario) { $manifest.Scenarios[$Scenario] } else { $null }
$kitMeta.setup = if ($selectedEntry) { $selectedEntry.Setup } else { '' }
$kitMeta.diagEnv = if ($selectedEntry -and $selectedEntry.ContainsKey('DiagEnv')) { $selectedEntry.DiagEnv } else { @{} }
$kitMeta.seconds = if (-not $Scenario) { 0 } elseif ($selectedEntry.ContainsKey('Seconds')) { [int]$selectedEntry.Seconds } else { 150 }
$kitMeta.killGraceSec = if ($selectedEntry -and $selectedEntry.ContainsKey('KillGraceSec')) { [int]$selectedEntry.KillGraceSec } else { [int]$manifest.Profiles.remote.KillGraceSec }
if ($Transport -eq "steam" -and $Role -eq "join" -and $myId64 -ne "") {
    $kitMeta.hostSteamId = $myId64
}
$kitMeta.runtimeSha256 = @{}
foreach ($name in $runtime.Keys) { $kitMeta.runtimeSha256[$name] = (Get-FileHash -LiteralPath $runtime[$name] -Algorithm SHA256).Hash.ToLowerInvariant() }
$kitMeta | ConvertTo-Json -Depth 6 | Set-Content "$kitDir\kit.json" -Encoding UTF8

$steamKit   = ($Transport -eq "steam")
$launcherUC = $launcher
$psCmd = if ($Role -eq "host") {
    "powershell -ExecutionPolicy Bypass -File friend_host.ps1"
} else {
    if ($steamKit) { "powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their code>" }
    else           { "powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <the host's public IP>" }
}
if (-not $steamKit) {
    $psCmd += ' (JOIN additionally requires $env:KENSHICOOP_UDP_INVITE_FILE pointing to your private per-client invitation outside the kit/game; HOST creates invitations through F2.)'
}

$sessionArgs = "-Port $Port -Save '$($Save.Replace("'","''"))' -Scenario '$(if ($Scenario) { $Scenario } else { 'free' })'"
$udpPreflight = if ($Role -eq 'host') {
    "Run 'powershell -ExecutionPolicy Bypass -File friend_host.ps1 -CheckOnly' from this HOST kit the day before."
} else {
    'Ask the HOST to check connectivity on their machine the day before; this JOIN kit does not include a host preflight script.'
}
@"
KenshiCoop co-op kit  (your role: $($Role.ToUpper()))
=====================================

QUICK START
-----------

This is a PRIVATE TEST HARNESS, not the playable distribution. It requires a
non-primary monitor and no user-owned Kenshi session. Exact configs/runtime and
named fixtures are restored after owned processes stop; failed cleanup retains
private backups for recovery. Free-play saves under new names are retained; a
starter fixture is restored. A successful results ZIP is published only after
restoration; logs are evidence, not a gameplay PASS.
Native Steam two-account P2P is UNVERIFIED. No local test certifies that route.
UDP uses the native provider directly and has no Steam client/API dependency.

Step 0 - one-time prerequisites (both players):

  1. Kenshi 1.0.65, set to WINDOWED mode: launch Kenshi once,
     Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ installed - a free mod that loads the co-op plugin:
     https://www.nexusmods.com/kenshi/mods/847
$(if ($steamKit) {
"  3. Steam RUNNING and ONLINE on both machines. That is the whole network
     setup - the connection goes through Steam, so there is no port
     forwarding, no router setup, and no IP addresses."
} else {
"  3. This is a direct-UDP kit: the HOST's UDP port $Port must be reachable
     from the internet. The host script sets up the Windows Firewall and
     asks the router to forward the port automatically (UPnP).
     $udpPreflight"
})

$(if ($steamKit) {
"Step 1 - swap Steam friend codes:

  Each player needs the OTHER player's code before connecting. The
  launcher reads YOUR code from Steam and prints it on screen the moment
  you start it - just read it to the other player and type theirs in.
  (Codes are also in Steam under Friends > Add a Friend; a 17-digit
  SteamID64 works too.)"
} else {
"Step 1 - get the HOST's public IP:

  The host player finds it at whatismyip.com (friend_host.ps1 also prints
  it) and sends it to the joining player before launching."
})

Step 2 - launch:

  Double-click $launcherUC in this folder. It asks for the other player's
  $(if ($steamKit) { "code" } else { "details" }), then does everything else: checks your setup (and tells you
  exactly what to fix if something is missing), installs the mod and the
  shared starter save, launches Kenshi, and connects the two games. When
  both of you are in-game, you're playing co-op.

  Prefer a terminal? The same flow is:
    $psCmd

WHAT SYNCS
----------

In free play the full sync set is active by default: positions, combat,
health/limbs, stats, game speed, carried bodies, inventory + equipment
changes, items dropped on the ground, direct trades between the two
squads, and squad management (recruits you hire AND units you move
between squad tabs mid-session stay tracked on the other machine - note
they won't appear in the other player's squad UI, that's a known
limitation).

Base-building syncs too: placed buildings, construction progress, doors,
dismantling, production machines - power switches, generators, crafting
bench / furnace / drill output, input fuel and farm growth - and
container CONTENTS: every storage chest and machine inventory near the
players holds the same items on both machines.

Saves are coordinated: any save either player makes during a connected
session becomes ONE shared save - the host's game writes it and streams
the whole save folder to the other machine automatically (verified +
committed only when it arrives intact). Loading works the same way: when
either player loads a save mid-session, both games load the identical
save and the session continues from there (expect a normal load screen
on both sides).

CHOOSING A SAVE
---------------

The launcher uses the bundled starter save '$Save'; it does not prompt for a
save selection. Both players must use the same fixture and scenario '$Scenario'.
Scenario overrides are accepted only when the bundled fixture supports them.
For free play pass -Scenario free. Once connected the HOST may load another
save in-game; coordinated loading transfers it to the other player.
To resume use -Resume on BOTH participants with the same -ResumeSave name.
The second player controls the SECOND squad tab by default.

UNINSTALL
---------

After successful restoration, delete only the extracted kit and its results
when no longer needed. Do NOT delete a user's mods or save folders.
The host's firewall admission is permanent provisioning; UPnP mappings owned
by this run are removed when it ends. If restoration fails, retain the private
backup path reported by the runner and recover it before starting another run.

TROUBLESHOOTING
---------------

  * "Running scripts is disabled" or Windows SmartScreen blocks the kit:
    right-click the downloaded zip BEFORE extracting, Properties > Unblock.
    The $launcherUC launcher also bypasses this automatically.
  * "RE_Kenshi not found": install RE_Kenshi into your Kenshi folder
    (https://www.nexusmods.com/kenshi/mods/847) and run the launcher again.
  * "The co-op plugin has not started": the game launched but RE_Kenshi did
    not load the plugin. Check <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop';
    reinstalling RE_Kenshi usually fixes it.
$(if ($steamKit) {
"  * No connection: both Steams must be RUNNING and ONLINE (not offline
    mode), and each side needs the correct native Steam identity/host allowlist.
    Inspect native provider state/errors in F2. Local loopback and UDP WAN
    proxy results are NOT evidence of real two-account Steam P2P."
} else {
"  * No connection: UDP $Port must be reachable on the HOST. friend_host.ps1
    handles the Windows Firewall and asks the router to open the port via
    UPnP; if the script said UPnP failed, forward UDP $Port to the host PC
    manually on the router's 'port forwarding' page. Carrier-grade NAT needs a
    genuinely reachable UDP endpoint; no tunnel or alternate transport fallback
    is provided by this native test."
})
  * "protocol mismatch" in the log: your kit is older/newer than the other
    player's build; both players should get the latest kit.
"@ | Set-Content "$kitDir\README.txt" -Encoding UTF8

$zip = Join-Path $repoRoot "dist\KenshiCoop-remote-kit-$kitId.zip"
Compress-Archive -Path "$kitDir\*" -DestinationPath $zip
Write-Host ""
Write-Host "Kit assembled: $kitDir"
Write-Host "Kit zipped:    $zip"
Write-Host ""
if ($Role -eq "host") {
    Write-Host "Next (friend-HOSTS topology):"
    Write-Host "  1. Dress-rehearse against the LAN machine: scripts\rehearse_host_kit.ps1"
    Write-Host "  2. Send the zip to your friend; schedule the session."
    if ($steamKit) {
        Write-Host "  3. Your side when it starts: scripts\join_session.ps1 -HostSteamId <their steamid64> $sessionArgs"
    } else {
        Write-Host "  3. Your side when it starts: scripts\join_session.ps1 -HostIp <their public IP> $sessionArgs"
    }
    if ($Scenario) { Write-Host "  4. Judge: scripts\analyze_run.ps1 -HostLog <theirs> -JoinLog <yours> -Scenario '$Scenario'" }
    else { Write-Host '  4. Free-play logs are evidence only; no automated gameplay certificate.' }
} else {
    Write-Host "Next (friend-JOINS topology):"
    Write-Host "  1. Dress-rehearse locally: scripts\rehearse_remote_kit.ps1"
    Write-Host "  2. Send the zip to your friend; schedule the session."
    if ($steamKit) {
        Write-Host "  3. Host side: scripts\host_session.ps1 $sessionArgs -Transport steam"
    } else {
        Write-Host "  3. Host side: scripts\host_session.ps1 $sessionArgs"
    }
    if ($Scenario) { Write-Host "  4. Judge: scripts\analyze_run.ps1 -HostLog <yours> -JoinLog <theirs> -Scenario '$Scenario'" }
    else { Write-Host '  4. Free-play logs are evidence only; no automated gameplay certificate.' }
}
