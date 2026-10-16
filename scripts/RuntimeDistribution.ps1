# Production kit/deploy operations; disposable tests call these same functions.
[CmdletBinding()]
param([switch]$Deploy,[string]$Kenshi='', [ValidateSet('Release','Harness','Debug')][string]$Configuration='Release',[string]$NativeBin='', [switch]$Mirror)
function Assert-CoopRuntimeFiles([hashtable]$Files) {
    $required=@('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')
    if($Files.Count -ne 6 -or (Compare-Object $required @($Files.Keys) -CaseSensitive)){throw 'Runtime must contain exactly the six core/UI/providers/mod/JSON files.'}
    foreach($name in $required){if(-not(Test-Path -LiteralPath $Files[$name] -PathType Leaf)){throw "Runtime input missing: $name"}}
    $loader=Get-Content -LiteralPath $Files['RE_Kenshi.json'] -Raw|ConvertFrom-Json
    if(@($loader.Plugins).Count -ne 1 -or $loader.Plugins[0] -cne 'KenshiCoop.dll'){throw 'RE_Kenshi.json Plugins must list only KenshiCoop.dll.'}
}
function Get-CoopRuntimeHashes([hashtable]$Files) {
    $pins=@{};$validated=$Files.PSObject.Properties['CoopValidatedHashes']
    foreach($name in $Files.Keys){$pins[$name]=(Get-FileHash -LiteralPath $Files[$name] -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()}
    if($null -ne $validated){foreach($name in $Files.Keys){if($pins[$name] -cne $validated.Value[$name]){throw "Receipt-validated runtime changed: $name"}}}
    Assert-CoopRuntimeFiles $Files
    foreach($name in $Files.Keys){if((Get-FileHash -LiteralPath $Files[$name] -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() -cne $pins[$name]){throw "Runtime changed during validation: $name"}}
    return $pins
}
function Assert-CoopDirectoryAncestors([string]$Path) {
    $path=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    while(-not(Test-Path -LiteralPath $path)){$path=Split-Path $path -Parent;if(-not $path){throw 'No filesystem ancestor'}}
    $item=Get-Item -LiteralPath $path -Force
    while($null -ne $item){if($item -isnot [IO.DirectoryInfo] -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unsafe runtime ancestor: $($item.FullName)"};$item=$item.Parent}
}
function Get-CoopRuntimeFiles([string]$Configuration,[string]$NativeBin='') {
    . (Join-Path $PSScriptRoot 'NativeBuild.ps1') -Configuration $Configuration
    $core=Get-CoreBuild -Configuration $Configuration; $native=Get-NativeBuild -Configuration $Configuration -NativeBin $NativeBin
    $files=@{}; foreach($name in $core.Files.Keys){$files[$name]=$core.Files[$name]}
    foreach($name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')){$files[$name]=$native.Files[$name]}
    foreach($name in @('KenshiCoop.mod','RE_Kenshi.json')){$files[$name]=Join-Path $script:NativeRepo "dist\mods\KenshiCoop\$name"}
    $pins=Get-CoopRuntimeHashes $files
    foreach($name in $core.Files.Keys){if($pins[$name] -cne $core.Receipt.files.$name){throw "Core changed after receipt validation: $name"}}
    foreach($name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')){if($pins[$name] -cne $native.Receipt.files.$name){throw "Provider changed after receipt validation: $name"}}
    Add-Member -InputObject $files -MemberType NoteProperty -Name CoopValidatedHashes -Value $pins
    return $files
}
function Assert-CoopRuntimeZip([string]$Zip,[hashtable]$Files) {
    $pins=Get-CoopRuntimeHashes $Files
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Zip))
    try {
        $required=@($Files.Keys|ForEach-Object{"KenshiCoop/$_"})
        if($archive.Entries.Count -ne 6 -or (Compare-Object $required @($archive.Entries|ForEach-Object{$_.FullName}) -CaseSensitive)){throw 'ZIP must contain exactly six runtime files, no secrets/config/source/provenance/extras.'}
        foreach($name in $Files.Keys) {
            $stream=$archive.GetEntry("KenshiCoop/$name").Open();$hash=[Security.Cryptography.SHA256]::Create()
            try {$actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}finally{$stream.Dispose();$hash.Dispose()}
            if($actual -cne $pins[$name]){throw "Stale ZIP entry: $name"}
        }
    }finally{$archive.Dispose()}
}
function New-CoopRuntimeKit([hashtable]$Files,[string]$Zip) {
    $ErrorActionPreference='Stop'
    $pins=Get-CoopRuntimeHashes $Files
    $zip=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Zip);$parent=Split-Path $zip -Parent
    Assert-CoopDirectoryAncestors $parent
    if(Test-Path -LiteralPath $zip -PathType Container){throw "ZIP destination is a directory: $zip"}
    $temporary="$zip.new"
    if(Test-Path -LiteralPath $temporary){throw "Pending ZIP needs inspection: $temporary"}
    New-Item -ItemType Directory -Force -Path $parent|Out-Null
    Add-Type -AssemblyName System.IO.Compression,System.IO.Compression.FileSystem
    $owned=$false;$stream=$null;$archive=$null;$failure=$null
    try {
        $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$owned=$true
        $archive=New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
        foreach($name in $Files.Keys){[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$Files[$name],"KenshiCoop/$name",[IO.Compression.CompressionLevel]::Optimal)}
        $archive.Dispose();$archive=$null;$stream.Dispose();$stream=$null
        foreach($name in $Files.Keys){if((Get-FileHash $Files[$name]).Hash.ToLowerInvariant() -cne $pins[$name]){throw "Runtime changed while packaging: $name"}}
        Assert-CoopRuntimeZip -Zip $temporary -Files $Files
        if(Test-Path -LiteralPath $zip){[IO.File]::Replace($temporary,$zip,[NullString]::Value)}else{[IO.File]::Move($temporary,$zip)}
        $owned=$false
        Assert-CoopRuntimeZip -Zip $zip -Files $Files
    } catch {$failure=$_;throw} finally {
        if($null -ne $archive){$archive.Dispose()}
        if($null -ne $stream){$stream.Dispose()}
        if($owned -and (Test-Path -LiteralPath $temporary)){try{Remove-Item -LiteralPath $temporary -Force -ErrorAction Stop}catch{throw "ZIP failed: $failure. Owned ZIP cleanup failed; inspect $temporary. $($_.Exception.Message)"}}
    }
}
function Install-CoopRuntime([hashtable]$Files,[string[]]$Installations) {
    $ErrorActionPreference='Stop'
    $hashes=Get-CoopRuntimeHashes $Files
    if(-not $Installations.Count){throw 'No explicit installation.'}
    $targets=@()
    $assertIdle={
        param([string]$Install)
        $executables=@((Join-Path $Install 'kenshi_x64.exe'),(Join-Path $Install 'RE_Kenshi\kenshi_x64.exe'))
        foreach($game in @(Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue)) {
            try {
                if($game.HasExited){continue}
                $path=$game.Path
            } catch {throw 'Cannot verify a running Kenshi executable; close it before deployment.'}
            if(-not $path){throw 'Cannot verify a running Kenshi executable; close it before deployment.'}
            if($path -in $executables){throw "Close Kenshi before deploying: $Install"}
        }
    }
    # Every destination must be usable before any runtime bytes are replaced.
    foreach($install in $Installations) {
        $install=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($install)
        Assert-CoopDirectoryAncestors $install
        if(-not(Test-Path -LiteralPath (Join-Path $install 'kenshi_x64.exe') -PathType Leaf)){throw "Kenshi not found: $install"}
        & $assertIdle $install
        $target=Join-Path $install 'mods\KenshiCoop'
        if($targets.Target -contains $target){continue}
        foreach($directory in @((Join-Path $install 'mods'),$target)) {
            if(Test-Path -LiteralPath $directory) {
                $item=Get-Item -LiteralPath $directory -Force
                if(-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unsafe runtime destination: $directory"}
            }
        }
        $pending=@('.kenshicoop-update.txn','.kenshicoop-update.committed','.kenshicoop-deploy.pending')
        foreach($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')){foreach($suffix in @('.old','.new','.quarantine')){$pending+="$name$suffix"}}
        foreach($name in $pending){if(Test-Path -LiteralPath (Join-Path $target $name)){throw "Pending runtime recovery must be resolved before deployment: $target/$name"}}
        foreach($name in $Files.Keys) {
            $path=Join-Path $target $name
            if(Test-Path -LiteralPath $path) {
                $item=Get-Item -LiteralPath $path -Force
                if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unsafe runtime file: $path"}
                $handle=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
                $handle.Dispose()
            }
        }
        $targets+=@{Install=$install;Target=$target}
    }
    $transaction=Join-Path ([IO.Path]::GetTempPath()) ('kenshicoop-deploy-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $transaction|Out-Null
    $entries=@();$changed=@();$markers=@();$created=@();$preserve=$false
    try {
        $stage=Join-Path $transaction 'stage';New-Item -ItemType Directory -Path $stage|Out-Null
        $stagedFiles=@{}
        foreach($name in ($Files.Keys|Sort-Object)) {
            $staged=Join-Path $stage $name
            Copy-Item -LiteralPath $Files[$name] -Destination $staged
            $stagedFiles[$name]=$staged
            if((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$name]){throw "Runtime source changed during staging: $name"}
        }
        Assert-CoopRuntimeFiles $stagedFiles
        [IO.File]::WriteAllText((Join-Path $transaction 'recovery.json'),(@{schema=1;entries=@()}|ConvertTo-Json -Depth 5))
        foreach($destination in $targets) {
            & $assertIdle $destination.Install
            Assert-CoopDirectoryAncestors $destination.Target
            foreach($directory in @((Join-Path $destination.Install 'mods'),$destination.Target)){
                if(-not(Test-Path -LiteralPath $directory)){New-Item -ItemType Directory -Path $directory|Out-Null;$created+=$directory}
            }
            $marker=Join-Path $destination.Target '.kenshicoop-deploy.pending'
            $handle=[IO.File]::Open($marker,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
            $markers+=$marker
            try{$bytes=[Text.Encoding]::UTF8.GetBytes($transaction);$handle.Write($bytes,0,$bytes.Length);$handle.Flush($true)}finally{$handle.Dispose()}
        }
        foreach($destination in $targets) {
            foreach($name in ($Files.Keys|Sort-Object)) {
                $path=Join-Path $destination.Target $name
                $entry=@{Path=$path;Name=$name;Existed=(Test-Path -LiteralPath $path);Backup=(Join-Path $transaction "backup-$($entries.Count)");Hash=''}
                if($entry.Existed) {
                    $entry.Hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                    $entries+=$entry
                    Copy-Item -LiteralPath $path -Destination $entry.Backup
                    if((Get-FileHash -LiteralPath $entry.Backup -Algorithm SHA256).Hash -cne $entry.Hash){throw "Runtime changed during backup: $path"}
                }
                if(-not $entry.Existed){$entries+=$entry}
            }
        }
        [IO.File]::WriteAllText((Join-Path $transaction 'recovery.json'),(@{schema=1;entries=$entries}|ConvertTo-Json -Depth 5))
        foreach($entry in $entries) {
            if($entry.Existed) {
                if((Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -cne $entry.Hash){throw "Runtime changed before promotion: $($entry.Path)"}
            } elseif(Test-Path -LiteralPath $entry.Path){throw "Runtime file appeared before promotion: $($entry.Path)"}
            $changed+=$entry
            Copy-Item -LiteralPath (Join-Path $stage $entry.Name) -Destination $entry.Path -Force
            if((Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$entry.Name]){throw "Deployment hash mismatch: $($entry.Path)"}
        }
        foreach($marker in $markers){if([IO.File]::ReadAllText($marker) -cne $transaction){throw "Deployment marker ownership changed: $marker"};Remove-Item -LiteralPath $marker -ErrorAction Stop}
    } catch {
        $failure=$_
        $rollbackFailures=@()
        for($i=$changed.Count-1;$i -ge 0;--$i) {
            $entry=$changed[$i]
            try {
                if($entry.Existed) {
                    Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force
                    if((Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -cne $entry.Hash){throw 'Restored runtime hash mismatch'}
                } elseif(Test-Path -LiteralPath $entry.Path){Remove-Item -LiteralPath $entry.Path -Force}
            } catch {$rollbackFailures+=$entry.Path}
        }
        foreach($marker in $markers) {
            try {
                if(Test-Path -LiteralPath $marker){
                    if([IO.File]::ReadAllText($marker) -cne $transaction){throw 'Deployment marker ownership changed'}
                    if(-not $rollbackFailures.Count){Remove-Item -LiteralPath $marker -ErrorAction Stop}
                }elseif($rollbackFailures.Count){
                    $handle=[IO.File]::Open($marker,'CreateNew','Write','Read')
                    try{$bytes=[Text.Encoding]::UTF8.GetBytes($transaction);$handle.Write($bytes,0,$bytes.Length);$handle.Flush($true)}finally{$handle.Dispose()}
                }
            }catch{$rollbackFailures+=$marker}
        }
        if($rollbackFailures.Count){
            $preserve=$true
            throw "Deployment failed: $failure. Recovery unsettled; preserve $transaction and recovery markers. Failed paths: $($rollbackFailures -join ', ')"
        }
        for($i=$created.Count-1;$i -ge 0;--$i){if(-not @(Get-ChildItem -LiteralPath $created[$i] -Force).Count){Remove-Item -LiteralPath $created[$i]}}
        throw $failure
    } finally {
        if(-not $preserve){try{Remove-Item -LiteralPath $transaction -Recurse -Force}catch{Write-Warning "Runtime state is settled, but temporary backups need cleanup: $transaction"}}
    }
    foreach($destination in $targets){Write-Host "Complete six-file runtime deployed: $($destination.Target)"}
}
if($Deploy) {
    $ErrorActionPreference='Stop'
    if(-not $Kenshi){throw 'Deploy requires an explicit Kenshi installation path.'}
    $files=Get-CoopRuntimeFiles -Configuration $Configuration -NativeBin $NativeBin
    $installations=@($Kenshi)
    if($Mirror){$join=Join-Path $env:USERPROFILE 'Kenshi-Join';if($join -ine [IO.Path]::GetFullPath($Kenshi)){$installations+=$join}}
    Install-CoopRuntime -Files $files -Installations $installations
}
