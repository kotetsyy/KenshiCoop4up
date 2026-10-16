<#
.SYNOPSIS
  DRESS REHEARSAL for the remote session kit, entirely on this machine: install
  the kit into the local Kenshi-Join install exactly as the friend would, run a
  host + kit-join session THROUGH the WAN relay proxy, then judge the collected
  logs with analyze_run.ps1 - so the eventual internet session changes only the
  network, not the workflow.

.DESCRIPTION
  Flow:
    1. (build +) assemble the kit          - scripts\make_remote_kit.ps1
    2. deploy the same build to the host   - scripts\deploy.cmd
    3. start dist\netsim.exe ('bad' WAN profile by default)
    4. launch the HOST (host_session-equivalent env, but unattended)
    5. run the kit's friend_join.ps1 against 127.0.0.1:<proxy port>,
       pointed at the local Kenshi-Join install
    6. judge host.log + the friend results join.log via Invoke-RunAnalysis

  PASS means: kit install works, kit connects through a degraded link, the
  scenario passes, and the results bundle contains everything analyze_run needs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_remote_kit.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_remote_kit.ps1 -SkipBuild -Wan regional
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$Wan = "bad",
    [string]$NativeBin = "",
    [switch]$SkipBuild,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'kit_preflight.ps1')
. (Join-Path $scriptDir 'NativeBuild.ps1')

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
$manifest = Get-ScenarioManifest
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }
if ($Save -eq "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $Save = $manifest.Scenarios[$Scenario].Save
}
if ($Save -eq "") { throw "No save for scenario '$Scenario' (pass -Save)." }
if (-not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }
$entry = $manifest.Scenarios[$Scenario]
if (-not $entry.PrimaryGate -and @($entry.Gating).Count -eq 0) { throw 'Kit rehearsal cannot certify a diagnostic-only scenario.' }
$prof = $manifest.Profiles.remote
$seconds = if ($entry.ContainsKey('Seconds')) { [int]$entry.Seconds } else { 150 }
$killGraceSec = if ($entry.ContainsKey('KillGraceSec')) { [int]$entry.KillGraceSec } else { [int]$prof.KillGraceSec }
$HostDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($HostDir)
$JoinDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($JoinDir)
if ($HostDir.TrimEnd('\') -ieq $JoinDir.TrimEnd('\') -or $HostDir.StartsWith($JoinDir.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase) -or $JoinDir.StartsWith($HostDir.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'HOST and JOIN require disjoint installations.' }

$runState = New-CoopRunState -GameDirs @($HostDir,$JoinDir) -SaveNames @($Save,'coopresume')
$credentials = $null
$wanProc = $null
try {
Clear-CoopRunEnvironment -State $runState
# ---- 1. Kit ---------------------------------------------------------------------
Write-Host "=== 1. assemble kit ==="
$kitArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", (Join-Path $scriptDir "make_remote_kit.ps1"),
             "-Role", "join",   # this rehearsal drives the kit's friend_join.ps1
             "-Save", $Save, "-Scenario", $Scenario, "-Port", "$Port")
if ($NativeBin) { $kitArgs += @('-NativeBin',$NativeBin) }
if ($SkipBuild) { $kitArgs += "-SkipBuild" }
& powershell @kitArgs | Tee-Object -Variable kitOutput | ForEach-Object { Write-Host "$_" }
if ($LASTEXITCODE -ne 0) { throw "kit assembly failed" }
$assembled = @($kitOutput | Where-Object { "$_" -match '^Kit assembled:\s+' } | Select-Object -Last 1)
if (-not $assembled.Count) { throw 'Kit builder did not identify its exact owned staging directory.' }
$kitDir = ("$($assembled[0])" -replace '^Kit assembled:\s+','').Trim()

# ---- 2. Host deploy (same build as the kit) ---------------------------------------
Write-Host ""
Write-Host "=== 2. deploy to host install ==="
# Owning preflight rejects existing games; never terminate named user processes.
Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($HostDir) -NativeBin $NativeBin
# Load the same immutable fixture used to assemble the kit, not a stale native
# installation-local save. The JOIN child selects its separate private root.
$hostFixture = Join-Path $HostDir "save\$Save"
if (Test-Path -LiteralPath $hostFixture) { Remove-Item -LiteralPath $hostFixture -Recurse -Force }
New-Item -ItemType Directory -Path (Split-Path -Parent $hostFixture) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $kitDir "save\$Save") -Destination $hostFixture -Recurse
Set-CoopSaveLocation -GameDir $HostDir -UserLocation 0

$native = Get-NativeBuild -Configuration Harness -NativeBin $NativeBin
$credentials = New-CoopLocalCredentials -NativeBin $native.Bin -GameDirs @($HostDir,$JoinDir)
# ---- 3. WAN proxy ------------------------------------------------------------------
Write-Host ""
Write-Host "=== 3. WAN relay proxy ($Wan) ==="
$netsimExe = Join-Path $repoRoot "dist\netsim.exe"
if (-not (Test-Path $netsimExe)) {
    & cmd.exe /c "`"$scriptDir\build_netsim.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "netsim build failed" }
}
$wp = $manifest.WanProfiles[$Wan]
if (-not $wp) { throw "Unknown WAN profile '$Wan'." }
$proxyPort = $Port + 1
$stampDir = Join-Path $repoRoot ("tools\test-runs\rehearsal_" + (Get-Date -Format "yyyyMMdd_HHmmss") + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stampDir | Out-Null
$wanSeed = Get-Random -Minimum 1 -Maximum ([int]::MaxValue)
$wanArgs = @("$proxyPort", "127.0.0.1", "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
if ($wp.ContainsKey('StallAtS')) { $wanArgs += @("$wanSeed","$($wp.StallAtS)","$($wp.StallForS)") }
@{profile=$Wan;seed=$wanSeed;arguments=$wanArgs} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stampDir 'wan.json') -Encoding UTF8
$wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $stampDir "netsim.log") `
    -ArgumentList $wanArgs
Start-Sleep -Milliseconds 500
if ($wanProc.HasExited) { throw "netsim exited immediately" }
Write-Host "  proxy up: 127.0.0.1:$proxyPort -> 127.0.0.1:$Port ($($wp.DelayMs)ms +/-$($wp.JitterMs)ms, $($wp.LossPct)% loss)"

$hostLog = Join-Path $stampDir "host.log"
try {
    # ---- 4. Host (unattended) --------------------------------------------------------
    Write-Host ""
    Write-Host "=== 4. launch host ==="
    Clear-CoopRunEnvironment -State $runState
    [void](Set-CoopDiagEnv -Entry $entry)
    $env:KENSHICOOP_AUTOCONNECT = '1'
    $env:KENSHICOOP_MODE         = "host"
    $env:KENSHICOOP_IP           = "0.0.0.0"
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_SAVE         = $Save
    $env:KENSHICOOP_LOG          = $hostLog
    $env:KENSHICOOP_SCENARIO     = $Scenario
    $env:KENSHICOOP_SETUP        = $manifest.Scenarios[$Scenario].Setup
    $env:KENSHICOOP_TEST_SECONDS = "$seconds"
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
    $env:KENSHICOOP_ARM_TIMEOUT_MS = "$($prof.ArmTimeoutMs)"
    $env:KENSHICOOP_TRANSPORT = 'udp'
    $env:KENSHICOOP_STEAM_PEER = '0'
    Set-CoopPrivateCredentials -Mode host -UseSteam $false -GameDirs @($HostDir,$JoinDir) -ProfilePath $credentials.Profile
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath (Join-Path $HostDir 'kenshi_x64.exe') -WorkDir $HostDir -TimeoutSec $prof.StartTimeoutSec -NativeOutput $NativeBin
    $out | ForEach-Object { Write-Host "    $_" }
    $hostGamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $hostGamePid = [int]$Matches[1] }
    if ($hostGamePid -eq 0) { throw "Host failed to launch." }
    $hostGame = $runState.Processes[$hostGamePid]
    $hostDeadline = Get-CoopRunDeadline -Process $hostGame -Seconds $seconds -StartTimeoutSec $prof.StartTimeoutSec -ArmTimeoutMs $prof.ArmTimeoutMs -KillGraceSec $killGraceSec

    Start-Sleep -Seconds 8

    # ---- 5. Kit join, exactly as the friend runs it -----------------------------------
    Write-Host ""
    Write-Host "=== 5. run the kit's friend_join.ps1 (as the friend would) ==="
    $env:KENSHICOOP_UDP_INVITE_FILE = $credentials.Invite
    $friendToken = [Guid]::NewGuid().ToString('N')
    $peerTokens = @($runState.Processes.Values | Where-Object { -not $_.HasExited -and $_.ProcessName -ieq 'kenshi_x64' } | ForEach-Object { "$($_.Id):$($_.StartTime.ToUniversalTime().Ticks)" })
    $peerLiteral = (@($peerTokens | ForEach-Object { "'$_'" }) -join ',')
    $action = "`$ErrorActionPreference='Stop'; & '$($kitDir.Replace("'","''"))\friend_join.ps1' -HostIp '127.0.0.1' -Port $proxyPort -KenshiDir '$($JoinDir.Replace("'","''"))' -OwnedPeerTokens @($peerLiteral) -RunToken '$friendToken'"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($action))
    & powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded
    $friendExit = $LASTEXITCODE

    Wait-CoopOwnedExit -Process $hostGame -Deadline $hostDeadline
    if ($friendExit -ne 0) { throw "friend_join lifecycle failed (exit=$friendExit)." }
    $engine = Join-Path $HostDir 'kenshi_info.log'
    if (Test-Path -LiteralPath $engine) { Copy-Item -LiteralPath $engine -Destination (Join-Path $stampDir 'host_engine.log') }

    # ---- 6. Judge the collected logs ------------------------------------------------------
    Write-Host ""
    Write-Host "=== 6. judge collected logs (analyze_run path) ==="
    $status = Get-Content -LiteralPath (Join-Path $kitDir "complete_$friendToken.json") -Raw | ConvertFrom-Json
    if ($status.runToken -cne $friendToken -or -not $status.success -or -not $status.restored -or $status.zip -cne "KenshiCoop-results-$friendToken.zip") { throw 'Current friend run did not complete restoration successfully.' }
    $resultsZip = Get-Item -LiteralPath (Join-Path $kitDir $status.zip)
    Expand-Archive -LiteralPath $resultsZip.FullName -DestinationPath $stampDir -Force

    $joinLog = Join-Path $stampDir "join.log"
    if (-not (Test-Path $joinLog)) { throw "results zip contained no join.log" }


} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { $wanProc.Kill(); $wanProc.WaitForExit() } catch {}
    }
}
} finally {
    Restore-CoopHarnessSession -State $runState -CredentialRoot $(if ($credentials) { $credentials.Root } else { '' }) -Proxy $wanProc
}
    $verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
                  -Tolerance $manifest.Scenarios[$Scenario].Tolerance -JoinExpected $true `
                  -RunInfo @{ rehearsal = $true; wan = $Wan; kit = $kitDir; outDir = $stampDir; ownedExitFailures = @() } `
                  -WanActive $true -OutJson (Join-Path $stampDir "verdict.json")
Write-Host ("REHEARSAL RESULT: " + $(if ($verdict.pass) { "PASS - UDP kit rehearsal only; two-account Steam remains UNVERIFIED" } else { "FAIL" }))
Write-Host "  artifacts: $stampDir"
if ($verdict.pass) { exit 0 } else { exit 1 }
