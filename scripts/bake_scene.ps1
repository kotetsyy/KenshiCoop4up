<#
.SYNOPSIS
  Bake duel, splitfar, bedcage, pole or buffpc from a named base save.
  Success requires exact native completion, stable save contents and clean owned
  process exit. -Promote transactionally captures into the tracked fixture store.
#>
param(
    [ValidateSet('duel','splitfar','bedcage','pole','buffpc')][string]$Setup,
    [string]$BaseSave='squad1', [string]$BakeSave,
    [string]$HostDir='C:\Program Files (x86)\Steam\steamapps\common\Kenshi',
    [ValidateRange(1,86400)][int]$Seconds=120,
    [ValidateRange(1,3600)][int]$StartTimeoutSec=240,
    [switch]$SkipDeploy, [switch]$Promote
)
function Read-CoopBakeCompletion {
    param([string[]]$Lines, [string]$Name, [string]$Folder)
    $pattern="\[setup\] auto-bake save COMPLETED name='"+[regex]::Escape($Name)+"' folder='(.+)' native=complete files=([1-9][0-9]*) bytes=([1-9][0-9]*)\s*$"
    $records=@($Lines | Where-Object { $_ -match $pattern })
    if ($records.Count -ne 1 -or $records[0] -notmatch $pattern) { throw 'Missing or ambiguous exact native bake completion.' }
    $nativeFolder=$Matches[1]; $files=[long]$Matches[2]; $bytes=[long]$Matches[3]
    if (-not [IO.Path]::IsPathRooted($nativeFolder) -or (Get-CoopSaveFullPath $nativeFolder) -ine (Get-CoopSaveFullPath $Folder)) { throw 'Native bake save root disagrees with the selected save root.' }
    return [pscustomobject]@{Files=$files;Bytes=$bytes}
}
if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference='Stop'
$scriptDir=$PSScriptRoot; $repo=Split-Path $scriptDir -Parent
. (Join-Path $scriptDir 'capture_save.ps1')
. (Join-Path $scriptDir 'kit_preflight.ps1')
if (-not $Setup) { throw 'Specify one supported automatic-bake Setup.' }
Assert-CoopSaveName $BaseSave -Bake; Assert-CoopSaveName $BakeSave -Bake
if ($BaseSave -ieq $BakeSave) { throw 'Bake input and output names must be distinct.' }
$HostDir=Get-CoopSaveFullPath $HostDir
$hostExe=Join-Path $HostDir 'kenshi_x64.exe'
if (-not(Test-Path -LiteralPath $hostExe -PathType Leaf)) { throw "Kenshi not found at $HostDir" }
$saveRoot=Resolve-CoopSelectedSaveRoot $HostDir
if (Test-CoopSavePathOverlap $saveRoot (Join-Path $repo 'fixtures\saves')) { throw 'Bake live root must not overlap pristine fixtures.' }
$baseDir=Join-Path $saveRoot $BaseSave; $bakedDir=Join-Path $saveRoot $BakeSave
Assert-CoopSavePath $baseDir; Assert-CoopSavePath $bakedDir
if (-not(Test-Path -LiteralPath (Join-Path $baseDir 'quick.save') -PathType Leaf)) { throw "Base save has no quick.save at $baseDir" }
$runState=New-CoopRunState -GameDirs @($HostDir) -SaveNames @()
$complete=$false
try {
    Clear-CoopRunEnvironment -State $runState -PreserveVariables @('KENSHICOOP_SPLITFAR_AT')
    Add-CoopRunBackup -State $runState -Path $baseDir
    Add-CoopRunBackup -State $runState -Path $bakedDir
    if (-not $SkipDeploy) { Install-CoopHarnessRuntime -RepoRoot $repo -GameDirs @($HostDir) }
    # Remove the old output under its snapshot: old bytes can never certify this run.
    if (Test-Path -LiteralPath $bakedDir) { Remove-Item -LiteralPath $bakedDir -Recurse -Force }
    $log=Join-Path $repo ('tools\_bake_'+$Setup+'_'+[guid]::NewGuid().ToString('N')+'.log')
    New-Item -ItemType Directory -Force -Path (Split-Path $log -Parent) | Out-Null
    $env:KENSHICOOP_MODE='host'; $env:KENSHICOOP_SAVE=$BaseSave
    $env:KENSHICOOP_SETUP=$Setup; $env:KENSHICOOP_BAKESAVE=$BakeSave
    $env:KENSHICOOP_TEST_SECONDS="$Seconds"; $env:KENSHICOOP_LOG=$log
    $env:KENSHICOOP_ARM_TIMEOUT_MS='1'; $env:KENSHICOOP_TRANSPORT='udp'; $env:KENSHICOOP_STEAM_PEER='0'
    $env:KENSHICOOP_SAVE_SYNC='0'; $env:KENSHICOOP_LOAD_SYNC='0'
    $out=Start-CoopOwnedGame -State $runState -ScriptDir $scriptDir -ExePath $hostExe -WorkDir $HostDir -TimeoutSec $StartTimeoutSec
    $line=@($out | Where-Object { "$_" -match '^GAMEPID=([1-9][0-9]*)$' })
    if ($line.Count -ne 1 -or "$($line[0])" -notmatch '^GAMEPID=([1-9][0-9]*)$') { throw 'Host failed native process handoff.' }
    $gamePid=[int]$Matches[1]; $process=$runState.Processes[$gamePid]
    $deadline=Get-CoopRunDeadline -Process $process -Seconds $Seconds -StartTimeoutSec $StartTimeoutSec -ArmTimeoutMs 1 -KillGraceSec 120
    $pattern="\[setup\] auto-bake save COMPLETED name='"+[regex]::Escape($BakeSave)+"' "
    if (-not(Wait-CoopOwnedLogLine -Process $process -Deadline $deadline -File $log -Pattern $pattern -TimeoutSec ($Seconds+$StartTimeoutSec+120))) { throw "No native completed bake; see $log" }
    $record=Read-CoopBakeCompletion (Get-Content -LiteralPath $log -Encoding UTF8) $BakeSave $bakedDir
    $before=@(Get-CoopSaveInventory $bakedDir)
    Wait-CoopOwnedExit -Process $process -Deadline $deadline
    $after=@(Get-CoopSaveInventory $bakedDir)
    if (($before -join "`n") -cne ($after -join "`n")) { throw 'Baked files changed after native completion.' }
    $files=@(Get-ChildItem -LiteralPath $bakedDir -Recurse -File -Force)
    if ($files.Count -ne $record.Files -or [long](($files | Measure-Object Length -Sum).Sum) -ne $record.Bytes) { throw 'Completed bake inventory disagrees with disk.' }
    if ($Promote) {
        $shell=(Get-Process -Id $PID).Path
        & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir 'capture_save.ps1') -Save $BakeSave -SaveRoot $saveRoot
        $captureExit=$LASTEXITCODE
        if ($captureExit -ne 0) { throw "Requested fixture promotion failed (exit $captureExit)." }
    }
    $complete=$true
    Write-Host "OK: completed bake at $bakedDir"
} finally {
    # Retain only the completed output; every failure restores its previous bytes.
    if ($complete) { $runState.Backups=@($runState.Backups | Where-Object { $_.Path -ine $bakedDir }) }
    Restore-CoopRunState $runState
}
