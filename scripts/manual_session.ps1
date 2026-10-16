<#
.SYNOPSIS
  Launch a HOST + JOIN co-op session for manual validation (you drive, no
  self-exit or automated screenshots). Host owns squad tab 0; join owns tab 1.

.DESCRIPTION
  Unlike run_test.ps1 (timed/scenario runs that self-exit and screenshot), this
  leaves both clients running so you can play. It drives the plugin via env vars:
    KENSHICOOP_SAVE         - save both clients auto-load
    KENSHICOOP_TEST_SECONDS - forced 0 here (NO self-exit; you close the windows)
    KENSHICOOP_SCENARIO     - forced empty (normal co-op tick, no scenario)

  Host runs from the Steam install; join from the Kenshi-Join install. With
  -Sync the host save is mirrored into the join install first.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "c" -Sync

.EXAMPLE
  # Free play from the TITLE SCREEN: both clients boot to the main menu tiled
  # side-by-side; you load the same save and go ONLINE via F2 on each by hand.
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -TitleScreen

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "c" -SkipBuild -Sync

.EXAMPLE
  # Long-run visual pop/snap inspection: 'zoom' save (outside town, camera far
  # out) + colored authority markers on the join (green DRV / red HID / yellow LOC).
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "zoom" -DebugMarkers
#>
[CmdletBinding()]
param(
    # Save both clients auto-load. Optional with -TitleScreen (both clients boot
    # to the main menu instead, and you load + go online by hand via F2).
    [string]$Save = "",
    # Title-screen free play: launch BOTH clients to the Kenshi main menu with
    # NO auto-load and NO auto-connect (KENSHICOOP_SAVE empty, AUTOCONNECT=0 on
    # both). You then load a save and go ONLINE via the in-game F2 panel - the
    # manual connection flow. Skips save validation and -Sync.
    [switch]$TitleScreen,
    # Must match the host save; accepted explicitly for paired launch callers.
    [string]$JoinSave = "",
    # Host-only recruit helper (KENSHICOOP_AUTORECRUIT=N seconds): N s after
    # gameplay settles, the host ONCE programmatically recruits the nearest
    # non-player world NPC via the same PlayerInterface::recruit the dialog
    # "join me" hits. Use to validate recruit sync on a populated save that has
    # no dialog-hireable NPC (camp/squad1). 0 = off.
    [int]$AutoRecruit = 0,
    # Supported ownership is a CSV of squad-tab ranks, not character indices.
    [ValidatePattern('^(0|[1-9][0-9]*)(,(0|[1-9][0-9]*))*$')]
    [string]$HostSquads = "0",
    [ValidatePattern('^(0|[1-9][0-9]*)(,(0|[1-9][0-9]*))*$')]
    [string]$JoinSquads = "1",
    # Push-save-on-connect test: launch the JOIN with NO save so it sits at the
    # main menu (Continue / New Game / ...). The host loads $Save and, on the
    # join's connect, bakes + announces its live world (LOAD_GO); the join loads
    # it straight from the menu. On a single machine both installs share the save
    # folder, so the join fingerprint MATCHES and it loads from disk (the connect
    # trigger + title-screen load pump); the folder-transfer half only engages
    # when the join genuinely lacks the save (a real second machine).
    [switch]$JoinFromMenu,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [switch]$Sync,
    [switch]$SkipBuild,
    [switch]$SkipDeploy,
    [switch]$NoJoin,
    [int]$JoinDelaySec = 8,
    [int]$StartTimeoutSec = 90,
    # Host-only deterministic test-scene setup (KENSHICOOP_SETUP). "chair" spawns a
    # seat in front of the player; "npc" also spawns a loose world NPC. The host
    # then stays up (no self-exit) so you can arrange the pose and SAVE the game.
    [string]$SetupScene = "",
    # AI-gating probe (join only, KENSHICOOP_PROBE_RECRUIT=1): recruit diverged
    # NPCs into the player squad to validate the "inhabit" lever.
    [switch]$ProbeRecruit,
    # AI-suspend probe (join only, KENSHICOOP_PROBE_AISUSPEND=1): detour
    # Character::periodicUpdate so host-driven NPCs stop self-tasking (decision
    # layer off) while still animating. Faction is untouched.
    [switch]$ProbeAiSuspend,
    # Bidirectional inventory sync (KENSHICOOP_INV_SYNC=1 on BOTH clients): each side
    # streams the contents of the squad tabs it owns (host tab 0, join tab 1) and
    # reconciles the peer's. Add an item to a join-owned character and watch it appear
    # on the host (and vice versa). Pair with -SetupScene inventory to also seed the
    # leader so the host->join direction is visible at startup.
    [switch]$InvSync,
    # World-item sync (KENSHICOOP_WORLD_SYNC=1 on BOTH clients, Phase W1): the HOST streams
    # free GROUND items in the interest sphere; the JOIN spawns local proxies so a dropped
    # item appears on the join at the same spot (and is culled when it despawns). In W1 only
    # host-authored drops sync (join-originated DROP intent is W2). Drop an item from a host
    # squad member's bag and watch it appear on the join.
    [switch]$WorldSync,
    # Diagnostic: log a full inventory dump (loose _allItems + every section + weapon
    # accessors) on each inventory SEND (host capture) and APPLY (peer reconcile result),
    # so we can see exactly where an unequipped weapon goes. Sets KENSHICOOP_INV_DUMP=1.
    [switch]$InvDump,
    # Debug authority markers (KENSHICOOP_DEBUG_MARKERS=1, spike-47 HUD labels):
    # the JOIN pins a colored label to every judged body - green DRV = host-
    # driven, red HID = suppressed/culled, yellow LOC = local-sim copy present
    # in the host census. Makes pops and ghosts self-explaining on screen
    # (pair with -Save zoom for long-run wide-camera inspection).
    [switch]$DebugMarkers,
    # Presence authority (KENSHICOOP_CELL_AUTH, protocol 49): each side claims
    # the 4608 u zone cells its own squad tabs are standing in and AUTHORS the
    # NPC census there, instead of the host authoring everywhere. Split the pair
    # across two towns and the join keeps its own population instead of having
    # it culled and re-minted from the host's stream.
    #
    # ON by default since v0.47 (see Config.cpp), so a plain manual session
    # already runs with it - the switch to reach for here is -NoCellAuth, which
    # pins it OFF for an A/B against unconditional host authority. Note the
    # scenario tier does NOT inherit the default: Set-CoopDiagEnv pins it to 0
    # for every scenario that does not ask for it in its manifest DiagEnv.
    # Pair either way with -DebugMarkers to see the handover on screen.
    [switch]$NoCellAuth,
    # Co-location collapse (KENSHICOOP_CELL_COLLAPSE): while both squads claim the
    # SAME 4608 u cell, every claimed cell resolves to the host, so standing
    # together behaves as unconditional host authority did and only the join
    # drives. ON by default; -NoCollapse pins it off for the A/B. Ignored when
    # -NoCellAuth is given, since the host already owns everything there.
    #
    # With -DebugMarkers this is directly visible: collapsed, green DRV tags
    # appear on the join only; walk a tab into the next cell and the host starts
    # showing them too as the split resumes.
    [switch]$NoCollapse,
    # Record where you walk (KENSHICOOP_TRACK_MOVE=1): one [track] line per squad
    # tab per second, with position, cell and whether it is moving. Log-only.
    # Use it to capture a route for a scenario to follow: the cell claims a session
    # already logs fire once per 4608 u cell, so a cross-map walk leaves under
    # thirty points, and run_apart wedged trying to walk straight between two of
    # them. A 1 Hz track over the same walk is ~500 points, which is a real path.
    [switch]$TrackMove,
    # Phase 6 shackle diagnostic (KENSHICOOP_DEBUG_SHACKLE=1 on both clients):
    # emits the ~1 Hz [shackledbg] per-body chained/lock trace so a manual camp
    # session captures the exact tick a peer's driven copy diverges from the
    # owner (the reported "peer PC unlocks the shackles" desync).
    [switch]$DebugShackle,
    # Jail long-play diagnostics (spike 58, KENSHICOOP_JAIL_PROBE/TASK_SPIKE=1 on
    # both clients): arms the [jail] STATE/SNAP captive traces + [spike] SELECT
    # task-selection + auditRows (SCENARIO WNPC/WORLD) so a manual
    # jailed/slaves/cage2 session captures the guard put-to-work cage<->pole
    # oscillation and census drift on BOTH sides with the NORMAL self-heal on
    # (i.e. real replicated behavior). On exit both logs are copied into
    # tools/manual-sessions/<stamp>/. All read-only, OFF by default.
    [switch]$JailProbe,
    # Jail OBSERVE (spike 57 phase A, KENSHICOOP_JAIL_OBSERVE=1): OPT-IN, requires
    # -JailProbe. Runs peer-owned captives UNOPPOSED (drive/suspend/self-heal OFF)
    # to classify the guard put-to-work trajectory. WARNING: this DISABLES the
    # captive self-heal, so captives WILL exit furniture and walk off on the
    # observing side (a diagnostic divergence, NOT a real desync). Do NOT use for
    # parity/visual A-B; use plain -JailProbe for that.
    [switch]$JailObserve,
    # The owning helper always sizes/pins windows on a live non-primary monitor.
    [string]$NativeBin = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'kit_preflight.ps1')
. (Join-Path $scriptDir 'NativeBuild.ps1')


$HostDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($HostDir)
$JoinDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($JoinDir)
$HostSquads = (@($HostSquads.Split(',') | ForEach-Object { [uint32]$_ } | Sort-Object -Unique) -join ',')
$JoinSquads = (@($JoinSquads.Split(',') | ForEach-Object { [uint32]$_ } | Sort-Object -Unique) -join ',')
if (@($HostSquads.Split(',') | Where-Object { $JoinSquads.Split(',') -contains $_ }).Count -and -not $NoJoin) { throw 'Host and join squad-tab ownership must not overlap.' }
$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not $NoJoin -and -not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

if ($JailObserve -and -not $JailProbe) {
    throw "-JailObserve requires -JailProbe (it only toggles observe mode on top of the jail probes)."
}

if ($JoinSave -eq "") { $JoinSave = $Save }

if ($JoinFromMenu) { $JoinSave = $Save }

# Title-screen mode boots both clients to the main menu (no auto-load), so no
# save is required or validated. Every other mode auto-loads and needs a save.
if (-not $TitleScreen -and $Save -eq "") {
    throw "Provide -Save <name>, or pass -TitleScreen to boot both clients to the main menu."
}

# Check the native save location selected by each install, not an assumed global
# folder. A join about to receive the explicit sync is validated after copying.
if (-not $TitleScreen) {
    $clients = @(@{Dir=$HostDir;Save=$Save})
    if (-not $NoJoin -and -not $JoinFromMenu -and -not ($Sync -and $Save -eq $JoinSave)) { $clients += @{Dir=$JoinDir;Save=$JoinSave} }
    foreach ($client in $clients) {
        if (-not $client.Save -or $client.Save -match '[\\/:]' -or $client.Save -in @('.','..')) { throw 'One named save is required.' }
        $settings = Join-Path $client.Dir 'settings.cfg'
        $location = @(Get-Content -LiteralPath $settings | Where-Object { $_ -match '^\s*User save location\s*=\s*[01]\s*$' })
        if ($location.Count -ne 1) { throw "Native save location cannot be established in $settings." }
        $saveRoot = if ($location[0] -match '=\s*1\s*$') { Join-Path $env:LOCALAPPDATA 'kenshi\save' } else { Join-Path $client.Dir 'save' }
        if (-not (Test-Path -LiteralPath (Join-Path $saveRoot "$($client.Save)\quick.save") -PathType Leaf)) { throw "Complete save '$($client.Save)' not found in $saveRoot." }
    }
}

Write-Host "== KenshiCoop MANUAL session =="
if ($TitleScreen) {
    Write-Host "  mode:       TITLE SCREEN (both clients boot to the main menu; no auto-load)"
    Write-Host "  connect:    by hand - load a save, then F2 -> Connection ONLINE on both"
} else {
    Write-Host "  host save:  $Save"
    Write-Host "  join save:  $JoinSave"
}
Write-Host "  self-exit:  OFF (close the windows yourself when done)"
if ($TitleScreen) {
    Write-Host "  NOTE: pick the SAME save on both clients (NPC sync is resolve-by-hand)."
} else {
    if (-not $NoJoin -and $Save -ne $JoinSave) { throw 'Both clients must load the same save for full entity identity.' }
    Write-Host "  ownership: host squad tabs {$HostSquads}; join squad tabs {$JoinSquads}"
}

$ownedDirs = @(Get-CoopManualGameDirs -HostDir $HostDir -JoinDir $JoinDir -NoJoin ([bool]$NoJoin))
$runState = New-CoopRunState -GameDirs $ownedDirs -PreserveSaves
$repoRoot = Split-Path -Parent $scriptDir
$dest = Join-Path $repoRoot ('tools\manual-sessions\' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
try {
New-Item -ItemType Directory -Path $dest | Out-Null

# Build and deploy are INDEPENDENT steps. -SkipBuild reuses the existing built
# DLL but STILL DEPLOYS it (the old behavior skipped deploy too, which silently
# ran a stale DLL in the installs - the bug that invalidated the inhabit test).
# Only -SkipDeploy skips the copy into the installs.
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build plugin ==="
    & cmd /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd failed ($LASTEXITCODE)" }
}
if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== deploy ==="
    Install-CoopHarnessRuntime -RepoRoot (Split-Path -Parent $scriptDir) -GameDirs $ownedDirs -NativeBin $NativeBin
    # Surface exactly which DLL is now deployed so we never validate a stale build.
    $deployed = Join-Path $HostDir "mods\KenshiCoop\KenshiCoop.dll"
    if (Test-Path $deployed) {
        $info = Get-Item $deployed
        Write-Host ("  deployed DLL: {0}  ({1:yyyy-MM-dd HH:mm:ss}, {2} bytes)" -f $deployed, $info.LastWriteTime, $info.Length)
    }
} else {
    Write-Host ""
    Write-Host "=== deploy SKIPPED (-SkipDeploy): installs keep their current DLL ==="
}


if ($Sync -and $NoJoin) {
    Write-Host 'Skipping -Sync: no join game is owned in host-only mode.'
} elseif ($Sync -and $TitleScreen) {
    Write-Host ""
    Write-Host "Skipping -Sync: title-screen mode auto-loads nothing (load a save by hand)."
} elseif ($Sync -and ($Save -eq $JoinSave)) {
    Write-Host ""
    Write-Host "Syncing only named save '$Save' host -> join ..."
    Add-CoopRunBackup -State $runState -Path (Join-Path $JoinDir "save\$Save")
    Sync-CoopNamedSave -Save $Save -HostDir $HostDir -JoinDir $JoinDir
} elseif ($Sync) {
    Write-Host ""
    Write-Host "Skipping -Sync: host and join load different saves (distinct-save mode)."
}

function Set-CoopEnv {
    param([string]$Mode, [string]$SaveName, [string]$Own)
    Clear-CoopRunEnvironment -State $runState
    $env:KENSHICOOP_MODE         = $Mode
    # Manual sessions run BOTH clients on this one machine (same Steam account), so
    # they must use direct-UDP loopback: a same-machine Steam P2P session can't
    # establish (active=0/err=4). Force it here rather than inheriting the deployed
    # coop_config.json, which may be left on transport=steam + a real steamPeer from
    # a friend session (that silently breaks the loopback connection). Env overrides
    # the file. For a real two-machine Steam test, use the in-game F2 panel instead.
    $env:KENSHICOOP_TRANSPORT    = "udp"
    $env:KENSHICOOP_STEAM_PEER   = "0"
    # Auto-connect at load using the env role/transport above. EXCEPTION: with
    # -JoinFromMenu the JOIN must WAIT at the main menu so the user can bring up
    # the F2 panel and go ONLINE by hand (the whole point of that mode - the join
    # user configures the connection at the menu). The host still auto-connects
    # (it hosts + loads its save); on the join's manual connect the host pushes
    # its world.
    $env:KENSHICOOP_AUTOCONNECT  = if ($TitleScreen) { "0" } elseif ($Mode -eq "join" -and $JoinFromMenu) { "0" } else { "1" }
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_IP           = $Ip
    $env:KENSHICOOP_SAVE         = $SaveName
    $env:KENSHICOOP_TEST_SECONDS = "0"     # manual: never self-exit
    $env:KENSHICOOP_SCENARIO     = ""      # manual: no scenario
    $env:KENSHICOOP_OWN_SQUAD    = $Own
    # Host-only one-shot world spawn for baking a deterministic test scene.
    $env:KENSHICOOP_SETUP        = if ($Mode -eq "join") { "" } else { $SetupScene }
    # Host-only auto-recruit (N s after gameplay): recruit nearest world NPC.
    $env:KENSHICOOP_AUTORECRUIT  = if ($Mode -eq "join") { "0" } else { "$AutoRecruit" }
    $env:KENSHICOOP_PROBE_RECRUIT = if ($Mode -eq "join" -and $ProbeRecruit) { "1" } else { "0" }
    $env:KENSHICOOP_PROBE_AISUSPEND = if ($Mode -eq "join" -and $ProbeAiSuspend) { "1" } else { "0" }
    # Inventory sync is bidirectional, so enable it on BOTH clients.
    if ($InvSync) { $env:KENSHICOOP_INV_SYNC = "1" }
    # World-item sync is host-authored + join-observed, but the gate is read on both
    # clients (host publishes, join applies), so enable it on BOTH.
    if ($WorldSync) { $env:KENSHICOOP_WORLD_SYNC = "1" }
    $env:KENSHICOOP_INV_DUMP     = if ($InvDump) { "1" } else { "0" }
    $env:KENSHICOOP_DEBUG_MARKERS = if ($DebugMarkers) { "1" } else { "0" }
    # Presence authority MUST match on both clients: each side publishes claims and
    # reads the peer's, so one side alone would author its cells while the other
    # went on enforcing host authority over the same bodies. Pinned explicitly in
    # BOTH directions rather than left unset, so the session does not silently
    # change meaning if the plugin-side default moves again.
    $env:KENSHICOOP_CELL_AUTH    = if ($NoCellAuth) { "0" } else { "1" }
    # Same reasoning as the line above: the collapse verdict has to be computed
    # identically on both sides or the two would disagree about who authors a
    # shared cell, so pin it explicitly rather than relying on the plugin default
    # being the same in both installs.
    $env:KENSHICOOP_CELL_COLLAPSE = if ($NoCollapse) { "0" } else { "1" }
    # Track on BOTH: each client can only see its own tabs' true positions, and
    # the interesting comparison is what the two logs say about the same walk.
    $env:KENSHICOOP_TRACK_MOVE   = if ($TrackMove) { "1" } else { "0" }
    # Phase 6 shackle trace on both clients (see -DebugShackle).
    $env:KENSHICOOP_DEBUG_SHACKLE = if ($DebugShackle) { "1" } else { "0" }
    # Jail long-play probes on BOTH clients (spike 58, see -JailProbe): STATE/SNAP
    # + task-selection traces + auditRows (auditRows keys off KENSHICOOP_JAIL_PROBE
    # in Plugin.cpp when no scenario name is set). NOTE: JAIL_OBSERVE is NOT armed
    # by -JailProbe - observe disables the captive self-heal (captives exit
    # furniture + walk off on the observing side), which is a diagnostic
    # divergence, not real behavior. It is opt-in via -JailObserve.
    $env:KENSHICOOP_JAIL_PROBE   = if ($JailProbe) { "1" } else { "0" }
    $env:KENSHICOOP_TASK_SPIKE   = if ($JailProbe) { "1" } else { "0" }
    $env:KENSHICOOP_JAIL_OBSERVE = if ($JailObserve) { "1" } else { "0" }
    $env:KENSHICOOP_LOG          = Join-Path $dest "$Mode.log"
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec -NativeOutput $NativeBin
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

$joinPid = 0
Write-Host ""
$hostOwnEnv = $HostSquads
$joinOwnEnv = $JoinSquads

$hostSaveName = if ($TitleScreen) { "" } else { $Save }
Write-Host "Launching HOST ($(if ($TitleScreen) { '<main menu - no save>' } else { "save $Save" })) ..."
Set-CoopEnv -Mode "host" -SaveName $hostSaveName -Own $hostOwnEnv
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

if (-not $NoJoin) {
    Write-Host "Waiting $JoinDelaySec s before launching JOIN ..."
    Start-Sleep -Seconds $JoinDelaySec
    # JoinFromMenu: empty save name -> the join stays at the main menu and the
    # host's push-on-connect pulls it into the world.
    $joinSaveName = if ($JoinFromMenu -or $TitleScreen) { "" } else { $JoinSave }
    Write-Host "Launching JOIN (save $(if ($JoinFromMenu -or $TitleScreen) { '<main menu - no save>' } else { $JoinSave })) ..."
    Set-CoopEnv -Mode "join" -SaveName $joinSaveName -Own $joinOwnEnv
    $joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
    if ($joinPid -eq 0) { throw "Join failed to get past the launcher." }
}

Write-Host ""
Write-Host "== Session live =="
Write-Host "  host PID=$hostPid  join PID=$joinPid"
if ($TitleScreen) {
    Write-Host "  BOTH clients are at the MAIN MENU (no auto-load, autoconnect OFF)."
    Write-Host "  1) Load the SAME save on both (Continue / Load)."
    Write-Host "  2) On each window press F2 -> set Connection ONLINE (host role on one,"
    Write-Host "     join role on the other; UDP loopback is preset). Then free-play."
    Write-Host "  Close both windows when done (no auto-exit)."
} elseif ($JoinFromMenu) {
    Write-Host "  JOIN is waiting at the MAIN MENU (no save, autoconnect OFF)."
    Write-Host "  On the JOIN window: press F2 -> set Connection to ONLINE (role JOIN, UDP is"
    Write-Host "  preset for loopback). The host then pushes its world and the join loads in"
    Write-Host "  straight from the menu - no save needed on the join. Close windows when done."
} else {
    Write-Host "  Move characters in your owned squad tabs; inspect both clients."
    Write-Host "  Close both windows when done (no auto-exit)."
}

# Check the unique current-run logs while the held processes are live. An old
# install-root log can never satisfy these checks.
foreach ($client in @(@{ Pid=$hostPid; Role='host'; Own=$HostSquads }, @{ Pid=$joinPid; Role='join'; Own=$JoinSquads })) {
    if ($client.Pid -le 0) { continue }
    $game = $runState.Processes[$client.Pid]
    $log = Join-Path $dest "$($client.Role).log"
    $pattern = 'ownership ranks = \{' + [regex]::Escape($client.Own) + '\}'
    if (-not (Wait-CoopOwnedLogLine -Process $game -Deadline ([DateTime]::MaxValue) -File $log -Pattern $pattern -TimeoutSec $StartTimeoutSec)) {
        throw "Current $($client.Role) plugin did not confirm requested squad ownership {$($client.Own)}."
    }
}
foreach ($game in @($hostPid,$joinPid) | Where-Object { $_ -gt 0 } | ForEach-Object { $runState.Processes[$_] }) {
    Wait-CoopOwnedExit -Process $game -Deadline ([DateTime]::MaxValue)
}
foreach ($client in @(@{Dir=$HostDir;Role='host'},@{Dir=$JoinDir;Role='join'})) {
    if ($client.Role -eq 'join' -and $NoJoin) { continue }
    $source = Join-Path $client.Dir 'kenshi_info.log'
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $dest "$($client.Role)_engine.log") }
}
Write-Host "Manual session artifacts: $dest"
} finally {
    Restore-CoopHarnessSession -State $runState
}
