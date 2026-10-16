[CmdletBinding()]
param(
    [ValidateSet('Release', 'Harness')][string]$Configuration = 'Release',
    [string]$BuildRoot = "$env:LOCALAPPDATA\KenshiCoop\native-build",
    [string]$CMake = '',
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$SourcesOnly
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Source = Join-Path $Repo 'src\transport'
. (Join-Path $PSScriptRoot 'NativeBuild.ps1') -Configuration $Configuration
$Repo = $script:NativeRepo
$nativeSnapshot=Get-CoopNativeInputSnapshot
$SourceDigest=$nativeSnapshot.Digest;$Pins=$nativeSnapshot.Pins
$Configuration=Get-CoopConfiguration $Configuration
$BuildRoot=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildRoot)
function Assert-SourceAncestors([string]$Path) {
    $path=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    while(-not(Test-Path -LiteralPath $path)){$path=Split-Path $path -Parent;if(-not $path){throw 'No existing filesystem ancestor'}}
    $ancestor=Get-Item -LiteralPath $path -Force
    while($null -ne $ancestor){if($ancestor -isnot [IO.DirectoryInfo] -or ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unsafe source cache ancestor: $($ancestor.FullName)"};$ancestor=$ancestor.Parent}
}
function Open-VerifiedSourceInput([string]$Path,[string]$ExpectedHash) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $hash=[Security.Cryptography.SHA256]::Create()
    try{$actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant();if($actual -cne $ExpectedHash){throw "Pinned input SHA-256 mismatch: $Path"};$stream.Position=0;return $stream}
    catch{$stream.Dispose();throw}
    finally{$hash.Dispose()}
}
function Invoke-CMake([string[]]$Arguments) {
    & $CMake @CMakePrefix @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed ($LASTEXITCODE): $Arguments" }
}
function Get-SourceHash([string]$Path) {
    $Stream = [System.IO.File]::OpenRead($Path)
    $Hash = [System.Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($Hash.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant() }
    finally { $Hash.Dispose(); $Stream.Dispose() }
}
function Get-SourceTreeHash([string]$Directory) {
    $base=[IO.Path]::GetFullPath($Directory).TrimEnd('\','/')
    $ancestor=Get-Item -LiteralPath $base -Force
    if(-not $ancestor.PSIsContainer){throw "Source root is not a directory: $Directory"}
    while($null -ne $ancestor) {
        if(($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Source root has a reparse ancestor: $($ancestor.FullName)"}
        $ancestor=$ancestor.Parent
    }
    # Inspect each direct entry before descending; -Recurse can follow a hostile
    # directory junction before the fingerprint has had a chance to reject it.
    $pending=New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($base)
    $lines=New-Object 'System.Collections.Generic.List[string]'
    while($pending.Count) {
        $current=$pending.Pop()
        foreach($entry in @(Get-ChildItem -LiteralPath $current -Force)) {
            $relative=$entry.FullName.Substring($base.Length+1).Replace('\','/')
            $reparse=($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            if($reparse) {
                if($entry.PSIsContainer -or $entry.LinkType -cne 'SymbolicLink'){throw "Unsupported source reparse point: $($entry.FullName)"}
                $targets=@($entry.Target)
                if($targets.Count -ne 1 -or [string]::IsNullOrEmpty($targets[0])){throw "Invalid source symlink target: $($entry.FullName)"}
                $target=[string]$targets[0]
                if([IO.Path]::IsPathRooted($target) -or $target.Contains(':')){throw "Absolute/stream source symlink target: $($entry.FullName)"}
                $resolved=[IO.Path]::GetFullPath((Join-Path $entry.DirectoryName $target))
                if(-not $resolved.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Source symlink escapes tree: $($entry.FullName)"}
                $targetRelative=$resolved.Substring($base.Length+1)
                $targetPath=$base
                $segments=$targetRelative -split '[\\/]'
                for($i=0;$i -lt $segments.Count;++$i) {
                    $targetPath=Join-Path $targetPath $segments[$i]
                    $targetEntry=Get-Item -LiteralPath $targetPath -Force
                    if(($targetEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Source symlink target has a reparse ancestor/target: $($entry.FullName)"}
                    if($i -lt $segments.Count-1 -and -not $targetEntry.PSIsContainer){throw "Source symlink target ancestor is not a directory: $($entry.FullName)"}
                }
                if($targetEntry.PSIsContainer){throw "Source symlink target is not a regular file: $($entry.FullName)"}
                if($relative -ceq '.kenshicoop-source-sha256'){throw 'Source marker must be a regular file'}
                $encoded=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($target))
                $lines.Add($relative+':symlink:'+ $encoded+':'+(Get-SourceHash $resolved))
            } elseif($entry.PSIsContainer) {
                $pending.Push($entry.FullName)
            } elseif($relative -cne '.kenshicoop-source-sha256') {
                $lines.Add($relative+':file:'+(Get-SourceHash $entry.FullName))
            }
        }
    }
    $hash=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes((@($lines|Sort-Object) -join "`n")))).Replace('-','').ToLowerInvariant()}
    finally {$hash.Dispose()}
}
function Get-PinnedSource([string]$Name, $Pin,[switch]$Verified,[switch]$Snapshot) {
    $Cache = Join-Path $BuildRoot 'sources'
    Assert-SourceAncestors $Cache
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    # Do not reuse or modify cached trees from the former locally-patched build.
    $Destination = Join-Path $Cache "$Name-$($Pin.commit)-original"
    $Marker = Join-Path $Destination '.kenshicoop-source-sha256'
    # A marker is bookkeeping, not proof of mutable extracted source contents.
    if(Test-Path -LiteralPath $Destination) {
        $rootEntry=Get-Item -LiteralPath $Destination -Force
        $ancestor=$rootEntry
        while($null -ne $ancestor) {
            if(($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Source cache has a reparse ancestor: $($ancestor.FullName)"}
            $ancestor=$ancestor.Parent
        }
        $markerEntry=Get-Item -LiteralPath $Marker -Force
        if(-not $rootEntry.PSIsContainer -or ($rootEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $markerEntry.PSIsContainer -or ($markerEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or (Get-Content -LiteralPath $Marker -Raw).Trim() -cne $Pin.sha256){throw "Unverified source directory exists: $Destination"}
    }
    $Archive = Join-Path $Cache "$Name-$($Pin.commit).tar.gz"
    if (-not (Test-Path -LiteralPath $Archive)) {
        $Partial = "$Archive.download"
        if (-not (Test-Path -LiteralPath $Partial)) {
            Invoke-WebRequest -Uri $Pin.url -OutFile $Partial -UseBasicParsing
        }
        if ((Get-SourceHash $Partial) -ne $Pin.sha256) {
            Remove-Item -LiteralPath $Partial -Force
            throw "Downloaded archive SHA-256 mismatch: $Name"
        }
        Move-Item -LiteralPath $Partial -Destination $Archive
    }
    $archiveHandle=Open-VerifiedSourceInput $Archive $Pin.sha256
    # Derive the expected tree anew from the verified, unpatched archive.
    # Never repair/delete an existing user cache or trust a self-written tree hash.
    $Extracting = "$Destination.verify-$([guid]::NewGuid().ToString('N'))"
    $keepSnapshot=$false
    try {
        New-Item -ItemType Directory -Path $Extracting | Out-Null
        & tar.exe -xzf $Archive -C $Extracting --strip-components 1
        if ($LASTEXITCODE -ne 0) { throw "Source extraction failed: $Name" }
        $expected=Get-SourceTreeHash $Extracting
        if(Test-Path -LiteralPath $Destination) {
            if((Get-SourceTreeHash $Destination) -cne $expected){throw "Cached source tree differs from verified archive; preserved for inspection: $Destination"}
        } else {
            Set-Content -LiteralPath (Join-Path $Extracting '.kenshicoop-source-sha256') -Value $Pin.sha256 -Encoding ascii
            Move-Item -LiteralPath $Extracting -Destination $Destination
            if($Snapshot){return (Get-PinnedSource $Name $Pin -Snapshot)}
        }
        if($Snapshot){$keepSnapshot=$true;return [pscustomobject]@{Path=$Extracting;ExpectedHash=$expected}}
        if($Verified){return [pscustomobject]@{Path=$Destination;ExpectedHash=$expected}}
        return $Destination
    } finally {
        $archiveHandle.Dispose()
        if(-not $keepSnapshot -and (Test-Path -LiteralPath $Extracting)){Remove-Item -LiteralPath $Extracting -Recurse -Force}
    }
}
if($SourcesOnly){return}
# Never inherit v100/SDK7 compiler selection from a caller. VS CMake's generator
# resolves v143/SDK10 independently, before build_plugin enters the v100 env.
foreach ($name in @('INCLUDE','LIB','LIBPATH','CC','CXX','CL','_CL_','LINK','VSINSTALLDIR','VCINSTALLDIR','VCIDEInstallDir','VCToolsInstallDir','VCToolsVersion','VCTargetsPath','VisualStudioVersion','DevEnvDir','WindowsSdkDir','WindowsSDKVersion','WindowsSdkBinPath','UniversalCRTSdkDir','UCRTVersion','VSCMD_ARG_TGT_ARCH','VSCMD_ARG_HOST_ARCH','VSCMD_VER')) {
    [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
}
$env:PATH = (($env:PATH -split ';') | Where-Object { $_ -notmatch '(?i)Microsoft Visual Studio 10\.0|Microsoft SDKs\\Windows\\v7\.1' }) -join ';'
$CMakePrefix = @()
if ($CMake) {
    if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) { throw "CMake executable not found: $CMake" }
} elseif (Get-Command cmake -ErrorAction SilentlyContinue) {
    $CMake = (Get-Command cmake).Source
} elseif (Get-Command uv -ErrorAction SilentlyContinue) {
    $CMake = (Get-Command uv).Source
    $CMakePrefix = @('tool', 'run', '--from', 'cmake==4.4.4', 'cmake')
} else {
    throw 'Install CMake >= 3.24 or uv, plus Visual Studio 2022 Build Tools and Windows SDK.'
}
Assert-SourceAncestors $BuildRoot
$snapshots=@();$inputHandles=@();$BuildWork=$null
try {
foreach($file in @(Get-CoopInputFiles Native)){$inputHandles+=[IO.File]::Open($file.FullName,'Open','Read','Read')}
if($SourceDigest -cne (Get-CoopDigest Native)){throw 'Native inputs changed before compilation ownership'}
$protobuf=Get-PinnedSource 'protobuf' $Pins.protobuf -Snapshot;$snapshots+=$protobuf.Path
$gns=Get-PinnedSource 'gns' $Pins.gns -Snapshot;$snapshots+=$gns.Path
$ProtobufSource=$protobuf.Path;$GnsSource=$gns.Path
$ProtobufTree=$protobuf.ExpectedHash;$GnsTree=$gns.ExpectedHash
foreach($snapshot in $snapshots){foreach($file in @(Get-ChildItem -LiteralPath $snapshot -Recurse -File -Force)){$inputHandles+=[IO.File]::Open($file.FullName,'Open','Read','Read')}}
if($ProtobufTree -cne (Get-SourceTreeHash $ProtobufSource) -or $GnsTree -cne (Get-SourceTreeHash $GnsSource)){throw 'Verified snapshot changed before build'}
    # Unique generated trees cannot carry unchecked cached objects into a receipt.
    $BuildWork=Join-Path $BuildRoot ('build-run-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $BuildWork|Out-Null
$ProtobufBuild = Join-Path $BuildWork 'protobuf'
$ProtobufInstall = Join-Path $ProtobufBuild 'install'
# Use the top-level entrypoint: the deprecated cmake/ wrapper enables MSVC
# before CMP0091 is selected and silently builds /MD despite the static option.
Invoke-CMake @('-S', $ProtobufSource, '-B', $ProtobufBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143', '-Dprotobuf_BUILD_TESTS=OFF',
    '-Dprotobuf_BUILD_SHARED_LIBS=OFF', '-Dprotobuf_MSVC_STATIC_RUNTIME=ON',
    '-Dprotobuf_WITH_ZLIB=OFF', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', "-DCMAKE_INSTALL_PREFIX=$ProtobufInstall")
Invoke-CMake @('--build', $ProtobufBuild, '--config', 'Release', '--clean-first', '--parallel', "$Jobs")
Invoke-CMake @('--install', $ProtobufBuild, '--config', 'Release')
# Keep this checkout's outputs separate from any other branch using the same source cache.
$Hasher = [Security.Cryptography.SHA256]::Create()
try { $CheckoutId = ([BitConverter]::ToString($Hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($Repo))).Replace('-', '').Substring(0, 16)).ToLowerInvariant() }
finally { $Hasher.Dispose() }
$PublishBin=Join-Path $BuildRoot "providers-v143-$CheckoutId-$Configuration\bin"
$NativeBuild = Join-Path $BuildWork 'providers'
Invoke-CMake @('-S', $Source, '-B', $NativeBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143',
    "-DCOOP_PROTOBUF_ROOT=$ProtobufInstall", "-DCOOP_CONFIGURATION=$Configuration", "-DFETCHCONTENT_SOURCE_DIR_GNS=$GnsSource")
$providerBuildStarted=[DateTime]::UtcNow
Invoke-CMake @('--build', $NativeBuild, '--config', 'Release', '--clean-first', '--parallel', "$Jobs")
$Bin = Join-Path $NativeBuild 'bin'
if ($SourceDigest -cne (Get-CoopDigest Native)) { throw 'Native sources changed during build; receipt not written.' }
if($ProtobufTree -cne (Get-SourceTreeHash $ProtobufSource) -or $GnsTree -cne (Get-SourceTreeHash $GnsSource)){throw 'Dependency sources changed during build; receipt not written.'}
$Files = @{}
foreach ($Name in @('KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','transport_integration.exe')) {
    if((Get-Item -LiteralPath (Join-Path $Bin $Name)).LastWriteTimeUtc -lt $providerBuildStarted){throw "Native output was not freshly rebuilt: $Name"}
    $Files[$Name] = (Get-FileHash -LiteralPath (Join-Path $Bin $Name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ($SourceDigest -cne (Get-CoopDigest Native)) { throw 'Native sources changed before receipt write.' }
$Receipt = [ordered]@{schema=1;transportAbi=3;checkout=$script:NativeRepo;configuration=$Configuration;sourceDigest=$SourceDigest;dependencies=$Pins;files=$Files}
Assert-SourceAncestors $PublishBin
$outputNames=@($Files.Keys)+@('KenshiCoopSteamProbe.dll')
$receiptPath=Join-Path $PublishBin 'native-build.json'
foreach($Name in $outputNames+@('native-build.json')){
    $destination=Join-Path $PublishBin $Name
    if(Test-Path -LiteralPath $destination){$entry=Get-Item -LiteralPath $destination -Force;if($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unsafe native output destination: $destination"}}
}
New-Item -ItemType Directory -Path $PublishBin -Force|Out-Null
foreach($Name in $outputNames){Copy-Item -LiteralPath (Join-Path $Bin $Name) -Destination (Join-Path $PublishBin $Name) -Force}
foreach($Name in $Files.Keys){if((Get-FileHash -LiteralPath (Join-Path $PublishBin $Name)).Hash.ToLowerInvariant() -cne $Files[$Name]){throw "Native publication hash mismatch: $Name"}}
[IO.File]::WriteAllText($receiptPath, ($Receipt | ConvertTo-Json -Depth 10))
Write-Output "Native binaries: $PublishBin"
Write-Output "Exact configuration/source/hash receipt: $receiptPath"
} finally {
    foreach($handle in $inputHandles){$handle.Dispose()}
    foreach($snapshot in $snapshots){Remove-Item -LiteralPath $snapshot -Recurse -Force}
    if($BuildWork -and (Test-Path -LiteralPath $BuildWork)){Remove-Item -LiteralPath $BuildWork -Recurse -Force}
}
