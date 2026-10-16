# Reliability diagnostics only; this report never certifies gameplay.
[CmdletBinding()]
param(
    [ValidateRange(0, 2147483647)][int]$Days = 0, # 0 = all non-future history
    [string]$Path = (Join-Path $PSScriptRoot 'test-runs/history.jsonl')
)
$ErrorActionPreference = 'Stop'
$now = [DateTimeOffset]::UtcNow
$cut = if ($Days -gt 0) { $now.AddDays(-$Days) } else { [DateTimeOffset]::MinValue }
$runs = New-Object System.Collections.ArrayList
$issues = New-Object System.Collections.ArrayList
$lineNumber = 0
$legacy = 0
$jsonArguments = @{ ErrorAction='Stop' }
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $jsonArguments.DateKind='String' }
try {
    $inputPath=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $lines=[IO.File]::ReadAllLines($inputPath,(New-Object Text.UTF8Encoding($false,$true)))
}
catch { Write-Host "INCOMPLETE: cannot read history: $($_.Exception.Message)"; exit 2 }
foreach ($line in $lines) {
    $lineNumber++
    if (-not $line.Trim()) { continue }
    try {
        $r = $line | ConvertFrom-Json @jsonArguments
        if ($r -isnot [pscustomobject] -or [string]::IsNullOrWhiteSpace($r.scenario) -or
            [string]::IsNullOrWhiteSpace($r.variant) -or $r.scenario -isnot [string] -or
            $r.variant -isnot [string] -or $r.pass -isnot [bool] -or $r.flaky -isnot [bool] -or
            $r.gates -isnot [array] -or $r.gates.Count -eq 0) { throw 'invalid run schema or absent gate evidence' }
        $names = @{}
        foreach ($g in $r.gates) {
            if ($g -isnot [pscustomobject] -or $g.gate -isnot [string] -or
                [string]::IsNullOrWhiteSpace($g.gate) -or $g.status -cnotin @('PASS','FAIL','SKIP') -or
                $names.ContainsKey($g.gate) -or ($null -ne $g.metrics -and $g.metrics -isnot [pscustomobject])) { throw 'invalid/duplicate gate schema' }
            $names[$g.gate] = $true
        }
        if ($r.timestamp -isnot [string]) { throw 'missing timestamp' }
        $stamp = [DateTimeOffset]::MinValue
        if ($r.timestamp -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?(?:Z|[+-]\d{2}:\d{2})$') {
            if (-not [DateTimeOffset]::TryParse($r.timestamp, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$stamp)) { throw 'invalid timestamp' }
        } elseif ($r.timestamp -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$') {
            # Legacy regress producer used local seconds without an offset. There
            # is no origin timezone to recover: explicitly assume this machine's local zone.
            if (-not [DateTimeOffset]::TryParseExact($r.timestamp, 'yyyy-MM-ddTHH:mm:ss',
                [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeLocal, [ref]$stamp)) { throw 'invalid legacy timestamp' }
            $legacy++
        } else { throw 'timestamp must be invariant ISO date/time' }
        if ($stamp -gt $now) { [void]$issues.Add("line ${lineNumber}: future timestamp outside window"); continue }
        if ($stamp -lt $cut) { continue }
        $r | Add-Member -NotePropertyName '_utc' -NotePropertyValue $stamp.UtcDateTime
        $r | Add-Member -NotePropertyName '_line' -NotePropertyValue $lineNumber
        [void]$runs.Add($r)
    } catch { [void]$issues.Add("line ${lineNumber}: $($_.Exception.Message)") }
}
foreach ($issue in $issues) { Write-Host "INVALID: $issue" }
if ($legacy) { Write-Host "LEGACY: $legacy offset-less timestamp(s) assumed in this machine's local timezone." }
$runs = @($runs | Sort-Object _utc, _line)
Write-Host "runs in window: $($runs.Count)"
if (-not $runs.Count) { Write-Host 'INCOMPLETE: no usable history evidence'; exit 2 }
Write-Host ('window (UTC): {0:o} -> {1:o}' -f $runs[0]._utc, $runs[-1]._utc)
Write-Host ('run pass rate: {0:P1}' -f (@($runs | Where-Object { $_.pass -eq $true }).Count / $runs.Count))
$byGate = @{}
foreach ($r in $runs) {
    foreach ($g in $r.gates) {
        if (-not $byGate.ContainsKey($g.gate)) {
            $byGate[$g.gate] = [pscustomobject]@{ Gate=$g.gate; Seen=0; Fail=0; Skip=0; Scenarios=@{} }
        }
        $e = $byGate[$g.gate]; $e.Seen++
        if ($g.status -eq 'FAIL') { $e.Fail++; $e.Scenarios["$($r.scenario)/$($r.variant)"] = 1 }
        if ($g.status -eq 'SKIP') { $e.Skip++ }
    }
}
Write-Host "`n=== gates by FAIL rate ==="
$byGate.Values | Sort-Object @{Expression={ -($_.Fail / $_.Seen) }}, Gate | ForEach-Object {
    '{0,-24} SEEN={1} FAIL={2} FAIL%={3:P1} SKIP={4} scenarios={5}' -f $_.Gate,$_.Seen,$_.Fail,($_.Fail/$_.Seen),$_.Skip,(($_.Scenarios.Keys | Sort-Object) -join ',')
}
Write-Host "`n=== scenario/variant reliability (chronological verdict flips and retry flakes) ==="
$runs | Group-Object scenario, variant | ForEach-Object {
    $group = @($_.Group | Sort-Object _utc, _line)
    $flips = 0
    for ($i = 1; $i -lt $group.Count; $i++) { if ($group[$i].pass -ne $group[$i-1].pass) { $flips++ } }
    [pscustomobject]@{ Scenario=$group[0].scenario; Variant=$group[0].variant; Runs=$group.Count;
        Fails=@($group | Where-Object { $_.pass -eq $false }).Count; Flips=$flips;
        RetryFlakes=@($group | Where-Object { $_.flaky -eq $true }).Count;
        Tail=(-join @($group | Select-Object -Last 12 | ForEach-Object { if ($_.pass) { '.' } else { 'X' } })) }
} | Sort-Object @{Expression={-$_.Flips}}, @{Expression={-$_.RetryFlakes}}, Scenario, Variant | ForEach-Object {
    '{0}/{1} runs={2} fails={3} flips={4} retryFlakes={5} tail={6}' -f $_.Scenario,$_.Variant,$_.Runs,$_.Fails,$_.Flips,$_.RetryFlakes,$_.Tail
}
Write-Host 'DIAGNOSTIC ONLY: reliability counts are not gameplay certification.'
if ($issues.Count) { exit 2 }
exit 0
