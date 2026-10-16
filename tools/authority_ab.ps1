<#
.SYNOPSIS
  Run one scenario under several authority arms and print one comparison table.

.DESCRIPTION
  An arm is a name plus supported, verifiable native boolean overrides. Keys
  without an effective-config readback are rejected rather than assumed applied.
  The default pair is the original question - KENSHICOOP_CELL_AUTH=0 restores
  unconditional host authority (the v0.46 model), so it is a ready-made control
  against today's bidirectional cell authority.

  Two things this measures that the gates alone do not:

  * HOW MUCH DRIVING each side does - the mean number of cls=drv rows per
    SCENARIO WNPC dump, per client. Cell authority makes both clients drive; host
    authority makes only the join drive. That difference is the whole cost
    question, and it is invisible in a PASS/FAIL.
  * WHETHER THE ARM ACTUALLY APPLIED - each arm's flags are read back out of the
    plugin's startup config dump on BOTH clients, and the run is marked bad if
    they disagree. An A/B that silently measures the same build twice is worse
    than no A/B.

  Runs are also marked valid/invalid per scenario (see Test-RunValid): a scenario
  that failed to set its own scene up did not exercise the thing being compared,
  and averaging it in just adds noise.

  Read-only with respect to the mod: no rebuild, no manifest edit. The arm is
  passed per run via run_test.ps1 -DiagEnvOverride.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\authority_ab.ps1 -Scenario escape_cohesion -Runs 4 -HostDir C:\PublicHost -JoinDir C:\PublicJoin -NativeBin C:\PublicNative

.EXAMPLE
  # The far-apart control: does host-only still lose when the squads are split?
  powershell -ExecutionPolicy Bypass -File tools\authority_ab.ps1 -Scenario split_far2 -Runs 3 -HostDir C:\PublicHost -JoinDir C:\PublicJoin -NativeBin C:\PublicNative

.EXAMPLE
  # Three arms: today's split, the co-location collapse, and the host-only
  # reference the collapse is supposed to reproduce while the squads are together.
  powershell -ExecutionPolicy Bypass -File tools\authority_ab.ps1 -Scenario escape_cohesion -Runs 4 -HostDir C:\PublicHost -JoinDir C:\PublicJoin -NativeBin C:\PublicNative -Arms "cellauth:KENSHICOOP_CELL_AUTH=1,KENSHICOOP_CELL_COLLAPSE=0;collapse:KENSHICOOP_CELL_AUTH=1,KENSHICOOP_CELL_COLLAPSE=1;hostonly:KENSHICOOP_CELL_AUTH=0"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9_]+$')][string]$Scenario,
    [ValidateRange(1, 2147483647)][int]$Runs = 4,
    # Arms to compare, each "name:KEY=VAL[,KEY2=VAL2]". The name becomes the run
    # directory prefix and the table's row label; the rest is handed to
    # run_test.ps1 -DiagEnvOverride verbatim.
    #
    # Several arms may also be packed into ONE element separated by ';', because
    # -File hands every argument over as a string and a shell that does not
    # itself build arrays turns "a","b" into the single token 'a,b' - which then
    # parses as one arm with a corrupt key. ';' cannot be confused with the ','
    # between keys, so the packed form survives any shell.
    #
    # The default pair reproduces the original cell-authority-on vs -off
    # comparison, and keeps the on_N / off_N directory names that earlier
    # batches already use so they can still be re-scored.
    [string[]]$Arms = @("on:KENSHICOOP_CELL_AUTH=1", "off:KENSHICOOP_CELL_AUTH=0"),
    # Reuse an existing batch directory: runs whose out dir already exists are
    # scored from their logs instead of re-played, so a batch can be topped up
    # (raise -Runs) or re-scored after a scoring change without more game time.
    [string]$ReuseDir = "",
    # Score whatever run dirs the batch already holds and launch nothing. Unlike
    # -ReuseDir with a run count, this does not care how many runs each arm has,
    # which is what an arm topped up to a valid-run target looks like.
    [switch]$ScoreOnly,
    # Launch mode requires explicit public installs and the intended native artifact.
    [string]$HostDir = "",
    [string]$JoinDir = "",
    [string]$NativeBin = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$runner    = Join-Path $repoRoot "scripts\run_test.ps1"

Import-Module (Join-Path $repoRoot 'scripts\CoopOracles.psm1') -Force
$PSNativeCommandUseErrorActionPreference = $false
# Only booleans with real native effective-config readback are supported.
$script:FlagFields = @{
    'KENSHICOOP_CELL_AUTH' = 'cellAuth'
    'KENSHICOOP_CELL_COLLAPSE' = 'cellCollapse'
    'KENSHICOOP_CENSUS_FREEZE_AI' = 'censusFreezeAi'
    'KENSHICOOP_CAM_INTEREST' = 'camInterest'
}

function Parse-Arm([string]$Spec) {
    $i = $Spec.IndexOf(':')
    if ($i -lt 1) { throw "Arm '$Spec' is not name:KEY=VALUE." }
    $name = $Spec.Substring(0, $i).Trim()
    if ($name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_-]*$' -or
        $name -match '^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') { throw "Unsafe arm name '$name'." }
    $envSpec = $Spec.Substring($i + 1).Trim()
    if ($envSpec -eq '') { throw "Arm '$name' has no overrides." }
    $expect = @{}; $overrides = @{}
    foreach ($pair in ($envSpec -split ',')) {
        $kv = $pair.Trim() -split '=', 2
        if ($kv.Count -ne 2) { throw "Arm '$name': '$pair' is not KEY=VALUE." }
        $key = $kv[0].Trim(); $value = $kv[1].Trim()
        if (-not $script:FlagFields.ContainsKey($key)) { throw "Arm '$name': unsupported effective-config readback for '$key'." }
        if ($value -eq '' -or $overrides.ContainsKey($key)) { throw "Arm '$name': empty or duplicate override '$key'." }
        $overrides[$key] = $value
        # Native Config.cpp tests value != "0", not an integer conversion.
        $expect[$script:FlagFields[$key]] = [int]($value -cne '0')
    }
    return [pscustomobject]@{ name=$name; diagEnv=$envSpec; expect=$expect; overrides=$overrides }
}

# ---- log parsing --------------------------------------------------------------

function Get-DrvPerDump([string]$LogFile) {
    if (-not (Test-Path -LiteralPath $LogFile)) { return [pscustomobject]@{mean=-1;dumps=0;peak=0} }
    $bursts = @(Group-WnpcSamples -Rows @(Get-WnpcRows -File $LogFile) -GapMs 1500)
    $samples = New-Object System.Collections.ArrayList
    foreach ($burst in $bursts) { [void]$samples.Add(@($burst.rows | Where-Object { $_.cls -eq 'drv' }).Count) }
    foreach ($world in @(Get-WorldRows -File $LogFile)) {
        $paired = @($bursts | Where-Object { [math]::Abs($_.t - $world.t) -le 1500 })
        $npcRows = @($paired | ForEach-Object { $_.rows } | Where-Object { $_.cls -ne 'pc' })
        if ($world.n -gt 0 -and -not $npcRows.Count) { throw "Nonempty WORLD has no NPC roster in $LogFile" }
        if ($world.n -eq 0 -and $npcRows.Count) { throw "Empty WORLD conflicts with NPC roster in $LogFile" }
        # WORLD n counts census NPCs, not the separately emitted cls=pc rows.
        # A PC-only burst already represents this zero-driving sample.
        if ($world.n -eq 0 -and -not $paired.Count) { [void]$samples.Add(0) }
    }
    if (-not $samples.Count) { return [pscustomobject]@{mean=-1;dumps=0;peak=0} }
    $stats = $samples | Measure-Object -Average -Maximum
    return [pscustomobject]@{mean=[math]::Round($stats.Average,1);dumps=$samples.Count;peak=$stats.Maximum}
}

function Get-LoggedFlag([string]$LogFile, [string]$Field) {
    if (-not (Test-Path -LiteralPath $LogFile)) { return -1 }
    $values = @()
    foreach ($line in (Get-Content -LiteralPath $LogFile -Encoding UTF8)) {
        if ($line -notmatch "^\[\d\d:\d\d:\d\d\.\d\d\d\](?: \[(?:HOST|JOIN)\])?(?: INFO:)? KenshiCoop: effective cfg scenario='([^']*)' ") { continue }
        if ($Matches[1] -cne $Scenario) { return -1 }
        $exact = [regex]::Match($line, '(?:^| )' + [regex]::Escape($Field) + '=([01])(?: |$)')
        if ($exact.Success) { $values += [int]$exact.Groups[1].Value; continue }
        $on = [regex]::Match($line, '(?:^| )on=\[([^\]]*)\](?: |$)')
        $off = [regex]::Match($line, '(?:^| )off=\[([^\]]*)\](?: |$)')
        $inOn = $on.Success -and ($on.Groups[1].Value -split ',') -ccontains $Field
        $inOff = $off.Success -and ($off.Groups[1].Value -split ',') -ccontains $Field
        if ($inOn -eq $inOff) { return -1 }
        $values += [int]$inOn
    }
    if (-not $values.Count -or @($values | Select-Object -Unique).Count -ne 1) { return -1 }
    return $values[0]
}

function Test-ArmApplied([string]$HostLog, [string]$JoinLog, $Expect) {
    # Every expected flag must read back as expected on BOTH clients. Returns the
    # mismatches, so a failure names the flag instead of just failing.
    $bad = New-Object System.Collections.ArrayList
    foreach ($f in $Expect.Keys) {
        $h = Get-LoggedFlag $HostLog $f
        $j = Get-LoggedFlag $JoinLog $f
        if ($h -ne $Expect[$f] -or $j -ne $Expect[$f]) {
            [void]$bad.Add("$f want=$($Expect[$f]) host=$h join=$j")
        }
    }
    return $bad
}

function Get-EscapeCells([string]$LogFile) {
    <#
      Distinct zone cells the escaping subject stood in, off the scenario's own
      per-tick ESCAPE line. Read from the world's coord mapping rather than from
      the claim map, which is what makes this usable as a validity test in BOTH
      arms: [cell] MAP only exists when cell authority is on, so judging an
      arm-off run by it discards the run for being arm-off.
    #>
    if (-not (Test-Path $LogFile)) { return 0 }
    $seen = @{}
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -match 'SCENARIO ESCAPE .*?(?:^| )cell=(-?\d+,-?\d+)(?: |$)') { $seen[$Matches[1]] = 1 }
    }
    return $seen.Count
}

function Get-SplitFarPops([string]$LogFile) {
    <#
      split_far2's own population counts, per side, once that side's tab has
      arrived at its town. Needed because the split_far2 GATE skips outright when
      cell authority is off ("no [cell] MAP dumps"), so it cannot score the
      control arm at all - but the populations underneath it can, and they are
      the thing cell authority was built for: whether each client still sees the
      NPCs around its OWN tab when the peer is a zone away.

      Returns medians of popHost and popJoin as logged by this client.
    #>
    $res = [pscustomobject]@{ popHost = $null; popJoin = $null; rows = 0 }
    if (-not (Test-Path $LogFile)) { return $res }
    $h = New-Object System.Collections.ArrayList
    $j = New-Object System.Collections.ArrayList
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -notlike '*SCENARIO SPLITFAR2*') { continue }
        if ($line -notmatch 'arrived=1') { continue }
        if ($line -match 'popHost=(\d+) popJoin=(\d+)') {
            [void]$h.Add([int]$Matches[1])
            [void]$j.Add([int]$Matches[2])
        }
    }
    if ($h.Count -eq 0) { return $res }
    function Med($Vals) {
        $s = @($Vals | Sort-Object)
        $n = $s.Count
        if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
        return [math]::Round(($s[$n / 2 - 1] + $s[$n / 2]) / 2.0, 1)
    }
    $res.popHost = Med $h
    $res.popJoin = Med $j
    $res.rows = $h.Count
    return $res
}

function Get-CellMapWidth([string]$LogFile) {
    # Widest 'cells=N' the resolved claim map ever reached. 2+ means the two
    # squads held DIFFERENT cells at some point, i.e. the boundary this whole
    # question is about actually existed during the run.
    if (-not (Test-Path $LogFile)) { return 0 }
    $w = 0
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -match '\[cell\] MAP cells=(\d+)') {
            $n = [int]$Matches[1]
            if ($n -gt $w) { $w = $n }
        }
    }
    return $w
}

function Get-CollapseFlips([string]$LogFile) {
    <#
      How many times the co-location verdict changed, and the timestamps it
      changed at. Each flip is an authority handover - the moment the squads
      separate far enough for the cell split to resume, or come back together -
      which is the transition worth checking the parity gates around.
    #>
    $flips = New-Object System.Collections.ArrayList
    $last = -1
    if (-not (Test-Path $LogFile)) { return [pscustomobject]@{ n = 0; at = $flips } }
    foreach ($record in (Select-String -LiteralPath $LogFile -Encoding UTF8 -Pattern '\[cell\] MAP .*collapse=([01])(?: |$)')) {
        $v = [int]$record.Matches[0].Groups[1].Value
        if ($last -ge 0 -and $v -ne $last) {
            [void]$flips.Add([pscustomobject]@{ t=(Get-LogTimeMs -Match $record); to=$v })
        }
        $last = $v
    }
    return [pscustomobject]@{ n = $flips.Count; at = $flips }
}

function Get-LineCount([string]$LogFile, [string]$Pattern) {
    if (-not (Test-Path $LogFile)) { return 0 }
    $n = 0
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -match $Pattern) { $n++ }
    }
    return $n
}

function Get-Counter([string]$LogFile, [string]$Name) {
    if (-not (Test-Path $LogFile)) { return 0 }
    $last = 0
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -match "$Name=(\d+)") { $last = [int]$Matches[1] }
    }
    return $last
}

# ---- verdict parsing ----------------------------------------------------------

function Get-GateMap([string]$VerdictFile) {
    $map = @{}
    if (-not (Test-Path $VerdictFile)) { return $map }
    try { $j = Get-Content -LiteralPath $VerdictFile -Encoding UTF8 -Raw | ConvertFrom-Json } catch { return $map }
    foreach ($g in $j.gates) {
        if ($null -eq $g -or [string]::IsNullOrWhiteSpace($g.gate)) { return @{} }
        $map[$g.gate] = $g
    }
    return $map
}

function Get-Metric($GateMap, [string]$Gate, [string]$Metric) {
    if (-not $GateMap.ContainsKey($Gate)) { return $null }
    $m = $GateMap[$Gate].metrics
    if ($null -eq $m) { return $null }
    if ($m.PSObject.Properties.Name -contains $Metric) { return $m.$Metric }
    return $null
}

function Get-Status($GateMap, [string]$Gate) {
    if ($null -eq $Gate -or $Gate -eq "") { return "-" }
    if (-not $GateMap.ContainsKey($Gate)) { return "-" }
    return $GateMap[$Gate].status
}

function Get-PrimaryGateName([string]$VerdictFile) {
    # The primary gate is named in the verdict, not after the scenario:
    # escape_cohesion's is pc_dupes, split_far2's is split_far2.
    if (-not (Test-Path $VerdictFile)) { return "" }
    try { $j = Get-Content -LiteralPath $VerdictFile -Encoding UTF8 -Raw | ConvertFrom-Json } catch { return "" }
    if ($j.PSObject.Properties.Name -contains 'primary') { return "$($j.primary)" }
    return ""
}

function Test-ArchiveEvidence($ArmDef, [string]$OutDir, $GateMap) {
    try {
        $run = Get-Content -LiteralPath (Join-Path $OutDir 'run.json') -Encoding UTF8 -Raw | ConvertFrom-Json
        $verdict = Get-Content -LiteralPath (Join-Path $OutDir 'verdict.json') -Encoding UTF8 -Raw | ConvertFrom-Json
        if ($run.scenario -cne $Scenario -or $verdict.scenario -cne $Scenario) { return 'SCENARIO-MISMATCH' }
        if ($run.diagEnvOverride -isnot [pscustomobject] -or
            @($run.diagEnvOverride.PSObject.Properties).Count -ne $ArmDef.overrides.Count) { return 'OVERRIDE-MISMATCH' }
        foreach ($key in $ArmDef.overrides.Keys) {
            if ($run.diagEnvOverride.PSObject.Properties.Name -cnotcontains $key -or
                [string]$run.diagEnvOverride.$key -cne $ArmDef.overrides[$key]) { return 'OVERRIDE-MISMATCH' }
        }
        if ($verdict.pass -isnot [bool] -or $verdict.gates -isnot [array] -or
            [string]::IsNullOrWhiteSpace($verdict.primary) -or -not $GateMap.ContainsKey($verdict.primary) -or
            $verdict.gating -isnot [array] -or -not $verdict.gating.Count) { return 'INCOMPLETE-VERDICT' }
        $seen = @{}
        foreach ($g in $verdict.gates) {
            if ([string]::IsNullOrWhiteSpace($g.gate) -or $g.status -cnotin @('PASS','FAIL','SKIP') -or $seen.ContainsKey($g.gate)) { return 'INVALID-VERDICT' }
            $seen[$g.gate] = $true
        }
        foreach ($name in @('health_host','health_join','check_fail','result_host','result_join','process_exit')) {
            if ((Get-Status $GateMap $name) -ne 'PASS') { return "INCOMPLETE-$name" }
        }
        foreach ($name in $verdict.gating) { if (-not $seen.ContainsKey($name)) { return 'INCOMPLETE-GATING' } }
        foreach ($side in @('host','join')) {
            $file = Join-Path $OutDir "$side.log"
            if ((Test-LogHealth -File $file -Label $side -Required $true -CleanPattern 'SCENARIO RESULT') -ne 'PASS' -or
                (Test-ScenarioResultPass -File $file -Label $side -Required $true -Scenario $Scenario) -ne 'PASS') { return "INCOMPLETE-NATIVE-$side" }
        }
        if ((Test-NoCheckFail -HostFile (Join-Path $OutDir 'host.log') -JoinFile (Join-Path $OutDir 'join.log')) -ne 'PASS') { return 'INCOMPLETE-NATIVE-CHECK' }
        return 'ok'
    } catch { return "INVALID-ARCHIVE: $($_.Exception.Message)" }
}

function Test-RunValid([string]$ScenarioName, $GateMap, [int]$CellsVisited) {
    <#
      Did the run actually set up the situation under comparison? A scenario that
      never got there is not evidence either way. Every test here has to hold in
      both arms - see Get-EscapeCells.
    #>
    switch ($ScenarioName) {
        "escape_cohesion" {
            # lockpick_escape covers both halves of the setup: the prisoner got
            # out of the cage AND then walked (its 20 u floor rejects the runs
            # where the release took but the body never moved). The distinct-cell
            # count then confirms the walk actually crossed a boundary, which is
            # the geometry the comparison is about.
            $esc = Get-Status $GateMap "lockpick_escape"
            if ($esc -ne "PASS") { return "no-escape" }
            if ($CellsVisited -lt 2) { return "one-cell" }
            return "ok"
        }
        default { return "ok" }
    }
}

# ---- one run ------------------------------------------------------------------

function Invoke-Arm($ArmDef, [int]$Index, [string]$BatchDir) {
    $outDir = Join-Path $BatchDir ("{0}_{1}" -f $ArmDef.name, $Index)
    if (-not (Test-Path $outDir)) {
        Write-Host ""
        Write-Host ("=== arm {0} ({1}) run {2}/{3} ===" -f $ArmDef.name, $ArmDef.diagEnv,
                                                           $Index, $Runs)
        # String form of -DiagEnvOverride: -File stringifies every argument, so a
        # hashtable literal cannot be passed this way.
        & powershell -NoProfile -ExecutionPolicy Bypass -File $runner `
            -Scenario $Scenario -OutDir $outDir -HostDir $HostDir -JoinDir $JoinDir -NativeBin $NativeBin `
            -DiagEnvOverride $ArmDef.diagEnv 2>&1 |
            Select-String -Pattern '^RESULT:|^  (dual_drive|pc_dupes|world_parity|lockpick_escape|split_far2)' |
            ForEach-Object { Write-Host ("    " + $_.Line.Trim()) }
        $childExit = $LASTEXITCODE
        # Exit 1 can be a completed negative oracle result, not a harness failure.
        if ($childExit -notin @(0,1)) { throw "A/B runner failed with exit $childExit for $outDir" }
    }
    return Measure-Run $ArmDef $Index $outDir
}

function Measure-Run($ArmDef, [int]$Index, [string]$OutDir) {
    Reset-GateResults
    $ArmName = $ArmDef.name
    $hostLog = Join-Path $OutDir "host.log"
    $joinLog = Join-Path $OutDir "join.log"
    $verdict = Join-Path $OutDir "verdict.json"
    $gates   = Get-GateMap $verdict
    # mapCells is informational only (it reads 0 for the whole arm-off arm by
    # construction); validity comes from the arm-neutral cell walk below.
    $mapW    = [math]::Max((Get-CellMapWidth $hostLog), (Get-CellMapWidth $joinLog))
    $cells   = [math]::Max((Get-EscapeCells $hostLog), (Get-EscapeCells $joinLog))

    $armBad = @(Test-ArmApplied $hostLog $joinLog $ArmDef.expect)
    $armOk  = ($armBad.Count -eq 0)

    $drvHost = Get-DrvPerDump $hostLog
    $drvJoin = Get-DrvPerDump $joinLog
    $popH    = Get-SplitFarPops $hostLog
    $popJ    = Get-SplitFarPops $joinLog

    $evidence = Test-ArchiveEvidence $ArmDef $OutDir $gates
    $valid = if ($evidence -ne 'ok') { $evidence } elseif (-not $armOk) { "ARM-MISMATCH" } elseif ($drvHost.dumps -eq 0 -or $drvJoin.dumps -eq 0) { 'NO-ROSTER-EVIDENCE' } elseif ($Scenario -eq 'split_far2' -and ($popH.rows -eq 0 -or $popJ.rows -eq 0)) { 'NO-POPULATION-EVIDENCE' } else { Test-RunValid $Scenario $gates $cells }

    return [pscustomobject]@{
        arm         = $ArmName
        run         = $Index
        dir         = (Split-Path -Leaf $OutDir)
        valid       = $valid
        armEnv      = $ArmDef.diagEnv
        armBad      = ($armBad -join "; ")
        cellAuth    = Get-LoggedFlag $hostLog "cellAuth"
        collapse    = Get-LoggedFlag $hostLog "cellCollapse"
        flipsHost   = (Get-CollapseFlips $hostLog).n
        flipsJoin   = (Get-CollapseFlips $joinLog).n
        mapCells    = $mapW
        cellsWalked = $cells
        drvHost     = $drvHost.mean
        drvJoin     = $drvJoin.mean
        drvHostPeak = $drvHost.peak
        drvJoinPeak = $drvJoin.peak
        dumps       = [math]::Min($drvHost.dumps, $drvJoin.dumps)
        dualDrive   = Get-Status $gates "dual_drive"
        drvBoth     = Get-Metric $gates "dual_drive" "drvSustainedHands"
        pcDupes     = Get-Status $gates "pc_dupes"
        parity      = Get-Status $gates "world_parity"
        nearExist   = Get-Metric $gates "world_parity" "nearExist"
        nearPosOk   = Get-Metric $gates "world_parity" "nearPosOk"
        taskParity  = Get-Metric $gates "world_parity" "taskParity"
        cenExist    = Get-Metric $gates "world_parity" "cenExist"
        cenPosOk    = Get-Metric $gates "world_parity" "cenPosOk"
        pcWorstMed  = Get-Metric $gates "world_parity" "pcWorstMed"
        zeroFrac    = Get-Metric $gates "smoothness" "zeroFrac"
        stallFrac   = Get-Metric $gates "smoothness" "stallFrac"
        ghostFrac   = Get-Metric $gates "existence_parity" "ghostFrac"
        zombieFrac  = Get-Metric $gates "anti_zombie" "zombieFrac"
        primary     = Get-Status $gates (Get-PrimaryGateName $verdict)
        # Census batches RECEIVED per client. The pair says which way the world
        # census flows: nonzero on both means each side is publishing a world
        # census to the other, which is the doubled bookkeeping that only cell
        # authority creates. Zero on the host means one-directional.
        censHost    = Get-LineCount $hostLog '\[census\] recv'
        censJoin    = Get-LineCount $joinLog '\[census\] recv'
        # split_far2 only. Each client's own town is the number that matters to it:
        # jOwnPop is the join's count of the join's town, hOwnPop the host's of the
        # host's. The cross columns are the peer's view of the same town.
        jOwnPop     = $popJ.popJoin
        hSeesJoin   = $popH.popJoin
        hOwnPop     = $popH.popHost
        jSeesHost   = $popJ.popHost
        yields      = Get-Counter $joinLog "cellYields"
        hostRefus   = Get-Counter $hostLog "hostRefus"
    }
}

# ---- aggregate ----------------------------------------------------------------

function Show-Arm([string]$ArmName, $Rows) {
    $valid = @($Rows | Where-Object { $_.valid -eq "ok" })
    Write-Host ""
    Write-Host ("--- arm '{0}': {1} run(s), {2} valid ---" -f $ArmName, $Rows.Count, $valid.Count)
    if ($Scenario -eq "split_far2") {
        # The split_far2 gate SKIPs with authority off, so the populations carry
        # this scenario's comparison instead. primary is still shown to make the
        # skip visible rather than silent.
        $Rows | Format-Table -AutoSize dir, valid, primary, jOwnPop, hSeesJoin, hOwnPop,
                                       jSeesHost, ghostFrac, zeroFrac |
            Out-String | Write-Host
    } else {
        $Rows | Format-Table -AutoSize dir, valid, cellsWalked, mapCells, flipsHost, flipsJoin,
                                       drvHost, drvJoin, dualDrive, drvBoth, pcDupes, parity,
                                       zeroFrac |
            Out-String | Write-Host
    }
    if ($valid.Count -eq 0) {
        Write-Host "  (no valid runs - nothing to average)"
        return $null
    }
    function Avg([string]$Field) {
        $vals = @($valid | ForEach-Object { $_.$Field } | Where-Object { $null -ne $_ -and $_ -ge 0 })
        if ($vals.Count -eq 0) { return $null }
        $s = 0.0; foreach ($v in $vals) { $s += [double]$v }
        return [math]::Round($s / $vals.Count, 3)
    }
    function PassRate([string]$Field) {
        $known = @($valid | Where-Object { $_.$Field -in @("PASS", "FAIL") })
        if ($known.Count -eq 0) { return "-" }
        $p = @($known | Where-Object { $_.$Field -eq "PASS" }).Count
        return "$p/$($known.Count)"
    }
    return [pscustomobject]@{
        arm         = $ArmName
        validRuns   = $valid.Count
        drvHost     = Avg "drvHost"
        drvJoin     = Avg "drvJoin"
        drvTotal    = $(if ((Avg "drvHost") -ne $null -and (Avg "drvJoin") -ne $null) { [math]::Round((Avg "drvHost") + (Avg "drvJoin"), 1) } else { $null })
        censHost    = Avg "censHost"
        censJoin    = Avg "censJoin"
        dualDrive   = PassRate "dualDrive"
        pcDupes     = PassRate "pcDupes"
        parity      = PassRate "parity"
        primary     = PassRate "primary"
        nearExist   = Avg "nearExist"
        nearPosOk   = Avg "nearPosOk"
        taskParity  = Avg "taskParity"
        cenExist    = Avg "cenExist"
        cenPosOk    = Avg "cenPosOk"
        pcWorstMed  = Avg "pcWorstMed"
        flips       = Avg "flipsJoin"
        zeroFrac    = Avg "zeroFrac"
        stallFrac   = Avg "stallFrac"
        ghostFrac   = Avg "ghostFrac"
        zombieFrac  = Avg "zombieFrac"
        jOwnPop     = Avg "jOwnPop"
        hSeesJoin   = Avg "hSeesJoin"
        hOwnPop     = Avg "hOwnPop"
        jSeesHost   = Avg "jSeesHost"
    }
}

# ---- main ---------------------------------------------------------------------

# Validate before creating artifacts or querying any game/launch boundary.
$armDefs = @($Arms | ForEach-Object { $_ -split ';' } | ForEach-Object { Parse-Arm $_ })
if (-not $armDefs.Count) { throw 'At least one verifiable arm is required.' }
$names = @{}
foreach ($arm in $armDefs) {
    if ($names.ContainsKey($arm.name)) { throw "Duplicate arm name '$($arm.name)'." }
    $names[$arm.name] = $true
}
if ($ScoreOnly -and $ReuseDir -eq '') { throw 'ScoreOnly requires an existing ReuseDir.' }
if (-not $ScoreOnly -and ($HostDir -eq '' -or $JoinDir -eq '' -or $NativeBin -eq '')) {
    throw 'Launch mode requires explicit -HostDir, -JoinDir and -NativeBin.'
}
# Two batches at once are worse than useless: both drive the same pair of Kenshi
# installs, the same fixture save and the same port 27800, so the runs interleave
# and every number produced is garbage - and a save restore racing a live client
# can bake one batch's world into the other's fixture.
if (-not $ScoreOnly) {
    $mine = $PID
    $others = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
                Where-Object { $_.ProcessId -ne $mine -and $_.CommandLine -match 'authority_ab\.ps1' })
    if ($others.Count -gt 0) {
        throw ("Another authority_ab.ps1 is already running (PID " +
               (($others | ForEach-Object { $_.ProcessId }) -join ", ") +
               "). Stop it first - concurrent batches share one game install and " +
               "invalidate each other's runs.")
    }
}

if ($ReuseDir -ne "") {
    $batchDir = (Resolve-Path $ReuseDir).Path
} else {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $batchDir = Join-Path $repoRoot "tools\test-runs\ab_${Scenario}_$stamp"
    New-Item -ItemType Directory -Force -Path $batchDir | Out-Null
}
Write-Host "Authority A/B: scenario=$Scenario runs=$Runs arms=$($Arms -join ' | ')"
Write-Host "  batch dir: $batchDir"

$all = New-Object System.Collections.ArrayList
foreach ($ad in $armDefs) {
    if ($ScoreOnly) {
        $pattern = '^' + [regex]::Escape($ad.name) + '_(\d+)$'
        $dirs = @(Get-ChildItem -Directory -LiteralPath $batchDir |
                  Where-Object { $_.Name -cmatch $pattern } |
                  Sort-Object { [int]($_.Name.Substring($ad.name.Length + 1)) })
        foreach ($d in $dirs) {
            $idx = [int]$d.Name.Substring($ad.name.Length + 1)
            [void]$all.Add((Measure-Run $ad $idx $d.FullName))
        }
    } else {
        for ($i = 1; $i -le $Runs; $i++) {
            [void]$all.Add((Invoke-Arm $ad $i $batchDir))
        }
    }
}

Write-Host ""
Write-Host "================ PER-RUN ================"
$summaries = New-Object System.Collections.ArrayList
foreach ($ad in $armDefs) {
    $rows = @($all | Where-Object { $_.arm -eq $ad.name })
    $s = Show-Arm $ad.name $rows
    if ($null -ne $s) { [void]$summaries.Add($s) }
}

Write-Host "================ ARM COMPARISON ================"
foreach ($ad in $armDefs) { Write-Host ("  arm '{0}' = {1}" -f $ad.name, $ad.diagEnv) }
Write-Host "  drvHost/drvJoin = mean bodies driven per WNPC dump on that client"
Write-Host ""
$summaries | Format-Table -AutoSize arm, validRuns, drvHost, drvJoin, drvTotal,
                                    censHost, censJoin,
                                    dualDrive, pcDupes, parity, primary | Out-String | Write-Host
$summaries | Format-Table -AutoSize arm, nearExist, nearPosOk, taskParity, cenExist, cenPosOk,
                                    pcWorstMed | Out-String | Write-Host
$summaries | Format-Table -AutoSize arm, zeroFrac, stallFrac, ghostFrac, zombieFrac |
    Out-String | Write-Host
if ($Scenario -eq "split_far2") {
    Write-Host "  jOwnPop = join's count of the JOIN's town (the number cell authority exists to protect)"
    Write-Host "  hOwnPop = host's count of the HOST's town; cross columns are the peer's view"
    $summaries | Format-Table -AutoSize arm, jOwnPop, hSeesJoin, hOwnPop, jSeesHost |
        Out-String | Write-Host
}

ConvertTo-Json -InputObject @($all.ToArray()) -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $batchDir "ab_runs.json")
ConvertTo-Json -InputObject @($summaries.ToArray()) -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $batchDir "ab_summary.json")
Write-Host "  wrote $batchDir\ab_runs.json + ab_summary.json"

$bad = @($all | Where-Object { $_.valid -eq "ARM-MISMATCH" })
if ($bad.Count -gt 0) {
    Write-Host ""
    Write-Host "WARNING: $($bad.Count) run(s) did not read back the arm's flags on both clients."
    Write-Host "         The override did not reach the game - do not read the table above."
    foreach ($b in $bad) { Write-Host ("         " + $b.dir + ": " + $b.armBad) }
}
Write-Host 'DIAGNOSTIC ONLY: arm comparisons are not gameplay certification.'
if (@($all | Where-Object { $_.valid -ne 'ok' }).Count -or $summaries.Count -ne $armDefs.Count) {
    Write-Host 'INCOMPLETE: invalid archives or an arm without comparable evidence.'
    exit 2
}
exit 0
