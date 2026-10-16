# Local manifest/command preparation only: never uploads, tags, pushes or mutates
# the stable feed. -Local and -WhatIf are strictly read-only, including receipts.
[CmdletBinding()]
param([string]$Owner='kotetsyy',[string]$Repo='KenshiCoop4up',[string]$NativeBin='',
      [string]$Kit='dist\KenshiCoop-kit.zip',[string]$ReleaseNotes='dist\RELEASE_NOTES.md',
      [string]$Out='',[string]$Notes='', [switch]$Amend,[switch]$WhatIf,[switch]$Local,[switch]$WriteManifest)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if($Owner -cne 'kotetsyy' -or $Repo -cne 'KenshiCoop4up'){throw 'Publisher repository is fixed to kotetsyy/KenshiCoop4up.'}
if($Notes -match '[\r\n\x00]'){throw 'Notes must be one line.'}
. (Join-Path $PSScriptRoot 'RuntimeDistribution.ps1') -Configuration Release -NativeBin $NativeBin
$files=Get-CoopRuntimeFiles -Configuration Release -NativeBin $NativeBin
$wire=Get-Content -LiteralPath (Join-Path $root 'src\netproto\Wire.h') -Raw
if($wire -notmatch 'COOP_BUILD_VERSION\s*=\s*"(\d+\.\d+\.\d+)"'){throw 'Exact build version missing.'}
$version=$Matches[1]
$releaseBranch=[regex]::Match($wire,'(?s)#ifdef\s+KENSHICOOP_NET_DIAG\b.*?#else\b(.*?)#endif\b')
if(-not $releaseBranch.Success){throw 'Explicit public Release protocol branch missing.'}
$protocols=[regex]::Matches($releaseBranch.Groups[1].Value,'PROTOCOL_VERSION\s*=\s*(\d+)')
if($protocols.Count -ne 1){throw 'Ambiguous Release protocol.'};$proto=$protocols[0].Groups[1].Value
$kitPath=if([IO.Path]::IsPathRooted($Kit)){$Kit}else{Join-Path $root $Kit}
Assert-CoopRuntimeZip -Zip $kitPath -Files $files
$runtimeHashes=Get-CoopRuntimeHashes $files
$notesPath=if([IO.Path]::IsPathRooted($ReleaseNotes)){$ReleaseNotes}else{Join-Path $root $ReleaseNotes}
if(-not(Test-Path -LiteralPath $notesPath -PathType Leaf)){throw 'Release notes missing.'}
$tag="v$version";$lines=@('# Schema 2: old updaters deliberately reject before download.','schema=2','transportAbi=3',"version=$version","proto=$proto")
$keys=@('core','ui','steam','udp');$names=@('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll')
for($i=0;$i -lt 4;++$i){$lines+="$($keys[$i])Sha256=$($runtimeHashes[$names[$i]])";$lines+="$($keys[$i])Url=https://github.com/$Owner/$Repo/releases/download/$tag/$($names[$i])"}
$lines+="notes=$Notes";$manifest=($lines -join "`n")+"`n"
$bytes=[Text.Encoding]::UTF8.GetBytes($manifest)
if($bytes.Length -gt 65536){throw 'Manifest exceeds native updater 65536-byte UTF-8 limit.'}
# Validate the exact local schema before any candidate write.
$fields=@{}
foreach($line in $lines){if($line.StartsWith('#')){continue};$pair=$line.Split([char[]]'=',2);if($pair.Count -ne 2 -or $fields.ContainsKey($pair[0])){throw 'Malformed/duplicate manifest field'};$fields[$pair[0]]=$pair[1]}
$allowed=@('schema','transportAbi','version','proto','notes','coreSha256','coreUrl','uiSha256','uiUrl','steamSha256','steamUrl','udpSha256','udpUrl')
if(Compare-Object $allowed @($fields.Keys) -CaseSensitive){throw 'Manifest schema mismatch'}
foreach($key in $keys){if($fields["${key}Sha256"] -cnotmatch '^[0-9a-f]{64}$'){throw 'Invalid component hash'}}
Write-Output $manifest
if($WriteManifest -and -not $Local -and -not $WhatIf) {
    if(-not $Out){throw 'Explicit non-stable -Out is required for -WriteManifest.'}
    $outPath=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($Out)){$Out}else{Join-Path $root $Out}))
    if($outPath -ieq (Join-Path $root 'dist\UPDATE.txt') -or $outPath -ieq (Join-Path $root 'dist\RELEASE_NOTES.md')){throw 'Stable feed/notes mutation forbidden.'}
    $stream=[IO.File]::Open($outPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
} else {Write-Host 'Read-only local/WhatIf preparation: no files, feeds or release assets changed.'}
$assets='"'+$kitPath+'" '+(($names|ForEach-Object{'"'+$files[$_]+'"'}) -join ' ')
if($Amend){Write-Host "UNEXECUTED: gh release upload $tag $assets --repo $Owner/$Repo --clobber"}
else{Write-Host "UNEXECUTED: gh release create $tag $assets --repo $Owner/$Repo --verify-tag --title $tag --notes-file `"$notesPath`""}
Write-Host 'LOCAL ONLY. Two-account Steam P2P is unverified; no stable publication is authorized.'
