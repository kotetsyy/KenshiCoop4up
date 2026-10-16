<#
.SYNOPSIS
  Two-machine LAN test runner: the LAN machine (machine 2, provisioned by
  setup_lan_host.ps1) plays HOST via its KenshiCoopHost scheduled task; this
  machine launches only the JOIN - mirroring the real session topology where
  the friend hosts and you iterate on the join client.

.DESCRIPTION
  Mirrors run_test.ps1's contract: manifest-driven save/setup/tolerance/gates,
  -Wan <profile> (the netsim proxy runs LOCALLY in front of the LAN link, so
  the run is LAN-real AND WAN-degraded), -FakeClockSkewMs (join side), exit
  code = verdict, verdict.json in the out dir. Differences:
    * host launch/collect goes over SSH/SCP (args JSON -> schtasks /run ->
      poll -> scp results back)
    * the freshly built DLL is pushed to machine 2 EVERY run (the protocol
      handshake rejects mixed builds by design - lockstep is mandatory)
    * CLOCKSYNC now measures a REAL two-machine clock offset

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario coop_presence

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario combat_kill -Wan bad -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Scenario = "",
    [string]$Save = "",
    [string]$Setup = "",
    [double]$Tolerance = 0,
    [int]$Seconds = 0,
    [int]$KillGraceSec = 0,
    [int]$Port = 27800,
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [string]$NativeBin = "",
    [switch]$SkipBuild,
    [string]$Wan = "",
    [int]$FakeClockSkewMs = 0,
    # Timeout profile; 'lan' covers machine 2's independent load times.
    [string]$Profile = "lan",
    [int]$Frames = 5,
    [int]$FrameIntervalMs = 16
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'NativeBuild.ps1')
. (Join-Path $scriptDir 'kit_preflight.ps1')

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
$manifest = Get-ScenarioManifest
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }

# ---- LAN config -----------------------------------------------------------------
$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (run setup_lan_host.ps1 first)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"
$dropFwd = $cfg.dropDir -replace '\\', '/'

function Invoke-LanSsh {
    param([string]$Cmd)
    # PowerShell 5.1 does not escape embedded double quotes for native args;
    # escape them so quoted remote paths (spaces) survive the trip. Tolerate
    # stderr noise (ssh WARNINGs) that 2>&1 + EAP=Stop would turn terminating.
    $escaped = $Cmd -replace '"', '\"'
    $PSNativeCommandArgumentPassing = 'Legacy'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

# ---- Manifest defaults ---------------------------------------------------------------
$manifestEntry = $null
if ($Scenario -ne "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $manifestEntry = $manifest.Scenarios[$Scenario]
    if ($Save -eq "")     { $Save = $manifestEntry.Save }
    if (-not $PSBoundParameters.ContainsKey('Setup')) { $Setup = $manifestEntry.Setup }
    if ($Tolerance -eq 0) { $Tolerance = $manifestEntry.Tolerance }
}
if ($Scenario -and -not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }
if (-not $manifestEntry -or (-not $manifestEntry.PrimaryGate -and @($manifestEntry.Gating).Count -eq 0)) { throw 'LAN gameplay certification requires a selected scenario with real required gates; use a session runner for diagnostic-only collection.' }
if ($Tolerance -eq 0) { $Tolerance = 3.0 }
if ($Save -eq "") { throw "No -Save given and scenario '$Scenario' has no manifest default." }
if (-not $PSBoundParameters.ContainsKey('Seconds')) { $Seconds = if ($manifestEntry -and $manifestEntry.ContainsKey('Seconds')) { [int]$manifestEntry.Seconds } else { 150 } }

$prof = $manifest.Profiles[$Profile]
if ($null -eq $prof) { throw "Unknown profile '$Profile'" }
$armTimeoutMs = $prof.ArmTimeoutMs
if (-not $PSBoundParameters.ContainsKey('KillGraceSec')) { $KillGraceSec = if ($manifestEntry -and $manifestEntry.ContainsKey('KillGraceSec')) { [int]$manifestEntry.KillGraceSec } else { [int]$prof.KillGraceSec } }
$JoinDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($JoinDir)

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\lan_$stamp"
}
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
$runId = [Guid]::NewGuid().ToString('N')
$OutDir = Join-Path $OutDir $runId
New-Item -ItemType Directory -Path $OutDir | Out-Null
$remoteTriggered = $false
$remoteReserved = $false
$ownedExitFailures = @()
$joinLog = Join-Path $OutDir "join.log"
$joinPng = Join-Path $OutDir "join.png"

$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe" }
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $Save))) { throw "Save '$Save' not found locally in $saveRoot" }

Write-Host "== KenshiCoop LAN test run =="
Write-Host "  remote host: $target ($($cfg.kenshiDir))"
Write-Host "  save:        $Save"
Write-Host "  scenario:    $Scenario (tolerance $Tolerance u)"
if ($Wan -ne "") { Write-Host "  wan:         $Wan (local proxy in front of the LAN link)" }
if ($FakeClockSkewMs -ne 0) { Write-Host "  skew:        join clock +${FakeClockSkewMs}ms (on top of the REAL clock offset)" }
Write-Host "  out dir:     $OutDir"


$runState = New-CoopRunState -GameDirs @($JoinDir) -SaveNames @($Save,'coopresume')
$inviteRoot = Join-Path $env:LOCALAPPDATA ('KenshiCoop\private\lan-' + [Guid]::NewGuid().ToString('N'))
$inviteFile = Assert-CoopPrivatePath (Join-Path $inviteRoot 'client.invite') @($JoinDir,$repoRoot,$OutDir)
New-Item -ItemType Directory -Path $inviteRoot | Out-Null
$wanProc = $null
try {
Clear-CoopRunEnvironment -State $runState
# ---- 1. Build + deploy locally, push DLL to machine 2 (lockstep) --------------------------
if (-not $SkipBuild) {
    Write-Host "=== build + local deploy ==="
    # Existing games are rejected by the owning state preflight, never killed.
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($JoinDir) -NativeBin $NativeBin
}
$runtime = Get-CoopHarnessRuntime -RepoRoot $repoRoot -NativeBin $NativeBin
$native = Get-NativeBuild -Configuration Harness -NativeBin $NativeBin
$runnerScripts = @('lan_host_run.ps1','start_kenshi.ps1','set_video_mode.ps1','kit_preflight.ps1','screenshot.ps1','CoopHarness.psm1','CoopOracles.psm1') | ForEach-Object { Join-Path $scriptDir $_ }
foreach ($file in $runnerScripts) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Complete current remote runner inputs required before mutation.' } }
$r = Invoke-LanSsh "mkdir `"$($cfg.dropDir)\owner.lock`" && > `"$($cfg.dropDir)\owner.lock\token.txt`" echo $runId && echo RESERVED"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'RESERVED') { throw 'Remote admission is occupied; never overwrite active owner inputs.' }
$remoteReserved = $true
$r = Invoke-LanSsh 'powershell -NoProfile -Command "if(Get-Process kenshi_x64 -ErrorAction SilentlyContinue){exit 1}; if(Get-ScheduledTask -TaskName KenshiCoopHost -ErrorAction SilentlyContinue | Where-Object {$_.State -eq ''Running''}){exit 1}; Write-Output SAFE"'
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'SAFE') { throw 'Remote game or a previous scheduled owner is active.' }
[void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)\mod`" 2>nul & echo OK")
& scp -o BatchMode=yes @runnerScripts "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw 'Current remote runner staging failed.' }
& scp -o BatchMode=yes -r (Join-Path $scriptDir 'oracles') "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw 'Remote oracle staging failed.' }
& scp -o BatchMode=yes @runtime "${target}:$dropFwd/mod/"
if ($LASTEXITCODE -ne 0) { throw 'Complete runtime staging failed.' }
& scp -o BatchMode=yes $native.Files['transport_credentials.exe'] $native.Files['KenshiCoopNetUDP.dll'] "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw 'Native private provision inputs push failed.' }
$r = Invoke-LanSsh "echo %LOCALAPPDATA%"
$remoteLocal = ("" + (@($r.out) | Select-Object -Last 1)).Trim()
$remotePrivate = "$remoteLocal\KenshiCoop\private\lan-$runId"
$provisionScript = ". '$((Join-Path $cfg.dropDir 'kit_preflight.ps1').Replace("'","''"))'; [void](New-CoopUdpCredentials -NativeBin '$($cfg.dropDir.Replace("'","''"))' -ProfilePath '$($remotePrivate.Replace("'","''"))\host.credential' -InviteFile '$($remotePrivate.Replace("'","''"))\client.invite' -GameDirs @('$($cfg.kenshiDir.Replace("'","''"))','$($cfg.dropDir.Replace("'","''"))'))"
$provisionEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($provisionScript))
$r = Invoke-LanSsh "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $provisionEncoded"
if ($r.exit -ne 0) { throw 'Remote native credential provisioning failed.' }
& scp -o BatchMode=yes "${target}:$($remotePrivate -replace '\\','/')/client.invite" $inviteFile
if ($LASTEXITCODE -ne 0) { throw 'Private invitation import failed.' }
# Restrict the imported private file immediately; do not add it to run metadata.
$acl = New-Object Security.AccessControl.FileSecurity
$acl.SetAccessRuleProtection($true,$false)
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule([Security.Principal.WindowsIdentity]::GetCurrent().User,'FullControl','Allow')))
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule('SYSTEM','FullControl','Allow')))
Set-Acl -LiteralPath $inviteFile -AclObject $acl

# Stage only the named fixture; the interactive owner snapshots/restores targets.
[void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)\save`" 2>nul & echo OK")
$r = Invoke-LanSsh "if exist `"$($cfg.dropDir)\save\$Save`" rmdir /S /Q `"$($cfg.dropDir)\save\$Save`""
if ($r.exit -ne 0) { throw 'Old staged named fixture could not be removed.' }
& scp -o BatchMode=yes -r (Join-Path $saveRoot $Save) "${target}:$dropFwd/save/"
if ($LASTEXITCODE -ne 0) { throw 'Named fixture staging failed.' }
# The native loader receives a name, not the validated AppData path. Select a
# private JOIN root and stage those exact bytes inside the owning snapshot.
$joinFixture = Join-Path $JoinDir "save\$Save"
if (Test-Path -LiteralPath $joinFixture) { Remove-Item -LiteralPath $joinFixture -Recurse -Force }
New-Item -ItemType Directory -Path (Split-Path -Parent $joinFixture) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $saveRoot $Save) -Destination $joinFixture -Recurse
Set-CoopSaveLocation -GameDir $JoinDir -UserLocation 0

# ---- 3. Trigger the remote host ---------------------------------------------------------------
# Env built HERE (single source of truth = the manifest), applied verbatim there.
$hostAnchor = "SCENARIO MEMBER"; $hostShotDelay = 1
if ($Scenario -eq "combat_kill") { $hostAnchor = "SCENARIO KO enforced"; $hostShotDelay = 2 }
if ($manifestEntry -and $manifestEntry.ContainsKey('ShotAnchorHost')) { $hostAnchor = $manifestEntry.ShotAnchorHost }
if ($manifestEntry -and $manifestEntry.ContainsKey('ShotDelaySec')) { $hostShotDelay = [int]$manifestEntry.ShotDelaySec }
$joinAnchor = if ($manifestEntry -and $manifestEntry.ContainsKey('ShotAnchorJoin')) { $manifestEntry.ShotAnchorJoin } else { 'SCENARIO RECV' }
$shotWait = if ($manifestEntry -and $manifestEntry.ContainsKey('ShotWaitSec')) { [int]$manifestEntry.ShotWaitSec } else { [int]$prof.JoinAnchorTimeoutSec }
$joinShotDelay = if ($manifestEntry -and $manifestEntry.ContainsKey('ShotDelaySec')) { [int]$manifestEntry.ShotDelaySec } else { 1 }
$runArgs = [pscustomobject]@{
    runId        = $runId
    udpProfilePath = "$remotePrivate\host.credential"
    seconds      = $Seconds
    anchor       = $(if ($Scenario -ne "") { $hostAnchor } else { "" })
    startTimeoutSec = [int]$prof.StartTimeoutSec
    requireResult = $true
    armTimeoutMs = [int]$armTimeoutMs
    killGraceSec = $KillGraceSec
    anchorTimeoutSec = $shotWait
    shotRequired = [bool]($manifestEntry -and $manifestEntry.ContainsKey('ShotAnchorHost'))
    shotDelaySec = $hostShotDelay
    kenshiDir    = $cfg.kenshiDir
    env          = [pscustomobject]@{
        KENSHICOOP_TRANSPORT          = "udp"
        KENSHICOOP_MODE               = "host"
        KENSHICOOP_IP                 = "0.0.0.0"
        KENSHICOOP_PORT               = "$Port"
        KENSHICOOP_SAVE               = $Save
        KENSHICOOP_TEST_SECONDS       = "$Seconds"
        KENSHICOOP_SCENARIO           = $Scenario
        KENSHICOOP_SETUP              = $Setup
        KENSHICOOP_CELL_AUTH          = "0"
        KENSHICOOP_AUTOCONNECT        = "1"
        KENSHICOOP_PROBE_AISUSPEND    = "0"
        KENSHICOOP_NETSIM_DELAY_MS    = "0"
        KENSHICOOP_NETSIM_JITTER_MS   = "0"
        KENSHICOOP_NETSIM_LOSS_PCT    = "0"
        KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
        KENSHICOOP_ARM_TIMEOUT_MS     = "$armTimeoutMs"
    }
}
$runArgs | Add-Member -NotePropertyName runtimeSha256 -NotePropertyValue ([pscustomobject]@{})
foreach ($file in $runtime) { $runArgs.runtimeSha256 | Add-Member -NotePropertyName ([IO.Path]::GetFileName($file)) -NotePropertyValue (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
# Merge the scenario's manifest DiagEnv into the remote host env (channel A/B
# knobs + log-only traces - the single source of truth Config.cpp no longer
# hard-codes). The remote runner applies run_args.json env verbatim.
if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey('DiagEnv')) {
    foreach ($k in $manifestEntry.DiagEnv.Keys) {
        Add-Member -InputObject $runArgs.env -NotePropertyName $k -NotePropertyValue "$($manifestEntry.DiagEnv[$k])" -Force
    }
}
$argsLocal = Join-Path $OutDir "run_args.json"
$runArgs | ConvertTo-Json -Depth 4 | Set-Content -Path $argsLocal -Encoding UTF8
& scp -o BatchMode=yes $argsLocal "${target}:$dropFwd/run_args.json"
if ($LASTEXITCODE -ne 0) { throw "args push failed" }

# An SSH disconnect can occur after task acceptance: retain admission unless
# schtasks actually returns rejection, then recheck remote ownership on release.
$remoteDeadline = [DateTime]::UtcNow.AddSeconds($prof.StartTimeoutSec + $armTimeoutMs/1000.0 + $Seconds + $KillGraceSec + 60)
$remoteTriggered = $true
$r = Invoke-LanSsh "schtasks /run /tn KenshiCoopHost"
if ($r.exit -ne 0) {
    if ($r.exit -ne 255) { $remoteTriggered = $false }
    throw "task trigger failed: $($r.out)"
}
Write-Host "Remote host task triggered; waiting for it to reach gameplay ..."

$remoteResults = "$($cfg.dropDir)\results\$runId"
$remoteLogCmd  = "type `"$remoteResults\host.log`" 2>nul"
$deadline = (Get-Date).AddSeconds($prof.StartTimeoutSec)
$hostUp = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $r = Invoke-LanSsh $remoteLogCmd
    if ("$($r.out)" -match "gameplay started") { $hostUp = $true; break }
}
if (-not $hostUp) { Write-Warning "Remote host did not reach gameplay within $($prof.StartTimeoutSec)s; continuing best-effort." }
else { Write-Host "Remote host in gameplay." }

# ---- 4. Local WAN proxy (optional) + local JOIN --------------------------------------------------
$wanProc = $null
$joinIp = $cfg.host; $joinPort = $Port
if ($Wan -ne "") {
    $wp = $manifest.WanProfiles[$Wan]
    if ($null -eq $wp) { throw "Unknown WAN profile '$Wan'" }
    $netsimExe = Join-Path $repoRoot "dist\netsim.exe"
    if (-not (Test-Path $netsimExe)) { throw "dist\netsim.exe not built (cmd /c scripts\build_netsim.cmd)" }
    $proxyPort = $Port + 1
    $wanSeed = Get-Random -Minimum 1 -Maximum ([int]::MaxValue)
    $wanArgs = @("$proxyPort", $cfg.host, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
    if ($wp.ContainsKey('StallAtS')) { $wanArgs += @("$wanSeed","$($wp.StallAtS)","$($wp.StallForS)") }
    @{profile=$Wan;seed=$wanSeed;arguments=$wanArgs} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutDir 'wan.json') -Encoding UTF8
    $wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $OutDir "netsim.log") `
        -ArgumentList $wanArgs
    Start-Sleep -Milliseconds 500
    if ($wanProc.HasExited) { throw "netsim exited immediately" }
    $joinIp = "127.0.0.1"; $joinPort = $proxyPort
    Write-Host "WAN proxy up: 127.0.0.1:$proxyPort -> $($cfg.host):$Port ($($wp.DelayMs)ms +/-$($wp.JitterMs)ms, $($wp.LossPct)%)"
}

try {
    Clear-CoopRunEnvironment -State $runState
    $env:KENSHICOOP_AUTOCONNECT = '1'
    $env:KENSHICOOP_MODE               = "join"
    $env:KENSHICOOP_IP                 = $joinIp
    $env:KENSHICOOP_PORT               = "$joinPort"
    $env:KENSHICOOP_SAVE               = $Save
    $env:KENSHICOOP_TEST_SECONDS       = "$Seconds"
    $env:KENSHICOOP_LOG                = $joinLog
    $env:KENSHICOOP_SCENARIO           = $Scenario
    $env:KENSHICOOP_SETUP              = ""
    $env:KENSHICOOP_PROBE_AISUSPEND    = "0"
    # Per-scenario channel A/B knobs + diagnostic traces from the manifest DiagEnv
    # (mirrors the remote host env above; hermetic clear + apply on the join side).
    [void](Set-CoopDiagEnv -Entry $manifestEntry)
    $env:KENSHICOOP_NETSIM_DELAY_MS    = "0"
    $env:KENSHICOOP_NETSIM_JITTER_MS   = "0"
    $env:KENSHICOOP_NETSIM_LOSS_PCT    = "0"
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "$FakeClockSkewMs"
    $env:KENSHICOOP_ARM_TIMEOUT_MS     = "$armTimeoutMs"
    $env:KENSHICOOP_TRANSPORT = 'udp'
    $env:KENSHICOOP_STEAM_PEER = '0'
    Set-CoopPrivateCredentials -Mode join -UseSteam $false -GameDirs @($JoinDir) -InviteFile $inviteFile

    Write-Host "Launching local JOIN -> ${joinIp}:$joinPort ..."
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath $joinExe -WorkDir $JoinDir -TimeoutSec $prof.StartTimeoutSec -NativeOutput $NativeBin
    $out | ForEach-Object { Write-Host "    $_" }
    $joinPid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $joinPid = [int]$Matches[1] }
    if ($joinPid -eq 0) { throw "Join failed to get past the launcher." }

    $game = $runState.Processes[$joinPid]
    $runDeadline = Get-CoopRunDeadline -Process $game -Seconds $Seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $armTimeoutMs -KillGraceSec $KillGraceSec

    if ($Scenario -ne "") {
        if (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $joinLog -Pattern $joinAnchor -TimeoutSec $shotWait) {
            Write-Host "Join scenario anchor observed; capturing screenshot."
            try {
                if ([DateTime]::UtcNow.AddSeconds($joinShotDelay) -ge $runDeadline -or $game.HasExited) { throw 'Game/deadline ended before capture.' }
                Start-Sleep -Seconds $joinShotDelay
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") `
                    -ProcessId $joinPid -ProcessStartTicks ($game.StartTime.ToUniversalTime().Ticks) -Out $joinPng -Frames $Frames -IntervalMs $FrameIntervalMs
                if ($LASTEXITCODE -ne 0) { throw 'Screenshot helper failed.' }
            } catch {
                if ($manifestEntry -and $manifestEntry.ContainsKey('ShotAnchorJoin')) { $ownedExitFailures += "join capture: $($_.Exception.Message)" }
                Write-Warning "Screenshot evidence unavailable: $($_.Exception.Message)"
            }
        } else {
            Write-Warning "No join SCENARIO RECV within $($prof.JoinAnchorTimeoutSec)s."
        }
    }

    try { Wait-CoopOwnedExit -Process $game -Deadline $runDeadline }
    catch { $ownedExitFailures += "join owned exit: $($_.Exception.Message)" }
    $engine = Join-Path $JoinDir 'kenshi_info.log'
    if (Test-Path -LiteralPath $engine) { Copy-Item -LiteralPath $engine -Destination (Join-Path $OutDir 'join_engine.log') }
    if ($manifestEntry -and $manifestEntry.ContainsKey('ShotAnchorJoin') -and -not (Test-Path -LiteralPath $joinPng)) { $ownedExitFailures += 'join: required scenario capture is missing.' }
} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { $wanProc.Kill(); $wanProc.WaitForExit() } catch {}
    }
}

# ---- 5. Collect the remote results -----------------------------------------------------------------
Write-Host "Waiting for the remote host to finish ..."
$deadline = $remoteDeadline
$done = $false
while ([DateTime]::UtcNow -lt $deadline) {
    $r = Invoke-LanSsh "if exist `"$remoteResults\done.json`" (echo DONE) else (echo WAIT)"
    if ("$($r.out)" -match "DONE") { $done = $true; break }
    Start-Sleep -Seconds 5
}
if (-not $done) {
    throw 'Remote owner did not finish; it retains its bounded timeout and restoration. No user process is killed.'
}
& scp -o BatchMode=yes -r "${target}:$dropFwd/results/$runId/*" "$OutDir/"
if ($LASTEXITCODE -ne 0) { throw 'Remote results collection failed.' }
$remoteDone = Get-Content -LiteralPath (Join-Path $OutDir 'done.json') -Raw | ConvertFrom-Json
if ($remoteDone.runToken -cne $runId) { throw 'Remote completion token differs from this request.' }
if (-not $remoteDone.success -or -not $remoteDone.restored) {
    $ownedExitFailures += "host: success=$($remoteDone.success); restored=$($remoteDone.restored); $($remoteDone.error)"
} else {
    [void](Invoke-LanSsh "rmdir /S /Q `"$remoteResults`" 2>nul & echo CLEANED")
}

$hostLog = Join-Path $OutDir "host.log"
Write-Host ""
Write-Host "== Results =="
Write-Host "  host log: $hostLog (collected from $($cfg.host))"
Write-Host "  join log: $joinLog"

} finally {
    try {
        if ($remoteReserved -and -not $remoteTriggered) {
            $release = "`$ErrorActionPreference='Stop'; if((Get-Content -LiteralPath '$($cfg.dropDir.Replace("'","''"))\owner.lock\token.txt' -Raw).Trim() -cne '$runId'){throw 'LAN admission ownership changed.'}; if(Test-Path -LiteralPath '$($cfg.dropDir.Replace("'","''"))\results\$runId'){throw 'Remote owner accepted this request; retain admission.'}; if(Get-ScheduledTask -ErrorAction Stop | Where-Object {`$_.TaskName -eq 'KenshiCoopHost' -and `$_.State -eq 'Running'}){throw 'Remote task is running; retain admission.'}; Remove-Item -LiteralPath '$($cfg.dropDir.Replace("'","''"))\owner.lock' -Recurse -Force"
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($release))
            $r = Invoke-LanSsh "powershell -NoProfile -EncodedCommand $encoded"
            if ($r.exit -ne 0) { throw "Remote admission cleanup failed: $($r.out)" }
        }
    } finally {
        try { Restore-CoopHarnessSession -State $runState -CredentialRoot $inviteRoot -Proxy $wanProc }
        catch { $ownedExitFailures += "join restoration: $($_.Exception.Message)" }
    }
}
# ---- 6. Judge with the shared oracle library --------------------------------------------------------
Write-Host ""
if ($Scenario -ne "") { Write-Host "== Scenario checks: $Scenario ==" }
$runInfo = @{
    lanHost    = $cfg.host
    save       = $Save
    setup      = $Setup
    profile    = $Profile
    wan        = $Wan
    fakeSkewMs = $FakeClockSkewMs
    outDir     = $OutDir
    topology   = "remote-host"
    ownedExitFailures = @($ownedExitFailures)
}
# On two REAL machines the wall clocks already differ by an unknown amount, so
# the "estimated offset must equal -injectedSkew" assertion from loopback runs
# is not measurable here (estimate = realOffset - skew). The injected skew still
# stresses alignment - the time-aligned oracles passing IS the validation - so
# clock_sync gates only on having a healthy estimate.
$expectedSkew = $null
$verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
              -Tolerance $Tolerance -JoinExpected $true `
              -RunInfo $runInfo -ExpectedSkewMs $expectedSkew `
              -WanActive ($Wan -ne "") `
              -OutJson (Join-Path $OutDir "verdict.json")

Write-Host ("RESULT: " + $(if ($verdict.pass) { "PASS" } else { "FAIL" }))
if ($verdict.pass) { exit 0 } else { exit 1 }
