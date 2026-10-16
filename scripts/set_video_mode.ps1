<#
.SYNOPSIS
  Set the windowed Video Mode (client size) in BOTH Kenshi installs' kenshi.cfg.

.DESCRIPTION
  Kenshi reads its window size from kenshi.cfg ("Video Mode=W x H @ 32-bit
  colour [0]"); the window tiler (arrange_windows.ps1) only MOVES windows, so
  the size each session wants must be written here before launch.   The two
  retained harnesses call this before launching only their owned game trees.
  Private runners bound the requested render size to a non-primary monitor
  and restore the exact previous config bytes in their encompassing finally.
  Idempotent: only rewrites a cfg whose current mode differs. Also enforces
  windowed mode (Full Screen=No), which side-by-side placement requires.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\set_video_mode.ps1 -Width 1700 -Height 1350
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'kit_preflight.ps1')
if ($Width -le 0 -or $Height -le 0) { throw 'Positive window dimensions required.' }

foreach ($dir in @($HostDir, $JoinDir) | Select-Object -Unique) {
    $cfg = Join-Path $dir "kenshi.cfg"
    Assert-CoopOwnedPath -Path $cfg
    if (-not (Test-Path -LiteralPath $cfg -PathType Leaf)) { throw "Existing kenshi.cfg required before safe windowed launch: $cfg" }
    $bytes=[IO.File]::ReadAllBytes($cfg)
    $encoding=[Text.Encoding]::GetEncoding(28591)
    if ($bytes.Length -ge 4 -and (($bytes[0] -eq 255 -and $bytes[1] -eq 254 -and $bytes[2] -eq 0 -and $bytes[3] -eq 0) -or
        ($bytes[0] -eq 0 -and $bytes[1] -eq 0 -and $bytes[2] -eq 254 -and $bytes[3] -eq 255))) { throw 'UTF-32 config is unsupported; no bytes changed.' }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) { $encoding=New-Object Text.UnicodeEncoding($false,$true,$true) }
    elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) { $encoding=New-Object Text.UnicodeEncoding($true,$true,$true) }
    elseif ($bytes -contains 0) { throw 'Unrecognized config encoding; no bytes changed.' }
    # Latin-1 is a reversible byte view for ASCII keys in UTF-8/legacy files;
    # UTF-16 BOM remains part of the decoded text and is re-encoded unchanged.
    $text=$encoding.GetString($bytes)
    $mode='(?m)^(\uFEFF|\xEF\xBB\xBF)?Video Mode=[^\r\n]*'
    $fullscreen='(?m)^(\uFEFF|\xEF\xBB\xBF)?Full Screen=[^\r\n]*'
    if ([regex]::Matches($text,$mode).Count -ne 1 -or [regex]::Matches($text,$fullscreen).Count -ne 1) {
        throw 'Exactly one recognized Video Mode and Full Screen key required before safe launch.'
    }
    $selected=[regex]::Replace($text,$mode,('${1}'+"Video Mode=$Width x $Height @ 32-bit colour [0]"))
    $selected=[regex]::Replace($selected,$fullscreen,'${1}Full Screen=No')
    if ($selected -cne $text) {
        [IO.File]::WriteAllBytes($cfg,$encoding.GetBytes($selected))
        Write-Host "  video mode -> ${Width}x${Height} (windowed) in $cfg"
    } else {
        Write-Host "  video mode already ${Width}x${Height} (windowed) in $cfg"
    }
}
