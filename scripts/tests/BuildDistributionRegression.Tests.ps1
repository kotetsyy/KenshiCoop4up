# Real filesystem failures against the production transaction; no game/build/network.
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'RuntimeDistribution.ps1')
$root=Join-Path ([IO.Path]::GetTempPath()) ('coop-builddistribution-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
function Require-Rejection([scriptblock]$Action,[string]$Message){$rejected=$false;try{& $Action}catch{$rejected=$true};if(-not $rejected){throw $Message}}
function Runtime-Hashes([string]$Directory){@(Get-ChildItem -LiteralPath $Directory -File -Recurse|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})}
$oldLocation=Get-Location;$oldCwd=[Environment]::CurrentDirectory
$script:fault='';$script:locks=@();$script:promotions=0
try {
    # A Windows PS5.1 child must hash real native inputs even when pwsh's
    # incompatible built-in modules precede its own in the inherited search path.
    $nativeHelper=Join-Path (Split-Path $PSScriptRoot -Parent) 'NativeBuild.ps1'
    . $nativeHelper -Configuration Harness
    $expectedDigest=Get-CoopDigest Native
    $ps7=Get-Command pwsh -ErrorAction SilentlyContinue
    if($null -ne $ps7){
        $savedModulePath=[Environment]::GetEnvironmentVariable('PSModulePath','Process')
        try{
            $env:PSModulePath=(Join-Path (Split-Path $ps7.Source -Parent) 'Modules')+';'+$savedModulePath
            $windowsPowerShell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $digestCommand=". '$($nativeHelper.Replace("'","''"))' -Configuration Harness; Get-CoopDigest Native"
            $actualDigest=@(& $windowsPowerShell -NoProfile -Command $digestCommand)
            if($LASTEXITCODE -ne 0 -or ($actualDigest -join "`n").Trim() -cne $expectedDigest){throw 'Inherited PS7 modules broke the Windows PS5.1 native source digest'}
        }finally{
            [Environment]::SetEnvironmentVariable('PSModulePath',$(if($null -eq $savedModulePath){[NullString]::Value}else{$savedModulePath}),'Process')
        }
    }else{Write-Host 'PS7 module-shadow regression not exercised: pwsh is unavailable'}
    if($null -ne $ps7){
        # Exercise actual production hashing over a disposable consumed-input
        # tree whose header names expose the NLS/ICU punctuation ordering split.
        $digestRoot=Join-Path $root 'digest-repo'
        foreach($dir in @('src\plugin','src\ui','src\netproto','src\transport','third_party\vc10_compat','third_party\KenshiLib_deps','scripts')){
            New-Item -ItemType Directory -Path (Join-Path $digestRoot $dir) -Force|Out-Null
        }
        Copy-Item -LiteralPath $nativeHelper -Destination (Join-Path $digestRoot 'scripts\NativeBuild.ps1')
        foreach($relative in @('scripts\build_plugin.cmd','scripts\v100_env.cmd','src\transport\TransportApi.h',
            'third_party\KenshiLib_deps\accumulators.hpp','third_party\KenshiLib_deps\accumulators_fwd.hpp')){
            [IO.File]::WriteAllText((Join-Path $digestRoot $relative),$relative)
        }
        $fixtureHelper=Join-Path $digestRoot 'scripts\NativeBuild.ps1'
        $digestCommand=". '$($fixtureHelper.Replace("'","''"))' -Configuration Harness; Get-CoopDigest Core"
        $windowsPowerShell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $windowsDigest=@(& $windowsPowerShell -NoProfile -Command $digestCommand)
        if($LASTEXITCODE -ne 0){throw 'Windows PS5.1 consumed-input hashing failed'}
        $modernDigest=@(& $ps7.Source -NoProfile -Command $digestCommand)
        if($LASTEXITCODE -ne 0 -or ($modernDigest -join "`n").Trim() -cne ($windowsDigest -join "`n").Trim()){
            throw 'The same consumed source tree has different receipts in PS5.1 and PS7'
        }
        [IO.File]::WriteAllText((Join-Path $digestRoot 'third_party\KenshiLib_deps\accumulators_fwd.hpp'),'changed consumed header')
        $changedDigest=@(& $windowsPowerShell -NoProfile -Command $digestCommand)
        if($LASTEXITCODE -ne 0 -or ($changedDigest -join "`n").Trim() -ceq ($windowsDigest -join "`n").Trim()){
            throw 'Changed consumed header did not invalidate the cross-engine source receipt'
        }
    }
    $files=@{}
    foreach($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')){
        $files[$name]=Join-Path $root $name
        [IO.File]::WriteAllText($files[$name],$(if($name -ceq 'RE_Kenshi.json'){'{"Plugins":["KenshiCoop.dll"]}'}else{"new-$name"}))
    }
    $location=Join-Path $root 'ps-location';New-Item -ItemType Directory -Path $location|Out-Null
    Set-Location $location;[Environment]::CurrentDirectory=$root
    New-CoopRuntimeKit $files 'deep\nested\kit.zip'
    Assert-CoopRuntimeZip 'deep\nested\kit.zip' $files
    if(-not(Test-Path (Join-Path $location 'deep\nested\kit.zip')) -or (Test-Path (Join-Path $root 'deep\nested\kit.zip'))){throw 'Archive ignored PowerShell location'}
    $zip=Join-Path $root 'replacement.zip';New-CoopRuntimeKit $files $zip
    $before=(Get-FileHash $zip).Hash
    $sourceLock=[IO.File]::Open($files['KenshiCoop.dll'],'Open','Read','None')
    try{Require-Rejection {New-CoopRuntimeKit $files $zip} 'Locked source accepted'}finally{$sourceLock.Dispose()}
    if((Get-FileHash $zip).Hash -cne $before -or (Test-Path "$zip.new")){throw 'Failed archive damaged old output or retained owned temporary'}
    New-CoopRuntimeKit $files $zip
    $outputLock=[IO.File]::Open($zip,'Open','Read','Read')
    try{Require-Rejection {New-CoopRuntimeKit $files $zip} 'Locked output accepted'}finally{$outputLock.Dispose()}
    if((Get-FileHash $zip).Hash -cne $before -or (Test-Path "$zip.new")){throw 'Failed promotion damaged output or retained temporary'}
    $zipAssertion=(Get-Command Assert-CoopRuntimeZip).ScriptBlock;$script:zipCleanupLock=$null
    function Assert-CoopRuntimeZip([string]$Zip,[hashtable]$Files){
        & $zipAssertion $Zip $Files
        if($Zip.EndsWith('.new')){$script:zipCleanupLock=[IO.File]::Open($Zip,'Open','Read','Read')}
    }
    try {
        $message='';try{New-CoopRuntimeKit $files $zip}catch{$message=$_.Exception.Message}
        if($message -notmatch 'Owned ZIP cleanup failed' -or -not(Test-Path "$zip.new")){throw 'Owned ZIP cleanup failure not reported/preserved'}
        if((Get-FileHash $zip).Hash -cne $before){throw 'Cleanup failure damaged prior ZIP'}
    }finally{if($null -ne $script:zipCleanupLock){$script:zipCleanupLock.Dispose()};Set-Item Function:\Assert-CoopRuntimeZip $zipAssertion}
    Remove-Item "$zip.new";New-CoopRuntimeKit $files $zip
    [IO.File]::WriteAllText("$zip.new",'foreign-pending')
    Require-Rejection {New-CoopRuntimeKit $files $zip} 'Foreign pending archive accepted'
    if([IO.File]::ReadAllText("$zip.new") -cne 'foreign-pending'){throw 'Foreign temporary deleted'}
    Remove-Item "$zip.new"
    $container=Join-Path $root 'container.zip';New-Item -ItemType Directory -Path $container|Out-Null
    [IO.File]::WriteAllText((Join-Path $container 'sentinel'),'keep')
    Require-Rejection {New-CoopRuntimeKit $files $container} 'Directory output accepted'
    if(@(Get-ChildItem $container).Count -ne 1){throw 'Container rejection wrote nested archive'}
    Set-Location $oldLocation;[Environment]::CurrentDirectory=$oldCwd
    $install=Join-Path $root 'install';$target=Join-Path $install 'mods\KenshiCoop'
    New-Item -ItemType Directory -Path $target -Force|Out-Null
    [IO.File]::WriteAllText((Join-Path $install 'kenshi_x64.exe'),'fixture-not-executable')
    foreach($name in $files.Keys){[IO.File]::WriteAllText((Join-Path $target $name),"old-$name")}
    foreach($name in @('coop_config.json','credentials.bin','user.save')){[IO.File]::WriteAllText((Join-Path $target $name),"keep-$name")}
    $baseline=Runtime-Hashes $target
    $fileShape=Join-Path $target 'KenshiCoopNetUDP.dll';$original=[IO.File]::ReadAllBytes($fileShape)
    Remove-Item $fileShape;New-Item -ItemType Directory -Path $fileShape|Out-Null
    [IO.File]::WriteAllText((Join-Path $fileShape 'sentinel'),'keep-shape')
    $shapeBefore=Runtime-Hashes $target
    Require-Rejection {Install-CoopRuntime $files @($install)} 'Runtime file directory accepted'
    if(Compare-Object $shapeBefore (Runtime-Hashes $target)){throw 'Shape rejection changed bytes'}
    Remove-Item $fileShape -Recurse;[IO.File]::WriteAllBytes($fileShape,$original)
    $externalFile=Join-Path $root 'external-runtime.dll';[IO.File]::WriteAllText($externalFile,'preserve-external-runtime')
    Remove-Item $fileShape
    & cmd.exe /d /c "mklink `"$fileShape`" `"$externalFile`""|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Cannot create disposable runtime file symlink'}
    try{
        Require-Rejection {Install-CoopRuntime $files @($install)} 'Runtime file reparse accepted'
        if([IO.File]::ReadAllText($externalFile) -cne 'preserve-external-runtime'){throw 'Runtime reparse rejection changed external bytes'}
    }finally{[IO.File]::Delete($fileShape);[IO.File]::WriteAllBytes($fileShape,$original)}
    # Interpose only to acquire actual incompatible filesystem handles after an
    # actual promotion. The failing copy/rollback/cleanup are production I/O.
    function Copy-Item {
        [CmdletBinding()]param([string]$LiteralPath,[string]$Destination,[switch]$Force)
        Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force -ErrorAction Stop
        if($script:fault -eq 'source' -and $Destination -match '[\\/]stage[\\/]' -and $LiteralPath -like '*KenshiCoop.dll'){
            [IO.File]::WriteAllText($files['RE_Kenshi.json'],'{"Plugins":["KenshiCoop.dll","KenshiCoopNetUDP.dll"]}')
        }
        if($script:fault -eq 'owner' -and $Destination -match '[\\/]backup-\d+$'){
            $marker=Join-Path $target '.kenshicoop-deploy.pending'
            if(-not(Test-Path $marker)){throw 'Destination backup preceded marker ownership'}
            $pointer=[IO.File]::ReadAllText($marker)
            Require-Rejection {Install-CoopRuntime $files @($install)} 'Concurrent deployer accepted'
            if([IO.File]::ReadAllText($marker) -cne $pointer){throw 'Concurrent deployer clobbered owner marker'}
        }
        if($LiteralPath -match '[\\/]stage[\\/]' -and $Destination.StartsWith($target,[StringComparison]::OrdinalIgnoreCase)){
            ++$script:promotions
            if($script:promotions -eq 1 -and $script:fault -in @('copy','rollback')){
                $next=Join-Path (Split-Path $LiteralPath -Parent) 'KenshiCoop.mod'
                $script:locks+= [IO.File]::Open($next,'Open','Read','None')
                if($script:fault -eq 'rollback'){$script:locks+= [IO.File]::Open($Destination,'Open','Read','Read')}
            }
            if($script:promotions -eq 6 -and $script:fault -eq 'cleanup'){
                $script:locks+= [IO.File]::Open((Join-Path $target '.kenshicoop-deploy.pending'),'Open','Read','Read')
            }
        }
    }
    foreach($mode in @('copy','rollback','cleanup')){
        $script:fault=$mode;$script:promotions=0
        Require-Rejection {Install-CoopRuntime $files @($install)} "Actual $mode failure accepted"
        if($script:promotions -lt 1){throw 'Failure did not occur after promotion'}
        foreach($handle in $script:locks){$handle.Dispose()};$script:locks=@()
        $marker=Join-Path $target '.kenshicoop-deploy.pending'
        if($mode -eq 'copy'){
            if(Compare-Object $baseline (Runtime-Hashes $target)){throw 'Actual failed promotion was not rolled back'}
            if(Test-Path $marker){throw 'Settled rollback retained marker'}
        }else{
            if(-not(Test-Path $marker)){throw 'Unsettled recovery lost marker'}
            $transaction=[IO.File]::ReadAllText($marker)
            $recovery=Get-Content (Join-Path $transaction 'recovery.json') -Raw|ConvertFrom-Json
            if(@($recovery.entries).Count -ne 6){throw 'Unsettled recovery lost manifest'}
            foreach($entry in $recovery.entries){if($entry.Existed){if(-not(Test-Path $entry.Backup) -or (Get-FileHash $entry.Backup).Hash -cne $entry.Hash){throw 'Recovery backup missing/corrupt'};Microsoft.PowerShell.Management\Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force}}
            Remove-Item $marker;Remove-Item $transaction -Recurse -Force
            if(Compare-Object $baseline (Runtime-Hashes $target)){throw 'Unrelated bytes changed during unsettled recovery'}
        }
    }
    $script:fault='source';$script:promotions=0
    Require-Rejection {Install-CoopRuntime $files @($install)} 'Post-validation loader mutation accepted'
    if($script:promotions -ne 0 -or (Compare-Object $baseline (Runtime-Hashes $target))){throw 'Source mutation changed installation'}
    [IO.File]::WriteAllText($files['RE_Kenshi.json'],'{"Plugins":["KenshiCoop.dll"]}')
    $script:fault='owner';$script:promotions=0
    Install-CoopRuntime $files @($install)
    Remove-Item Function:\Copy-Item
    Install-CoopRuntime $files @($install)
    foreach($name in $files.Keys){if((Get-FileHash (Join-Path $target $name)).Hash -cne (Get-FileHash $files[$name]).Hash){throw "Successful deployment mismatch: $name"}}
    $fresh=Join-Path $root 'fresh-install';New-Item -ItemType Directory -Path $fresh|Out-Null
    [IO.File]::WriteAllText((Join-Path $fresh 'kenshi_x64.exe'),'fixture')
    Install-CoopRuntime $files @($fresh)
    foreach($name in $files.Keys){if((Get-FileHash (Join-Path $fresh "mods\KenshiCoop\$name")).Hash -cne (Get-FileHash $files[$name]).Hash){throw "Fresh-target deployment mismatch: $name"}}
    $alias=Join-Path $root 'alias'; & cmd.exe /d /c "mklink /J `"$alias`" `"$install`""|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Cannot create disposable junction'}
    try{Require-Rejection {Install-CoopRuntime $files @($alias)} 'Reparse installation accepted'}finally{[IO.Directory]::Delete($alias)}
    $ancestorAlias=Join-Path $root 'ancestor-alias'; & cmd.exe /d /c "mklink /J `"$ancestorAlias`" `"$root`""|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Cannot create disposable ancestor junction'}
    try{Require-Rejection {Install-CoopRuntime $files @((Join-Path $ancestorAlias 'install'))} 'Reparse installation ancestor accepted'}finally{[IO.Directory]::Delete($ancestorAlias)}
    Write-Host 'BuildDistribution filesystem regression fixtures PASS (not native build/gameplay proof)'
}finally{
    foreach($handle in $script:locks){$handle.Dispose()}
    if(Test-Path Function:\Copy-Item){Remove-Item Function:\Copy-Item}
    Set-Location $oldLocation;[Environment]::CurrentDirectory=$oldCwd
    Remove-Item $root -Recurse -Force
}
