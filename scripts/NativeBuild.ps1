# Shared exact-input receipt validation; never searches installed or newest DLLs.
[CmdletBinding()]
param([switch]$RecordCore,[switch]$PrintCoreDigest,[switch]$RecordTests,[switch]$PrintTestDigest,[ValidateSet('CoreNative','Protocol')][string]$TestKind='Protocol',[string]$ExpectedDigest='',[long]$BuildStartedUtcTicks=0,[ValidateSet('Release','Harness','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
# A PS5.1 child of pwsh inherits PS7 module search paths. Select this engine's
# built-ins for this process and its children; preserve external module paths.
$engineModules=Join-Path $PSHOME 'Modules'
$enginePrefix=$engineModules+[IO.Path]::PathSeparator
if($env:PSModulePath -ine $engineModules -and -not($env:PSModulePath -and $env:PSModulePath.StartsWith($enginePrefix,[StringComparison]::OrdinalIgnoreCase))){
    $env:PSModulePath=$enginePrefix+$env:PSModulePath
}
Import-Module (Join-Path $engineModules 'Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$script:NativeRepo=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\','/')
function Get-CoopConfiguration([string]$Value) {
    switch($Value.ToLowerInvariant()){'release'{return 'Release'}'harness'{return 'Harness'}'debug'{return 'Debug'}default{throw "Unknown configuration: $Value"}}
}
$Configuration=Get-CoopConfiguration $Configuration
function Get-CoopInputFiles([string]$Kind) {
    $repo=$script:NativeRepo
    $dirs=if($Kind -eq 'Native'){@('src\transport')}else{@('src\plugin','src\ui','src\netproto','third_party\vc10_compat','third_party\KenshiLib_deps')}
    $files=@(foreach($dir in $dirs){Get-ChildItem -LiteralPath (Join-Path $repo $dir) -Recurse -File | Where-Object {
        $relative=$_.FullName.Substring($repo.Length+1).Replace('\','/')
        $_.Extension -in '.h','.hpp','.inl','.ipp','.inc','.lib','.cpp','.c','.json','.txt','.patch','.vcxproj' -and $relative -notmatch '^src/(plugin|ui)/x64/' -and $relative -notmatch '^src/transport/(build|CMakeFiles)/'
    }})
    $extra=if($Kind -eq 'Native'){@('scripts\build_transports.ps1','scripts\NativeBuild.ps1','src\plugin\core\UdpEndpoint.h')}else{@('scripts\build_plugin.cmd','scripts\v100_env.cmd','scripts\NativeBuild.ps1','src\transport\TransportApi.h')}
    $files+=@(foreach($p in $extra){Get-Item -LiteralPath (Join-Path $repo $p)})
    if($Kind -eq 'Tests'){
        $files+=@(Get-ChildItem -LiteralPath (Join-Path $repo 'src\prototest') -Recurse -File)
        $files+=@(foreach($p in @('scripts\build_prototest.cmd','scripts\build_core_native_tests.cmd')){Get-Item -LiteralPath (Join-Path $repo $p)})
    }
    # NLS (Windows PS5.1) and ICU (PS7) collate punctuation differently. Receipts
    # must not depend on engine/culture, or freshly built DLLs look stale.
    $ordered=[Collections.Generic.SortedDictionary[string,IO.FileInfo]]::new([StringComparer]::Ordinal)
    foreach($file in $files){$ordered[$file.FullName]=$file}
    return @($ordered.Values)
}
function Assert-CoopReceiptFiles($Receipt,[string[]]$Names) {
    if($null -eq $Receipt.files -or (Compare-Object $Names @($Receipt.files.PSObject.Properties.Name) -CaseSensitive)){throw 'Malformed receipt file set'}
    foreach($name in $Names){if($Receipt.files.$name -isnot [string] -or $Receipt.files.$name -cnotmatch '^[0-9a-f]{64}$'){throw "Malformed receipt hash: $name"}}
    if($Receipt.sourceDigest -isnot [string] -or $Receipt.sourceDigest -cnotmatch '^[0-9a-f]{64}$' -or $Receipt.checkout -isnot [string]){throw 'Malformed receipt identity'}
    if(($Receipt.schema -isnot [int] -and $Receipt.schema -isnot [long]) -or $Receipt.schema -ne 1){throw 'Malformed receipt schema'}
}
function Get-CoopDigest([string]$Kind) {
    $repo=$script:NativeRepo
    $files=Get-CoopInputFiles $Kind
    $lines=@($files | ForEach-Object { $_.FullName.Substring($repo.Length+1).Replace('\','/')+':'+(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
    $hash=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
}
function Get-CoopNativeInputSnapshot {
    $digest=Get-CoopDigest Native
    $pins=Get-Content -LiteralPath (Join-Path $script:NativeRepo 'src\transport\dependencies.json') -Raw|ConvertFrom-Json
    if($digest -cne (Get-CoopDigest Native)){throw 'Native dependency pins changed while capturing source snapshot'}
    return [pscustomobject]@{Digest=$digest;Pins=$pins}
}
function Get-NativeBuild([ValidateSet('Release','Harness','Debug')][string]$Configuration='Release',[string]$NativeBin='') {
    $Configuration=Get-CoopConfiguration $Configuration
    $logical=if($Configuration -eq 'Debug'){'Harness'}else{$Configuration}
    if(-not $NativeBin) {
        $hash=[Security.Cryptography.SHA256]::Create()
        try { $id=[BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($script:NativeRepo))).Replace('-','').Substring(0,16).ToLowerInvariant() }
        finally { $hash.Dispose() }
        $NativeBin=Join-Path "$env:LOCALAPPDATA\KenshiCoop\native-build" "providers-v143-$id-$logical\bin"
    }
    $bin=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($NativeBin).TrimEnd('\','/')
    $path=Join-Path $bin 'native-build.json'
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "Exact native build receipt not found: $path. Rebuild with build_transports.ps1 or pass -NativeBin."}
    $receipt=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    Assert-CoopReceiptFiles $receipt @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','transport_integration.exe')
    if(($receipt.transportAbi -isnot [int] -and $receipt.transportAbi -isnot [long]) -or $receipt.transportAbi -ne 3){throw 'Malformed native transport ABI'}
    $snapshot=Get-CoopNativeInputSnapshot
    if($receipt.schema -ne 1 -or $receipt.transportAbi -ne 3 -or $receipt.configuration -cne $logical -or $receipt.checkout -ine $script:NativeRepo -or $receipt.sourceDigest -cne $snapshot.Digest -or ($receipt.dependencies|ConvertTo-Json -Depth 10 -Compress) -cne ($snapshot.Pins|ConvertTo-Json -Depth 10 -Compress)){throw 'Native receipt configuration/checkout/source/dependency mismatch; rebuild providers.'}
    $files=@{}
    foreach($name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','transport_integration.exe')) {
        $file=Join-Path $bin $name; $expected=$receipt.files.$name
        if(-not $expected -or -not(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected){throw "Native receipt hash mismatch: $name"}
        $files[$name]=$file
    }
    return [pscustomobject]@{Bin=$bin;Receipt=$receipt;Files=$files}
}
function Get-CoreBuild([ValidateSet('Release','Harness','Debug')][string]$Configuration='Release') {
    $Configuration=Get-CoopConfiguration $Configuration
    $path=Join-Path $script:NativeRepo "dist\build-receipts\core-$Configuration.json"
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "Core/UI receipt missing: $path; build both with build_plugin.cmd."}
    $receipt=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    Assert-CoopReceiptFiles $receipt @('KenshiCoop.dll','KenshiCoopUI.dll')
    if($receipt.schema -ne 1 -or $receipt.configuration -cne $Configuration -or $receipt.checkout -ine $script:NativeRepo -or $receipt.sourceDigest -cne (Get-CoopDigest Core)){throw 'Core/UI source receipt stale or mismatched; rebuild both DLLs.'}
    $files=@{'KenshiCoop.dll'=(Join-Path $script:NativeRepo "src\plugin\x64\$Configuration\KenshiCoop.dll");'KenshiCoopUI.dll'=(Join-Path $script:NativeRepo "src\ui\x64\$Configuration\KenshiCoopUI.dll")}
    foreach($name in $files.Keys) { if(-not(Test-Path -LiteralPath $files[$name] -PathType Leaf) -or (Get-FileHash -LiteralPath $files[$name] -Algorithm SHA256).Hash.ToLowerInvariant() -cne $receipt.files.$name){throw "Core/UI build receipt hash mismatch: $name"} }
    return [pscustomobject]@{Receipt=$receipt;Files=$files}
}
function Get-CoopTestNames([string]$Kind) {
    switch($Kind){'CoreNative'{return @('core_native_tests.exe')}'Protocol'{return @('prototest.exe','updater_tests.exe','native_frame_tests.exe','npc_census_tests.exe','environment_items_state_tests.exe')}default{throw 'Unknown native test kind'}}
}
function Get-CoopTestBuild([ValidateSet('CoreNative','Protocol')][string]$Kind,[ValidateSet('Release','Harness')][string]$Configuration='Release',[string]$Runner='') {
    $Configuration=Get-CoopConfiguration $Configuration
    $Kind=if($Kind -eq 'Protocol'){'Protocol'}else{'CoreNative'}
    if($Kind -eq 'Protocol' -and $Configuration -cne 'Release'){throw 'Protocol runners are compiled as Release only'}
    $path=Join-Path $script:NativeRepo "dist\build-receipts\tests-$Kind-$Configuration.json"
    $receipt=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    $names=@(Get-CoopTestNames $Kind);Assert-CoopReceiptFiles $receipt $names
    if($receipt.kind -cne $Kind -or $receipt.configuration -cne $Configuration -or $receipt.checkout -ine $script:NativeRepo -or $receipt.sourceDigest -cne (Get-CoopDigest Tests)){throw 'Native test provenance stale or mismatched'}
    $files=@{}
    foreach($name in $names){
        $file=Join-Path $script:NativeRepo "dist\$name"
        if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $receipt.files.$name){throw "Native test output changed: $name"}
        $files[$name]=$file
    }
    if($Runner -and ($names.Count -ne 1 -or $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Runner) -ine $files[$names[0]])){throw 'Custom runner is not the canonical receipted native consumer'}
    return [pscustomobject]@{Files=$files;Receipt=$receipt}
}
if($PrintTestDigest){Write-Output (Get-CoopDigest Tests)}
if($RecordTests){
    $TestKind=if($TestKind -eq 'Protocol'){'Protocol'}else{'CoreNative'}
    if(-not $ExpectedDigest -or $ExpectedDigest -cne (Get-CoopDigest Tests) -or $BuildStartedUtcTicks -le 0 -or $BuildStartedUtcTicks -gt [DateTime]::UtcNow.Ticks){throw 'Native test input/start snapshot invalid'}
    if($TestKind -eq 'Protocol' -and $Configuration -cne 'Release'){throw 'Protocol configuration must be Release'}
    $latest=Get-CoopInputFiles Tests|Sort-Object LastWriteTimeUtc -Descending|Select-Object -First 1
    $files=@{}
    foreach($name in @(Get-CoopTestNames $TestKind)){
        $file=Get-Item -LiteralPath (Join-Path $script:NativeRepo "dist\$name")
        if($file.LastWriteTimeUtc.Ticks -lt $BuildStartedUtcTicks -or $file.LastWriteTimeUtc -lt $latest.LastWriteTimeUtc){throw "Native test output not freshly compiled: $name"}
        $files[$name]=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    }
    if($ExpectedDigest -cne (Get-CoopDigest Tests)){throw 'Native test inputs changed before receipt write'}
    $receipt=@{schema=1;kind=$TestKind;checkout=$script:NativeRepo;configuration=$Configuration;sourceDigest=$ExpectedDigest;files=$files}
    $dir=Join-Path $script:NativeRepo 'dist\build-receipts';New-Item -ItemType Directory -Path $dir -Force|Out-Null
    [IO.File]::WriteAllText((Join-Path $dir "tests-$TestKind-$Configuration.json"),($receipt|ConvertTo-Json -Depth 5))
}
if($PrintCoreDigest) { Write-Output (Get-CoopDigest Core) }
if($RecordCore) {
    if(-not $ExpectedDigest -or $ExpectedDigest -cne (Get-CoopDigest Core)){throw 'Core/UI sources changed during build; no freshness receipt recorded.'}
    if($BuildStartedUtcTicks -le 0 -or $BuildStartedUtcTicks -gt [DateTime]::UtcNow.Ticks){throw 'Recording requires the canonical pre-build UTC start ticks.'}
    $latest=Get-CoopInputFiles Core | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $files=@{}
    foreach($pair in @(@('KenshiCoop.dll','plugin'),@('KenshiCoopUI.dll','ui'))) {
        $path=Join-Path $script:NativeRepo "src\$($pair[1])\x64\$Configuration\$($pair[0])"
        $output=Get-Item -LiteralPath $path
        if($output.LastWriteTimeUtc -lt $latest.LastWriteTimeUtc -or $output.LastWriteTimeUtc.Ticks -lt $BuildStartedUtcTicks){throw "Output not freshly rebuilt from complete consumed inputs: $path"}
        $files[$pair[0]]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if($ExpectedDigest -cne (Get-CoopDigest Core)){throw 'Core/UI sources changed before receipt write.'}
    $receipt=[ordered]@{schema=1;checkout=$script:NativeRepo;configuration=$Configuration;sourceDigest=$ExpectedDigest;files=$files}
    $dir=Join-Path $script:NativeRepo 'dist\build-receipts'; New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir "core-$Configuration.json"),($receipt|ConvertTo-Json -Depth 5))
}
