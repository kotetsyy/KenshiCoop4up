<#
.SYNOPSIS
  Package one core/UI build pair as a four-file drop-in KenshiCoop mod.

.DESCRIPTION
  The archive contains only KenshiCoop.dll, KenshiCoopUI.dll, KenshiCoop.mod
  and RE_Kenshi.json inside KenshiCoop/. No launchers, shipped config,
  README, provenance or source archive. F2 writes the local configuration;
  release instructions and corresponding sources are provided separately.

  Release -> dist/KenshiCoop-kit.zip
  Harness -> dist/KenshiCoop-dev-kit.zip (private protocol)

.EXAMPLE
  pwsh -NoProfile -File scripts/make_mod_kit.ps1

.EXAMPLE
  pwsh -NoProfile -File scripts/make_mod_kit.ps1 -SkipBuild -Configuration Harness
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [ValidateSet("Release", "Harness")]
    [string]$Configuration = "Release",
    # Where to find KenshiCoop.mod / RE_Kenshi.json if they aren't in dist\mods.
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    Write-Host "=== build plugin ($Configuration) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" $Configuration"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# Resolve the mod files from the first place each exists.
function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
# The two DLLs are one canonical build pair, never a stale distribution fallback.
$dll = Join-Path $repoRoot "src\plugin\x64\$Configuration\KenshiCoop.dll"
$uiDll = Join-Path $repoRoot "src\ui\x64\$Configuration\KenshiCoopUI.dll"
if (-not (Test-Path $dll) -or -not (Test-Path $uiDll)) {
    throw "$Configuration DLL pair not found; build both DLLs first"
}
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
$canonUiSha = (Get-FileHash -Algorithm SHA256 $uiDll).Hash
Write-Host "Canonical $Configuration core SHA-256: $canonSha"
Write-Host "Canonical $Configuration UI SHA-256: $canonUiSha"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir  "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"
$mod  = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir  "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"

# Rebuild dist\mod-kit from scratch so no stale install script survives.
$kitName = if ($Configuration -eq "Release") { "mod-kit" } else { "dev-mod-kit" }
$kitDir = Join-Path $repoRoot "dist\$kitName"
$modDir  = Join-Path $kitDir "KenshiCoop"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling KenshiCoop mod folder ==="
Copy-Item $dll  (Join-Path $modDir "KenshiCoop.dll")
Copy-Item $uiDll (Join-Path $modDir "KenshiCoopUI.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "KenshiCoop.mod")

# Verify the packaged pair without adding build metadata to the runtime archive.
$packagedSha = (Get-FileHash -Algorithm SHA256 (Join-Path $modDir "KenshiCoop.dll")).Hash
if ($packagedSha -ne $canonSha) {
    throw "packaged DLL hash ($packagedSha) != canonical Release DLL hash ($canonSha)"
}
$packagedUiSha = (Get-FileHash -Algorithm SHA256 (Join-Path $modDir "KenshiCoopUI.dll")).Hash
if ($packagedUiSha -ne $canonUiSha) {
    throw "packaged UI DLL hash ($packagedUiSha) != canonical Release UI DLL hash ($canonUiSha)"
}
Write-Host "Packaged DLL pair SHA-256 verified == canonical."

# Zip only the mod folder; instructions and sources are separate release assets.
$zipName = if ($Configuration -eq "Release") { "KenshiCoop-kit.zip" } else { "KenshiCoop-dev-kit.zip" }
$zip = Join-Path $repoRoot "dist\$zipName"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $modDir -DestinationPath $zip

Write-Host ""
Write-Host "Mod folder: $modDir"
Write-Host "Kit zipped: $zip"
Get-ChildItem -Recurse $kitDir | ForEach-Object {
    Write-Host ("  " + $_.FullName.Substring($kitDir.Length + 1))
}
