<#
.SYNOPSIS
  Prove coordinated save/transfer followed by exact site/progress resume lineage.
.DESCRIPTION
  Stage 1 must pass save_stage1, preserving its transferred coopresume output.
  Stage 2 loads each installation's configured root without deployment or mirror.
  Both child verdicts plus Test-SaveResumeLineage are required. -SkipStage1 is
  diagnostic-only: even a successful stage 2 cannot certify the full proof.
#>
[CmdletBinding()]
param(
    [string]$OutDir='', [int]$Port=27800,
    [string]$HostDir='C:\Program Files (x86)\Steam\steamapps\common\Kenshi',
    [string]$JoinDir="$env:USERPROFILE\Kenshi-Join",
    [ValidateRange(0,300)][int]$InterStageSec=5, [switch]$SkipStage1
)
$ErrorActionPreference='Stop'
$scriptDir=$PSScriptRoot; $repoRoot=Split-Path $scriptDir -Parent
. (Join-Path $scriptDir 'capture_save.ps1')
. (Join-Path $scriptDir 'kit_preflight.ps1')
$HostDir=Get-CoopSaveFullPath $HostDir; $JoinDir=Get-CoopSaveFullPath $JoinDir
$saveRoots=@((Resolve-CoopSelectedSaveRoot $HostDir),(Resolve-CoopSelectedSaveRoot $JoinDir) | Select-Object -Unique)
if (-not $OutDir) { $OutDir=Join-Path $repoRoot ('tools\test-runs\resume_'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'_'+[guid]::NewGuid().ToString('N')) }
$OutDir=Get-CoopSaveFullPath $OutDir
Assert-CoopSavePath $OutDir
$protected=@((Join-Path $repoRoot 'fixtures\saves'))
foreach ($root in $saveRoots) { foreach ($name in @('sync','coopresume')) { $protected+=Join-Path $root $name } }
foreach ($dir in @($HostDir,$JoinDir)) {
    foreach ($name in @('settings.cfg','kenshi.cfg','controls.cfg','data\mods.cfg','RE_Kenshi.json','mods\KenshiCoop')) { $protected+=Join-Path $dir $name }
}
foreach ($path in $protected) { if (Test-CoopSavePathOverlap $OutDir $path) { throw "Resume artifacts overlap protected input/output: $path" } }
$runState=New-CoopRunState -GameDirs @($HostDir,$JoinDir) -SaveNames @()
try {
    Clear-CoopRunEnvironment -State $runState
    foreach ($root in $saveRoots) { Add-CoopRunBackup -State $runState -Path (Join-Path $root 'coopresume') }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $stage1Dir=Join-Path $OutDir 'stage1'; $stage2Dir=Join-Path $OutDir 'stage2'
    if ((Test-Path -LiteralPath $stage1Dir) -or (Test-Path -LiteralPath $stage2Dir)) { throw 'Resume stage directories already exist; use a fresh artifact directory.' }
    $runTest=Join-Path $scriptDir 'run_test.ps1'; $shell=(Get-Process -Id $PID).Path
    $common=@('-Port',"$Port",'-HostDir',$HostDir,'-JoinDir',$JoinDir)
    $stage1Pass=$false
    if (-not $SkipStage1) {
        & $shell -NoProfile -ExecutionPolicy Bypass -File $runTest -Scenario save_stage1 -OutDir $stage1Dir -PreserveFixtureOutput @common
        $stage1Exit=$LASTEXITCODE; $stage1Pass=$stage1Exit -eq 0
        if (-not $stage1Pass) { throw "Stage 1 failed (exit $stage1Exit); nothing trustworthy to resume." }
    } else { Write-Host 'STAGE 1 SKIPPED: diagnostic resume only, full proof cannot PASS.' }
    foreach ($root in $saveRoots) {
        $save=Join-Path $root 'coopresume'; Assert-CoopSavePath $save
        if (-not(Test-Path -LiteralPath (Join-Path $save 'quick.save') -PathType Leaf)) { throw "Transferred save missing literal quick.save at $save" }
    }
    Start-Sleep -Seconds $InterStageSec
    & $shell -NoProfile -ExecutionPolicy Bypass -File $runTest -Scenario resume_check -OutDir $stage2Dir -UseSessionSave @common
    $stage2Exit=$LASTEXITCODE; $stage2Pass=$stage2Exit -eq 0
    $lineage=[pscustomobject]@{ok=$false;reasons=@('Stage 1 skipped; no current-run transfer/site provenance.');metrics=@{}}
    if ($stage1Pass -and $stage2Pass) {
        Import-Module (Join-Path $scriptDir 'CoopOracles.psm1') -Force
        $logs=@{}
        foreach ($stage in @('stage1','stage2')) { foreach ($side in @('host','join')) {
            $path=Join-Path (Join-Path $OutDir $stage) ($side+'.log')
            if (-not(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing cross-stage proof log: $path" }
            $logs[$stage+$side]=@(Get-Content -LiteralPath $path -Encoding UTF8)
        } }
        $lineage=Test-SaveResumeLineage -Stage1HostLines $logs.stage1host -Stage1JoinLines $logs.stage1join -Stage2HostLines $logs.stage2host -Stage2JoinLines $logs.stage2join
    }
    $pass=$stage1Pass -and $stage2Pass -and $lineage.ok
    $result=[ordered]@{schema=1;ok=$pass;stage1=$stage1Pass;stage2=$stage2Pass;skippedStage1=[bool]$SkipStage1;saveRoots=$saveRoots;lineage=$lineage}
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutDir 'resume-verdict.json') -Encoding UTF8
    foreach ($reason in $lineage.reasons) { Write-Host "Lineage: $reason" }
    Write-Host ('RESUME-TEST RESULT: '+$(if($pass){'PASS'}else{'FAIL'}))
    if (-not $pass) { throw "Resume proof failed; see $OutDir\resume-verdict.json" }
} finally { Restore-CoopRunState $runState }
