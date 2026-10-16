<#
.SYNOPSIS
  Pair the two sides' SCENARIO WNPC roster dumps and diff them by HAND.

.DESCRIPTION
  The split_far phase table compares countNpcsNear totals, which are medians
  over ~30 samples and therefore blind to an intermittent divergence: a join
  that carries five extra bodies in 7 of 37 samples still reports the same
  median as the host. This walks the rosters instead, so a body present on one
  side and absent on the other is named, located and counted directly.

  Both sides emit a burst of WNPC rows every ~5 s. Rows are clustered into
  dumps on a timestamp gap, host dumps are paired to the nearest join dump,
  and each pair is reduced to two sets: hands only the join holds (the
  "join-only" class the field reports describe) and hands only the host holds.
#>
param(
    [Parameter(Mandatory = $true)][string]$RunDir,
    [int]$GapMs = 1000,      # timestamp gap that starts a new dump
    [int]$PairTolMs = 4000,  # host<->join dump pairing window
    [int]$TopN = 12          # how many offenders to name
)

Import-Module (Join-Path $PSScriptRoot 'CoopOracles.psm1') -Force
$hostLog = Join-Path $RunDir "host.log"
$joinLog = Join-Path $RunDir "join.log"
foreach ($f in @($hostLog, $joinLog)) {
    if (-not (Test-Path $f)) { throw "missing $f" }
}

function Get-Rows([string]$File) {
    $rows = New-Object System.Collections.ArrayList
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WNPC hand=([\d,]+) " +
           "pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) cls=(\w+) name='([^']*)'"
    foreach ($m in (Select-String -Path $File -Pattern $pat)) {
        $g = $m.Matches[0].Groups
        if ($g[9].Value -eq "pc") { continue }   # player squad is not world state
        $t = Get-LogTimeMs -Match $m
        [void]$rows.Add([pscustomobject]@{
            t = $t; hand = $g[5].Value
            x = [double]$g[6].Value; y = [double]$g[7].Value; z = [double]$g[8].Value
            cls = $g[9].Value; name = $g[10].Value })
    }
    return $rows
}

# Cluster a flat row list into per-dump buckets on a timestamp gap.
function Get-Dumps($rows) {
    $dumps = New-Object System.Collections.ArrayList
    $cur = $null; $last = -1
    foreach ($r in ($rows | Sort-Object t)) {
        if ($null -eq $cur -or ($r.t - $last) -gt $GapMs) {
            if ($null -ne $cur) { [void]$dumps.Add($cur) }
            $cur = [pscustomobject]@{ t = $r.t; rows = (New-Object System.Collections.ArrayList) }
        }
        [void]$cur.rows.Add($r); $last = $r.t
    }
    if ($null -ne $cur) { [void]$dumps.Add($cur) }
    return $dumps
}

$hRows = @(Get-Rows $hostLog)
$jRows = @(Get-Rows $joinLog)
$hDumps = @(Get-Dumps $hRows)
$jDumps = @(Get-Dumps $jRows)
Write-Host "host: $($hRows.Count) rows in $($hDumps.Count) dumps | join: $($jRows.Count) rows in $($jDumps.Count) dumps"

$paired = 0; $withJoinOnly = 0; $withHostOnly = 0
$joinOnlyTotal = 0; $hostOnlyTotal = 0
$offenders = @{}   # join-only: hand -> @{ n; name; x; z }
$hostOnly  = @{}   # host-only, same shape. Symmetry is the diagnostic: if BOTH
                   # sides carry local-only bodies near their own squad, each
                   # engine is instantiating its own population and the fault is
                   # not specific to the join.

foreach ($hd in $hDumps) {
    $jd = $jDumps | Sort-Object { [Math]::Abs($_.t - $hd.t) } | Select-Object -First 1
    if ($null -eq $jd -or [Math]::Abs($jd.t - $hd.t) -gt $PairTolMs) { continue }
    $paired++
    $hSet = @{}; foreach ($r in $hd.rows) { $hSet[$r.hand] = $r }
    $jSet = @{}; foreach ($r in $jd.rows) { $jSet[$r.hand] = $r }

    $jOnly = @($jSet.Keys | Where-Object { -not $hSet.ContainsKey($_) })
    $hOnly = @($hSet.Keys | Where-Object { -not $jSet.ContainsKey($_) })
    if ($jOnly.Count -gt 0) { $withJoinOnly++; $joinOnlyTotal += $jOnly.Count }
    if ($hOnly.Count -gt 0) { $withHostOnly++; $hostOnlyTotal += $hOnly.Count }

    foreach ($k in $jOnly) {
        $r = $jSet[$k]
        if (-not $offenders.ContainsKey($k)) {
            $offenders[$k] = @{ n = 0; name = $r.name; cls = $r.cls; x = $r.x; z = $r.z }
        }
        $offenders[$k].n++
        $offenders[$k].cls = $r.cls
    }
    foreach ($k in $hOnly) {
        $r = $hSet[$k]
        if (-not $hostOnly.ContainsKey($k)) {
            $hostOnly[$k] = @{ n = 0; name = $r.name; cls = $r.cls; x = $r.x; z = $r.z }
        }
        $hostOnly[$k].n++
    }
}

Write-Host ""
Write-Host "paired dumps: $paired"
if ($paired -eq 0) { Write-Error 'Insufficient evidence: no clock-aligned roster dump pair'; exit 2 }
Write-Host "  dumps where the JOIN holds a hand the host does not: $withJoinOnly ($joinOnlyTotal row(s) total)"
Write-Host "  dumps where the HOST holds a hand the join does not: $withHostOnly ($hostOnlyTotal row(s) total)"

if ($offenders.Count -gt 0) {
    Write-Host ""
    Write-Host "join-only bodies, by how many dumps they persisted:"
    $ranked = $offenders.GetEnumerator() | Sort-Object { -$_.Value.n }
    foreach ($e in ($ranked | Select-Object -First $TopN)) {
        Write-Host ("  {0,3} dump(s)  cls={1,-5} {2,-28} at {3,9:N0},{4,9:N0}  hand={5}" -f `
                    $e.Value.n, $e.Value.cls, "'$($e.Value.name)'", $e.Value.x, $e.Value.z, $e.Key)
    }
    if ($offenders.Count -gt $TopN) {
        Write-Host "  ... and $($offenders.Count - $TopN) more"
    }
    Write-Host ""
    Write-Host "distinct join-only bodies: $($offenders.Count)"
}

if ($hostOnly.Count -gt 0) {
    Write-Host ""
    Write-Host "host-only bodies, by how many dumps they persisted:"
    $rankedH = $hostOnly.GetEnumerator() | Sort-Object { -$_.Value.n }
    foreach ($e in ($rankedH | Select-Object -First $TopN)) {
        Write-Host ("  {0,3} dump(s)  {1,-28} at {2,9:N0},{3,9:N0}  hand={4}" -f `
                    $e.Value.n, "'$($e.Value.name)'", $e.Value.x, $e.Value.z, $e.Key)
    }
    if ($hostOnly.Count -gt $TopN) { Write-Host "  ... and $($hostOnly.Count - $TopN) more" }
    Write-Host ""
    Write-Host "distinct host-only bodies: $($hostOnly.Count)"
}
