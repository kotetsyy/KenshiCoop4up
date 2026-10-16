<#
.SYNOPSIS
  DRESS REHEARSAL of the friend-HOSTS session, end to end, using the LAN
  machine as the "friend": assemble the host-role kit, install + run it on
  machine 2 exactly as the friend would (friend_host.ps1 from the unzipped
  kit), join from this machine through the WAN proxy via join_session.ps1,
  collect the friend-side results zip, and judge both logs offline.

.DESCRIPTION
  PASS means the real internet session only changes the network: kit install
  works, the host comes up from the kit alone, the join connects through a
  degraded link, the scenario passes, and the results zip contains everything
  analyze_run.ps1 needs.

  Uses scripts\lan.config.json (setup_lan_host.ps1 must have run once - the
  scheduled-task pattern and firewall rule are reused).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_host_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$Wan = "bad",
    [string]$NativeBin = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'kit_preflight.ps1')
. (Join-Path $scriptDir 'NativeBuild.ps1')

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest
if ($Scenario -eq 'bootstrap_stream') { throw 'bootstrap_stream is runner-only; use stream_test.ps1 for its menu/bootstrap lifecycle.' }
if ($Save -eq "" -and $manifest.Scenarios.ContainsKey($Scenario)) { $Save = $manifest.Scenarios[$Scenario].Save }
if ($Save -eq "") { throw "No save for scenario '$Scenario'" }
if (-not $manifest.Scenarios.ContainsKey($Scenario)) { throw "Unknown scenario '$Scenario'." }
$entry = $manifest.Scenarios[$Scenario]
if (-not $entry.PrimaryGate -and @($entry.Gating).Count -eq 0) { throw 'Kit rehearsal cannot certify a diagnostic-only scenario.' }
$prof = $manifest.Profiles.remote
$seconds = if ($entry.ContainsKey('Seconds')) { [int]$entry.Seconds } else { 150 }
$killGraceSec = if ($entry.ContainsKey('KillGraceSec')) { [int]$entry.KillGraceSec } else { [int]$prof.KillGraceSec }
$runToken = [Guid]::NewGuid().ToString('N')
$taskName = "KenshiCoopKitHost-$runToken"

$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (run setup_lan_host.ps1 first)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"

function Invoke-LanSsh {
    param([string]$Cmd)
    # Escape embedded quotes (PS 5.1 native-arg quirk) and tolerate stderr noise:
    # under ErrorActionPreference=Stop a mere WARNING on ssh's stderr becomes a
    # terminating NativeCommandError when merged with 2>&1.
    $escaped = $Cmd -replace '"', '\"'
    $PSNativeCommandArgumentPassing = 'Legacy'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

$runState = New-CoopRunState -GameDirs @("$env:USERPROFILE\Kenshi-Join",'C:\Program Files (x86)\Steam\steamapps\common\Kenshi') -SaveNames @($Save,'coopresume')
$wanProc = $null
try {
Clear-CoopRunEnvironment -State $runState
$kitTestDir = "C:\KenshiCoopKitTest-" + [Guid]::NewGuid().ToString('N')
$stampDir = Join-Path $repoRoot ("tools\test-runs\kitrehearsal_" + (Get-Date -Format "yyyyMMdd_HHmmss") + '-' + $runToken)
New-Item -ItemType Directory -Force -Path $stampDir | Out-Null

# ---- 1. Assemble the host-role kit -----------------------------------------------
Write-Host "=== 1. assemble host-role kit ==="
$kitArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", (Join-Path $scriptDir "make_remote_kit.ps1"),
             "-Role", "host", "-Save", $Save, "-Scenario", $Scenario, "-Port", "$Port")
if ($NativeBin) { $kitArgs += @('-NativeBin',$NativeBin) }
if ($SkipBuild) { $kitArgs += "-SkipBuild" }
$kitOutput = & powershell @kitArgs 6>&1
$kitOutput | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "kit assembly failed" }
$zipLine = $kitOutput | Where-Object { "$_" -match '^Kit zipped:\s+(.+\.zip)$' } | Select-Object -Last 1
if (-not $zipLine -or "$zipLine" -notmatch '^Kit zipped:\s+(.+\.zip)$') { throw 'Exact kit output path was not returned.' }
$kitZip = Get-Item -LiteralPath $Matches[1]

# Local deploy so the join runs the same build as the kit.
Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @("$env:USERPROFILE\Kenshi-Join") -NativeBin $NativeBin
$joinDir = "$env:USERPROFILE\Kenshi-Join"
$joinFixture = Join-Path $joinDir "save\$Save"
if (Test-Path -LiteralPath $joinFixture) { Remove-Item -LiteralPath $joinFixture -Recurse -Force }
New-Item -ItemType Directory -Path (Split-Path -Parent $joinFixture) -Force | Out-Null
# Use the exact packaged fixture even if AppData changes after assembly.
$kitFixtureRoot = Join-Path $stampDir 'packaged-fixture'
Expand-Archive -LiteralPath $kitZip.FullName -DestinationPath $kitFixtureRoot
Copy-Item -LiteralPath (Join-Path $kitFixtureRoot "save\$Save") -Destination $joinFixture -Recurse
Remove-Item -LiteralPath $kitFixtureRoot -Recurse -Force
Set-CoopSaveLocation -GameDir $joinDir -UserLocation 0

# ---- 2. Install the kit on machine 2 exactly as the friend would ---------------------
Write-Host ""
Write-Host "=== 2. install kit on $($cfg.host) (as the friend would) ==="
$r = Invoke-LanSsh 'powershell -NoProfile -Command "if(Get-Process kenshi_x64 -ErrorAction SilentlyContinue){exit 1}; Write-Output SAFE"'
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'SAFE') { throw 'Remote user-owned game exists.' }
[void](Invoke-LanSsh "mkdir `"$kitTestDir`" & echo OK")
$kitFwd = $kitTestDir -replace '\\','/'
& scp -o BatchMode=yes $kitZip.FullName "${target}:$kitFwd/kit.zip"
if ($LASTEXITCODE -ne 0) { throw "kit push failed" }
$r = Invoke-LanSsh "powershell -NoProfile -Command `"Expand-Archive -Path $kitTestDir\kit.zip -DestinationPath $kitTestDir -Force; 'EXPANDED'`""
if ("$($r.out)" -notmatch "EXPANDED") { throw "kit expand failed: $($r.out)" }
Write-Host "  kit expanded -> $kitTestDir"

# ---- 3. Run friend_host.ps1 there via a one-off interactive task ------------------------
Write-Host ""
Write-Host "=== 3. run friend_host.ps1 on $($cfg.host) ==="
$action = "& '$kitTestDir\friend_host.ps1' -NoPrompt -KenshiDir '$($cfg.kenshiDir.Replace("'","''"))' -RunToken '$runToken'"
$actionEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($action))
$taskCmd = "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $actionEncoded"
$r = Invoke-LanSsh "schtasks /create /tn $taskName /tr `"$taskCmd`" /sc once /st 00:00 /it /f && schtasks /run /tn $taskName"
if ($r.exit -ne 0) { throw "kit host task failed: $($r.out)" }
$remoteDeadline = [DateTime]::UtcNow.AddSeconds($prof.StartTimeoutSec + $prof.ArmTimeoutMs/1000.0 + $seconds + $killGraceSec + 60)
Write-Host "  friend_host task triggered; waiting for the host to reach gameplay ..."
$deadline = (Get-Date).AddSeconds(240)
$hostUp = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $r = Invoke-LanSsh "type `"$kitTestDir\results_$runToken\host.log`" 2>nul"
    if ("$($r.out)" -match "gameplay started") { $hostUp = $true; break }
}
if (-not $hostUp) { Write-Warning "kit host not confirmed in gameplay; continuing best-effort" }
else { Write-Host "  kit host in gameplay." }

# ---- 4. Join from here through the WAN proxy ----------------------------------------------
Write-Host ""
Write-Host "=== 4. join through WAN proxy ($Wan) via join_session.ps1 ==="
$wp = $manifest.WanProfiles[$Wan]
if (-not $wp) { throw "Unknown WAN profile '$Wan'." }
$netsimExe = Join-Path $repoRoot "dist\netsim.exe"
if (-not (Test-Path $netsimExe)) { & cmd.exe /c "`"$scriptDir\build_netsim.cmd`""; if ($LASTEXITCODE -ne 0) { throw "netsim build failed" } }
$proxyPort = $Port + 1
$wanSeed = Get-Random -Minimum 1 -Maximum ([int]::MaxValue)
$wanArgs = @("$proxyPort", $cfg.host, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
if ($wp.ContainsKey('StallAtS')) { $wanArgs += @("$wanSeed","$($wp.StallAtS)","$($wp.StallForS)") }
@{profile=$Wan;seed=$wanSeed;arguments=$wanArgs} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stampDir 'wan.json') -Encoding UTF8
$wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $stampDir "netsim.log") `
    -ArgumentList $wanArgs
Start-Sleep -Milliseconds 500
if ($wanProc.HasExited) { throw "netsim exited immediately" }

$joinOutDir = ""
try {
    $joinArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $scriptDir 'join_session.ps1'),'-HostIp','127.0.0.1','-Port',"$proxyPort",'-Scenario',$Scenario,'-Save',$Save,'-SkipBuild')
    if ($NativeBin) { $joinArgs += @('-NativeBin',$NativeBin) }
    $out = & powershell @joinArgs 2>&1
    $joinExit = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    foreach ($line in $out) { if ("$line" -match "join log:\s+(.+join\.log)") { $joinOutDir = Split-Path -Parent $Matches[1].Trim() } }
} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { $wanProc.Kill(); $wanProc.WaitForExit() } catch {}
    }
}
if ($joinOutDir -eq "") { throw "join_session did not report a join log" }
if ($joinExit -ne 0) { throw "join_session lifecycle failed (exit=$joinExit)." }
Copy-Item -Path (Join-Path $joinOutDir "join*.*") -Destination $stampDir -Force

# ---- 5. Collect the friend-side results zip -------------------------------------------------
Write-Host ""
Write-Host "=== 5. collect the friend results zip ==="
$deadline = $remoteDeadline
$remoteStatusFile = Join-Path $stampDir "complete_$runToken.json"
$complete = $false
while ([DateTime]::UtcNow -lt $deadline) {
    $r = Invoke-LanSsh "if exist `"$kitTestDir\complete_$runToken.json`" (echo COMPLETE) else (echo WAIT)"
    if ($r.exit -eq 0 -and "$($r.out)" -match 'COMPLETE') { $complete = $true; break }
    Start-Sleep -Seconds 5
}
if (-not $complete) { throw 'Exact remote owner has not completed restoration; retain remote task and backups.' }
& scp -o BatchMode=yes "${target}:$kitFwd/complete_$runToken.json" $remoteStatusFile
if ($LASTEXITCODE -ne 0) { throw 'Remote completion collection failed.' }
$status = Get-Content -LiteralPath $remoteStatusFile -Raw | ConvertFrom-Json
$zipName = "KenshiCoop-results-$runToken.zip"
if ($status.runToken -cne $runToken -or -not $status.success -or -not $status.restored -or $status.zip -cne $zipName) { throw "Remote owner failed: $($status.error)" }
& scp -o BatchMode=yes "${target}:$kitFwd/$zipName" "$stampDir/"
if ($LASTEXITCODE -ne 0) { throw "results zip collection failed" }
Expand-Archive -LiteralPath (Join-Path $stampDir $zipName) -DestinationPath $stampDir -Force
Write-Host "  collected + expanded: $zipName"

# Cleanup remote kit test artifacts.
$cleanup = "`$ErrorActionPreference='Stop'; & schtasks /delete /tn '$taskName' /f; if(`$LASTEXITCODE -ne 0){throw 'Task cleanup failed.'}; Remove-Item -LiteralPath '$($kitTestDir.Replace("'","''"))' -Recurse -Force; Write-Output CLEANED"
$cleanupEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cleanup))
$r = Invoke-LanSsh "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $cleanupEncoded"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'CLEANED') { throw "Remote run-owned kit/task cleanup failed: $($r.out)" }

} finally {
    Restore-CoopHarnessSession -State $runState -Proxy $wanProc
}
# ---- 6. Judge --------------------------------------------------------------------------------
Write-Host ""
Write-Host "=== 6. judge collected logs ==="
$hostLog = Join-Path $stampDir "host.log"
$joinLog = Join-Path $stampDir "join.log"
if (-not (Test-Path $hostLog)) { throw "results zip contained no host.log" }
$verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
              -Tolerance $manifest.Scenarios[$Scenario].Tolerance -JoinExpected $true `
              -RunInfo @{ rehearsal = "host-kit"; wan = $Wan; lanHost = $cfg.host; outDir = $stampDir; ownedExitFailures = @() } `
              -WanActive $true -OutJson (Join-Path $stampDir "verdict.json")

Write-Host ("HOST-KIT REHEARSAL RESULT: " + $(if ($verdict.pass) { "PASS - UDP kit rehearsal only; two-account Steam remains UNVERIFIED" } else { "FAIL" }))
Write-Host "  artifacts: $stampDir"
if ($verdict.pass) { exit 0 } else { exit 1 }
