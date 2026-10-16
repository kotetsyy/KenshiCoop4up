<#
.SYNOPSIS
  Replay the dual_drive oracle over archived runs of the cell-authority
  scenarios, so its baseline comes from history instead of an hour of fresh
  game time.

.DESCRIPTION
  dual_drive is the mutual-drive detector added for the 2026-08-08 cell
  authority bug. Before it is allowed to gate anything it needs a baseline: how
  often does it fire on runs nobody considered broken? Every scenario that
  opts into KENSHICOOP_CELL_AUTH already has archived host/join log pairs under
  tools\test-runs, and the oracle is a pure log reader, so the baseline is a
  replay rather than a re-run.

  Prints one row per run and a per-scenario tally.
#>
param(
    [string]$RunsDir = (Join-Path $PSScriptRoot "test-runs"),
    [string[]]$Scenarios = @('split_far2', 'run_apart', 'town_arrive',
                             'town_arrive_far', 'cell_auth_together')
)

$ErrorActionPreference = 'Stop'

function Get-EffectiveCellAuth([string]$File, [string]$Scenario) {
    $records = @(Select-String -LiteralPath $File -Encoding UTF8 -Pattern "^\[\d\d:\d\d:\d\d\.\d\d\d\](?: \[(?:HOST|JOIN)\])?(?: INFO:)? KenshiCoop: effective cfg scenario='([^']*)' ")
    if (-not $records.Count) { throw "missing effective config: $File" }
    foreach ($record in $records) {
        if ($record.Matches[0].Groups[1].Value -cne $Scenario -or
            $record.Line -notmatch '(?:^| )cellAuth=1(?: |$)') { throw "incompatible cellAuth/scenario effective config: $File" }
    }
}
Import-Module (Join-Path $PSScriptRoot "..\scripts\CoopOracles.psm1") -Force

$rows = @()
$rejected = New-Object System.Collections.ArrayList
foreach ($d in (Get-ChildItem -LiteralPath $RunsDir -Directory | Sort-Object Name)) {
    try {
        $rj = Join-Path $d.FullName "run.json"
        $j = Get-Content -LiteralPath $rj -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace($j.scenario)) { throw 'missing scenario metadata' }
        if ($Scenarios -notcontains $j.scenario) { continue }
        $h = Join-Path $d.FullName "host.log"
        $jl = Join-Path $d.FullName "join.log"
        $verdict = Get-Content -LiteralPath (Join-Path $d.FullName 'verdict.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($verdict.scenario -cne $j.scenario -or $verdict.pass -isnot [bool]) { throw 'invalid/conflicting verdict identity' }
        foreach ($name in @('health_host','health_join','check_fail','result_host','result_join','process_exit')) {
            $gates = @($verdict.gates | Where-Object { $_.gate -eq $name })
            if ($gates.Count -ne 1 -or $gates[0].status -ne 'PASS') { throw "incomplete $name evidence" }
        }
        Reset-GateResults
        foreach ($side in @('host','join')) {
            $file = Join-Path $d.FullName "$side.log"
            Get-EffectiveCellAuth $file $j.scenario
            if ((Test-LogHealth -File $file -Label $side -Required $true -CleanPattern 'SCENARIO RESULT') -ne 'PASS' -or
                (Test-ScenarioResultPass -File $file -Label $side -Required $true -Scenario $j.scenario) -ne 'PASS') { throw "incomplete native $side result/health" }
        }
        $status = Test-DualDrive -HostFile $h -JoinFile $jl
    } catch { [void]$rejected.Add("$($d.Name): $($_.Exception.Message)"); continue }
    $m = (Get-GateResults | Where-Object { $_.gate -eq 'dual_drive' } | Select-Object -Last 1).metrics
    $rows += [pscustomobject]@{
        run      = $d.Name
        scenario = $j.scenario
        status   = $status
        hostH    = $m.hostHands
        joinH    = $m.joinHands
        shared   = $m.sharedHands
        concur   = $m.concurrentHands
        worstS   = if ($null -ne $m.worstOverlapMs) { [int]($m.worstOverlapMs / 1000) } else { 0 }
        drvBoth  = $m.drvBothHands
        drvSust  = $m.drvSustainedHands
        drvWorstS = if ($null -ne $m.drvWorstMs) { [int]($m.drvWorstMs / 1000) } else { 0 }
    }
}

Write-Host ""
Write-Host "== dual_drive replay over archived cell-authority runs =="
$rows | Format-Table -AutoSize

Write-Host "== per-scenario tally =="
$rows | Group-Object scenario | ForEach-Object {
    $f = @($_.Group | Where-Object { $_.status -eq 'FAIL' }).Count
    $p = @($_.Group | Where-Object { $_.status -eq 'PASS' }).Count
    $s = @($_.Group | Where-Object { $_.status -eq 'SKIP' }).Count
    "{0,-18} runs={1,-3} FAIL={2,-3} PASS={3,-3} SKIP={4}" -f $_.Name, $_.Count, $f, $p, $s
}
foreach ($reason in $rejected) { Write-Host "REJECTED: $reason" }
Write-Host 'DIAGNOSTIC ONLY: baseline oracle tallies are not gameplay certification.'
if (-not $rows.Count -or $rejected.Count -or -not @($rows | Where-Object { $_.status -in @('PASS','FAIL') }).Count) {
    Write-Host 'INCOMPLETE: no fully usable baseline, rejected candidates, or only unjudged samples.'
    exit 2
}
exit 0
