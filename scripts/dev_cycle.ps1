<#
.SYNOPSIS
  One-shot autonomous iteration cycle for KenshiCoop:
  reject existing user games -> build the plugin -> deploy to explicit installs ->
  run the host+join functional test -> exit with PASS/FAIL.

.DESCRIPTION
  Intended to be the single command a Cursor session calls each iteration. It
  guarantees a clean slate (no leftover instance holding the DLL lock), rebuilds
  and redeploys the plugin, then runs scripts/run_test.ps1. The exit code is the
  test verdict (0 = PASS, non-zero = FAIL/build error), so an agent can branch on
  it without parsing output. Per-client logs + screenshots land under
  tools/test-runs/<stamp>/ (path is printed by run_test.ps1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\dev_cycle.ps1 -Save "c" -Seconds 60 -Sync

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\dev_cycle.ps1 -Save "c" -Scenario squad_spawn_sync -Sync
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Save,
    [int]$Seconds = 60,
    [switch]$Sync,
    [switch]$SkipBuild,
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$NativeBin = "",
    # Optional compiled scenario to run (KENSHICOOP_SCENARIO); passthrough to run_test.ps1.
    [string]$Scenario = "",
    [double]$Tolerance = 3.0,
    # Passthrough: host-only setup/re-arm scene (see run_test.ps1 -Setup). Use "craft"
    # to re-arm a baked crafting worker each session during validation.
    [string]$Setup = "",
    # Passthrough: join-only AI-suspend probe (see run_test.ps1 -ProbeAiSuspend).
    [switch]$ProbeAiSuspend
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'kit_preflight.ps1')
$runState = New-CoopRunState -GameDirs @($HostDir,$JoinDir)
try {

function Invoke-Step {
    param([string]$Name, [scriptblock]$Action)
    Write-Host ""
    Write-Host "=== $Name ==="
    & $Action
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL ($Name failed, exit $LASTEXITCODE)"; exit $LASTEXITCODE }
}

# The owning preflight rejects existing user sessions rather than killing them.

# Confirm the deployed DLL is actually unlocked before we bother building.
$deployedDll = Join-Path $HostDir "mods\KenshiCoop\KenshiCoop.dll"
if (Test-Path $deployedDll) {
    $unlocked = $false
    for ($i = 0; $i -lt 10; $i++) {
        try { $fs = [System.IO.File]::Open($deployedDll, 'Open', 'ReadWrite', 'None'); $fs.Close(); $unlocked = $true; break }
        catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $unlocked) {
        Write-Host "RESULT: FAIL (mod DLL still locked: $deployedDll)"
        Write-Host "  A process still holds the file. End all Kenshi instances and re-run."
        exit 1
    }
}

# 2/3. Build + deploy (skippable when only the test changed).
if (-not $SkipBuild) {
    Invoke-Step "build plugin" { & cmd /c "`"$scriptDir\build_plugin.cmd`"" }
    Install-CoopHarnessRuntime -RepoRoot (Split-Path -Parent $scriptDir) -GameDirs @($HostDir,$JoinDir) -NativeBin $NativeBin
}

# 4. Run the functional test; its exit code is the verdict.
Write-Host ""
Write-Host "=== run test ==="
$testArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $scriptDir "run_test.ps1"),
              "-Save", $Save, "-Seconds", "$Seconds", "-Port", "$Port", "-HostDir", $HostDir, "-JoinDir", $JoinDir)
if ($NativeBin) { $testArgs += @("-NativeBin", $NativeBin) }
if ($Sync) { $testArgs += "-Sync" }
if ($Scenario -ne "") { $testArgs += @("-Scenario", $Scenario, "-Tolerance", "$Tolerance") }
if ($Setup -ne "") { $testArgs += @("-Setup", $Setup) }
if ($ProbeAiSuspend) { $testArgs += "-ProbeAiSuspend" }
& powershell @testArgs
exit $LASTEXITCODE
} finally {
    Restore-CoopRunState $runState
}
