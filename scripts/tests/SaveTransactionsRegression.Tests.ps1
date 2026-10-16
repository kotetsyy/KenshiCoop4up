# Run under Windows PowerShell 5.1 and PowerShell 7; disposable trees only.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference=$false
$scripts = Split-Path $PSScriptRoot -Parent
. (Join-Path $scripts 'capture_save.ps1')
. (Join-Path $scripts 'capture_baseline.ps1')
. (Join-Path $scripts 'bake_scene.ps1')
function Assert($Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) { $failed=$false; try { & $Action } catch { $failed=$true }; Assert $failed $Message }
function Put-ReceiptFixture([string]$Path,[string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    [IO.File]::WriteAllText($Path,$Text)
}
$root=Join-Path ([IO.Path]::GetTempPath()) ('save-transactions-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    foreach($name in @('../escape','trailing.','trailing ','a*b','CON','aux.txt',('x'*48))) {
        Reject { Assert-CoopSaveName $name -Bake } "Accepted unsafe bake name: $name"
    }
    Assert-CoopSaveName ('x'*47) -Bake
    $multibyte=[string][char]0x00e9
    Assert-CoopSaveName (($multibyte*23)+'x') -Bake
    Reject { Assert-CoopSaveName ($multibyte*24) -Bake } 'Accepted a 48-byte UTF-8 bake identity'
    $game=Join-Path $root 'game'; New-Item -ItemType Directory $game | Out-Null
    Set-Content -LiteralPath (Join-Path $game 'settings.cfg') -Value 'User save location=0'
    Assert ((Resolve-CoopSelectedSaveRoot $game) -eq (Join-Path $game 'save')) 'Ignored installation-local setting'
    Set-Content -LiteralPath (Join-Path $game 'settings.cfg') -Value @('User save location=1','User save location=0')
    Reject { Resolve-CoopSelectedSaveRoot $game } 'Accepted ambiguous save-root setting'
    Set-Content -LiteralPath (Join-Path $game 'settings.cfg') -Value 'User save location=1'
    Assert ((Resolve-CoopSelectedSaveRoot $game) -eq (Join-Path $env:LOCALAPPDATA 'kenshi\save')) 'Ignored AppData save-location selection'
    $other=Join-Path $root 'other'; New-Item -ItemType Directory $other | Out-Null
    [IO.File]::WriteAllText((Join-Path $other 'sentinel'),'outside')
    $src=Join-Path $root 'source'; $dst=Join-Path $root 'destination'
    New-Item -ItemType Directory $src,$dst | Out-Null
    [IO.File]::WriteAllText((Join-Path $src 'quick.save'),'new')
    [IO.File]::WriteAllText((Join-Path $dst 'quick.save'),'old')
    [IO.File]::WriteAllText((Join-Path $dst 'old-only'),'sentinel')
    Reject { Copy-CoopSaveTransaction $src (Join-Path $src 'nested') } 'Accepted overlapping save trees'
    Reject { Copy-CoopSaveTransaction (Join-Path $root 'missing') $dst } 'Accepted missing source'
    Assert ([IO.File]::ReadAllText((Join-Path $dst 'quick.save')) -eq 'old') 'Failed copy modified destination'
    $lock=[IO.File]::Open((Join-Path $src 'quick.save'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try { Reject { Copy-CoopSaveTransaction $src $dst } 'Accepted unreadable source' } finally { $lock.Dispose() }
    Assert ([IO.File]::ReadAllText((Join-Path $dst 'old-only')) -eq 'sentinel') 'Failed staging deleted original file'
    $junction=Join-Path $src 'redirect'
    New-Item -ItemType Junction -Path $junction -Target $other | Out-Null
    try { Reject { Copy-CoopSaveTransaction $src $dst } 'Traversed a source junction' }
    finally { [IO.Directory]::Delete($junction) }
    Assert ([IO.File]::ReadAllText((Join-Path $other 'sentinel')) -eq 'outside') 'Reparse refusal modified external data'
    $blocked=Join-Path $root 'blocked-destination'
    [IO.File]::WriteAllText($blocked,'original-file')
    Reject { Copy-CoopSaveTransaction $src $blocked } 'Accepted a file as promotion destination'
    Assert ([IO.File]::ReadAllText($blocked) -eq 'original-file') 'Failed promotion lost original destination'
    Copy-CoopSaveTransaction $src $dst
    Assert ([IO.File]::ReadAllText((Join-Path $dst 'quick.save')) -eq 'new') 'Successful transaction did not replace save'
    Assert (-not(Test-Path -LiteralPath (Join-Path $dst 'old-only'))) 'Successful replacement retained stale file'
    # Exercise the actual deploy entry point, not a source/wiring assertion.
    $sandbox=Join-Path $root 'repo'; $sandboxScripts=Join-Path $sandbox 'scripts'
    $fixture=Join-Path $sandbox 'fixtures\saves\sync'; $liveRoot=Join-Path $root 'live'
    New-Item -ItemType Directory -Path $sandboxScripts,$fixture,$liveRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $scripts 'capture_save.ps1'),(Join-Path $scripts 'deploy_saves.ps1') -Destination $sandboxScripts
    [IO.File]::WriteAllText((Join-Path $fixture 'quick.save'),'fixture')
    $shell=(Get-Process -Id $PID).Path; $deploy=Join-Path $sandboxScripts 'deploy_saves.ps1'
    & $shell -NoProfile -ExecutionPolicy Bypass -File $deploy -Save sync -SaveRoot $liveRoot -Force
    Assert ($LASTEXITCODE -eq 0) 'Disposable deployment failed'
    Assert ([IO.File]::ReadAllText((Join-Path $liveRoot 'sync\quick.save')) -eq 'fixture') 'Deploy did not use selected root'
    & $shell -NoProfile -ExecutionPolicy Bypass -File $deploy -Save '..\escape' -SaveRoot $liveRoot -Force
    Assert ($LASTEXITCODE -ne 0) 'Deploy accepted traversal'
    & $shell -NoProfile -ExecutionPolicy Bypass -File $deploy -Save sync -SaveRoot (Join-Path $sandbox 'fixtures\saves\nested') -Force
    Assert ($LASTEXITCODE -ne 0) 'Deploy accepted fixture-store overlap'
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'quick.save')) -eq 'fixture') 'Rejected deploy modified pristine fixture'
    # Real resume entry-point rejects artifact/save collisions for all root pairs,
    # before acquiring any screen, process or backup ownership.
    Copy-Item -LiteralPath (Join-Path $scripts 'resume_test.ps1'),(Join-Path $scripts 'kit_preflight.ps1') -Destination $sandboxScripts
    $savedAppData=$env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA=Join-Path $root 'appdata'
        $hostGame=Join-Path $root 'host-game'; $joinGame=Join-Path $root 'join-game'
        New-Item -ItemType Directory -Path $hostGame,$joinGame,$env:LOCALAPPDATA | Out-Null
        foreach ($hostLocation in 0,1) { foreach ($joinLocation in 0,1) {
            Set-Content -LiteralPath (Join-Path $hostGame 'settings.cfg') -Value "User save location=$hostLocation"
            Set-Content -LiteralPath (Join-Path $joinGame 'settings.cfg') -Value "User save location=$joinLocation"
            $selected=if($joinLocation -eq 0){Join-Path $joinGame 'save'}else{Join-Path $env:LOCALAPPDATA 'kenshi\save'}
            $collision=Join-Path $selected 'coopresume\artifacts'
            try {
                $ErrorActionPreference='Continue'
                $failure=& $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sandboxScripts 'resume_test.ps1') -HostDir $hostGame -JoinDir $joinGame -OutDir $collision 2>&1
                $resumeExit=$LASTEXITCODE
            } finally { $ErrorActionPreference='Stop' }
            Assert ($resumeExit -ne 0 -and ($failure -join "`n") -match 'overlap protected input/output') 'Resume did not reject selected-root artifact collision before launch'
            Assert (-not(Test-Path -LiteralPath $collision)) 'Resume collision created artifacts inside save'
        } }
    } finally { $env:LOCALAPPDATA=$savedAppData }
    # Actual baseline consumer rejection tests. Receipt-backed byte fixtures are
    # never executed as native runners and can never produce a positive baseline.
    foreach ($name in @('capture_baseline.ps1','NativeBuild.ps1','build_transports.ps1','build_plugin.cmd','v100_env.cmd')) {
        Copy-Item -LiteralPath (Join-Path $scripts $name) -Destination $sandboxScripts
    }
    foreach ($relative in @('src\transport\TransportApi.h','src\plugin\core\UdpEndpoint.h','src\ui\CoopUiApi.h','third_party\vc10_compat\OgreConfig.h','third_party\KenshiLib_deps\KenshiLib\Include\Defines.h')) {
        Put-ReceiptFixture (Join-Path $sandbox $relative) 'receipt-input'
    }
    Put-ReceiptFixture (Join-Path $sandbox 'src\netproto\Wire.h') (@('#ifdef KENSHICOOP_NET_DIAG','const u16 PROTOCOL_VERSION=77;','#else','const u16 PROTOCOL_VERSION=76;','#endif') -join "`n")
    Copy-Item -LiteralPath (Join-Path (Split-Path $scripts -Parent) 'src\transport\dependencies.json') -Destination (Join-Path $sandbox 'src\transport\dependencies.json')
    $baselineLatest=Join-Path $sandbox 'tools\baseline\latest.json'
    Put-ReceiptFixture $baselineLatest 'unchanged-baseline'
    $nativeBin=Join-Path $sandbox 'native\bin'; $nativeHashes=@{}; $coreHashes=@{}
    foreach ($name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','transport_integration.exe','transport_credentials.exe')) {
        $path=Join-Path $nativeBin $name; Put-ReceiptFixture $path ('receipt-bytes-'+$name)
        $nativeHashes[$name]=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    }
    foreach ($pair in @(@('plugin','KenshiCoop.dll'),@('ui','KenshiCoopUI.dll'))) {
        $path=Join-Path $sandbox "src\$($pair[0])\x64\Release\$($pair[1])"; Put-ReceiptFixture $path ('selected-release-'+$pair[1])
        $coreHashes[$pair[1]]=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    }
    . (Join-Path $sandboxScripts 'NativeBuild.ps1') -Configuration Release
    $pins=Get-Content -LiteralPath (Join-Path $sandbox 'src\transport\dependencies.json') -Raw | ConvertFrom-Json
    Put-ReceiptFixture (Join-Path $nativeBin 'native-build.json') (@{schema=1;transportAbi=2;configuration='Release';checkout=$sandbox;sourceDigest=(Get-CoopDigest Native);dependencies=$pins;files=$nativeHashes}|ConvertTo-Json -Depth 10)
    Put-ReceiptFixture (Join-Path $sandbox 'dist\build-receipts\core-Release.json') (@{schema=1;configuration='Release';checkout=$sandbox;sourceDigest=(Get-CoopDigest Core);files=$coreHashes}|ConvertTo-Json -Depth 5)
    function Reject-Baseline([string]$DllPath,[string]$ReasonPattern) {
        try {
            $ErrorActionPreference='Continue'
            $failure=& $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sandboxScripts 'capture_baseline.ps1') -Configuration Release -NativeBin $nativeBin -Dll $DllPath 2>&1
            $exitCode=$LASTEXITCODE
        } finally { $ErrorActionPreference='Stop' }
        Assert ($exitCode -ne 0 -and ($failure -join "`n") -match $ReasonPattern) 'Baseline failed outside the intended consumer boundary'
        Assert ([IO.File]::ReadAllText($baselineLatest) -eq 'unchanged-baseline') 'Failed baseline replaced latest.json'
    }
    Reject-Baseline (Join-Path $sandbox 'missing.dll') 'Required baseline input missing'
    $alternate=Join-Path $sandbox 'alternate.dll'; Put-ReceiptFixture $alternate 'other-configuration'
    Reject-Baseline $alternate 'Selected DLL does not match'
    $selected=Join-Path $sandbox 'src\plugin\x64\Release\KenshiCoop.dll'
    Put-ReceiptFixture $selected 'tampered'
    Reject-Baseline $selected 'receipt hash mismatch'
    Put-ReceiptFixture $selected 'selected-release-KenshiCoop.dll'
    $sdk=Join-Path $sandbox 'third_party\KenshiLib_deps\KenshiLib\Include\Defines.h'
    Put-ReceiptFixture $sdk 'changed-consumed-sdk'
    Reject-Baseline $selected 'source receipt stale'
    Put-ReceiptFixture $sdk 'receipt-input'
    Reject-Baseline $selected 'tests-Protocol-Release.json'
    $wire=@('#ifdef KENSHICOOP_NET_DIAG','const u16 PROTOCOL_VERSION = 77;','#else','const u16 PROTOCOL_VERSION = 76;','#endif') -join "`n"
    Assert ((Get-CoopBaselineProtocol $wire Harness) -eq 77) 'Harness selected wrong protocol branch'
    Assert ((Get-CoopBaselineProtocol $wire Release) -eq 76) 'Release selected wrong protocol branch'
    $rows=@('prototest: KenshiCoop wire/hash/interp unit layer (protocol v76)','  ok sizeof(HelloPacket) (= 4)','  ok sizeof(WelcomePacket) (= 7)','  ok sizeof(EventPacket) (= 71)','  ok HELLO version field offset (= 76)','  note canonical invEntryHash(wooden_sandals x2 q150) = 123','prototest: 5/5 checks passed - PASS')
    $facts=Read-CoopBaselineProtoOutput $rows @('HelloPacket','WelcomePacket','EventPacket')
    Assert ($facts.Protocol -eq 76 -and $facts.Hash -eq 123) 'Real native output syntax was not parsed'
    Reject { Read-CoopBaselineProtoOutput $rows[0..5] @('HelloPacket') } 'Accepted missing terminal success'
    Reject { Read-CoopBaselineProtoOutput ($rows+@('  FAIL late assertion')) @('HelloPacket') } 'Accepted failed native assertion'
    Reject { Read-CoopBaselineProtoOutput $rows @('MissingPacket') } 'Accepted incomplete wire inventory'
    $folder=Join-Path $root 'baked'
    $completion="[setup] auto-bake save COMPLETED name='fresh' folder='$folder' native=complete files=1 bytes=3"
    Assert ((Read-CoopBakeCompletion @($completion) fresh $folder).Files -eq 1) 'Rejected exact native completion'
    Reject { Read-CoopBakeCompletion @("[setup] auto-bake save ISSUED name='fresh'") fresh $folder } 'ISSUED counted as completion'
    Reject { Read-CoopBakeCompletion @($completion) stale $folder } 'Accepted other bake identity'
    Reject { Read-CoopBakeCompletion @($completion) fresh (Join-Path $root 'wrong') } 'Accepted mismatched native save root'
    Write-Host 'SaveTransactionsRegression: PASS'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
