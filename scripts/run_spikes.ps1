<#
.SYNOPSIS
  Run a batch of spikes off a SINGLE build: build+deploy once, then run each id in
  the list with run_spike.ps1 -SkipBuild. Skip-on-error: a spike that fails to
  capture is logged and the loop continues to the next (the autonomous-run rule).

.DESCRIPTION
  Amortizes the (slow) build over many diagnostic probes that share one DLL. The
  per-spike SpikeScenario branch is selected at runtime by KENSHICOOP_SPIKE, so a
  single build can serve every probe currently compiled into the dispatcher.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spikes.ps1 -Ids 0,401 -Save c

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spikes.ps1 -Ids 402,451 -Save c -Seconds 60
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Ids,
    [string]$Save = "c",
    [int]$Seconds = 45,
    [switch]$SkipBuild,
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$NativeBin = ""
)

$ErrorActionPreference = "Stop"   # child failures are caught per spike, preflight is fail-closed
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Ids=@($Ids | ForEach-Object { $_.Split(',') } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if (-not $Ids.Count) { throw 'No spike IDs requested.' }
foreach ($id in $Ids) { if ($id -notin @('0','401','402','451')) { throw "Unsupported spike ID '$id'." } }
. (Join-Path $scriptDir 'kit_preflight.ps1')
$runState = New-CoopRunState -GameDirs @($HostDir,$JoinDir)
try {

# ---- build once --------------------------------------------------------------
if (-not $SkipBuild) {
    # The owning state rejects existing sessions; no process-name killing.
    Write-Host "=== batch build ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "BATCH BUILD-FAIL (exit $LASTEXITCODE)"; exit 2 }
    Install-CoopHarnessRuntime -RepoRoot (Split-Path -Parent $scriptDir) -GameDirs @($HostDir,$JoinDir) -NativeBin $NativeBin
}

# ---- run each id, skip on error ----------------------------------------------
$results = @()
foreach ($id in $Ids) {
    Write-Host ""
    Write-Host "################# SPIKE $id #################"
    $code = $null
    try {
        $childArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $scriptDir 'run_spike.ps1'),
                     '-Id',$id,'-Save',$Save,'-Seconds',"$Seconds",'-Port',"$Port",'-SkipBuild',
                     '-HostDir',$HostDir,'-JoinDir',$JoinDir)
        if ($NativeBin) { $childArgs+=@('-NativeBin',$NativeBin) }
        & powershell @childArgs
        $code = $LASTEXITCODE
    } catch {
        Write-Host "SPIKE $id THREW: $($_.Exception.Message)"
        $code = 99
    }
    $results += [pscustomobject]@{ id = $id; exit = $code }
}

Write-Host ""
Write-Host "================= BATCH SUMMARY ================="
foreach ($r in $results) {
    $v = switch ($r.exit) { 0 { "CAPTURE-OK" } 1 { "CAPTURE-INCOMPLETE" } default { "ERROR($($r.exit))" } }
    Write-Host ("  spike {0,-3} {1}" -f $r.id, $v)
}
if (@($results | Where-Object { $_.exit -ne 0 }).Count) { exit 1 }
exit 0
} finally {
    Restore-CoopRunState $runState
}
