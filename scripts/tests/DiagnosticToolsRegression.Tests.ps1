# Offline consumer fixtures only: never launches Kenshi, winget or registry repair.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shell = (Get-Process -Id $PID).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ('CoopDiagnosticFixtures_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
Push-Location -LiteralPath $temp
$script:failures = 0
function Check($Name, [bool]$Condition) { if (-not $Condition) { $script:failures++; Write-Host "FAIL $Name" } else { Write-Host "ok $Name" } }
function Invoke-FixtureShell([string[]]$Arguments) {
    # Windows PowerShell maps captured native stderr to error records. Expected
    # negative child outcomes must be assertions, not terminate this fixture.
    $ErrorActionPreference='Continue'
    $text=@(& $shell -NoProfile -ExecutionPolicy Bypass @Arguments 2>&1) -join "`n"
    [pscustomobject]@{ code=$LASTEXITCODE; text=$text }
}
function Run-Tool($Name, [string[]]$Arguments) {
    Invoke-FixtureShell (@('-File',(Join-Path $root "tools/$Name.ps1")) + $Arguments)
}
function Save-Json($Path, $Value) { ConvertTo-Json -InputObject $Value -Depth 10 | Set-Content -LiteralPath $Path -Encoding UTF8 }
function History-Row($Stamp, [bool]$Pass = $true, [bool]$Flaky = $false, $Variant = 'clean') {
    [pscustomobject]@{ timestamp=$Stamp; scenario='fixture'; variant=$Variant; pass=$Pass; flaky=$Flaky; gates=@(@{gate='fixture';status='PASS';metrics=@{}}) }
}
function Archive($Batch, $Name = 'on_1', $Scenario = 'split_far2', $Auth = '1') {
    $dir = Join-Path $Batch $Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Save-Json (Join-Path $dir 'run.json') @{ scenario=$Scenario; diagEnvOverride=@{KENSHICOOP_CELL_AUTH=$Auth}; outDir='Z:/stale/archive' }
    $gates = @('health_host','health_join','check_fail','result_host','result_join','process_exit' | ForEach-Object { @{gate=$_;status='PASS';metrics=@{}} })
    $primaryStatus = if ($Auth -eq '0') { 'SKIP' } else { 'FAIL' }
    $gates += @{gate='split_far2';status=$primaryStatus;metrics=@{}}
    Save-Json (Join-Path $dir 'verdict.json') @{scenario=$Scenario;pass=$false;primary='split_far2';gating=@('split_far2');gates=$gates}
    $log = @(
        "[23:59:50.000] KenshiCoop: effective cfg scenario='$Scenario' on=[censusFreezeAi] off=[camInterest] cellAuth=$Auth cellCollapse=1",
        '[23:59:50.001] gameplay started', "[23:59:50.002] SCENARIO $Scenario start",
        '[23:59:55.000] SCENARIO SPLITFAR2 arrived=1 popHost=10 popJoin=11',
        '[23:59:59.000] SCENARIO WNPC hand=1,2,3,4,5 pos=1,0,1 cls=drv name=''fixture''',
        '[23:59:59.001] SCENARIO WORLD n=1 cls=host',
        '[00:00:04.000] SCENARIO WORLD n=0 cls=host',
        '[00:00:05.000] SCENARIO RESULT PASS')
    $log | Set-Content -LiteralPath (Join-Path $dir 'host.log') -Encoding UTF8
    $log | Set-Content -LiteralPath (Join-Path $dir 'join.log') -Encoding UTF8
    return $dir
}
function Import-ToolFunctions($Name, [string[]]$Names) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name.ps1"),[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw ($errors -join '; ') }
    foreach ($name in $Names) {
        $fn=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
        if (-not $fn) { throw "Missing production function $Name/$name" }
        # Publish only the real function, never the tool's launch/main statements.
        . ([scriptblock]::Create(($fn.Extent.Text -replace '^function ', 'function script:')))
    }
}
try {
    $history = Join-Path $temp 'history.jsonl'
    $row = History-Row '2020-01-01T00:00:00Z'
    Save-Json $history $row
    # JSONL records must occupy one line.
    $row | ConvertTo-Json -Compress -Depth 5 | Set-Content $history -Encoding UTF8
    $r = Run-Tool analyze_history @('-Path',$history)
    Check 'valid history is a measured diagnostic' ($r.code -eq 0 -and $r.text -match 'runs in window: 1')
    foreach ($invalid in @('{broken', ($row | Select-Object * -ExcludeProperty timestamp | ConvertTo-Json -Compress -Depth 5), '{"timestamp":"2020-01-01T00:00:00Z","scenario":"fixture","variant":"clean","pass":"false","flaky":false,"gates":[]}')) {
        $invalid | Add-Content $history -Encoding UTF8
        $r = Run-Tool analyze_history @('-Path',$history)
        Check 'malformed latest history remains visible and nonzero' ($r.code -ne 0 -and $r.text -match 'INVALID')
        $row | ConvertTo-Json -Compress -Depth 5 | Set-Content $history -Encoding UTF8
    }
    foreach ($bad in @(@{flaky='false'}, @{gates=@(@{gate='fixture';status='BROKEN'})}, @{gates=@(@{gate='';status='PASS'})}, @{timestamp='not-a-date'}, @{gates='garbage'})) {
        $copy = History-Row '2020-01-01T00:00:00Z'
        foreach ($key in $bad.Keys) { $copy.$key=$bad[$key] }
        $copy | ConvertTo-Json -Compress -Depth 5 | Set-Content $history -Encoding UTF8
        Check 'invalid producer schema fails' ((Run-Tool analyze_history @('-Path',$history)).code -ne 0)
    }
    '' | Set-Content $history -Encoding UTF8
    Check 'blank history fails' ((Run-Tool analyze_history @('-Path',$history)).code -ne 0)
    Check 'missing history fails' ((Run-Tool analyze_history @('-Path',(Join-Path $temp 'missing'))).code -ne 0)
    Check 'negative window fails' ((Run-Tool analyze_history @('-Path',$history,'-Days','-1')).code -ne 0)
    $sequence = @(
        (History-Row '2020-01-04T00:00:00Z' $false),
        (History-Row '2020-01-01T00:00:00Z' $true),
        (History-Row '2020-01-03T00:00:00Z' $true),
        (History-Row '2020-01-02T00:00:00Z' $false),
        (History-Row '2020-01-01T00:00:00Z' $true $true 'wan'),
        (History-Row '2020-01-02T00:00:00Z' $true $true 'wan')
    )
    $sequence | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 5 } | Set-Content $history -Encoding UTF8
    $r=Run-Tool analyze_history @('-Path',$history)
    Check 'chronological flips and retry flakes stay variant-specific' ($r.code -eq 0 -and $r.text -match 'fixture/clean runs=4 fails=2 flips=3 retryFlakes=0 tail=\.X\.X' -and $r.text -match 'fixture/wan runs=2 fails=0 flips=0 retryFlakes=2')
    $offsetRows=@((History-Row '2019-12-31T23:00:00Z' $true),(History-Row '2020-01-01T00:30:00+02:00' $false))
    $offsetRows | ForEach-Object {$_ | ConvertTo-Json -Compress -Depth 5} | Set-Content $history -Encoding UTF8
    foreach($culture in @('en-US','de-DE','fr-FR')) {
        $historyTool=(Join-Path $root 'tools/analyze_history.ps1').Replace("'","''")
        $historyArgument=$history.Replace("'","''")
        $command="[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('$culture'); & '$historyTool' -Path '$historyArgument'; exit `$LASTEXITCODE"
        $r=Invoke-FixtureShell @('-Command',$command)
        Check "offset chronology is invariant under $culture from non-repository cwd" ($r.code -eq 0 -and $r.text -match 'fixture/clean runs=2 fails=1 flips=1 retryFlakes=0 tail=X\.')
    }
    (History-Row '2020-01-01T00:00:00') | ConvertTo-Json -Compress -Depth 5 | Set-Content $history -Encoding UTF8
    $r=Run-Tool analyze_history @('-Path',$history)
    Check 'legacy offset-less history explicitly reports local-zone assumption' ($r.code -eq 0 -and $r.text -match 'LEGACY:')
    [IO.File]::WriteAllBytes($history,[byte[]]@(0xC3,0x28))
    Check 'invalid UTF8 bytes cannot become clean history' ((Run-Tool analyze_history @('-Path',$history)).code -ne 0)
    (History-Row ([DateTimeOffset]::UtcNow.AddDays(1).ToString('o'))) | ConvertTo-Json -Compress -Depth 5 | Set-Content $history -Encoding UTF8
    Check 'future evidence is not silently accepted' ((Run-Tool analyze_history @('-Path',$history,'-Days','1')).code -ne 0)
    $batch = Join-Path $temp 'batch'; New-Item -ItemType Directory $batch | Out-Null
    $dir = Archive $batch
    $r = Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')
    Check 'completed negative primary remains comparable' ($r.code -eq 0)
    $runsFile = Join-Path $batch 'ab_runs.json'
    Check 'singleton runs artifact is array' ((Get-Content $runsFile -Raw).TrimStart().StartsWith('['))
    $runs = @(Get-Content $runsFile -Raw | ConvertFrom-Json)
    Check 'midnight and explicit empty roster contribute driving zero' ($runs[0].dumps -eq 2 -and $runs[0].drvHost -eq 0.5)
    Check 'singleton summary artifact is array' ((Get-Content (Join-Path $batch 'ab_summary.json') -Raw).TrimStart().StartsWith('['))
    foreach ($gate in @('health_host','result_host','process_exit')) {
        $dir=Archive $batch
        $v=Get-Content (Join-Path $dir 'verdict.json') -Raw | ConvertFrom-Json
        ($v.gates | Where-Object {$_.gate -eq $gate}).status='FAIL'
        Save-Json (Join-Path $dir 'verdict.json') $v
        Check "$gate failure rejects comparison sample" ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')).code -ne 0)
    }
    foreach ($mutation in @('missing-result','native-fail','wrong-native','substring-config','conflicting-config')) {
        $dir=Archive $batch
        $p=Join-Path $dir 'join.log'; $log=@(Get-Content $p -Encoding UTF8)
        switch($mutation) {
            'missing-result' {$log=@($log | Where-Object {$_ -notmatch 'SCENARIO RESULT'})}
            'native-fail' {$log=@($log -replace 'RESULT PASS','RESULT FAIL')}
            'wrong-native' {$log=@($log -replace 'SCENARIO split_far2 start','SCENARIO wrong start')}
            'substring-config' {$log[0]='[23:59:50.000] diagnostic cellAuth=1'}
            'conflicting-config' {$log+=($log[0] -replace 'cellAuth=1','cellAuth=0')}
        }
        $log | Set-Content $p -Encoding UTF8
        Check "$mutation rejects stale/partial native evidence" ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')).code -ne 0)
    }
    $dir=Archive $batch
    foreach ($badScenario in @('wrong-scenario')) {
        Save-Json (Join-Path $dir 'run.json') @{scenario=$badScenario;diagEnvOverride=@{KENSHICOOP_CELL_AUTH='1'}}
        Check 'wrong cached identity fails' ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')).code -ne 0)
    }
    foreach($fault in @('missing-run','override-mismatch','no-roster','no-populations')) {
        $dir=Archive $batch
        switch($fault) {
            'missing-run' {Remove-Item (Join-Path $dir 'run.json')}
            'override-mismatch' {Save-Json (Join-Path $dir 'run.json') @{scenario='split_far2';diagEnvOverride=@{KENSHICOOP_CELL_AUTH='0'}}}
            'no-roster' {
                foreach($side in @('host','join')) {
                    $file=Join-Path $dir "$side.log"
                    @(Get-Content $file | Where-Object {$_ -notmatch 'SCENARIO (?:WNPC|WORLD)'}) | Set-Content $file -Encoding UTF8
                }
            }
            'no-populations' {
                $file=Join-Path $dir 'join.log'
                @(Get-Content $file | Where-Object {$_ -notmatch 'SCENARIO SPLITFAR2'}) | Set-Content $file -Encoding UTF8
            }
        }
        Check "$fault archive is never a valid comparison sample" ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')).code -ne 0)
    }
    $dir = Archive $batch
    Remove-Item (Join-Path $dir 'verdict.json')
    Check 'missing verdict cannot certify comparison' ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')).code -ne 0)
    foreach ($arm in @('../outside:KENSHICOOP_CELL_AUTH=1','*:KENSHICOOP_CELL_AUTH=1','on:KENSHICOOP_CELL_AUTH=1;on:KENSHICOOP_CELL_AUTH=0','empty:','unknown:KENSHICOOP_NOT_REAL=1','blank:KENSHICOOP_CELL_AUTH=','   :KENSHICOOP_CELL_AUTH=1')) {
        Check 'unsafe or unverifiable arms fail' ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms',$arm)).code -ne 0)
    }
    Check 'zero launch count fails offline' ((Run-Tool authority_ab @('-Scenario','split_far2','-Runs','0','-ScoreOnly','-ReuseDir',$batch)).code -ne 0)
    Check 'score only requires selected archive' ((Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly')).code -ne 0)
    $empty = Join-Path $temp 'empty'; New-Item -ItemType Directory $empty | Out-Null
    Check 'empty baseline is incomplete' ((Run-Tool dual_drive_baseline @('-RunsDir',$empty)).code -ne 0)
    Check 'missing baseline root fails' ((Run-Tool dual_drive_baseline @('-RunsDir',(Join-Path $temp 'missing'))).code -ne 0)
    Check 'empty handover archive is incomplete' ((Run-Tool collapse_handover @('-BatchDir',$empty)).code -ne 0)
    $r=Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$empty,'-Arms','on:KENSHICOOP_CELL_AUTH=1')
    $emptyRuns=Get-Content (Join-Path $empty 'ab_runs.json') -Raw | ConvertFrom-Json
    $emptySummary=Get-Content (Join-Path $empty 'ab_summary.json') -Raw | ConvertFrom-Json
    Check 'empty A/B writes empty evidence and fails assessment' ($r.code -ne 0 -and @($emptyRuns).Count -eq 0 -and @($emptySummary).Count -eq 0)
    $dir=Archive $batch
    $r=Run-Tool dual_drive_baseline @('-RunsDir',$batch)
    Check 'baseline admits compatible native config independently of unjudged oracle' ($r.text -match 'on_1' -and $r.text -notmatch 'REJECTED:')
    $dir=Archive $batch 'on_1' 'split_far2' '0'
    $r=Run-Tool dual_drive_baseline @('-RunsDir',$batch)
    Check 'authority-off baseline candidate explicitly rejected' ($r.code -ne 0 -and $r.text -match 'REJECTED:')
    foreach($fault in @('metadata','native-identity','missing-terminal','disagreeing-config')) {
        $dir=Archive $batch
        switch($fault) {
            'metadata' {'{malformed' | Set-Content (Join-Path $dir 'run.json')}
            'native-identity' {$file=Join-Path $dir 'join.log'; @(Get-Content $file) -replace 'SCENARIO split_far2 start','SCENARIO wrong start' | Set-Content $file -Encoding UTF8}
            'missing-terminal' {$file=Join-Path $dir 'join.log'; @(Get-Content $file | Where-Object {$_ -notmatch 'SCENARIO RESULT'}) | Set-Content $file -Encoding UTF8}
            'disagreeing-config' {$file=Join-Path $dir 'join.log'; @(Get-Content $file) -replace 'cellAuth=1','cellAuth=0' | Set-Content $file -Encoding UTF8}
        }
        $r=Run-Tool dual_drive_baseline @('-RunsDir',$batch)
        Check "$fault baseline candidate explicitly rejected" ($r.code -ne 0 -and $r.text -match 'REJECTED:')
    }
    # Exercise real parsing functions without entering any launch path.
    Import-Module (Join-Path $root 'scripts/CoopOracles.psm1') -Force
    Import-ToolFunctions authority_ab @('Parse-Arm','Get-LoggedFlag','Get-EscapeCells','Get-DrvPerDump')
    $script:FlagFields=@{KENSHICOOP_CELL_AUTH='cellAuth';KENSHICOOP_CELL_COLLAPSE='cellCollapse';KENSHICOOP_CENSUS_FREEZE_AI='censusFreezeAi';KENSHICOOP_CAM_INTEREST='camInterest'}
    $Scenario='split_far2'
    foreach($value in @('0','1','2','00','false')) {
        $arm=Parse-Arm "flag:KENSHICOOP_CELL_AUTH=$value"
        Check "native boolean $value follows exact zero semantics" ($arm.expect.cellAuth -eq [int]($value -cne '0'))
    }
    $dir=Archive $batch
    Check 'on/off config roster readback is real' ((Get-LoggedFlag (Join-Path $dir 'host.log') 'censusFreezeAi') -eq 1 -and (Get-LoggedFlag (Join-Path $dir 'host.log') 'camInterest') -eq 0)
    $signed=Join-Path $temp 'signed.log'
    @('[12:00:00.000] SCENARIO ESCAPE subject=1 cell=-1,2','[12:00:05.000] SCENARIO ESCAPE subject=1 cell=0,2','[12:00:06.000] SCENARIO ESCAPE subject=1 cell=-1,2') | Set-Content $signed -Encoding UTF8
    Check 'signed distinct subject cells count without duplicates' ((Get-EscapeCells $signed) -eq 2)
    $two=Archive $batch 'on_2'
    $r=Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$batch,'-Arms','on:KENSHICOOP_CELL_AUTH=1')
    $twoRows=Get-Content (Join-Path $batch 'ab_runs.json') -Raw | ConvertFrom-Json
    Check 'two rows remain separately measured' ($r.code -eq 0 -and @($twoRows).Count -eq 2 -and @($twoRows|Where-Object{$_.dumps -eq 2 -and $_.drvHost -eq 0.5}).Count -eq 2)
    $armsBatch=Join-Path $temp 'two-arms'; New-Item -ItemType Directory $armsBatch | Out-Null
    Archive $armsBatch 'on_1' | Out-Null
    Archive $armsBatch 'off_1' 'split_far2' '0' | Out-Null
    $r=Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$armsBatch)
    $summaryPath=Join-Path $armsBatch 'ab_summary.json'
    $twoArms=Get-Content $summaryPath -Raw | ConvertFrom-Json
    Check 'two-arm summary retains distinct measured arms' ($r.code -eq 0 -and @($twoArms).Count -eq 2)
    $offBatch=Join-Path $temp 'off'; New-Item -ItemType Directory $offBatch | Out-Null
    $off=Archive $offBatch 'off_1' 'split_far2' '0'
    $r=Run-Tool authority_ab @('-Scenario','split_far2','-ScoreOnly','-ReuseDir',$offBatch,'-Arms','off:KENSHICOOP_CELL_AUTH=0')
    Check 'intentional authority-off primary SKIP with populations remains comparable' ($r.code -eq 0 -and $r.text -match 'SKIP')
    $handover=Join-Path $temp 'handover'; New-Item -ItemType Directory $handover | Out-Null
    $hd=Archive $handover
    $hostRows=@(Get-Content (Join-Path $hd 'host.log') -Encoding UTF8)
    $hostRows=@($hostRows -replace '\[00:00:05.000\]','[00:00:10.000]' | Where-Object {$_ -notmatch 'WORLD n=0'})
    $hostRows=$hostRows[0..5] + @('[00:00:04.000] SCENARIO WNPC hand=1,2,3,4,6 pos=1,0,1 cls=drv name=''other full identity''','[00:00:04.001] SCENARIO WORLD n=1 cls=host') + $hostRows[6]
    $hostRows=$hostRows[0..3] + @('[23:59:58.000] [cell] MAP cells=1 collapse=0') + $hostRows[4..5] + @('[00:00:03.000] [cell] MAP cells=2 collapse=1') + $hostRows[6..8]
    $hostRows | Set-Content (Join-Path $hd 'host.log') -Encoding UTF8
    # Join shifted +30 sec across midnight; its genuine empty dump must still pair.
    $join=@(Get-Content (Join-Path $hd 'join.log') -Encoding UTF8) -replace '\[00:00:05.000\]','[00:00:10.000]'
    $shifted=@($join | ForEach-Object {
        $stamp=[regex]::Match($_,'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
        $ms=([int]$stamp.Groups[1].Value*3600000+[int]$stamp.Groups[2].Value*60000+[int]$stamp.Groups[3].Value*1000+[int]$stamp.Groups[4].Value+30000)%86400000
        '['+[timespan]::FromMilliseconds($ms).ToString('hh\:mm\:ss\.fff')+']'+$_.Substring($stamp.Length)
    })
    $shifted=$shifted[0..2]+@('[00:00:20.003] CLOCKSYNC offset=-30000 rtt=2')+$shifted[3..($shifted.Count-1)]
    $shifted | Set-Content (Join-Path $hd 'join.log') -Encoding UTF8
    Import-ToolFunctions collapse_handover @('Get-Dumps','Get-Flips')
    $pcOnly=Join-Path $temp 'pc-only.log'
    @('[12:00:00.000] SCENARIO WORLD n=0 cls=host','[12:00:00.001] SCENARIO WNPC hand=9,8,7,6,5 pos=1,0,1 cls=pc name=''fixture pc''') | Set-Content $pcOnly -Encoding UTF8
    Reset-GateResults
    $pcDriving=Get-DrvPerDump $pcOnly; $pcDumps=@(Get-Dumps $pcOnly)
    Check 'empty census with separately emitted PCs is one real zero-driving roster' ($pcDriving.dumps -eq 1 -and $pcDriving.mean -eq 0 -and $pcDumps.Count -eq 1 -and $pcDumps[0].hands.ContainsKey('9,8,7,6,5'))
    '[12:00:00.000] SCENARIO WORLD n=5 cls=host' | Set-Content $pcOnly -Encoding UTF8
    foreach($reader in @('Get-DrvPerDump','Get-Dumps')) {
        $rejected=$false
        try { & $reader $pcOnly | Out-Null } catch { $rejected=$true }
        Check "$reader rejects a nonempty summary without roster evidence" $rejected
    }
    Reset-GateResults
    $hostDumps=@(Get-Dumps (Join-Path $hd 'host.log')); $joinDumps=@(Get-Dumps (Join-Path $hd 'join.log'))
    Check 'clock aligned midnight rosters retain full identities and explicit empty sample' ($hostDumps.Count -eq 2 -and $joinDumps.Count -eq 2 -and $joinDumps[1].hands.Count -eq 0 -and $hostDumps[1].hands.ContainsKey('1,2,3,4,6') -and $hostDumps[1].t -eq $joinDumps[1].t)
    $r=Run-Tool collapse_handover @('-BatchDir',$handover,'-WindowSec','1')
    Check 'complete aligned handover archive is accepted' ($r.code -eq 0)
    $shifted=@($shifted -replace '00:00:34','00:00:44' -replace '00:00:29','00:00:49')
    $shifted | Set-Content (Join-Path $hd 'join.log') -Encoding UTF8
    Check 'unpaired handover logs fail no-evidence assessment' ((Run-Tool collapse_handover @('-BatchDir',$handover,'-WindowSec','1')).code -ne 0)
    # A native cmd shim returns real child statuses, never installed winget.
    $shim=Join-Path $temp 'winget-fixture.cmd'
    @('@echo off','echo %*>> "%COOP_FIXTURE_CALLS%"','if "%4"=="Microsoft.VCRedist.2010.x64" exit /b %COOP_FIXTURE_X64%','exit /b %COOP_FIXTURE_X86%') | Set-Content $shim -Encoding ASCII
    $wrapper=Join-Path $temp 'redist-boundary.ps1'
    $wrapperBody = {
param($Tool,$Shim,$Calls,$Trace,$X64,$X86,$Preference,$Mode='normal',[switch]$Preview)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=($Preference -eq 'true')
$env:COOP_FIXTURE_CALLS=$Calls
$env:COOP_FIXTURE_X64=$X64
$env:COOP_FIXTURE_X86=$X86
$script:lookups=0
function Get-Command {
    param($Name,$ErrorAction)
    $script:lookups++
    [IO.File]::AppendAllText($Trace,"lookup`n")
    if($Mode -eq 'body-fail' -and $script:lookups -eq 2){throw 'fixture lookup failure'}
    [pscustomobject]@{Source=$Shim}
}
function Start-Transcript {
    param($Path,[switch]$Force)
    [IO.File]::AppendAllText($Trace,"start`n")
    if($Mode -eq 'start-fail'){throw 'fixture transcript start failure'}
}
function Stop-Transcript {[IO.File]::AppendAllText($Trace,"stop`n")}
$invoke=@{}
if($Preview){$invoke.WhatIf=$true}
& $Tool @invoke
$code=$LASTEXITCODE
[IO.File]::AppendAllText($Trace,"callerPreference=$PSNativeCommandUseErrorActionPreference`n")
exit $code
    }
    $wrapperBody.ToString() | Set-Content $wrapper -Encoding UTF8
    foreach($tool in @('reinstall_redists','uninstall_redists')) {
        foreach($preference in @('true','false')) {
            foreach($codes in @(@(0,0),@(1,0),@(0,1),@(1641,3010))) {
                $calls=Join-Path $temp 'calls.txt'; $trace=Join-Path $temp 'trace.txt'
                Remove-Item $calls,$trace -ErrorAction SilentlyContinue
                $result=Invoke-FixtureShell @('-File',$wrapper,'-Tool',(Join-Path $root "tools/$tool.ps1"),'-Shim',$shim,'-Calls',$calls,'-Trace',$trace,'-X64',[string]$codes[0],'-X86',[string]$codes[1],'-Preference',$preference)
                $text=$result.text; $code=$result.code
                $expected=if($codes -contains 1){1}else{0}
                $attempts=@(Get-Content $calls); $events=@(Get-Content $trace)
                Check "$tool native statuses aggregate with preference=$preference codes=$codes" ($code -eq $expected -and $attempts.Count -eq 2 -and $text -match "x64 exit code: $($codes[0])" -and $text -match "x86 exit code: $($codes[1])")
                Check "$tool exact package IDs and transcript lifetime" ($attempts[0] -match '--exact --id Microsoft\.VCRedist\.2010\.x64(?: |$)' -and $attempts[1] -match '--exact --id Microsoft\.VCRedist\.2010\.x86(?: |$)' -and @($events | Where-Object {$_ -eq 'start'}).Count -eq 1 -and @($events | Where-Object {$_ -eq 'stop'}).Count -eq 1 -and $events -contains "callerPreference=$preference")
            }
        }
        foreach($mode in @('normal','start-fail','body-fail')) {
            $calls=Join-Path $temp 'calls.txt'; $trace=Join-Path $temp 'trace.txt'
            Remove-Item $calls,$trace -ErrorAction SilentlyContinue
            $preview=if($mode -eq 'normal'){@('-Preview')}else{@()}
            $result=Invoke-FixtureShell (@('-File',$wrapper,'-Tool',(Join-Path $root "tools/$tool.ps1"),'-Shim',$shim,'-Calls',$calls,'-Trace',$trace,'-X64','0','-X86','0','-Preference','true','-Mode',$mode) + $preview)
            $text=$result.text; $code=$result.code
            $events=if(Test-Path $trace){@(Get-Content $trace)}else{@()}
            if($mode -eq 'normal') {
                Check "$tool WhatIf performs no external lookup/transcript/native action" ($code -eq 0 -and -not(Test-Path $calls) -and -not @($events | Where-Object {$_ -in @('lookup','start','stop')}).Count)
            } else {
                $stops=if($mode -eq 'start-fail'){0}else{1}
                Check "$tool $mode cleans only a started transcript" ($code -ne 0 -and @($events | Where-Object {$_ -eq 'stop'}).Count -eq $stops)
            }
        }
    }
    # Run the real read-only registry validation/ShouldProcess prefix, excluding
    # all elevation/registry/transcript statements so HKLM is unreachable.
    $tokens=$null; $errors=$null
    $registryAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/fix_vs7_registry.ps1'),[ref]$tokens,[ref]$errors)
    $principal=$registryAst.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$principal'},$true)
    if(-not $principal){throw 'Registry validation boundary not found'}
    $validation=Join-Path $temp 'registry-validation.ps1'
    ($registryAst.Extent.Text.Substring(0,$principal.Extent.StartOffset) + "`nWrite-Output 'APPROVED-MUTATION-BOUNDARY'") | Set-Content $validation -Encoding UTF8
    $vc=Join-Path $temp 'compiler-only'
    New-Item -ItemType Directory -Path (Join-Path $vc 'VC/bin'),(Join-Path $vc 'VC/include'),(Join-Path $vc 'VC/lib') -Force | Out-Null
    '' | Set-Content (Join-Path $vc 'VC/vcvarsall.bat')
    # Read a real installed VC10 compiler; no generated PE is accepted as proof.
    Copy-Item -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio 10.0\VC\bin\cl.exe') -Destination (Join-Path $vc 'VC/bin/cl.exe')
    $result=Invoke-FixtureShell @('-File',$validation,'-VsRoot',$vc,'-WhatIf')
    Check 'supported compiler-only shape WhatIf never crosses mutation boundary' ($result.code -eq 0 -and $result.text -notmatch 'APPROVED-MUTATION-BOUNDARY')
    $modern=Join-Path $temp 'modern-toolchain'
    New-Item -ItemType Directory -Path (Join-Path $modern 'VC/bin'),(Join-Path $modern 'VC/include'),(Join-Path $modern 'VC/lib') -Force | Out-Null
    '' | Set-Content (Join-Path $modern 'VC/vcvarsall.bat')
    Copy-Item $shell (Join-Path $modern 'VC/bin/cl.exe')
    $result=Invoke-FixtureShell @('-File',$validation,'-VsRoot',$modern,'-WhatIf')
    Check 'modern/unrelated executable fails VC10 version validation' ($result.code -ne 0 -and $result.text -notmatch 'APPROVED-MUTATION-BOUNDARY')
    # Safety paths are real script invocations; -WhatIf must not look up winget or write transcripts.
    foreach ($tool in @('reinstall_redists','uninstall_redists')) {
        Check "$tool WhatIf is action-free" ((Run-Tool $tool @('-WhatIf')).code -eq 0)
    }
    Check 'registry repair rejects unrelated existing tree before any mutation' ((Run-Tool fix_vs7_registry @('-VsRoot',$empty,'-WhatIf')).code -ne 0)
} finally { Pop-Location; Remove-Item -LiteralPath $temp -Recurse -Force }
Write-Host "Diagnostic fixture failures: $script:failures"
exit $script:failures
