<#
.SYNOPSIS
  Capture a closed, complete named save into fixtures/saves with verified staging
  and rollback. Use -SaveRoot for the game's selected installation-local root.
  bake_scene.ps1 -Promote additionally requires native completion and clean exit.
#>
[CmdletBinding()]
param([Alias('Save')][string]$CaptureSaveName, [Alias('SaveRoot')][string]$CaptureSaveRoot = (Join-Path $env:LOCALAPPDATA 'kenshi\save'))

function Assert-CoopSaveName {
    param([string]$Name, [switch]$Bake)
    if (-not $Name -or $Name -match '[\x00-\x1f\\/:*?"<>|]' -or $Name -match '[. ]$' -or
        $Name -in '.','..' -or $Name -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)' -or
        ($Bake -and [Text.Encoding]::UTF8.GetByteCount($Name) -gt 47)) {
        throw 'Save requires a canonical Windows leaf name (bake identity is limited to 47 UTF-8 bytes).'
    }
}
function Get-CoopSaveFullPath {
    param([string]$Path)
    if (-not $Path) { throw 'An explicit filesystem path is required.' }
    if ($Path -match '^\\\\[?.]\\') { throw 'Save paths must not use Windows device aliases.' }
    foreach ($component in ($Path -split '[\\/]')) {
        if ($component -notin '.','..' -and $component -match '[. ]$') { throw 'Save paths must not use trailing-dot/space aliases.' }
    }
    $full=[IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path))
    if ($full -eq [IO.Path]::GetPathRoot($full)) { return $full }
    return $full.TrimEnd('\','/')
}
function Test-CoopSavePathOverlap {
    param([string]$Left, [string]$Right)
    $a=Get-CoopSaveFullPath $Left; $b=Get-CoopSaveFullPath $Right
    return $a -ieq $b -or $a.StartsWith($b.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $b.StartsWith($a.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)
}
function Assert-CoopSavePath {
    param([string]$Path)
    $full=Get-CoopSaveFullPath $Path
    $cursor=$full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Save path traverses a reparse point: $cursor" }
        }
        $parent=Split-Path -Parent $cursor
        if ($parent -eq $cursor) { break }; $cursor=$parent
    }
    # Walk one level at a time: never recurse into a link before rejecting it.
    if (Test-Path -LiteralPath $full -PathType Container) {
        $pending=New-Object 'System.Collections.Generic.Stack[string]'; $pending.Push($full)
        while ($pending.Count) {
            foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Save tree contains a reparse point: $($item.FullName)" }
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            }
        }
    }
}
function Resolve-CoopSelectedSaveRoot {
    param([string]$GameDir)
    $game=Get-CoopSaveFullPath $GameDir
    $settings=Join-Path $game 'settings.cfg'; Assert-CoopSavePath $settings
    if (-not(Test-Path -LiteralPath $settings -PathType Leaf)) { throw "Missing save-location settings: $settings" }
    $keys=@(Get-Content -LiteralPath $settings | Where-Object { $_ -match '^\s*User save location\s*=' })
    if ($keys.Count -ne 1 -or $keys[0] -notmatch '^\s*User save location\s*=\s*([01])\s*$') { throw "Missing, ambiguous or invalid User save location in $settings" }
    $root=if ($Matches[1] -eq '0') { Join-Path $game 'save' } else { Join-Path $env:LOCALAPPDATA 'kenshi\save' }
    $root=Get-CoopSaveFullPath $root; Assert-CoopSavePath $root
    return $root
}
function Get-CoopSaveInventory {
    param([string]$Path)
    Assert-CoopSavePath $Path
    if (-not(Test-Path -LiteralPath (Join-Path $Path 'quick.save') -PathType Leaf)) { throw "Save has no literal quick.save: $Path" }
    $root=Get-CoopSaveFullPath $Path
    return @(Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $relative=$_.FullName.Substring($root.Length+1)
        if ($_.PSIsContainer) { 'D|'+$relative } else { 'F|'+$relative+'|'+$_.Length+'|'+(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function Copy-CoopSaveTransaction {
    param([string]$Source, [string]$Destination)
    $src=Get-CoopSaveFullPath $Source; $dst=Get-CoopSaveFullPath $Destination
    if (Test-CoopSavePathOverlap $src $dst) { throw 'Save source and destination must not overlap.' }
    Assert-CoopSavePath $src; Assert-CoopSavePath $dst
    if ((Test-Path -LiteralPath $dst) -and -not (Test-Path -LiteralPath $dst -PathType Container)) { throw "Save destination is not a directory: $dst" }
    $before=@(Get-CoopSaveInventory $src)
    $parent=Split-Path -Parent $dst
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $stage=Join-Path $parent ('.save-stage-'+[guid]::NewGuid().ToString('N'))
    $backup=Join-Path $parent ('.save-backup-'+[guid]::NewGuid().ToString('N'))
    $oldMoved=$false; $installed=$false
    try {
        $robocopy=Join-Path ([Environment]::GetFolderPath('System')) 'robocopy.exe'
        & $robocopy $src $stage /MIR /XJ /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
        $copyExit=$LASTEXITCODE
        if ($copyExit -ge 8) { throw "Save staging failed (robocopy rc=$copyExit)." }
        $copied=@(Get-CoopSaveInventory $stage); $after=@(Get-CoopSaveInventory $src)
        if (($before -join "`n") -cne ($after -join "`n") -or ($before -join "`n") -cne ($copied -join "`n")) { throw 'Source changed or staged save inventory/content is incomplete.' }
        Assert-CoopSavePath $dst
        if (Test-Path -LiteralPath $dst) { [IO.Directory]::Move($dst,$backup); $oldMoved=$true }
        [IO.Directory]::Move($stage,$dst); $installed=$true
    } catch {
        if ($installed -and (Test-Path -LiteralPath $dst)) { Remove-Item -LiteralPath $dst -Recurse -Force }
        if ($oldMoved) { [IO.Directory]::Move($backup,$dst); $oldMoved=$false }
        throw
    } finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
        if ($installed -and $oldMoved) { Remove-Item -LiteralPath $backup -Recurse -Force }
    }
}
if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference='Stop'
Assert-CoopSaveName $CaptureSaveName
$repoRoot=Split-Path $PSScriptRoot -Parent
$fixtures=Join-Path $repoRoot 'fixtures\saves'
$src=Join-Path (Get-CoopSaveFullPath $CaptureSaveRoot) $CaptureSaveName
$dst=Join-Path $fixtures $CaptureSaveName
if (Test-CoopSavePathOverlap $src $fixtures) { throw 'Capture source overlaps the pristine fixture store.' }
if (@(Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue).Count) { throw 'Close Kenshi before capturing a save.' }
Copy-CoopSaveTransaction $src $dst
Write-Host "Captured verified save '$CaptureSaveName' into $dst. Review the fixture changes before committing."
