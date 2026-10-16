# Calls production source provisioning, without CMake, compilation or network.
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('coop-cache-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
function New-FixtureLink([string]$Path,[string]$Target) {
    & cmd.exe /d /c "mklink `"$Path`" `"$Target`"" | Out-Null
    if($LASTEXITCODE -ne 0){throw "Fixture symbolic-link creation failed: $Path"}
}
try {
    . (Join-Path (Split-Path $PSScriptRoot -Parent) 'build_transports.ps1') -BuildRoot $root -SourcesOnly
    $fixture=Join-Path $root 'fixture';$top=Join-Path $fixture 'pinned'
    New-Item -ItemType Directory -Path $top -Force|Out-Null
    [IO.File]::WriteAllText((Join-Path $top 'source.cpp'),"pinned-source`n",[Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $top 'alternative.cpp'),"pinned-source`n",[Text.Encoding]::ASCII)
    New-Item -ItemType Directory -Path (Join-Path $top 'nested')|Out-Null
    New-FixtureLink (Join-Path $top 'same-directory.cpp') 'source.cpp'
    New-FixtureLink (Join-Path $top 'nested\parent-relative.cpp') '../source.cpp'
    $cache=Join-Path $root 'sources';New-Item -ItemType Directory -Path $cache -Force|Out-Null
    $archive=Join-Path $cache 'fixture-testcommit.tar.gz'
    & tar.exe -czf $archive -C $fixture pinned
    if($LASTEXITCODE -ne 0){throw 'Fixture archive creation failed'}
    $pin=[pscustomobject]@{commit='testcommit';sha256=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant();url='https://invalid.test/no-network'}
    $destination=Get-PinnedSource 'fixture' $pin
    if([IO.File]::ReadAllText((Join-Path $destination 'source.cpp')) -cne "pinned-source`n"){throw 'Pinned archive source was not extracted'}
    if((Get-PinnedSource 'fixture' $pin) -cne $destination){throw 'Verified source reuse changed path'}
    $verified=Get-PinnedSource 'fixture' $pin -Verified
    if($verified.Path -cne $destination -or $verified.ExpectedHash -cne (Get-SourceTreeHash $destination)){throw 'Archive-derived baseline not retained'}
    [IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),'mutation-after-provision')
    if($verified.ExpectedHash -ceq (Get-SourceTreeHash $destination)){throw 'Mutable cache re-established trusted baseline'}
    [IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),"pinned-source`n",[Text.Encoding]::ASCII)
    $snapshot=Get-PinnedSource 'fixture' $pin -Snapshot
    try{
        if($snapshot.Path -ceq $destination -or $snapshot.ExpectedHash -cne (Get-SourceTreeHash $snapshot.Path)){throw 'Build snapshot not archive-derived/private'}
        [IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),'changed-user-cache')
        if((Get-SourceTreeHash $snapshot.Path) -cne $snapshot.ExpectedHash){throw 'Mutable cache changed compilation snapshot'}
    }finally{[IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),"pinned-source`n",[Text.Encoding]::ASCII);Remove-Item $snapshot.Path -Recurse -Force}
    $held=Open-VerifiedSourceInput $archive $pin.sha256
    try {
        $rejected=$false;try{[IO.File]::AppendAllText($archive,'race')}catch{$rejected=$true}
        if(-not $rejected){throw 'Verified archive can change before consumption'}
        & tar.exe -tzf $archive|Out-Null
        if($LASTEXITCODE -ne 0){throw 'tar cannot consume held verified archive'}
    }finally{$held.Dispose()}
    if([IO.File]::ReadAllText((Join-Path $destination 'nested\parent-relative.cpp')) -cne "pinned-source`n"){throw 'Pinned internal relative file symlink was not consumed'}
    $marker=Join-Path $destination '.kenshicoop-source-sha256';$beforeMarker=[IO.File]::ReadAllBytes($marker)
    [IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),'tampered-source')
    [IO.File]::WriteAllText((Join-Path $cache 'unrelated-user-file'),'preserve-user-cache')
    $rejected=$false;try{Get-PinnedSource 'fixture' $pin|Out-Null}catch{$rejected=$true}
    if(-not $rejected){throw 'Tampered source accepted with intact archive marker'}
    if([IO.File]::ReadAllText((Join-Path $destination 'source.cpp')) -cne 'tampered-source'){throw 'Rejected source evidence was modified'}
    if(Compare-Object $beforeMarker ([IO.File]::ReadAllBytes($marker))){throw 'Rejected source marker was modified'}
    if([IO.File]::ReadAllText((Join-Path $cache 'unrelated-user-file')) -cne 'preserve-user-cache'){throw 'Unrelated user cache modified'}
    [IO.File]::WriteAllText((Join-Path $destination 'source.cpp'),"pinned-source`n",[Text.Encoding]::ASCII)
    # Same target bytes cannot disguise a changed link target.
    $link=Join-Path $destination 'same-directory.cpp'
    Remove-Item -LiteralPath $link -Force
    New-FixtureLink $link 'alternative.cpp'
    $rejected=$false;try{Get-PinnedSource 'fixture' $pin|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Changed internal symlink accepted with identical target bytes'}
    if(@((Get-Item -LiteralPath $link -Force).Target)[0] -cne 'alternative.cpp'){throw 'Rejected link evidence was modified'}
    Remove-Item -LiteralPath $link -Force;New-FixtureLink $link 'source.cpp'
    $outside=Join-Path $root 'outside';New-Item -ItemType Directory -Path $outside|Out-Null
    [IO.File]::WriteAllText((Join-Path $outside 'outside.cpp'),'preserve-outside-source')
    foreach($unsafe in @((Join-Path $outside 'outside.cpp'),'../../outside/outside.cpp','same-directory.cpp')) {
        $unsafeLink=Join-Path $destination 'unsafe.cpp';New-FixtureLink $unsafeLink $unsafe
        $rejected=$false;try{Get-SourceTreeHash $destination|Out-Null}catch{$rejected=$true};if(-not $rejected){throw "Unsafe source link accepted: $unsafe"}
        if(@((Get-Item -LiteralPath $unsafeLink -Force).Target)[0] -cne $unsafe){throw 'Rejected unsafe link evidence was modified'}
        Remove-Item -LiteralPath $unsafeLink -Force
    }
    $junction=Join-Path $destination 'external-directory'
    & cmd.exe /d /c "mklink /J `"$junction`" `"$outside`"" | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Fixture directory-junction creation failed'}
    New-FixtureLink (Join-Path $destination 'through-junction.cpp') 'external-directory/outside.cpp'
    $rejected=$false;try{Get-SourceTreeHash $destination|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Directory junction or link with reparse ancestor accepted'}
    if([IO.File]::ReadAllText((Join-Path $outside 'outside.cpp')) -cne 'preserve-outside-source'){throw 'External source was modified'}
    Remove-Item -LiteralPath (Join-Path $destination 'through-junction.cpp') -Force
    [IO.Directory]::Delete($junction)
    # GNS uses the same verified, unpatched archive contract as other sources.
    Copy-Item -LiteralPath $archive -Destination (Join-Path $cache 'gns-testcommit.tar.gz')
    $legacyGns=Join-Path $cache 'gns-testcommit'
    New-Item -ItemType Directory -Path $legacyGns|Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyGns 'source.cpp'),"legacy-patched-source`n",[Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $legacyGns '.kenshicoop-source-sha256'),$pin.sha256,[Text.Encoding]::ASCII)
    $gns=Get-PinnedSource 'gns' $pin
    if([IO.File]::ReadAllText((Join-Path $gns 'source.cpp')) -cne "pinned-source`n"){throw 'Legacy patched GNS source reused'}
    if([IO.File]::ReadAllText((Join-Path $legacyGns 'source.cpp')) -cne "legacy-patched-source`n"){throw 'Legacy source cache was modified'}
    if((Get-PinnedSource 'gns' $pin) -cne $gns){throw 'Verified GNS source reuse rejected'}
    $gnsFile=Join-Path $gns 'source.cpp'
    [IO.File]::WriteAllText($gnsFile,"modified-source`n",[Text.Encoding]::ASCII)
    $rejected=$false;try{Get-PinnedSource 'gns' $pin|Out-Null}catch{$rejected=$true}
    if(-not $rejected){throw 'Modified GNS source accepted'}
    if([IO.File]::ReadAllText($gnsFile) -cne "modified-source`n"){throw 'Rejected GNS source evidence was modified'}
    $external=Join-Path $root 'external-cache';New-Item -ItemType Directory -Path $external|Out-Null
    Copy-Item $archive (Join-Path $external 'fixture-testcommit.tar.gz')
    $aliasRoot=Join-Path $root 'alias-build';New-Item -ItemType Directory -Path $aliasRoot|Out-Null
    $cacheAlias=Join-Path $aliasRoot 'sources'
    & cmd.exe /d /c "mklink /J `"$cacheAlias`" `"$external`""|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Cannot create first-provision cache junction'}
    $beforeExternal=@(Get-ChildItem $external -File|ForEach-Object{$_.Name+':'+(Get-FileHash $_.FullName).Hash})
    $savedBuildRoot=$BuildRoot;$BuildRoot=$aliasRoot
    try {
        $rejected=$false;try{Get-PinnedSource 'fixture' $pin|Out-Null}catch{$rejected=$true}
        if(-not $rejected){throw 'First-provision reparse cache accepted'}
        $afterExternal=@(Get-ChildItem $external -File|ForEach-Object{$_.Name+':'+(Get-FileHash $_.FullName).Hash})
        if(Compare-Object $beforeExternal $afterExternal){throw 'Reparse rejection wrote external cache'}
    }finally{$BuildRoot=$savedBuildRoot;[IO.Directory]::Delete($cacheAlias)}
    [IO.File]::AppendAllText($archive,'corrupt-archive')
    $rejected=$false;try{Get-PinnedSource 'fixture' $pin|Out-Null}catch{$rejected=$true};if(-not $rejected){throw 'Cached archive tampering accepted despite intact source marker'}
    Write-Host 'Dependency cache fixtures: production verified archive/tree/tamper rejection/unpatched GNS/user-cache preservation PASS'
} finally {Remove-Item -LiteralPath $root -Recurse -Force}
