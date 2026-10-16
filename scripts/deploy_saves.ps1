<#
.SYNOPSIS
  Restore pristine fixture save(s) from the repo (fixtures\saves\<name>) into the
  live Kenshi save folder (%LOCALAPPDATA%\kenshi\save\<name>) before a test run.

.DESCRIPTION
  Restores into the explicitly selected -SaveRoot (AppData by default). Runners
  must pass each installation's configured root. Each save is staged, hashed and
  atomically replaced; staging failures leave the previous save untouched.

  Uses robocopy /MIR, so the destination becomes an EXACT copy of the repo fixture
  (stale drift files are removed). Debug saves that have no repo fixture are skipped.

.EXAMPLE
  # Restore one save (what run_test.ps1 calls automatically):
  powershell -ExecutionPolicy Bypass -File scripts\deploy_saves.ps1 -Save sync

.EXAMPLE
  # Restore every tracked fixture (e.g. once before a full regression):
  powershell -ExecutionPolicy Bypass -File scripts\deploy_saves.ps1 -All
#>
[CmdletBinding()]
param(
    # Single fixture name to restore. Ignored when -All is given.
    [string]$Save = "",
    # Restore every fixture present under fixtures\saves.
    [switch]$All,
    # Restore even if a Kenshi process is running (unsafe: may fight a mid-write).
    [switch]$Force,
    # Override the live save root (defaults to the per-user AppData folder).
    [string]$SaveRoot = (Join-Path $env:LOCALAPPDATA "kenshi\save")
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot    = Split-Path -Parent $scriptDir
$fixturesDir = Join-Path $repoRoot "fixtures\saves"
. (Join-Path $scriptDir 'capture_save.ps1')
$SaveRoot=Get-CoopSaveFullPath $SaveRoot
$fixturesDir=Get-CoopSaveFullPath $fixturesDir
if (-not $All) { Assert-CoopSaveName $Save }
if (Test-CoopSavePathOverlap $fixturesDir $SaveRoot) { throw 'Live save root and pristine fixture store must not overlap.' }
Assert-CoopSavePath $fixturesDir
Assert-CoopSavePath $SaveRoot

if (-not $All -and $Save -eq "") {
    throw "Specify -Save <name> or -All."
}
if (-not (Test-Path $fixturesDir)) {
    throw "Fixture store not found: $fixturesDir"
}

# Refuse to clobber a save the game might be mid-writing.
if (-not $Force) {
    $live = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($live.Count -gt 0) {
        throw "Kenshi is running ($($live.Count) process(es)); refusing to restore saves. Close it or pass -Force."
    }
}

# Resolve the target list.
if ($All) {
    $targets = @(Get-ChildItem -LiteralPath $fixturesDir -Directory | ForEach-Object Name)
    if (-not $targets -or $targets.Count -eq 0) { Write-Host "No fixtures under $fixturesDir; nothing to restore."; exit 0 }
} else {
    $targets = @($Save)
}

$restored = 0
$skipped  = 0
foreach ($name in $targets) {
    Assert-CoopSaveName $name
    $src = Join-Path $fixturesDir $name
    if (-not (Test-Path -LiteralPath (Join-Path $src "quick.save") -PathType Leaf)) {
        # No repo fixture (e.g. a debug save): leave whatever is in AppData as-is.
        Write-Host "  skip '$name' (no repo fixture)"
        $skipped++
        continue
    }
    $dst = Join-Path $SaveRoot $name
    Copy-CoopSaveTransaction -Source $src -Destination $dst
    Write-Host "  restored '$name' -> $dst"
    $restored++
}

Write-Host "Saves restored: $restored (skipped $skipped)."
exit 0
