# Disposable behavioral checks only; no game/provider/test process is launched.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'kit_preflight.ps1')
function Assert-Throws([scriptblock]$Action,[string]$Name) {
    $threw=$false
    try { & $Action } catch { $threw=$true }
    if(-not $threw){throw "Missing rejection: $Name"}
}
$root=Join-Path $env:TEMP ('KenshiCoop-private-harness-check-'+[Guid]::NewGuid().ToString('N'))
$mod=Join-Path $root 'mod'
New-Item -ItemType Directory -Path $mod -Force | Out-Null
try {
    $names=@('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')
    foreach($name in $names){[IO.File]::WriteAllText((Join-Path $mod $name),'fixture-runtime-bytes')}
    [IO.File]::WriteAllText((Join-Path $mod 'RE_Kenshi.json'),'{"Plugins":["KenshiCoop.dll"]}')
    Assert-CoopRuntime $mod
    # Removing any runtime component, including UI/each provider, must fail.
    foreach($name in $names){
        $path=Join-Path $mod $name
        $bytes=[IO.File]::ReadAllBytes($path)
        Remove-Item -LiteralPath $path
        Assert-Throws {Assert-CoopRuntime $mod} "missing $name"
        [IO.File]::WriteAllBytes($path,$bytes)
    }
    [IO.File]::WriteAllText((Join-Path $mod 'RE_Kenshi.json'),'{"Plugins":["KenshiCoop.dll","KenshiCoopNetUDP.dll"]}')
    Assert-Throws {Assert-CoopRuntime $mod} 'direct provider loader entry'
    $game=Join-Path $root 'copied-game'
    New-Item -ItemType Directory -Path $game | Out-Null
    $missingJoin=Join-Path $root 'missing-join'
    $ownedDirs=@(Get-CoopManualGameDirs -HostDir $game -JoinDir $missingJoin -NoJoin $true)
    if($ownedDirs.Count -ne 1 -or $ownedDirs[0] -cne $game -or (Test-Path -LiteralPath $missingJoin)){throw 'Host-only ownership included or mutated the absent join tree'}
    # The actual restore helper must put back exact config bytes and remove only
    # the absent-before owned fixture; an unrelated sibling save stays unchanged.
    $backupRoot=Join-Path $root 'backup'
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $config=Join-Path $root 'kenshi.cfg'
    $backup=Join-Path $backupRoot 'config'
    [IO.File]::WriteAllBytes($backup,[byte[]]@(0,255,13,10,83,69,84))
    [IO.File]::WriteAllText($config,'modified display')
    $fixture=Join-Path $root 'owned-fixture'
    $unrelated=Join-Path $root 'unrelated-user-save'
    New-Item -ItemType Directory -Path $fixture,$unrelated | Out-Null
    [IO.File]::WriteAllText((Join-Path $unrelated 'quick.save'),'user-owned')
    $environment=@{}
    Get-ChildItem Env: | Where-Object {$_.Name -like 'KENSHICOOP_*'} | ForEach-Object {$environment[$_.Name]=$_.Value}
    $state=@{Root=$backupRoot;Processes=@{};Environment=$environment;Backups=@(
        @{Path=$config;Backup=$backup;Existed=$true},
        @{Path=$fixture;Backup=(Join-Path $backupRoot 'absent');Existed=$false})}
    Restore-CoopRunState $state
    if([Convert]::ToBase64String([IO.File]::ReadAllBytes($config)) -cne 'AP8NClNFVA=='){throw 'Exact config bytes were not restored'}
    if(Test-Path -LiteralPath $fixture){throw 'Owned absent-before fixture was retained'}
    if([IO.File]::ReadAllText((Join-Path $unrelated 'quick.save')) -cne 'user-owned'){throw 'Unrelated user save was modified'}
    # A real unassociated Process throws on HasExited; later cleanup still runs.
    $backupRoot=Join-Path $root 'cleanup-backup'
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $backup=Join-Path $backupRoot 'config'
    [IO.File]::WriteAllText($backup,'original-after-proxy-error')
    [IO.File]::WriteAllText($config,'changed-before-proxy-error')
    $state=@{Root=$backupRoot;Processes=@{};Environment=$environment;Backups=@(@{Path=$config;Backup=$backup;Existed=$true})}
    $unassociatedProxy=New-Object Diagnostics.Process
    try { Assert-Throws {Restore-CoopHarnessSession -State $state -Proxy $unassociatedProxy} 'real proxy object cleanup error' }
    finally {$unassociatedProxy.Dispose()}
    if([IO.File]::ReadAllText($config) -cne 'original-after-proxy-error' -or (Test-Path -LiteralPath $backupRoot)){throw 'Proxy error skipped config restoration or backup cleanup'}
    Write-Output 'Private harness preflight/restoration behavior passed; NOT gameplay/Steam certification.'
} finally {
    if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
