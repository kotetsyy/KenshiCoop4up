<#
.SYNOPSIS
  Does world parity degrade at the moment the co-location collapse releases?

.DESCRIPTION
  The collapse hands NPC authorship to the host while the squads share a cell and
  hands it back when they part, so every flip of the [cell] MAP 'collapse=' field
  is an authority handover - and a handover is where a body can fall between two
  authors. This lines the flips up against a per-dump roster-miss count and asks
  whether the misses cluster around them.

  The miss count is deliberately cruder than the world_parity gate: bodies the
  host lists in a SCENARIO WNPC dump that the join's nearest dump does not, keyed
  on the full 5-component hand. That is enough to see a cliff, and unlike the
  gate's verdict it is resolved in TIME, which is the whole question here.

  Windowing: +-15 s around each flip, against the run's own baseline outside
  those windows, so a run that is uniformly bad reads as uniformly bad rather
  than as a handover problem.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\collapse_handover.ps1 -BatchDir tools\test-runs\ab_escape_cohesion_20260809_144835
#>
param(
    [Parameter(Mandatory = $true)][string]$BatchDir,
    [ValidateRange(0.001, 2147483)][double]$WindowSec = 15
)

$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot '..\scripts\CoopOracles.psm1') -Force

function Get-Dumps([string]$LogFile) {
    $dumps = New-Object System.Collections.ArrayList
    $bursts = @(Group-WnpcSamples -Rows @(Get-WnpcRows -File $LogFile) -GapMs 1500)
    foreach ($burst in $bursts) {
        $hands = @{}
        foreach ($row in $burst.rows) { $hands[$row.hand] = 1 }
        [void]$dumps.Add([pscustomobject]@{ t=$burst.t; hands=$hands })
    }
    foreach ($world in @(Get-WorldRows -File $LogFile)) {
        $paired = @($bursts | Where-Object { [math]::Abs($_.t - $world.t) -le 1500 })
        $npcRows = @($paired | ForEach-Object { $_.rows } | Where-Object { $_.cls -ne 'pc' })
        if ($world.n -gt 0 -and -not $npcRows.Count) { throw "Nonempty WORLD has no NPC roster in $LogFile" }
        if ($world.n -eq 0 -and $npcRows.Count) { throw "Empty WORLD conflicts with NPC roster in $LogFile" }
        # cls=pc bodies are outside WORLD n; keep that real roster if present.
        if ($world.n -eq 0 -and -not $paired.Count) {
            [void]$dumps.Add([pscustomobject]@{ t=$world.t; hands=@{} })
        }
    }
    return @($dumps | Sort-Object t)
}

function Get-Flips([string]$LogFile) {
    $flips = New-Object System.Collections.ArrayList
    $last = -1
    foreach ($record in (Select-String -LiteralPath $LogFile -Encoding UTF8 -Pattern '\[cell\] MAP .*collapse=([01])(?: |$)')) {
        $v = [int]$record.Matches[0].Groups[1].Value
        if ($last -ge 0 -and $v -ne $last) {
            [void]$flips.Add([pscustomobject]@{ t=(Get-LogTimeMs -Match $record); to=$v })
        }
        $last = $v
    }
    return $flips
}

$winMs = [int]($WindowSec * 1000)
$rows = New-Object System.Collections.ArrayList
$issues = New-Object System.Collections.ArrayList

foreach ($d in (Get-ChildItem -Directory -LiteralPath $BatchDir | Sort-Object Name)) {
    $hostLog = Join-Path $d.FullName "host.log"
    $joinLog = Join-Path $d.FullName "join.log"
    try {
        $run = Get-Content -LiteralPath (Join-Path $d.FullName 'run.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $verdict = Get-Content -LiteralPath (Join-Path $d.FullName 'verdict.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace($run.scenario) -or $verdict.scenario -cne $run.scenario) { throw 'missing/conflicting scenario metadata' }
        foreach ($name in @('health_host','health_join','check_fail','result_host','result_join','process_exit')) {
            $gates = @($verdict.gates | Where-Object { $_.gate -eq $name })
            if ($gates.Count -ne 1 -or $gates[0].status -ne 'PASS') { throw "incomplete $name evidence" }
        }
        Reset-GateResults
        foreach ($side in @('host','join')) {
            $file = Join-Path $d.FullName "$side.log"
            if ((Test-LogHealth -File $file -Label $side -Required $true -CleanPattern 'SCENARIO RESULT') -ne 'PASS' -or
                (Test-ScenarioResultPass -File $file -Label $side -Required $true -Scenario $run.scenario) -ne 'PASS') { throw "incomplete native $side evidence" }
        }
        $hd = @(Get-Dumps $hostLog)
        $jd = @(Get-Dumps $joinLog)
        if ($hd.Count -eq 0 -or $jd.Count -eq 0) { throw 'no roster samples' }
    } catch { [void]$issues.Add("$($d.Name): $($_.Exception.Message)"); continue }

    # The last flip of a run is the peer disconnecting, which empties the claim
    # set and legitimately un-collapses. That is teardown, not a handover, so it
    # is dropped: keeping it would score the shutdown as a parity cliff.
    $flips = @(Get-Flips $hostLog)
    $lastJoinT = ($jd | Select-Object -Last 1).t
    $flips = @($flips | Where-Object { $_.t -lt $lastJoinT })

    $inMiss = New-Object System.Collections.ArrayList
    $outMiss = New-Object System.Collections.ArrayList
    foreach ($h in $hd) {
        # Pair each host dump with the join dump nearest in time.
        $best = $null; $bestD = [long]::MaxValue
        foreach ($j in $jd) {
            $dt = [math]::Abs($j.t - $h.t)
            if ($dt -lt $bestD) { $bestD = $dt; $best = $j }
        }
        if ($null -eq $best -or $bestD -gt 2500) { continue }
        $miss = 0
        foreach ($k in $h.hands.Keys) { if (-not $best.hands.ContainsKey($k)) { $miss++ } }
        $nearFlip = $false
        foreach ($f in $flips) { if ([math]::Abs($h.t - $f.t) -le $winMs) { $nearFlip = $true } }
        if ($nearFlip) { [void]$inMiss.Add($miss) } else { [void]$outMiss.Add($miss) }
    }
    function Mean($a) {
        if ($a.Count -eq 0) { return $null }
        $s = 0.0; foreach ($v in $a) { $s += $v }
        return [math]::Round($s / $a.Count, 2)
    }
    if ($inMiss.Count -eq 0 -or $outMiss.Count -eq 0) {
        [void]$issues.Add("$($d.Name): no paired handover-window or outside-window evidence (near=$($inMiss.Count), away=$($outMiss.Count))")
    }
    [void]$rows.Add([pscustomobject]@{
        run        = $d.Name
        flips      = $flips.Count
        flipTimes  = (($flips | ForEach-Object { "{0:hh\:mm\:ss}->{1}" -f [timespan]::FromMilliseconds($_.t), $_.to }) -join " ")
        dumpsNear  = $inMiss.Count
        missNear   = (Mean $inMiss)
        dumpsAway  = $outMiss.Count
        missAway   = (Mean $outMiss)
    })
}

$rows | Format-Table -AutoSize run, flips, dumpsNear, missNear, dumpsAway, missAway, flipTimes |
    Out-String | Write-Host
Write-Host "  missNear / missAway = mean host-listed bodies absent from the join's"
Write-Host "  paired dump, inside vs outside +-${WindowSec}s of a collapse flip."
Write-Host "  A handover cost would show as missNear well above missAway."
foreach ($issue in $issues) { Write-Host "REJECTED/INCOMPLETE: $issue" }
Write-Host 'DIAGNOSTIC ONLY: paired roster differences are not gameplay certification.'
if ($rows.Count -eq 0 -or $issues.Count) { Write-Host 'INCOMPLETE: no complete handover comparison.'; exit 2 }
exit 0
