<#
.SYNOPSIS
  Missing-save bootstrap proof (protocol 31/32): force the JOIN to STREAM the
  host's world instead of loading a local copy, then load it - the real
  "seamless join, no pre-shared save" path players hit on a second machine.

.DESCRIPTION
  The normal harness (run_test.ps1) auto-loads the SAME save on both clients, and
  on one machine both installs share %LOCALAPPDATA%\kenshi\save, so a join always
  finds a fingerprint MATCH and loads from disk - the folder-transfer half never
  runs. This runner exercises it end-to-end on a single machine:

    * HOST  loads $Save and goes ONLINE (auto). On the join's connect it bakes its
      live world and announces it with a LOAD_GO ([boot] baking / GO->join).
    * JOIN  stays at the MAIN MENU (it goes ONLINE via auto-start and takes the
      menu bootstrap branch, which returns before any config auto-load) with
      KENSHICOOP_FORCE_STREAM=1 so it NACKs the LOAD_GO, forcing the host to
      stream the folder (SaveXfer over CH_BULK). The join stages, CRC-verifies,
      commits, then loads from its isolated installation-local save root.

  Both clients self-exit (KENSHICOOP_TEST_SECONDS), the per-client logs are
  written straight into the out dir, and the run is graded by the connect_stream
  oracle (Test-ConnectStream): it REQUIRES the transfer edges (NACK -> XFER-BEGIN
  -> XFER-COMMIT badCrc=0 -> XFER-ACK ok=1 -> transfer-committed load -> gameplay),
  so a run that quietly fell back to a MATCH load FAILS.

  The source save is immutable: HOST bakes a unique installation-local copy,
  while JOIN starts without that copy and must receive it through the transfer.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\stream_test.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\stream_test.ps1 -Save sync -SkipBuild
#>
[CmdletBinding()]
param(
    # Save the HOST loads (and therefore bakes + streams to the join). Must exist
    # in %LOCALAPPDATA%\kenshi\save. The join receives THIS save by name.
    [string]$Save = "sync",
    [int]$Seconds = 90,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [int]$JoinDelaySec = 8,
    [int]$StartTimeoutSec = 90,
    [string]$NativeBin = "",
    [switch]$SkipBuild,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'kit_preflight.ps1')
. (Join-Path $scriptDir 'NativeBuild.ps1')

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

if ($Seconds -le 0 -or $StartTimeoutSec -le 0 -or $JoinDelaySec -lt 0) { throw 'Stream durations must be positive (join delay may be zero).' }
$transferAllowance = 180
$hostSeconds = $Seconds + $JoinDelaySec + $StartTimeoutSec + $transferAllowance
if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\stream_$stamp"
}
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
if ((Test-Path -LiteralPath $OutDir) -and @(Get-ChildItem -LiteralPath $OutDir -Force).Count) { throw "OutDir must be empty: $OutDir" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

. (Join-Path $scriptDir 'capture_save.ps1')
Assert-CoopSaveName -Name $Save
$saveRoot = Resolve-CoopSelectedSaveRoot -GameDir $HostDir
$sourceSave = Join-Path $saveRoot $Save
Assert-CoopSavePath -Path $sourceSave
if (-not (Test-Path -LiteralPath (Join-Path $sourceSave 'quick.save') -PathType Leaf)) { throw "Missing host quick.save: $sourceSave" }
$Save = 'stream_' + [Guid]::NewGuid().ToString('N')

$hostLog = Join-Path $OutDir "host.log"
$joinLog = Join-Path $OutDir "join.log"

Write-Host "== KenshiCoop STREAM (missing-save bootstrap) test =="
Write-Host "  host save:  $Save (baked + streamed to the join)"
Write-Host "  join:       stays at menu online, FORCE_STREAM=1 (NACKs to pull the folder)"
Write-Host "  seconds:    $Seconds (self-exit)"
Write-Host "  out dir:    $OutDir"

$runState = New-CoopRunState -GameDirs @($HostDir,$JoinDir)
$credentials = $null
try {
$encoding = [Text.Encoding]::GetEncoding(28591)
$pattern = '(?m)^(\s*User save location\s*=\s*)[01](\s*)$'
foreach ($dir in @($HostDir,$JoinDir)) {
    [void](Resolve-CoopSelectedSaveRoot -GameDir $dir)
    $cfg = Join-Path $dir 'settings.cfg'
    $text = $encoding.GetString([IO.File]::ReadAllBytes($cfg))
    if ([regex]::Matches($text,$pattern).Count -ne 1) { throw "Invalid native save location in $cfg" }
    Add-CoopRunBackup -State $runState -Path $cfg
    foreach ($suffix in @('','__incoming','__old')) {
        $path = Join-Path $dir "save\$Save$suffix"
        Add-CoopRunBackup -State $runState -Path $path
        if (Test-Path -LiteralPath $path) { throw "Run-owned save path unexpectedly exists: $path" }
    }
    [IO.File]::WriteAllBytes($cfg,$encoding.GetBytes([regex]::Replace($text,$pattern,'${1}0${2}')))
}
New-Item -ItemType Directory -Force -Path (Join-Path $HostDir 'save') | Out-Null
Copy-Item -LiteralPath $sourceSave -Destination (Join-Path $HostDir "save\$Save") -Recurse

# Build + deploy the FORCE_STREAM plugin into BOTH installs (unless skipped).
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build plugin ==="
    & cmd /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd failed ($LASTEXITCODE)" }
}
if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== deploy ==="
    Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($HostDir,$JoinDir) -NativeBin $NativeBin
}

$native = Get-NativeBuild -Configuration Harness -NativeBin $NativeBin
$credentials = New-CoopLocalCredentials -NativeBin $native.Bin -GameDirs @($HostDir,$JoinDir)
function Set-CoopEnv {
    param([string]$Mode, [string]$SaveName, [string]$Log, [string]$ForceStream)
    Clear-CoopRunEnvironment -State $runState
    $env:KENSHICOOP_MODE          = $Mode
    # Same-machine loopback: force direct UDP (Steam P2P can't self-connect).
    $env:KENSHICOOP_TRANSPORT     = "udp"
    $env:KENSHICOOP_STEAM_PEER    = "0"
    Set-CoopPrivateCredentials -Mode $Mode -UseSteam $false -GameDirs @($HostDir,$JoinDir) -ProfilePath $credentials.Profile -InviteFile $credentials.Invite
    $env:KENSHICOOP_PORT          = "$Port"
    $env:KENSHICOOP_IP            = $Ip
    $env:KENSHICOOP_SAVE          = $SaveName   # host: real save; join: "" (menu)
    # TEST_SECONDS>0 both self-exits AND forces the load-time auto-start, so the
    # join goes ONLINE from the menu with no manual F2 (see Plugin.cpp autoStart).
    $env:KENSHICOOP_TEST_SECONDS  = if ($Mode -eq 'host') { "$hostSeconds" } else { "$Seconds" }
    $env:KENSHICOOP_SCENARIO      = ""          # normal co-op tick, no scenario
    $env:KENSHICOOP_LOG           = $Log
    # The whole point: join NACKs a matching save so the transfer really runs.
    if ($Mode -eq 'join') { $env:KENSHICOOP_FORCE_STREAM = '1' }
    # Keep the coordinated save/load channels ON (defaults) - leave them unset.
}

$deadlines = @{}
function Wait-ForLogLine {
    param([string]$File,[string]$Pattern,[int]$TimeoutSec)
    $id = if ($File -eq $hostLog) { $hostPid } else { $joinPid }
    return (Wait-CoopOwnedLogLine -Process $runState.Processes[$id] -Deadline $deadlines[$id] -File $File -Pattern $Pattern -TimeoutSec $TimeoutSec)
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec -NativeOutput $NativeBin
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

function Test-Alive { param([int]$ProcId) return ($ProcId -gt 0 -and $runState.Processes.ContainsKey($ProcId) -and -not $runState.Processes[$ProcId].HasExited) }

# ---- Launch HOST (loads $Save, hosts, bakes on connect) -------------------------
Write-Host ""
Write-Host "Launching HOST (save $Save) ..."
Set-CoopEnv -Mode "host" -SaveName $Save -Log $hostLog -ForceStream ""
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }
$deadlines[$hostPid] = Get-CoopRunDeadline -Process $runState.Processes[$hostPid] -Seconds $hostSeconds -StartTimeoutSec $StartTimeoutSec -ArmTimeoutMs 0 -KillGraceSec 60

Write-Host "Waiting for HOST to reach gameplay (timeout ${StartTimeoutSec}s) ..."
if (Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec) {
    Write-Host "Host in gameplay; launching JOIN after ${JoinDelaySec}s ..."
} else {
    throw 'Host did not reach gameplay within its owned startup budget.'
}
$remaining = ($deadlines[$hostPid] - [DateTime]::UtcNow).TotalSeconds
if ($remaining -lt $JoinDelaySec) { throw 'Host budget expired before JOIN launch.' }
Start-Sleep -Seconds $JoinDelaySec
if ($runState.Processes[$hostPid].HasExited) { throw 'Host exited before JOIN launch.' }

# ---- Launch JOIN (stays at menu, online, FORCE_STREAM) --------------------------
Write-Host "Launching JOIN (stays at menu, online, FORCE_STREAM=1) ..."
Set-CoopEnv -Mode "join" -SaveName '' -Log $joinLog -ForceStream "1"
$joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
if ($joinPid -eq 0) { throw "Join failed to get past the launcher." }
$deadlines[$joinPid] = Get-CoopRunDeadline -Process $runState.Processes[$joinPid] -Seconds $Seconds -StartTimeoutSec ($StartTimeoutSec + $transferAllowance) -ArmTimeoutMs 0 -KillGraceSec 60

Write-Host "Host game PID=$hostPid  Join game PID=$joinPid"

# Progress breadcrumbs (best-effort; the oracle is the real verdict).
if (Wait-ForLogLine -File $joinLog -Pattern "\[save\] XFER-RECV" -TimeoutSec 60) {
    Write-Host "  join is RECEIVING the streamed save ..."
    if (Wait-ForLogLine -File $joinLog -Pattern "\[save\] XFER-COMMIT" -TimeoutSec 60) {
        Write-Host "  join COMMITTED the streamed save."
    }
} else {
    Write-Warning "  did not see [save] XFER-RECV on the join within 60s (transfer may not have engaged)."
}

$ownedExitFailures = @()
foreach ($p in @($hostPid,$joinPid)) {
    try { Wait-CoopOwnedExit -Process $runState.Processes[$p] -Deadline $deadlines[$p] }
    catch { $ownedExitFailures += $_.Exception.Message }
}

Write-Host ""
Write-Host "== Results =="
Write-Host "  host log: $hostLog"
Write-Host "  join log: $joinLog"

# ---- Grade with the connect_stream oracle (transfer REQUIRED) -------------------
Write-Host ""
Write-Host "== connect_stream oracle =="
Reset-GateResults
$status = Invoke-OneOracle -Id "connect_stream" -HostLog $hostLog -JoinLog $joinLog -Tolerance 0 -ExpectedSkewMs $null

$healthHost = Test-LogHealth -File $hostLog -Label 'host' -Required $true -CleanPattern 'test duration elapsed; exiting'
$healthJoin = Test-LogHealth -File $joinLog -Label 'join' -Required $true -CleanPattern 'test duration elapsed; exiting'
$ok = $status -eq 'PASS' -and $healthHost -eq 'PASS' -and $healthJoin -eq 'PASS' -and $ownedExitFailures.Count -eq 0
} finally {
    Restore-CoopHarnessSession -State $runState -CredentialRoot $(if ($credentials) { $credentials.Root } else { '' })
}
Write-Host ("STREAM-TEST RESULT: " + $(if ($ok) { 'PASS' } else { 'FAIL' }))
if ($ok) { exit 0 } else { exit 1 }
