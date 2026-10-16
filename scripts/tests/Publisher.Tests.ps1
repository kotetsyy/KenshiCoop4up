# Exercise the real publisher and receipt resolvers in a disposable source tree.
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
$root=Join-Path ([IO.Path]::GetTempPath()) ('build\x64\coop-publish-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $root 'scripts') -Force|Out-Null
function Put-Fixture([string]$path,[string]$text){New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force|Out-Null;[IO.File]::WriteAllText($path,$text)}
function Tree-Hashes { return @(Get-ChildItem -LiteralPath $root -Recurse -File|Sort-Object FullName|ForEach-Object{$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash}) }
$engine=(Get-Process -Id $PID).Path
try {
    foreach($name in @('publish_release.ps1','NativeBuild.ps1','RuntimeDistribution.ps1','build_plugin.cmd','v100_env.cmd','build_transports.ps1')){Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $root "scripts\$name")}
    Put-Fixture (Join-Path $root 'src\netproto\Wire.h') "const char* COOP_BUILD_VERSION = `"0.2.0`";`n#ifdef KENSHICOOP_NET_DIAG`nconst unsigned PROTOCOL_VERSION = 77;`n#else`nconst unsigned PROTOCOL_VERSION = 76;`n#endif"
    Put-Fixture (Join-Path $root 'src\transport\TransportApi.h') 'receipt-input-fixture'
    Put-Fixture (Join-Path $root 'src\plugin\core\UdpEndpoint.h') 'receipt-input-fixture'
    Put-Fixture (Join-Path $root 'src\transport\dependencies.json') '{"protobuf":{"commit":"fixture-proto","sha256":"fixture-pin"},"gns":{"commit":"fixture-gns"}}'
    Put-Fixture (Join-Path $root 'src\ui\CoopUiApi.h') 'receipt-input-fixture'
    $compat=Join-Path $root 'third_party\vc10_compat\OgreConfig.h'
    Put-Fixture $compat '#define OGRE_USE_SIMD 0'
    $sdkInputs=@{
        'third_party\KenshiLib_deps\KenshiLib\Include\Defines.h'='sdk-layout-original'
        'third_party\KenshiLib_deps\boost_1_60_0\boost\version.hpp'='boost-header-original'
        'third_party\KenshiLib_deps\KenshiLib\Libraries\OgreMain_x64.lib'='sdk-library-original'
        'third_party\KenshiLib_deps\build\consumed.inl'='sdk-inline-original'
        'third_party\KenshiLib_deps\x64\consumed.ipp'='sdk-template-original'
    }
    foreach($relative in $sdkInputs.Keys){Put-Fixture (Join-Path $root $relative) $sdkInputs[$relative]}
    Put-Fixture (Join-Path $root 'dist\UPDATE.txt') 'stable-feed-must-not-change'
    Put-Fixture (Join-Path $root 'dist\RELEASE_NOTES.md') 'stable-notes-must-not-change'
    Put-Fixture (Join-Path $root 'dist\mods\KenshiCoop\RE_Kenshi.json') '{"Plugins":["KenshiCoop.dll"]}'
    Put-Fixture (Join-Path $root 'dist\mods\KenshiCoop\KenshiCoop.mod') 'fixture-mod'
    $nativeBin=Join-Path $root 'native\bin';$nativeFiles=@{};$coreFiles=@{}
    foreach($pair in @(@('KenshiCoop.dll','plugin'),@('KenshiCoopUI.dll','ui'))){$p=Join-Path $root "src\$($pair[1])\x64\Release\$($pair[0])";Put-Fixture $p "MZ-fixture-$($pair[0])";$coreFiles[$pair[0]]=(Get-FileHash -LiteralPath $p).Hash.ToLowerInvariant()}
    foreach($name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','transport_integration.exe','transport_credentials.exe')){$p=Join-Path $nativeBin $name;Put-Fixture $p "MZ-fixture-$name";$nativeFiles[$name]=(Get-FileHash -LiteralPath $p).Hash.ToLowerInvariant()}
    . (Join-Path $root 'scripts\NativeBuild.ps1') -Configuration Release
    Put-Fixture (Join-Path $nativeBin 'native-build.json') (@{schema=1;transportAbi=2;configuration='Release';checkout=$root;sourceDigest=(Get-CoopDigest Native);dependencies=(Get-CoopNativeInputSnapshot).Pins;files=$nativeFiles}|ConvertTo-Json -Depth 10)
    Put-Fixture (Join-Path $root 'dist\build-receipts\core-Release.json') (@{schema=1;configuration='Release';checkout=$root;sourceDigest=(Get-CoopDigest Core);files=$coreFiles}|ConvertTo-Json -Depth 5)
    . (Join-Path $root 'scripts\RuntimeDistribution.ps1') -Configuration Release -NativeBin $nativeBin
    $files=Get-CoopRuntimeFiles Release $nativeBin
    foreach($spelling in @('release','RELEASE')){Get-CoreBuild $spelling|Out-Null;Get-NativeBuild $spelling $nativeBin|Out-Null}
    $location=Get-Location;$cwd=[Environment]::CurrentDirectory
    try{Set-Location $root;[Environment]::CurrentDirectory=[IO.Path]::GetTempPath();Get-NativeBuild Release 'native\bin'|Out-Null}finally{Set-Location $location;[Environment]::CurrentDirectory=$cwd}
    $nativeReceiptPath=Join-Path $nativeBin 'native-build.json';$nativeReceiptText=[IO.File]::ReadAllText($nativeReceiptPath)
    foreach($bad in @('schema','abi','dependencies','files')){
        $receipt=$nativeReceiptText|ConvertFrom-Json
        switch($bad){'schema'{$receipt.schema='1'}'abi'{$receipt.transportAbi='1'}'dependencies'{$receipt.dependencies.protobuf.commit='wrong-pin'}'files'{$receipt.files|Add-Member NoteProperty unexpected.dll ('0'*64)}}
        Put-Fixture $nativeReceiptPath ($receipt|ConvertTo-Json -Depth 10)
        $rejected=$false;try{Get-NativeBuild Release $nativeBin|Out-Null}catch{$rejected=$true}
        if(-not $rejected){throw "Malformed native receipt accepted: $bad"}
    }
    $receipt=$nativeReceiptText|ConvertFrom-Json;$receipt.configuration='Harness'
    Put-Fixture $nativeReceiptPath ($receipt|ConvertTo-Json -Depth 10)
    foreach($spelling in @('harness','HARNESS','debug','DEBUG')){Get-NativeBuild $spelling $nativeBin|Out-Null}
    $rejected=$false;try{Get-CoreBuild debug|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Debug core reused Release pair'}
    Put-Fixture $nativeReceiptPath $nativeReceiptText
    $pinsPath=Join-Path $root 'src\transport\dependencies.json';$pinsText=[IO.File]::ReadAllText($pinsPath)
    $digestImplementation=(Get-Command Get-CoopDigest).ScriptBlock;$script:digestCalls=0
    function Get-CoopDigest([string]$Kind){
        $result=& $digestImplementation $Kind
        if(++$script:digestCalls -eq 1){[IO.File]::WriteAllText($pinsPath,'{"protobuf":{"commit":"changed-at-boundary"}}')}
        return $result
    }
    try{$rejected=$false;try{Get-CoopNativeInputSnapshot|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Mismatched pins/source snapshot accepted'}}
    finally{Set-Item Function:\Get-CoopDigest $digestImplementation;Put-Fixture $pinsPath $pinsText}
    New-CoopRuntimeKit -Files $files -Zip (Join-Path $root 'dist\KenshiCoop-kit.zip')
    $before=Tree-Hashes
    foreach($mode in @('-WhatIf','-Local')) {
        $output=@(& $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin $mode -WriteManifest -Out 'dist\UPDATE.txt')
        if($LASTEXITCODE -ne 0){throw "Actual publisher $mode failed"}
        if(Compare-Object $before (Tree-Hashes)){throw "Actual publisher $mode mutated disposable tree"}
        $text=$output -join "`n";$values=@{}
        foreach($line in ($text -split "`r?`n")){if($line -match '^([A-Za-z0-9]+)=(.*)$'){$values[$Matches[1]]=$Matches[2]}}
        $expectedKeys=@('schema','transportAbi','version','proto','notes','coreSha256','coreUrl','uiSha256','uiUrl','steamSha256','steamUrl','udpSha256','udpUrl')
        if(Compare-Object $expectedKeys @($values.Keys) -CaseSensitive){throw 'Manifest fields differ from schema-2 native contract'}
        if($values.schema -cne '2' -or $values.transportAbi -cne '2' -or $values.version -cne '0.2.0' -or $values.proto -cne '76'){throw 'Release identity/protocol selection wrong'}
        $components=@{core='KenshiCoop.dll';ui='KenshiCoopUI.dll';steam='KenshiCoopNetSteam.dll';udp='KenshiCoopNetUDP.dll'}
        foreach($key in $components.Keys){$name=$components[$key];if($values["${key}Sha256"] -cne (Get-FileHash $files[$name]).Hash.ToLowerInvariant() -or $values["${key}Url"] -cne "https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.2.0/$name"){throw "Wrong manifest component: $key"}}
        if($text -notmatch 'UNEXECUTED: gh release create v0.2.0'){throw 'Local release command not shown'}
    }
    & $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin -WriteManifest -Out 'dist\UPDATE.txt'
    if($LASTEXITCODE -eq 0){throw 'Protected stable feed write accepted'}
    if(Compare-Object $before (Tree-Hashes)){throw 'Rejected protected write mutated files'}
    foreach($out in @('dist\RELEASE_NOTES.md','dist\UPDATE.txt')){
        & $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin -WriteManifest -Out $out
        if($LASTEXITCODE -eq 0 -or (Compare-Object $before (Tree-Hashes))){throw 'Protected output was mutated/accepted'}
    }
    $candidate=Join-Path $root 'dist\candidate.txt'
    & $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin -WriteManifest -Out $candidate
    if($LASTEXITCODE -ne 0 -or -not(Test-Path $candidate)){throw 'New candidate refused'}
    $candidateHash=(Get-FileHash $candidate).Hash
    $baseBytes=([IO.File]::ReadAllBytes($candidate)).Length
    & $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin -WriteManifest -Out $candidate
    if($LASTEXITCODE -eq 0 -or (Get-FileHash $candidate).Hash -cne $candidateHash){throw 'Existing candidate overwritten'}
    Remove-Item $candidate
    # Child expressions avoid Windows' native command-line limit and PS5 encoding ambiguity.
    $invoker=Join-Path $root 'scripts\notes-fixture.ps1'
    $prefix="`$ErrorActionPreference='Stop'; & '"+(Join-Path $root 'scripts\publish_release.ps1')+"' -NativeBin '"+$nativeBin+"'"
    Put-Fixture $invoker ($prefix+" -WriteManifest -Out '"+$candidate+"' -Notes ('a'*"+(65536-$baseBytes)+")")
    $null=@(& $engine -NoProfile -ExecutionPolicy Bypass -File $invoker)
    if($LASTEXITCODE -ne 0 -or ([IO.File]::ReadAllBytes($candidate)).Length -ne 65536){throw 'Exact native byte-limit candidate rejected'}
    Remove-Item $candidate
    foreach($expression in @("('a'*65536)","(([string][char]0x754c)*24000)")){
        Put-Fixture $invoker ($prefix+" -Local -Notes "+$expression)
        $beforeOversize=Tree-Hashes
        & $engine -NoProfile -ExecutionPolicy Bypass -File $invoker
        if($LASTEXITCODE -eq 0 -or (Compare-Object $beforeOversize (Tree-Hashes))){throw 'Oversize native-consumer manifest accepted/mutated'}
    }
    Remove-Item $invoker
    $contenders=@()
    try {
        foreach($note in @('owner-one','owner-two')){
            $child=Join-Path $root "scripts\$note.ps1"
            Put-Fixture $child ($prefix+" -WriteManifest -Out '"+$candidate+"' -Notes '"+$note+"'")
            $contenders+=Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$child+'"')) -RedirectStandardOutput (Join-Path $root "$note.out") -RedirectStandardError (Join-Path $root "$note.err") -PassThru
            $null=$contenders[-1].Handle
        }
        foreach($process in $contenders){$process.WaitForExit()}
        if(@($contenders|Where-Object{$_.ExitCode -eq 0}).Count -ne 1){
            $codes=@($contenders|ForEach-Object{"pid=$($_.Id) code=$($_.ExitCode) exited=$($_.HasExited)"})
            $errors=@('owner-one','owner-two'|ForEach-Object{[IO.File]::ReadAllText((Join-Path $root "$_.err"))})
            throw "Competing publisher creates did not yield one owner: $($codes -join '; '); $($errors -join '; ')"
        }
        $winner=if($contenders[0].ExitCode -eq 0){'owner-one'}else{'owner-two'}
        if([IO.File]::ReadAllText($candidate) -notmatch ("(?m)^notes="+$winner+"$")){throw 'Winning candidate overwritten'}
    }finally{foreach($process in $contenders){if(-not $process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}}
    Remove-Item $candidate
    # Changed consumed SDK inputs invalidate unchanged core/UI output receipts.
    # Native test provenance consumer boundaries, not compilation proof.
    foreach($name in @('build_prototest.cmd','build_core_native_tests.cmd')){Copy-Item (Join-Path $source $name) (Join-Path $root "scripts\$name")}
    $testSource=Join-Path $root 'src\prototest\main.cpp';Put-Fixture $testSource 'test-runner-source'
    foreach($kind in @('Protocol','CoreNative')){
        $configuration=if($kind -eq 'Protocol'){'Release'}else{'Harness'}
        $digest=Get-CoopDigest Tests;$started=[DateTime]::UtcNow.Ticks
        foreach($name in @(Get-CoopTestNames $kind)){Put-Fixture (Join-Path $root "dist\$name") "synthetic-provenance-$name"}
        & (Join-Path $root 'scripts\NativeBuild.ps1') -RecordTests -TestKind $kind -Configuration $configuration -ExpectedDigest $digest -BuildStartedUtcTicks $started
        Get-CoopTestBuild $kind $configuration|Out-Null
        $unrelated=Join-Path $root 'unrelated.exe';Put-Fixture $unrelated 'unrelated-zero-exit-would-not-be-proof'
        $rejected=$false;try{Get-CoopTestBuild $kind $configuration -Runner $unrelated|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Unrelated custom runner accepted'}
        $wrong=if($configuration -eq 'Release'){'Harness'}else{'Release'}
        $rejected=$false;try{Get-CoopTestBuild $kind $wrong|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Wrong test configuration accepted'}
        $names=@(Get-CoopTestNames $kind);$output=Join-Path $root ('dist\'+$names[0]);$original=[IO.File]::ReadAllText($output)
        Put-Fixture $output 'changed-output'
        $rejected=$false;try{Get-CoopTestBuild $kind $configuration|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Changed test runner accepted'}
        Put-Fixture $output $original;Put-Fixture $testSource 'changed-test-source'
        $rejected=$false;try{Get-CoopTestBuild $kind $configuration|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Stale test runner input accepted'}
        Put-Fixture $testSource 'test-runner-source'
    }
    foreach($relative in $sdkInputs.Keys) {
        $path=Join-Path $root $relative
        Put-Fixture $path 'changed-sdk-input'
        $rejected=$false;try{Get-CoreBuild Release|Out-Null}catch{$rejected=$true}
        if(-not $rejected){throw "Consumed SDK input change accepted by core receipt: $relative"}
        $newDigest=Get-CoopDigest Core
        $outputAge=(Get-Item $files['KenshiCoop.dll']).LastWriteTimeUtc
        (Get-Item $path).LastWriteTimeUtc=$outputAge.AddSeconds(1)
        $rejected=$false
        try{& (Join-Path $root 'scripts\NativeBuild.ps1') -RecordCore -Configuration release -ExpectedDigest $newDigest -BuildStartedUtcTicks ([DateTime]::UtcNow.Ticks)}catch{$rejected=$true}
        if(-not $rejected){throw "Old outputs re-attested for changed consumed input: $relative"}
        $beforeRejected=Tree-Hashes
        & $engine -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') -NativeBin $nativeBin -Local
        if($LASTEXITCODE -eq 0){throw "Publisher accepted stale SDK input: $relative"}
        if(Compare-Object $beforeRejected (Tree-Hashes)){throw "Stale SDK rejection mutated files: $relative"}
        Put-Fixture $path $sdkInputs[$relative]
    }
    # Receipt wrong configuration, changed source and changed binaries fail closed.
    $rejected=$false;try{Get-NativeBuild Harness $nativeBin|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Wrong logical native configuration accepted'}
    Put-Fixture (Join-Path $nativeBin 'KenshiCoopNetUDP.dll') 'tampered'
    $rejected=$false;try{Get-NativeBuild Release $nativeBin|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Native binary hash mismatch accepted'}
    Put-Fixture $compat '#define OGRE_USE_SIMD 1'
    $rejected=$false;try{Get-CoreBuild Release|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Consumed compatibility-header change accepted by core receipt'}
    Put-Fixture $compat '#define OGRE_USE_SIMD 0'
    Put-Fixture (Join-Path $root 'src\ui\CoopUiApi.h') 'changed-source'
    $rejected=$false;try{Get-CoreBuild Release|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Stale source receipt accepted'}
    Write-Host 'Publisher fixtures: actual Local/WhatIf no-mutation/stable protection/source-config-hash rejection PASS'
} finally {Remove-Item -LiteralPath $root -Recurse -Force}
