[CmdletBinding()]
param([string]$OracleRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $OracleRoot) { $OracleRoot = Split-Path -Parent $PSScriptRoot }
. (Join-Path $OracleRoot 'kit_preflight.ps1')
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $OracleRoot 'manual_session.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Manual environment fixture requires valid PowerShell.' }
$function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Set-CoopEnv' }, $true)
if (-not $function) { throw 'Manual environment boundary is missing.' }
. ([scriptblock]::Create($function.Extent.Text))
$root = Join-Path $env:LOCALAPPDATA ('KenshiCoop\private\session-fixture-' + [Guid]::NewGuid().ToString('N'))
$original = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'KENSHICOOP_*' } | ForEach-Object { $original[$_.Name] = $_.Value }
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $ownedDirs = @((Join-Path $root 'game'))
    $runState = @{ Environment=@{} }
    $dest = Join-Path $root 'artifacts'
    New-Item -ItemType Directory -Path $dest | Out-Null
    $TitleScreen=$false; $JoinFromMenu=$false; $Port=27800; $Ip='127.0.0.1'; $SetupScene=''; $AutoRecruit=0
    $ProbeRecruit=$false; $ProbeAiSuspend=$false; $InvSync=$false; $WorldSync=$false; $InvDump=$false
    $DebugMarkers=$false; $NoCellAuth=$false; $NoCollapse=$false; $TrackMove=$false; $DebugShackle=$false
    foreach ($mode in @('host','join')) {
        foreach ($probe in @($false,$true)) {
            $JailProbe=$probe; $JailObserve=$false
            $env:KENSHICOOP_TEST_CRASH='1'; $env:KENSHICOOP_BAKESAVE='user-save'; $env:KENSHICOOP_OWN_RANK='9'; $env:KENSHICOOP_JAIL_OBSERVE='1'
            Set-CoopEnv -Mode $mode -SaveName 'fixture' -Own $(if ($mode -eq 'host') { '0' } else { '1' })
            foreach ($key in @('KENSHICOOP_TEST_CRASH','KENSHICOOP_BAKESAVE','KENSHICOOP_OWN_RANK')) {
                if ($null -ne [Environment]::GetEnvironmentVariable($key,'Process')) { throw "Ambient control survived: $key" }
            }
            if ($env:KENSHICOOP_JAIL_OBSERVE -ne '0') { throw 'Plain jail probe enabled observation.' }
            if ($env:KENSHICOOP_JAIL_PROBE -ne "$([int]$probe)" -or $env:KENSHICOOP_TASK_SPIKE -ne "$([int]$probe)") { throw 'Jail probe flags differ from request.' }
            if ($null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_INV_SYNC','Process') -or $null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_WORLD_SYNC','Process')) { throw 'Free-play automatic channel defaults were overridden.' }
            if ($env:KENSHICOOP_LOG -ne (Join-Path $dest "$mode.log") -or -not [IO.Path]::IsPathRooted($env:KENSHICOOP_LOG)) { throw 'Log escaped the current-run artifact directory.' }
            if ($env:KENSHICOOP_OWN_SQUAD -ne $(if ($mode -eq 'host') { '0' } else { '1' })) { throw 'Squad-tab ownership was not applied.' }
        }
    }
    $JailProbe=$true; $JailObserve=$true
    Set-CoopEnv -Mode host -SaveName fixture -Own 0
    if ($env:KENSHICOOP_JAIL_OBSERVE -ne '1') { throw 'Explicit observation was not enabled.' }
    $TitleScreen=$true
    Set-CoopEnv -Mode host -SaveName '' -Own 0
    if ($env:KENSHICOOP_AUTOCONNECT -ne '0') { throw 'Title screen unexpectedly connected.' }
    $TitleScreen=$false; $JoinFromMenu=$true
    Set-CoopEnv -Mode join -SaveName '' -Own 1
    if ($env:KENSHICOOP_AUTOCONNECT -ne '0') { throw 'Join menu unexpectedly connected.' }
    # The manual sync transaction owns only its destination fixture, not every
    # save produced during free play.
    $hostDir = Join-Path $root 'host'
    $joinDir = Join-Path $root 'join'
    $sourceSave = Join-Path $hostDir 'save\fixture'
    $destinationSave = Join-Path $joinDir 'save\fixture'
    $unrelatedSave = Join-Path $joinDir 'save\unrelated'
    New-Item -ItemType Directory -Path $sourceSave,$destinationSave,$unrelatedSave | Out-Null
    [IO.File]::WriteAllText((Join-Path $hostDir 'settings.cfg'),'User save location=0')
    [IO.File]::WriteAllText((Join-Path $joinDir 'settings.cfg'),'User save location=1')
    [IO.File]::WriteAllText((Join-Path $sourceSave 'quick.save'),'source fixture')
    [IO.File]::WriteAllText((Join-Path $destinationSave 'quick.save'),'original user fixture')
    $friendAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $OracleRoot 'friend_join.ps1'), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Shared fixture boundary requires valid PowerShell.' }
    $sharedGuard = $friendAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-CoopSharedFixture' }, $true)
    if (-not $sharedGuard) { throw 'Shared fixture ownership boundary is missing.' }
    . ([scriptblock]::Create($sharedGuard.Extent.Text))
    $rejected = $false
    try { Assert-CoopSharedFixture -KitSave $sourceSave -SharedSave $destinationSave } catch { $rejected = $true }
    if (-not $rejected) { throw 'Different shared fixture bytes were accepted.' }
    [IO.File]::WriteAllText((Join-Path $destinationSave 'quick.save'),'source fixture')
    Assert-CoopSharedFixture -KitSave $sourceSave -SharedSave $destinationSave
    [IO.File]::WriteAllText((Join-Path $destinationSave 'unowned-extra.save'),'source fixture')
    $rejected = $false
    try { Assert-CoopSharedFixture -KitSave $sourceSave -SharedSave $destinationSave } catch { $rejected = $true }
    if (-not $rejected) { throw 'Different shared fixture file identity was accepted.' }
    Remove-Item -LiteralPath (Join-Path $destinationSave 'unowned-extra.save')
    [IO.File]::WriteAllText((Join-Path $destinationSave 'quick.save'),'original user fixture')
    [IO.File]::WriteAllText((Join-Path $unrelatedSave 'quick.save'),'unrelated user save')
    $backupRoot = Join-Path $root 'backup'
    New-Item -ItemType Directory -Path $backupRoot | Out-Null
    $state = @{Root=$backupRoot; Backups=@(); Processes=@{}; Environment=$original}
    Add-CoopRunBackup -State $state -Path $destinationSave
    Add-CoopRunBackup -State $state -Path (Join-Path $joinDir 'settings.cfg')
    Sync-CoopNamedSave -Save fixture -HostDir $hostDir -JoinDir $joinDir
    if ([IO.File]::ReadAllText((Join-Path $destinationSave 'quick.save')) -ne 'source fixture') { throw 'Named sync did not use the host native save location.' }
    if ([IO.File]::ReadAllText((Join-Path $joinDir 'settings.cfg')) -notmatch 'User save location\s*=\s*0') { throw 'Native join save lookup ignores the explicitly synced fixture.' }
    $freshSave = Join-Path $joinDir 'save\new-free-play'
    New-Item -ItemType Directory -Path $freshSave | Out-Null
    [IO.File]::WriteAllText((Join-Path $freshSave 'quick.save'),'new free-play output')
    Restore-CoopRunState $state
    if ([IO.File]::ReadAllText((Join-Path $joinDir 'settings.cfg')) -ne 'User save location=1') { throw 'Native save-location setting was not restored exactly.' }
    if ([IO.File]::ReadAllText((Join-Path $destinationSave 'quick.save')) -ne 'original user fixture') { throw 'Sync clobbered the preexisting destination fixture.' }
    if ([IO.File]::ReadAllText((Join-Path $unrelatedSave 'quick.save')) -ne 'unrelated user save') { throw 'Sync restoration touched an unrelated save.' }
    if ([IO.File]::ReadAllText((Join-Path $freshSave 'quick.save')) -ne 'new free-play output') { throw 'Restoration deleted new free-play output.' }
    # Exercise the kit JOIN's real preparation block with conflicting save roots.
    # No game is launched: settings select the private fixture we inspect.
    $fixtureInstall = $friendAst.Find({
        param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.ElseClause -and $node.ElseClause.Extent.Text -match '\$kitSave\s*='
    }, $true)
    if (-not $fixtureInstall) { throw 'Kit fixture installation boundary is missing.' }
    $prepareFixture = [scriptblock]::Create(($fixtureInstall.ElseClause.Statements.Extent.Text -join "`n"))
    $kitDir = Join-Path $root 'fixture-kit'
    $save = 'fixture'
    $kitSavePath = Join-Path $kitDir "save\$save"
    $saveRoot = Join-Path $root 'isolated-appdata-save'
    New-Item -ItemType Directory -Path $kitSavePath,$saveRoot -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $kitSavePath 'quick.save'),'exact packaged bytes')
    $KenshiDir = $joinDir
    foreach ($location in @(0,1)) {
        foreach ($existingDestination in @($false,$true)) {
            foreach ($withPeer in @($false,$true)) {
                $OwnedPeerTokens = if ($withPeer) { @('fixture-incarnation') } else { @() }
                $shared = Join-Path $saveRoot $save
                New-Item -ItemType Directory -Path $shared -Force | Out-Null
                # With an exempt parent, shared fixture bytes already match and
                # must not be replaced or restored by the child.
                [IO.File]::WriteAllText((Join-Path $shared 'quick.save'),$(if ($withPeer) { 'exact packaged bytes' } else { 'old AppData bytes' }))
                $sharedHash = (Get-FileHash -LiteralPath (Join-Path $shared 'quick.save')).Hash
                if (Test-Path -LiteralPath $destinationSave) { Remove-Item -LiteralPath $destinationSave -Recurse -Force }
                if ($existingDestination) {
                    New-Item -ItemType Directory -Path $destinationSave | Out-Null
                    [IO.File]::WriteAllText((Join-Path $destinationSave 'quick.save'),'old installation bytes')
                }
                $settingsPath = Join-Path $joinDir 'settings.cfg'
                [IO.File]::WriteAllBytes($settingsPath,[Text.Encoding]::GetEncoding(28591).GetBytes("User save location=$location`r`nOther=caf$([char]233)`r`n"))
                $settingsHash = (Get-FileHash -LiteralPath $settingsPath).Hash
                $backup = Join-Path $root ([Guid]::NewGuid().ToString('N'))
                New-Item -ItemType Directory -Path $backup | Out-Null
                $fixtureState = @{Root=$backup;Backups=@();Processes=@{};Environment=$original}
                Add-CoopRunBackup -State $fixtureState -Path $destinationSave
                Add-CoopRunBackup -State $fixtureState -Path $settingsPath
                if (-not $withPeer) { Add-CoopRunBackup -State $fixtureState -Path $shared }
                try {
                    . $prepareFixture
                    if ([IO.File]::ReadAllText((Join-Path $destinationSave 'quick.save')) -cne 'exact packaged bytes') { throw 'JOIN selected a stale installation fixture.' }
                    if ([IO.File]::ReadAllText($settingsPath) -notmatch 'User save location=0') { throw 'JOIN did not select its private native root.' }
                    if ($withPeer -and (Get-FileHash -LiteralPath (Join-Path $shared 'quick.save')).Hash -ne $sharedHash) { throw 'Exempt peer shared fixture was mutated.' }
                } finally { Restore-CoopRunState $fixtureState }
                if ((Get-FileHash -LiteralPath $settingsPath).Hash -ne $settingsHash) { throw 'Kit save selection did not restore exact settings bytes.' }
                if ($existingDestination) {
                    if ([IO.File]::ReadAllText((Join-Path $destinationSave 'quick.save')) -cne 'old installation bytes') { throw 'Preexisting installation fixture was not restored.' }
                } elseif (Test-Path -LiteralPath $destinationSave) { throw 'Initially absent installation fixture survived restoration.' }
                if ((Get-FileHash -LiteralPath (Join-Path $shared 'quick.save')).Hash -ne $sharedHash) { throw 'Shared fixture bytes were not preserved/restored.' }
                if ([IO.File]::ReadAllText((Join-Path $unrelatedSave 'quick.save')) -cne 'unrelated user save') { throw 'Kit preparation touched an unrelated save.' }
            }
        }
    }
    # Pure deadline arithmetic uses an actual process incarnation, never a
    # second Now+budget. Timeout proof terminates only a disposable owned shell.
    $current = [Diagnostics.Process]::GetCurrentProcess()
    $deadline = Get-CoopRunDeadline -Process $current -Seconds 650 -StartTimeoutSec 240 -ArmTimeoutMs 240000 -KillGraceSec 620
    if (($deadline-$current.StartTime.ToUniversalTime()).TotalSeconds -ne 1750) { throw 'Scenario budget omitted startup, arming, duration or grace.' }
    $shell = (Get-Process -Id $PID).Path
    $owned = Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-NonInteractive','-Command','Start-Sleep -Seconds 60') -WindowStyle Hidden -PassThru
    try {
        $null = $owned.Handle
        $rejected = $false
        try { Wait-CoopOwnedExit -Process $owned -Deadline ([DateTime]::UtcNow.AddSeconds(-1)) } catch { $rejected = $true }
        if (-not $rejected -or -not $owned.HasExited) { throw 'Forced termination was reported as success or left the owned shell alive.' }
        $log = Join-Path $root 'final.log'
        [IO.File]::WriteAllText($log,'final current-run evidence')
        if (-not (Wait-CoopOwnedLogLine -Process $owned -Deadline ([DateTime]::UtcNow) -File $log -Pattern 'current-run evidence' -TimeoutSec 60)) { throw 'Final log evidence from the held exited process was discarded.' }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        if (Wait-CoopOwnedLogLine -Process $owned -Deadline ([DateTime]::UtcNow.AddSeconds(60)) -File (Join-Path $root 'missing.log') -Pattern '.' -TimeoutSec 60) { throw 'Missing log was reported as evidence.' }
        if ($watch.Elapsed.TotalSeconds -gt 5) { throw 'Log observation waited after the owned process exited.' }
    } finally {
        if (-not $owned.HasExited) { $owned.Kill(); $owned.WaitForExit() }
        $owned.Dispose()
        $current.Dispose()
    }
    Write-Host 'PASS: session environment isolation, opt-in diagnostics, native save sync/restoration, exact kit fixture/private lookup with peer preservation, free-play preservation, exact process deadline and forced-timeout failure.'
} finally {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'KENSHICOOP_*' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,[NullString]::Value,'Process') }
    foreach ($key in $original.Keys) { [Environment]::SetEnvironmentVariable($key,$original[$key],'Process') }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
