# Primary-repository behavioral checks. Disposable files/processes only; no Kenshi,
# desktop input, router/firewall/registry writes, winget, build or publication.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$scripts=Split-Path -Parent $PSScriptRoot
. (Join-Path $scripts 'kit_preflight.ps1')
. (Join-Path $scripts 'capture_save.ps1')
Import-Module (Join-Path $scripts 'CoopHarness.psm1') -Force
function Require($Value,[string]$Message){if(-not $Value){throw $Message}}
function Reject([scriptblock]$Action,[string]$Message){$rejected=$false;try{& $Action}catch{$rejected=$true};Require $rejected $Message}
$root=Join-Path ([IO.Path]::GetTempPath()) ('CoopMainAudit-'+[guid]::NewGuid().ToString('N'))
$environment=@{}
Get-ChildItem Env:|Where-Object {$_.Name -like 'KENSHICOOP_*'}|ForEach-Object {$environment[$_.Name]=$_.Value}
[void][IO.Directory]::CreateDirectory($root)
try {
    $env:KENSHICOOP_CELL_AUTH='1';$env:KENSHICOOP_JAIL_OBSERVE='1'
    [void](Set-CoopDiagEnv $null)
    Require (-not [Environment]::GetEnvironmentVariables('Process').Contains('KENSHICOOP_CELL_AUTH')) 'Free play retained an authority override'
    Require (-not [Environment]::GetEnvironmentVariables('Process').Contains('KENSHICOOP_JAIL_OBSERVE')) 'Stale NPC-observer flag survived free-play cleanup'
    [void](Set-CoopDiagEnv @{})
    Require ($env:KENSHICOOP_CELL_AUTH -ceq '0') 'Scripted scenario lost its isolated baseline'
    [void](Set-CoopDiagEnv @{DiagEnv=@{KENSHICOOP_CELL_AUTH='1'}})
    Require ($env:KENSHICOOP_CELL_AUTH -ceq '1') 'Explicit scenario override lost precedence'
    $manifest=Import-PowerShellDataFile (Join-Path $scripts 'scenarios.psd1')
    [void](Set-CoopDiagEnv $manifest.Scenarios.environment_items)
    Require ($env:KENSHICOOP_ENVIRONMENT_ITEMS -ceq '1') 'Environment-item probe could not opt into native reconciliation'
    [void](Set-CoopDiagEnv $null)
    Require (-not [Environment]::GetEnvironmentVariables('Process').Contains('KENSHICOOP_ENVIRONMENT_ITEMS')) 'Environment-item reconciliation leaked from a probe into free play'
    $state=@{Environment=$environment.Clone()}
    Clear-CoopRunEnvironment -State $state
    Require (-not [Environment]::GetEnvironmentVariables('Process').Contains('KENSHICOOP_CELL_AUTH')) 'Shared run cleanup retained an empty variable'
    $game=Join-Path $root 'game[fixture]';[void][IO.Directory]::CreateDirectory($game)
    $encoding=New-Object Text.UTF8Encoding($true)
    $cfg=Join-Path $game 'kenshi.cfg'
    $before="Other=caf$([char]233)`r`nVideo Mode=1 x 2 @ 32-bit colour [0]`r`nFull Screen=Yes`r`nTail=unchanged"
    $after="Other=caf$([char]233)`r`nVideo Mode=640 x 480 @ 32-bit colour [0]`r`nFull Screen=No`r`nTail=unchanged"
    [IO.File]::WriteAllBytes($cfg,[byte[]]($encoding.GetPreamble()+$encoding.GetBytes($before)))
    & (Join-Path $scripts 'set_video_mode.ps1') -HostDir $game -JoinDir $game -Width 640 -Height 480
    Require ([Convert]::ToBase64String([IO.File]::ReadAllBytes($cfg)) -ceq [Convert]::ToBase64String([byte[]]($encoding.GetPreamble()+$encoding.GetBytes($after)))) 'Video setter damaged unrelated bytes/BOM/CRLF'
    $settings=Join-Path $game 'settings.cfg'
    [IO.File]::WriteAllText($settings,"User save location=0`r`nOther=unchanged")
    Require ((Resolve-CoopSelectedSaveRoot $game) -eq (Join-Path $game 'save')) 'Installation-local save selection ignored'
    [IO.File]::WriteAllText($settings,"User save location=1`r`nUser save location=0")
    Reject {Resolve-CoopSelectedSaveRoot $game} 'Ambiguous save setting accepted'
    $source=Join-Path $root 'source';$destination=Join-Path $root 'destination'
    [void][IO.Directory]::CreateDirectory($source);[void][IO.Directory]::CreateDirectory($destination)
    [IO.File]::WriteAllText((Join-Path $source 'quick.save'),'new-save')
    [IO.File]::WriteAllText((Join-Path $destination 'quick.save'),'old-save')
    $lock=[IO.File]::Open((Join-Path $source 'quick.save'),'Open','ReadWrite','None')
    try {Reject {Copy-CoopSaveTransaction $source $destination} 'Unreadable save source accepted'}finally{$lock.Dispose()}
    Require ([IO.File]::ReadAllText((Join-Path $destination 'quick.save')) -ceq 'old-save') 'Failed staging destroyed the original save'
    $fileDestination=Join-Path $root 'file-destination';[IO.File]::WriteAllText($fileDestination,'keep-file')
    Reject {Copy-CoopSaveTransaction $source $fileDestination} 'File destination accepted as a save directory'
    Require ([IO.File]::ReadAllText($fileDestination) -ceq 'keep-file') 'Rejected destination file was modified'
    Copy-CoopSaveTransaction $source $destination
    Require ([IO.File]::ReadAllText((Join-Path $destination 'quick.save')) -ceq 'new-save') 'Complete save promotion failed'
    Reject {Copy-CoopSaveTransaction $source (Join-Path $source 'child')} 'Overlapping save directories accepted'
    $live=Join-Path $root 'restore.cfg';[IO.File]::WriteAllText($live,'before')
    $backupRoot=Join-Path $root 'backup';[void][IO.Directory]::CreateDirectory($backupRoot)
    $snapshot=@{Root=$backupRoot;Backups=@();Processes=@{};Environment=$environment.Clone()}
    Add-CoopRunBackup $snapshot $live
    [IO.File]::WriteAllText($live,'recoverable-current')
    [IO.File]::Delete($snapshot.Backups[0].Backup)
    Reject {Restore-CoopRunState $snapshot} 'Missing restoration backup accepted'
    Require ([IO.File]::ReadAllText($live) -ceq 'recoverable-current') 'Missing backup caused destructive destination deletion'
    Require ([IO.Directory]::Exists($backupRoot)) 'Failed restoration discarded its recovery evidence'
    Write-Output 'Primary audit integration boundaries PASS; no gameplay certification.'
} finally {
    Get-ChildItem Env:|Where-Object {$_.Name -like 'KENSHICOOP_*'}|ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,[NullString]::Value,'Process')}
    foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,[string]$environment[$name],'Process')}
    if([IO.Directory]::Exists($root)){[IO.Directory]::Delete($root,$true)}
}
