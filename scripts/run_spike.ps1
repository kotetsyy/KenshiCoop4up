<#
.SYNOPSIS
  Run ONE investigative spike end to end: (optionally) build + deploy the plugin,
  launch host+join via run_test.ps1 with the generic "spike" scenario selected by
  KENSHICOOP_SPIKE=<id>, then collect the per-client logs + screenshots into
  resources/spikes/<id>/raw/<unique-run>/run for evidence collection.

.DESCRIPTION
  The "spike" scenario (src/plugin/test/Scenario.cpp -> SpikeScenario) dispatches
  on the KENSHICOOP_SPIKE env var, which run_test.ps1 does NOT touch, so we set it
  in this parent shell and it is inherited by the launched game processes.

  Diagnostic by nature: the deliverable is the EVIDENCE in the collected logs
  (lines beginning "SPIKE <id> ..."), not a PASS/FAIL gate. A run that reaches
  gameplay and self-exits cleanly is a successful capture.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spike.ps1 -Id 401 -Save c

.EXAMPLE
  # Reuse the currently deployed DLL (batch several spikes off one build):
  powershell -ExecutionPolicy Bypass -File scripts\run_spike.ps1 -Id 402 -Save c -SkipBuild
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('0','401','402','451')][string]$Id,
    [string]$Save = "c",
    [int]$Seconds = 45,
    [switch]$SkipBuild,
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$NativeBin = "",
    [string]$Setup = "",
    [int]$NetSimDelayMs = 0,
    [int]$NetSimJitterMs = 0,
    [int]$NetSimLossPct = 0
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$spikeDir  = Join-Path $repoRoot "resources\spikes\$Id"
$rawDir    = Join-Path $spikeDir ("raw\" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

Write-Host "================================================================"
Write-Host "== SPIKE $Id  (save=$Save seconds=$Seconds skipBuild=$SkipBuild)"
Write-Host "================================================================"

. (Join-Path $scriptDir 'kit_preflight.ps1')
$runState = New-CoopRunState -GameDirs @($HostDir,$JoinDir)
try {
# ---- 1. build + deploy (unless reusing a batch build) ------------------------
if (-not $SkipBuild) {
    if (@(Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue).Count) { throw 'Close your user-owned Kenshi session before running the harness.' }

    Write-Host "=== build ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "SPIKE $Id BUILD-FAIL (exit $LASTEXITCODE)"; exit 2 }

    Write-Host "=== deploy ==="
    Install-CoopHarnessRuntime -RepoRoot $repoRoot -GameDirs @($HostDir,$JoinDir) -NativeBin $NativeBin
} else {
    Write-Host "=== skip build (reusing deployed DLL) ==="
}

# ---- 2. run the spike --------------------------------------------------------
# KENSHICOOP_SPIKE rides through to the launched processes (run_test.ps1's
# Set-CoopEnv never overwrites it).
$env:KENSHICOOP_SPIKE = $Id

$outDir = Join-Path $rawDir 'run'
$runArgs = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $scriptDir "run_test.ps1"),
    "-Save", $Save, "-Scenario", "spike", "-Port", "$Port", "-Seconds", "$Seconds",
    "-HostDir", $HostDir, "-JoinDir", $JoinDir, "-OutDir", $outDir
)
if ($NativeBin) { $runArgs += @("-NativeBin", $NativeBin) }
if ($Setup -ne "")  { $runArgs += @("-Setup", $Setup) }
if ($NetSimDelayMs)  { $runArgs += @("-NetSimDelayMs", "$NetSimDelayMs") }
if ($NetSimJitterMs) { $runArgs += @("-NetSimJitterMs", "$NetSimJitterMs") }
if ($NetSimLossPct)  { $runArgs += @("-NetSimLossPct", "$NetSimLossPct") }

$out = & powershell @runArgs 2>&1
$childExit = $LASTEXITCODE
$out | ForEach-Object {
    Write-Host $_
}

# Judge only this invocation's explicit, fresh output path.
$hostLog = Join-Path $outDir 'host.log'
$joinLog = Join-Path $outDir 'join.log'
$receipt = Join-Path $outDir 'verdict.json'
$verdict = $null
if (Test-Path -LiteralPath $receipt) { $verdict = Get-Content -LiteralPath $receipt -Raw -Encoding UTF8 | ConvertFrom-Json }
Write-Host ""
Write-Host "== SPIKE $Id evidence (host) =="
if (Test-Path $hostLog) {
    Select-String -Path $hostLog -Pattern "SPIKE $Id " -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("  " + ($_.Line -replace '^.*SPIKE', 'SPIKE').Trim()) }
}
    Write-Host "== SPIKE $Id evidence (join) =="
    if (Test-Path $joinLog) {
        Select-String -Path $joinLog -Pattern "SPIKE $Id " -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("  " + ($_.Line -replace '^.*SPIKE', 'SPIKE').Trim()) }
    }

$ok = $childExit -eq 0 -and $null -ne $verdict -and $verdict.scenario -eq 'spike' -and `
      ($verdict.captureOk -eq $true) -and (Test-Path -LiteralPath $hostLog) -and (Test-Path -LiteralPath $joinLog)
Write-Host ""
Write-Host ("SPIKE $Id " + $(if ($ok) { "CAPTURE-OK" } else { "CAPTURE-INCOMPLETE" }))
if ($ok) { exit 0 } else { exit 1 }
} finally {
    Restore-CoopRunState $runState
}
