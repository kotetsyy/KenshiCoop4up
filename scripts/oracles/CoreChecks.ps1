# oracles/CoreChecks.ps1 - run-health gates (monolith split of
# CoopOracles.psm1, 2026-07-12): Test-LogHealth, Test-NoCheckFail,
# Test-ScenarioResultPass, Test-ClockSync - the gates every scenario runs
# regardless of domain. Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or "SCENARIO RESULT"/"CHECK FAIL" phrasing -
# scenarios.psd1 and the C++ log contract key on them (resources/CODE_MAP.md).
# ---- Log health / scenario-result gates ---------------------------------------

# Evaluate one client's log health. PASS if it reached gameplay, exited cleanly
# (did NOT crash mid-run), and logged no ERROR lines. The clean-exit marker
# differs by mode: the self-exit timer logs "test duration elapsed", while a
# scenario logs "SCENARIO RESULT".
function Test-LogHealth {
    param([string]$File, [string]$Label, [bool]$Required, [string]$CleanPattern)
    $gate = "health_$Label"
    if ([string]::IsNullOrWhiteSpace($File) -or -not (Test-Path -LiteralPath $File)) {
        if ($Required) {
            Write-Host "  [$Label] FAIL - no log produced"
            return (Add-GateResult -Name $gate -Status FAIL -Detail "no log produced")
        }
        Write-Host "  [$Label] skipped (not launched)"
        return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched")
    }
    $text     = Get-Content $File -Raw -Encoding UTF8
    $errs     = @(Select-String -Path $File -Pattern "\] ERROR:" -ErrorAction SilentlyContinue)
    $reached  = $text -match "gameplay started"
    $cleanEnd = if ($CleanPattern -eq 'SCENARIO RESULT') {
        @(Select-String -Path $File -Pattern '^\[\d\d:\d\d:\d\d\.\d\d\d\](?: \[(?:HOST|JOIN)\])?(?: INFO:)? SCENARIO RESULT (?:PASS|FAIL)\s*$' -Encoding UTF8).Count -eq 1
    } else { $text -match $CleanPattern }
    $ok = ($reached -and $cleanEnd -and $errs.Count -eq 0)
    $why = @()
    if (-not $reached)  { $why += "never reached gameplay" }
    if (-not $cleanEnd) { $why += "no clean exit" }
    if ($errs.Count -gt 0) { $why += "$($errs.Count) error line(s)" }
    $verdict = if ($ok) { "PASS" } else { "FAIL - " + ($why -join "; ") }
    Write-Host "  [$Label] $verdict"
    foreach ($e in ($errs | Select-Object -First 5)) { Write-Host "      $($e.Line)" }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ errors = $errs.Count } -Detail ($why -join "; "))
}

# Engine-side bookkeeping integrity, read from the archived kenshi_info.log
# (run_test.ps1 copies it as <label>_engine.log; absent for older runs -> SKIP).
#
# Two lines Kenshi prints when its object bookkeeping has been damaged:
#   "<obj> was not removed from any zones"  - destroyed while a zone still holds
#                                             a pointer to it (a dangling entry)
#   "alredy has destroy reason <a> (<b>)"   - destroyed twice; the engine's typo
#
# A coop-tagged reason provides direct attribution. Absence of that tag does not
# establish an engine-only fault: a duplicate native addItem issued by the mod
# still reports just "addItem". Any lifetime error prevents a clean integrity
# result; attribution remains separate in the metrics and archived native logs.
# Scenarios choose whether this result is gating or advisory.
function Test-EngineIntegrity {
    param([string]$OutDir, [bool]$JoinExpected = $true)
    $gate  = "engine_integrity"
    $dblRe = "al(?:ready|redy) has destroy reason"
    $zoneRe = "was not removed from any zones"
    $ours = @(); $dbl = 0; $zone = 0; $seen = 0
    if ($OutDir -eq "" -or -not (Test-Path $OutDir)) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no run dir")
    }
    $labels = @('host'); if ($JoinExpected) { $labels += 'join' }
    foreach ($label in $labels) {
        $f = Join-Path $OutDir "${label}_engine.log"
        if (-not (Test-Path $f)) { continue }
        $seen++
        foreach ($m in @(Select-String -Path $f -Pattern $dblRe -ErrorAction SilentlyContinue)) {
            $dbl++
            if ($m.Line -match "coop-") { $ours += "[$label] $($m.Line.Trim())" }
        }
        $zone += @(Select-String -Path $f -Pattern $zoneRe -ErrorAction SilentlyContinue).Count
    }
    $m = @{ doubleDestroy = $dbl; coopAttributed = $ours.Count; zoneLeak = $zone }
    if ($ours.Count -gt 0) {
        Write-Host "  ENGINE-INTEGRITY FAIL - $($ours.Count) object(s) destroyed twice with a coop reason"
        foreach ($l in ($ours | Select-Object -First 5)) { Write-Host "      $l" }
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $m `
                    -Detail "coop-attributed double destroy")
    }
    if ($dbl -gt 0 -or $zone -gt 0) {
        Write-Host "  ENGINE-INTEGRITY FAIL - doubleDestroy=$dbl zoneLeak=$zone (cause not established)"
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $m `
                    -Detail "native lifetime errors; lack of a coop tag does not establish attribution")
    }
    if ($seen -lt $labels.Count) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "incomplete engine archives ($seen/$($labels.Count) required logs)")
    }
    Write-Host "  ENGINE-INTEGRITY PASS - complete archives contain no double-destroy or dangling-zone errors"
    return (Add-GateResult -Name $gate -Status PASS -Metrics $m)
}

# Report any in-plugin "CHECK <key> FAIL" lines across both logs.
function Test-NoCheckFail {
    param([string]$HostFile, [string]$JoinFile)
    $count = 0
    foreach ($entry in @(@{ f = $HostFile; l = "host" }, @{ f = $JoinFile; l = "join" })) {
        if ($entry.f -eq "" -or -not (Test-Path $entry.f)) { continue }
        $fails = @(Select-String -Path $entry.f -Pattern "CHECK \S+ FAIL" -ErrorAction SilentlyContinue)
        foreach ($f in $fails) { Write-Host "  [$($entry.l)] $($f.Line.Trim())" }
        $count += $fails.Count
    }
    return (Add-GateResult -Name "check_fail" -Status $(if ($count -eq 0) { "PASS" } else { "FAIL" }) `
                -Metrics @{ failLines = $count })
}

# The in-plugin scenario verdict ("SCENARIO RESULT PASS") for one client.
function Test-ScenarioResultPass {
    param([string]$File, [string]$Label, [bool]$Required, [string]$Scenario = '')
    $gate = "result_$Label"
    if ($File -eq "" -or -not (Test-Path $File)) {
        if (-not $Required) { return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched") }
        Write-Host "  $Label SCENARIO RESULT missing (no log)"
        return (Add-GateResult -Name $gate -Status FAIL -Detail "no log")
    }
    $records = @(Select-String -Path $File -Pattern '^\[\d\d:\d\d:\d\d\.\d\d\d\](?: \[(HOST|JOIN)\])?(?: INFO:)? SCENARIO RESULT (PASS|FAIL)\s*$' -Encoding UTF8)
    $ok = $records.Count -eq 1
    if ($ok) {
        $g = $records[0].Matches[0].Groups
        $ok = $g[2].Value -eq 'PASS' -and (-not $g[1].Success -or $g[1].Value -eq $Label.ToUpperInvariant())
    }
    # The producer binds the scenario in its separate start record.
    if ($Scenario -ne '') {
        $starts = @(Select-String -Path $File -Pattern '^\[\d\d:\d\d:\d\d\.\d\d\d\](?: \[(?:HOST|JOIN)\])?(?: INFO:)? SCENARIO (\S+) start\s*$' -Encoding UTF8)
        $ok = $ok -and $starts.Count -eq 1
        if ($starts.Count -eq 1 -and $records.Count -eq 1) {
            $ok = $ok -and $starts[0].Matches[0].Groups[1].Value -eq $Scenario -and $starts[0].LineNumber -lt $records[0].LineNumber
        }
    }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { 'PASS' } else { 'FAIL' }) `
        -Metrics @{ terminalRecords = $records.Count } -Detail $(if ($ok) { '' } else { 'missing, conflicting, wrong-role or unsuccessful terminal result' }))
}

# ---- Clock-sync gate ------------------------------------------------------------

# Validates the wire time-sync itself. SKIP when the join log carries no CLOCKSYNC
# lines (pre-time-sync DLL). When -ExpectedSkewMs is provided (a run with injected
# fake clock skew), the estimated offset must recover the injected skew: the join's
# wall clock was shifted +S, so its estimated host-relative offset must be ~ -S.
function Test-ClockSync {
    param([string]$HostFile, [string]$JoinFile, $ExpectedSkewMs = $null, [int]$MinSamples = 3)
    $s = Get-ClockSyncStats -File $JoinFile
    if ($s.samples -eq 0) {
        Write-Host "  CLOCK-SYNC SKIP - no CLOCKSYNC lines in the join log (plugin without time-sync?)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Detail "no CLOCKSYNC lines")
    }
    $m = @{ samples = $s.samples; offsetMs = $s.offsetMs; rttMs = $s.rttMs }
    if ($s.samples -lt $MinSamples) {
        Write-Host "  CLOCK-SYNC SKIP - only $($s.samples) CLOCKSYNC sample(s) (need >= $MinSamples)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Metrics $m -Detail "too few samples")
    }
    if ($null -ne $ExpectedSkewMs -and "$ExpectedSkewMs" -ne "") {
        $exp = [int]$ExpectedSkewMs
        $tol = [Math]::Max(100, 2 * $s.rttMs)
        $err = [Math]::Abs($s.offsetMs + $exp)
        $m.expectedSkewMs = $exp; $m.recoverErrMs = $err; $m.tolMs = $tol
        $ok = ($err -le $tol)
        $v = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host "  CLOCK-SYNC $v - injected skew ${exp}ms, estimated offset $($s.offsetMs)ms (|err|=${err}ms <= ${tol}ms, rtt=$($s.rttMs)ms, n=$($s.samples))"
        return (Add-GateResult -Name "clock_sync" -Status $v -Metrics $m)
    }
    Write-Host "  CLOCK-SYNC PASS - offset=$($s.offsetMs)ms rtt=$($s.rttMs)ms n=$($s.samples)"
    return (Add-GateResult -Name "clock_sync" -Status PASS -Metrics $m)
}

