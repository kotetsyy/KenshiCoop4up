# Exact six-file runtime ZIP. No config/credentials/provenance/test/source extras.
[CmdletBinding()]
param([switch]$SkipBuild,[ValidateSet('Release','Harness')][string]$Configuration='Release',[string]$NativeBin='', [string]$OutputZip='')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(-not $SkipBuild) {
    & cmd.exe /c "`"$PSScriptRoot\build_plugin.cmd`" $Configuration"
    if($LASTEXITCODE -ne 0){throw "Complete runtime build failed ($LASTEXITCODE)"}
}
. (Join-Path $PSScriptRoot 'RuntimeDistribution.ps1') -Configuration $Configuration -NativeBin $NativeBin
$files=Get-CoopRuntimeFiles -Configuration $Configuration -NativeBin $NativeBin
if(-not $OutputZip){$name=if($Configuration -eq 'Release'){'KenshiCoop-kit.zip'}else{'KenshiCoop-dev-kit.zip'};$OutputZip=Join-Path $repo "dist\$name"}
$OutputZip=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputZip)
New-CoopRuntimeKit -Files $files -Zip $OutputZip
foreach($name in ($files.Keys|Sort-Object)){Write-Host "$name SHA-256: $((Get-FileHash -LiteralPath $files[$name] -Algorithm SHA256).Hash.ToLowerInvariant())"}
Write-Host "Exact six-file $Configuration kit: $OutputZip"
