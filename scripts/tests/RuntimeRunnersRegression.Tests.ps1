$ErrorActionPreference = 'Stop'
$scripts = Split-Path -Parent $PSScriptRoot
function Assert($condition,$message) { if (-not $condition) { throw $message } }
function Parse-Runner($name) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $scripts $name),[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    return $ast
}
$regress=Parse-Runner 'regress.ps1'
$spike=Parse-Runner 'run_spike.ps1'
$run=Parse-Runner 'run_test.ps1'
$stream=Parse-Runner 'stream_test.ps1'
[void](Parse-Runner 'dev_cycle.ps1')
[void](Parse-Runner 'run_spikes.ps1')
$bindSpike=[scriptblock]::Create("[CmdletBinding()]`n" + $spike.ParamBlock.Extent.Text + "`nreturn `$Id")
foreach ($valid in @('0','401','402','451')) { Assert ((& $bindSpike -Id $valid) -eq $valid) 'Supported spike rejected.' }
foreach ($invalid in @('9','not-a-probe','')) {
    $rejected=$false
    try { & $bindSpike -Id $invalid | Out-Null } catch { $rejected=$true }
    Assert $rejected 'Unimplemented spike accepted.'
}
foreach ($obsolete in @('HostOnly','SpikeArg')) {
    $rejected=$false
    $obsoleteArgs=@{}; $obsoleteArgs[$obsolete]='unsupported'
    try { & $bindSpike -Id 401 @obsoleteArgs | Out-Null } catch { $rejected=$true }
    Assert $rejected 'Obsolete spike option was silently accepted.'
}
$unitStatements=$regress.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.StartsWith('if (Test-Path $prototest)')},$true).Clauses[0].Item2.Statements
$summaryStart=@($unitStatements | Where-Object { $_.Extent.Text.StartsWith('$summaries =') })[0].Extent.StartOffset
$summaryEnd=@($unitStatements | Where-Object { $_.Extent.Text.StartsWith('Write-Host ("UNIT LAYER:') })[0].Extent.StartOffset
$source=$regress.Extent.Text
$judgeUnit=[scriptblock]::Create($source.Substring($summaryStart,$summaryEnd-$summaryStart))
foreach ($case in @(
    @{lines=@();code=0;ok=$false},
    @{lines=@('prototest: 0/0 checks passed - PASS');code=0;ok=$false},
    @{lines=@('prototest: 1/2 checks passed - PASS');code=0;ok=$false},
    @{lines=@('prototest: 2/2 checks passed - PASS','prototest: 2/2 checks passed - PASS');code=0;ok=$false},
    @{lines=@('prototest: 2/2 checks passed - PASS');code=7;ok=$false},
    @{lines=@('prototest: 2/2 checks passed - PASS');code=0;ok=$true}
)) {
    $unitOutput=$case.lines; $unitExit=$case.code
    . $judgeUnit
    Assert ($unitOk -eq $case.ok) 'Unit terminal receipt accepted/rejected incorrectly.'
}
$root=Join-Path ([IO.Path]::GetTempPath()) ('RuntimeRunners_'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$originalLocalAppData=$env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA=Join-Path $root 'appdata'
    $OutDir=Join-Path $root 'out'
    New-Item -ItemType Directory -Path $OutDir | Out-Null
    $fresh=$run.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.Contains('OutDir must be empty:')},$true)
    . ([scriptblock]::Create($fresh.Extent.Text))
    Set-Content -LiteralPath (Join-Path $OutDir 'host.log') -Value 'old successful gameplay' -Encoding UTF8
    $rejected=$false
    try { . ([scriptblock]::Create($fresh.Extent.Text)) } catch { $rejected=$true }
    Assert $rejected 'Old participant evidence was accepted as fresh.'
    $spikeJudge=$spike.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$ok'},$true)
    $hostLog=Join-Path $OutDir 'host.log'; $joinLog=Join-Path $OutDir 'join.log'
    Set-Content -LiteralPath $joinLog -Value 'current join' -Encoding UTF8
    foreach ($case in @(
        @{exit=1;receipt=[pscustomobject]@{scenario='spike';captureOk=$true};ok=$false},
        @{exit=0;receipt=$null;ok=$false},
        @{exit=0;receipt=[pscustomobject]@{scenario='other';captureOk=$true};ok=$false},
        @{exit=0;receipt=[pscustomobject]@{scenario='spike';captureOk=$false};ok=$false},
        @{exit=0;receipt=[pscustomobject]@{scenario='spike';captureOk=$true};ok=$true}
    )) {
        $childExit=$case.exit; $verdict=$case.receipt
        . ([scriptblock]::Create($spikeJudge.Extent.Text))
        Assert ($ok -eq $case.ok) 'Spike capture bypassed child/receipt failure.'
    }
    $historyFile=Join-Path $root 'history.jsonl'
    $hist=[pscustomobject]@{outDir=([string][char]0x65E5 + [char]0x672C + [char]0x8A9E + '-' + [char]0xE9);pass=$false}
    $append=$regress.Find({param($n) $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Extent.Text.StartsWith('[IO.File]::AppendAllText')},$true)
    $utf8=New-Object System.Text.UTF8Encoding($false,$true)
    . ([scriptblock]::Create($append.Extent.Text))
    . ([scriptblock]::Create($append.Extent.Text))
    $lines=[IO.File]::ReadAllLines($historyFile,$utf8)
    Assert ($lines.Count -eq 2) 'History is not JSONL.'
    Assert (($lines[1] | ConvertFrom-Json).outDir -ceq $hist.outDir) 'Unicode history did not round-trip.'
    . (Join-Path $scripts 'kit_preflight.ps1')
    . (Join-Path $scripts 'capture_save.ps1')
    $HostDir=Join-Path $root 'host'; $JoinDir=Join-Path $root 'join'
    foreach ($dir in @($HostDir,$JoinDir)) {
        New-Item -ItemType Directory -Path $dir | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $dir 'settings.cfg'),[Text.Encoding]::ASCII.GetBytes("User save location=1`r`nOther=keep`r`n"))
    }
    $sourceSave=Join-Path $root 'original'
    New-Item -ItemType Directory -Path $sourceSave | Out-Null
    Set-Content -LiteralPath (Join-Path $sourceSave 'quick.save') -Value 'immutable source'
    $before=(Get-FileHash -LiteralPath (Join-Path $sourceSave 'quick.save')).Hash
    $Save='stream_fixture'
    $backupRoot=Join-Path $root 'backup'
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $runState=@{Root=$backupRoot;Backups=@()}
    $prepare=$stream.Find({param($n) $n -is [Management.Automation.Language.TryStatementAst]},$true).Body.Statements
    $first=$prepare[0].Extent.StartOffset
    $last=@($prepare | Where-Object { $_.Extent.Text.StartsWith('Copy-Item -LiteralPath $sourceSave') })[0].Extent.EndOffset
    . ([scriptblock]::Create($stream.Extent.Text.Substring($first,$last-$first)))
    Assert ((Get-FileHash -LiteralPath (Join-Path $sourceSave 'quick.save')).Hash -eq $before) 'Stream setup mutated input save.'
    Assert (Test-Path -LiteralPath (Join-Path $HostDir "save\$Save\quick.save")) 'Host fixture was not copied.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $JoinDir "save\$Save"))) 'JOIN incorrectly received a pre-shared copy.'
    foreach ($entry in $runState.Backups) {
        if ($entry.Existed) { Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force }
    }
    Assert ([IO.File]::ReadAllText((Join-Path $JoinDir 'settings.cfg')) -ceq "User save location=1`r`nOther=keep`r`n") 'Settings backup lost original bytes.'
    $shell=(Get-Process -Id $PID).Path
    $owned=Start-Process -FilePath $shell -ArgumentList '-NoProfile -Command "exit 7"' -PassThru -WindowStyle Hidden
    $deadline=Get-CoopRunDeadline -Process $owned -Seconds 2 -StartTimeoutSec 2 -ArmTimeoutMs 0 -KillGraceSec 2
    $exitFailed=$false
    try { Wait-CoopOwnedExit -Process $owned -Deadline $deadline } catch { $exitFailed=$true }
    Assert $exitFailed 'Nonzero owned process was accepted.'
    $missing=Join-Path $root 'never.log'
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $seen=Wait-CoopOwnedLogLine -Process $owned -Deadline $deadline -File $missing -Pattern 'gameplay started' -TimeoutSec 60
    Assert (-not $seen -and $clock.Elapsed.TotalSeconds -lt 3) 'Exited process consumed an unowned observation timeout.'
    # The HOST gameplay clock starts before the JOIN launcher/load. A capture
    # already due must not sleep that elapsed interval a second time.
    $delayFunction=$run.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Wait-RunDelay'},$true)
    . ([scriptblock]::Create($delayFunction.Extent.Text))
    $held=[Diagnostics.Process]::GetCurrentProcess()
    $runState=@{Processes=@{}}
    $runState.Processes[$held.Id]=$held
    $deadlines=@{}
    $deadlines[$held.Id]=[DateTime]::UtcNow.AddSeconds(30)
    $clock=[Diagnostics.Stopwatch]::StartNew()
    Wait-RunDelay -ProcId $held.Id -Seconds 1 -Since ([DateTime]::UtcNow.AddMinutes(-1))
    Assert ($clock.Elapsed.TotalSeconds -lt 0.5) 'Capture delay restarted after JOIN startup instead of using HOST gameplay time.'
} finally {
    $env:LOCALAPPDATA=$originalLocalAppData
    Remove-Item -LiteralPath $root -Recurse -Force
}
Write-Host 'RuntimeRunnersRegression: PASS'
