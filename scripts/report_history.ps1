<#
.SYNOPSIS
  Report recorded reliability and measured trends without certifying missing evidence.
#>
[CmdletBinding()]
param(
    [ValidateRange(4,2147483647)][int]$Last = 20,
    [ValidateRange(0,1000000)][double]$DriftPct = 30.0,
    [string]$HistoryFile = ''
)
$ErrorActionPreference = 'Stop'
if ($HistoryFile -eq '') { $HistoryFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\test-runs\history.jsonl' }
if (-not (Test-Path -LiteralPath $HistoryFile)) { Write-Host "History missing: $HistoryFile"; exit 2 }
# Population, unattributed engine noise, walking and chase are context, not
# monotonic health metrics. Explicit directional names are actual producer keys.
$higherIsWorse = @('worstMedian','latencyMs','zeroFrac','floatFrac','dxz','recoverErrMs',
    'idleFrac','perMin','churnRatePerMin','wrongTgt','errors','failLines','coopAttributed',
    'divergentHands','medianGap','maxGap','bloodMedian','bloodFinal','ratePerMin',
    'ghostFrac','stallFrac','relapseFrac','maxPersistPerHand','medianDrift',
    'AKoLatMs','BKoLatMs','ARevLatMs','BRevLatMs')
$lowerIsWorse = @('ratio','downRatio','postRatio','upRatio','trackRatio',
    'ADownRatio','BDownRatio','AUpRatio','BUpRatio')
$gateDirections = @{ 'combat_order.preRatio'=$true; 'craft_order.preRatio'=$false; 'down_order.preRatio'=$false }
foreach ($direction in @('h2j','j2h')) {
    $gateDirections["coop_presence.${direction}_ratio"] = $false
    $gateDirections["coop_presence.${direction}_worstMedian"] = $true
}
foreach ($gate in @('assault_travel','assault_mint','mint_aggro')) {
    $gateDirections["$gate.joinOnlyFrac"] = $true
    $gateDirections["$gate.hostOnlyFrac"] = $true
}
$fractions = @('ratio','downRatio','postRatio','upRatio','trackRatio','preRatio','zeroFrac','floatFrac',
    'idleFrac','ghostFrac','stallFrac','relapseFrac','joinOnlyFrac','hostOnlyFrac',
    'ADownRatio','BDownRatio','AUpRatio','BUpRatio','h2j_ratio','j2h_ratio')
$jsonParameters = @{ ErrorAction='Stop' }
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $jsonParameters.DateKind = 'String' }
$entries = @(); $issues = 0; $line = 0
foreach ($ln in (Get-Content -LiteralPath $HistoryFile -Encoding UTF8)) {
    $line++
    if ([string]::IsNullOrWhiteSpace($ln)) { continue }
    try {
        $entry = $ln | ConvertFrom-Json @jsonParameters
        if ($entry -isnot [pscustomobject] -or $entry.scenario -isnot [string] -or
            [string]::IsNullOrWhiteSpace($entry.scenario) -or $entry.variant -isnot [string] -or
            $entry.pass -isnot [bool] -or $entry.flaky -isnot [bool] -or $entry.gates -isnot [array]) {
            throw 'expected scenario, variant, boolean pass/flaky and gates array'
        }
        foreach ($gate in $entry.gates) {
            if ($gate -isnot [pscustomobject] -or $gate.gate -isnot [string] -or
                [string]::IsNullOrWhiteSpace($gate.gate) -or @('PASS','FAIL','SKIP') -cnotcontains $gate.status -or
                $gate.metrics -isnot [pscustomobject]) { throw 'invalid gate name/status/metrics object' }
        }
        if ($entry.timestamp -isnot [string]) { throw 'missing invariant timestamp' }
        $stamp = [DateTimeOffset]::MinValue
        if ($entry.timestamp -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?(?:Z|[+-]\d{2}:\d{2})$') {
            if (-not [DateTimeOffset]::TryParse($entry.timestamp, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$stamp)) { throw 'invalid timestamp' }
        } elseif ($entry.timestamp -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?$') {
            $local = [DateTime]::MinValue
            if (-not [DateTime]::TryParse($entry.timestamp, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$local)) { throw 'invalid legacy timestamp' }
            # Legacy regress wrote local machine time without an offset. This
            # explicit compatibility policy never assumes it was UTC.
            $stamp = [DateTimeOffset]::new([DateTime]::SpecifyKind($local, [DateTimeKind]::Local))
            Write-Host "LEGACY timestamp line ${line}: interpreted in this machine's local timezone"
        } else { throw 'timestamp must be ISO local legacy time or explicitly offset ISO time' }
        $entries += [pscustomobject]@{ Record=$entry; When=$stamp.UtcDateTime; Line=$line
            Group=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($entry.scenario)) + '/' +
                [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($entry.variant)) }
    } catch { Write-Host "INVALID history line ${line}: $($_.Exception.Message)"; $issues++ }
}
if ($entries.Count -eq 0) { Write-Host 'No valid history records to judge.'; exit 2 }
$groups = $entries | Group-Object Group
Write-Host "================= HISTORY REPORT ($($entries.Count) valid run(s)) ================="
$drifts = 0; $judged = 0
foreach ($grp in ($groups | Sort-Object Name)) {
    $ordered = @($grp.Group | Sort-Object -Property @('When','Line') | Select-Object -Last $Last)
    $runs = @($ordered | ForEach-Object { $_.Record })
    $passN = @($runs | Where-Object { $_.pass }).Count
    $flakyN = @($runs | Where-Object { $_.flaky }).Count
    Write-Host ("  {0}|{1} runs={2} pass={3}/{2} flaky={4} UTC={5:o}..{6:o}" -f
        $runs[-1].scenario,$runs[-1].variant,$runs.Count,$passN,$flakyN,$ordered[0].When,$ordered[-1].When)
    if (-not $runs[-1].pass) { Write-Host 'RELIABILITY latest run FAIL'; $issues++ }
    else { Write-Host 'RELIABILITY latest run PASS (historical failures shown above)' }
    $series = @{}; $context = @{}; $groupJudged = 0
    for ($n = 0; $n -lt $runs.Count; $n++) {
        foreach ($g in $runs[$n].gates) {
            foreach ($p in $g.metrics.PSObject.Properties) {
                $key = "$($g.gate).$($p.Name)"
                if (-not $gateDirections.ContainsKey($key) -and
                    $higherIsWorse -notcontains $p.Name -and $lowerIsWorse -notcontains $p.Name) {
                    $context[$key] = $true; continue
                }
                if (-not $series.ContainsKey($key)) { $series[$key] = New-Object 'object[]' $runs.Count }
                if ($g.status -ne 'PASS') { Write-Host "UNAVAILABLE $key from $($g.status) gate in run $n"; continue }
                $val = 0.0; $valid = $false
                if ($p.Value -is [ValueType] -and $p.Value -isnot [bool]) {
                    try { $val = [double]$p.Value; $valid = $true } catch {}
                } elseif ($p.Value -is [string]) {
                    $valid = [double]::TryParse($p.Value, [Globalization.NumberStyles]::Float,
                        [Globalization.CultureInfo]::InvariantCulture, [ref]$val)
                }
                if ($valid -and -not [double]::IsNaN($val) -and -not [double]::IsInfinity($val) -and
                    $val -ge 0 -and ($fractions -notcontains $p.Name -or $val -le 1)) {
                    if ($null -ne $series[$key][$n]) { Write-Host "INVALID duplicate metric $key in run $n"; $issues++ }
                    $series[$key][$n] = $val
                } else { Write-Host "INVALID or unavailable measurement $key in run $n"; $issues++ }
            }
        }
    }
    foreach ($key in ($context.Keys | Sort-Object)) { Write-Host "UNJUDGED context $key" }
    foreach ($key in ($series.Keys | Sort-Object)) {
        $vals = $series[$key]
        if ($null -eq $vals[-1]) { Write-Host "INCOMPLETE latest metric $key"; $issues++; continue }
        $missing = @($vals | Where-Object { $null -eq $_ }).Count
        if ($missing) { Write-Host "INCOMPLETE history metric $key ($missing missing run(s))"; $issues++; continue }
        if ($vals.Count -lt 4) { Write-Host "INSUFFICIENT history for $key ($($vals.Count) run(s))"; $issues++; continue }
        $groupJudged++; $judged++
        $newest = [double]$vals[-1]
        $prior = @($vals[0..($vals.Count-2)] | Sort-Object)
        $mid = [int][Math]::Floor($prior.Count / 2)
        $median = [double]$prior[$mid]
        if ($prior.Count % 2 -eq 0) { $median = ([double]$prior[$mid-1]+[double]$prior[$mid])/2.0 }
        $name = $key.Split('.')[-1]
        $delta = $newest-$median
        $higher = if ($gateDirections.ContainsKey($key)) { $gateDirections[$key] } else { $higherIsWorse -contains $name }
        $worse = if ($higher) { $delta -gt 0 } else { $delta -lt 0 }
        if ($median -eq 0) {
            if ($worse) { $drifts++; Write-Host "DRIFT $key newest=$newest vs zero baseline (percentage undefined)" }
        } else {
            $pct = 100.0*$delta/[Math]::Abs($median)
            if ($worse -and [Math]::Abs($pct) -gt $DriftPct) {
                $drifts++; Write-Host "DRIFT $key newest=$newest vs rolling median=$median ($pct%)"
            }
        }
    }
    if ($groupJudged -eq 0) { Write-Host 'INCOMPLETE no usable judged trend in this scenario/variant'; $issues++ }
}
if ($issues) { Write-Host "INCOMPLETE HISTORY: $issues issue(s); no clean trend verdict"; exit 2 }
if ($drifts) { Write-Host "DRIFT WARNINGS: $drifts"; exit 2 }
Write-Host "No drift beyond $DriftPct% across $judged measured trend(s); latest reliability reported separately."
exit 0
