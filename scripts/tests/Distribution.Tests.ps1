$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'RuntimeDistribution.ps1')
$root=Join-Path ([IO.Path]::GetTempPath()) ('coop-dist-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
try {
    $files=@{}
    foreach($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')) {
        $files[$name]=Join-Path $root $name
        [IO.File]::WriteAllText($files[$name],$(if($name -eq 'RE_Kenshi.json'){'{"Plugins":["KenshiCoop.dll"]}'}else{"MZ-test-$name"}))
    }
    $zip=Join-Path $root 'kit.zip'; New-CoopRuntimeKit -Files $files -Zip $zip
    Assert-CoopRuntimeZip -Zip $zip -Files $files
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $a=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Update)
    try {$e=$a.CreateEntry('KenshiCoop/coop_config.json');$w=New-Object IO.StreamWriter($e.Open());$w.Write('private');$w.Dispose()} finally {$a.Dispose()}
    $rejected=$false;try{Assert-CoopRuntimeZip -Zip $zip -Files $files}catch{$rejected=$true};if(-not $rejected){throw 'Extra-file ZIP accepted'}
    $install=Join-Path $root 'install';New-Item -ItemType Directory -Path $install|Out-Null
    [IO.File]::WriteAllText((Join-Path $install 'kenshi_x64.exe'),'test-install')
    $mod=Join-Path $install 'mods\KenshiCoop';New-Item -ItemType Directory -Path $mod -Force|Out-Null
    [IO.File]::WriteAllText((Join-Path $mod 'coop_config.json'),'keep-config');[IO.File]::WriteAllText((Join-Path $install 'user.save'),'keep-save')
    [IO.File]::WriteAllText((Join-Path $mod 'credentials.bin'),'keep-credentials')
    $missing=$files.Clone();$missing['KenshiCoopNetUDP.dll']=Join-Path $root 'absent.dll'
    $rejected=$false;try{Install-CoopRuntime -Files $missing -Installations @($install)}catch{$rejected=$true};if(-not $rejected){throw 'Missing-provider preflight accepted'}
    if(Test-Path (Join-Path $mod 'KenshiCoop.dll')){throw 'Incomplete preflight mutated install'}
    Install-CoopRuntime -Files $files -Installations @($install)
    foreach($name in $files.Keys){if((Get-FileHash -LiteralPath (Join-Path $mod $name)).Hash -cne (Get-FileHash -LiteralPath $files[$name]).Hash){throw "Successful deployment hash mismatch: $name"}}
    if([IO.File]::ReadAllText((Join-Path $mod 'credentials.bin')) -cne 'keep-credentials'){throw 'Credentials altered'}
    if([IO.File]::ReadAllText((Join-Path $mod 'coop_config.json')) -cne 'keep-config' -or [IO.File]::ReadAllText((Join-Path $install 'user.save')) -cne 'keep-save'){throw 'Config/save altered'}
    [IO.File]::WriteAllText($files['KenshiCoop.dll'],'new-core-runtime')
    # The real executable is below RE_Kenshi, not necessarily the root launcher.
    $reDir=Join-Path $install 'RE_Kenshi'
    New-Item -ItemType Directory -Path $reDir|Out-Null
    $probeExe=Join-Path $reDir 'kenshi_x64.exe'
    Copy-Item -LiteralPath $env:ComSpec -Destination $probeExe
    $probe=Start-Process -FilePath $probeExe -ArgumentList '/d /k rem KenshiCoop deployment guard fixture' -WindowStyle Hidden -PassThru
    try {
        $null=$probe.Handle
        if($probe.HasExited){throw 'Owned deployment guard fixture exited before verification'}
        $before=@(Get-ChildItem -LiteralPath $install -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
        $rejected=$false;try{Install-CoopRuntime -Files $files -Installations @($install)}catch{$rejected=$true}
        if(-not $rejected){throw 'Deployment accepted a running RE_Kenshi executable'}
        $after=@(Get-ChildItem -LiteralPath $install -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
        if(Compare-Object $before $after){throw 'Running-RE rejection changed installation bytes'}
    } finally {
        if(-not $probe.HasExited){$probe.Kill();$probe.WaitForExit()}
    }
    # Root-launcher counterpart: launch/terminate only this disposable shell copy.
    $rootProbeExe=Join-Path $install 'kenshi_x64.exe';Copy-Item $env:ComSpec $rootProbeExe -Force
    $rootProbe=Start-Process -FilePath $rootProbeExe -ArgumentList '/d /k rem root deployment guard fixture' -WindowStyle Hidden -PassThru
    try{
        $null=$rootProbe.Handle;if($rootProbe.HasExited){throw 'Root guard fixture exited early'}
        $before=@(Get-ChildItem $install -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash $_.FullName).Hash})
        $rejected=$false;try{Install-CoopRuntime $files @($install)}catch{$rejected=$true}
        if(-not $rejected){throw 'Running root executable accepted'}
        $after=@(Get-ChildItem $install -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash $_.FullName).Hash})
        if(Compare-Object $before $after){throw 'Root-process rejection changed bytes'}
    }finally{if(-not $rootProbe.HasExited){$rootProbe.Kill();$rootProbe.WaitForExit()}}
    # A malformed later destination cannot partially update the earlier one.
    $malformed=Join-Path $root 'malformed-install'
    New-Item -ItemType Directory -Path $malformed|Out-Null
    [IO.File]::WriteAllText((Join-Path $malformed 'kenshi_x64.exe'),'test-install')
    [IO.File]::WriteAllText((Join-Path $malformed 'mods'),'not-a-directory')
    $before=@(Get-ChildItem -LiteralPath $install,$malformed -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    $rejected=$false;try{Install-CoopRuntime -Files $files -Installations @($install,$malformed)}catch{$rejected=$true}
    if(-not $rejected){throw 'Malformed later destination accepted'}
    $after=@(Get-ChildItem -LiteralPath $install,$malformed -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    if(Compare-Object $before $after){throw 'Later-destination failure partially updated earlier installation'}
    # A later destination's recovery evidence must block every destination.
    $other=Join-Path $root 'second-install';New-Item -ItemType Directory -Path (Join-Path $other 'mods\KenshiCoop') -Force|Out-Null
    [IO.File]::WriteAllText((Join-Path $other 'kenshi_x64.exe'),'test-install')
    $otherMod=Join-Path $other 'mods\KenshiCoop'
    foreach($name in $files.Keys){[IO.File]::WriteAllText((Join-Path $otherMod $name),"preserve-$name")}
    [IO.File]::WriteAllText((Join-Path $otherMod 'coop_config.json'),'second-config')
    [IO.File]::WriteAllText((Join-Path $other 'user.save'),'second-save')
    $pending=@('.kenshicoop-update.txn','.kenshicoop-update.committed','.kenshicoop-deploy.pending')
    foreach($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')){foreach($suffix in @('.old','.new','.quarantine')){$pending+="$name$suffix"}}
    foreach($evidence in $pending) {
        foreach($destination in @($mod,$otherMod)) {
            $path=Join-Path $destination $evidence;[IO.File]::WriteAllText($path,'recovery-evidence')
            $before=@(Get-ChildItem -LiteralPath $install,$other -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
            $rejected=$false;try{Install-CoopRuntime -Files $files -Installations @($install,$other)}catch{$rejected=$true};if(-not $rejected){throw "Pending updater state accepted: $evidence"}
            $after=@(Get-ChildItem -LiteralPath $install,$other -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
            if(Compare-Object $before $after){throw "Pending preflight mutated a destination: $evidence"}
            Remove-Item -LiteralPath $path
        }
    }
    # Actual committed/uncommitted combinations also preserve every target.
    foreach($committed in @($false,$true)) {
        $evidence=@('.kenshicoop-update.txn');if($committed){$evidence+='.kenshicoop-update.committed'}
        foreach($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')){$evidence+="$name.old"}
        foreach($name in $evidence){[IO.File]::WriteAllText((Join-Path $otherMod $name),'transaction-evidence')}
        $before=@(Get-ChildItem -LiteralPath $install,$other -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
        $rejected=$false;try{Install-CoopRuntime -Files $files -Installations @($install,$other)}catch{$rejected=$true};if(-not $rejected){throw 'Pending transaction accepted'}
        $after=@(Get-ChildItem -LiteralPath $install,$other -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
        if(Compare-Object $before $after){throw 'Pending transaction mutated a destination'}
        foreach($name in $evidence){Remove-Item -LiteralPath (Join-Path $otherMod $name)}
    }
    [IO.File]::WriteAllText($files['RE_Kenshi.json'],'{"Plugins":["KenshiCoop.dll","KenshiCoopNetUDP.dll"]}')
    $rejected=$false;try{New-CoopRuntimeKit -Files $files -Zip (Join-Path $root 'bad.zip')}catch{$rejected=$true};if(-not $rejected){throw 'Provider loaded as game plugin'}
    Write-Host 'Distribution fixtures: production exact-six ZIP/extra rejection/no-mutation preflight/config-save preservation PASS'
} finally {Remove-Item -LiteralPath $root -Recurse -Force}
