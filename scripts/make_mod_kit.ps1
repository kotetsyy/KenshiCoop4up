<#
.SYNOPSIS
  Package the PLAYER release: a single folder called "KenshiCoop" with the mod
  files inside, that a player copies straight into <Kenshi>\mods\. No install
  scripts, no launchers, no bundled save.

.DESCRIPTION
  Assembles dist\mod-kit\ as:
    KenshiCoop\                 <- the drop-in mod folder (copy this into mods\)
      KenshiCoop.dll              the plugin (protocol-version-matched; a mismatch
                                  is rejected at handshake by design)
      KenshiCoopUI.dll            the F2 panel/banner companion; KenshiCoop.dll
                                  loads it from this folder (same build pair)
      KenshiCoop.mod              mod-list entry so it shows in Kenshi's Mods menu
      RE_Kenshi.json              tells RE_Kenshi to load the plugin
      coop_config.json            defaults and settings remembered by F2;
                                  both Steam and UDP are configured in-game
    README.txt                  <- plain copy-the-folder instructions (NOT copied
                                  into mods, so it never clutters the game folder)
  ...then zips it to dist\KenshiCoop-kit.zip (the release artifact the README
  and the GitHub release point at).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1

.EXAMPLE
  # Reuse the current build instead of recompiling.
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # Where to find KenshiCoop.mod / RE_Kenshi.json if they aren't in dist\mods.
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    # The PLAYER release ships the Release config: the shipped DLL excludes the
    # scenario harness (~12k lines) and does not define KENSHICOOP_HARNESS
    # (Phase 1 build separation). The test pipeline uses Harness instead.
    Write-Host "=== build plugin (Release / shipped, no scenario harness) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# Resolve the mod files from the first place each exists.
function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
# The two DLLs are one build pair: take both from the same place, never mix a
# fresh core with a stale UI (or vice versa).
$pairs = @(
    @((Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"),
      (Join-Path $repoRoot "src\ui\x64\Release\KenshiCoopUI.dll")),
    @((Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.dll"),
      (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoopUI.dll"))
)
$dll = $null; $uiDll = $null
foreach ($p in $pairs) {
    if ((Test-Path $p[0]) -and (Test-Path $p[1])) { $dll = $p[0]; $uiDll = $p[1]; break }
}
if (-not $dll) {
    throw "KenshiCoop.dll + KenshiCoopUI.dll pair not found (looked in: $(($pairs | ForEach-Object { $_ -join ' + ' }) -join '; '))"
}

# Canonical shipped-DLL hashes (Phase 1 provenance). Package from ONE pair and
# assert the packaged copies are byte-identical to it, so the release artifact's
# SHA-256 is verifiable rather than a mutable file tracked under dist\.
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
$canonUiSha = (Get-FileHash -Algorithm SHA256 $uiDll).Hash
Write-Host "Canonical Release DLL SHA-256:    $canonSha"
Write-Host "  source: $dll"
Write-Host "Canonical Release UI DLL SHA-256: $canonUiSha"
Write-Host "  source: $uiDll"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir  "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"
$mod  = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir  "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"

# Rebuild dist\mod-kit from scratch so no stale install script survives.
$kitDir  = Join-Path $repoRoot "dist\mod-kit"
$modDir  = Join-Path $kitDir "KenshiCoop"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling KenshiCoop mod folder ==="
Copy-Item $dll  (Join-Path $modDir "KenshiCoop.dll")
Copy-Item $uiDll (Join-Path $modDir "KenshiCoopUI.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "KenshiCoop.mod")

# F2 remembers role, transport, nick and endpoint in this file.
# The release ships clean defaults; editing it is not required for either transport.
@'
{
  // Configure the session in the F2 panel. Type or paste a nick first.
  // For Steam, the host shares its Steam ID and clients enter that ID.
  // For UDP, the host chooses a port and clients enter the host's ip:port.
  // The panel remembers these settings here; connecting is always an explicit action.
  "transport": "steam",
  "ip": "127.0.0.1",
  "port": 27800,
  "autoConnect": false
}
'@ | Set-Content (Join-Path $modDir "coop_config.json") -Encoding UTF8

# Top-level README (sibling to the KenshiCoop folder, so it is NOT copied into
# the game). Plain "copy the folder" instructions - no install script.
@'
KenshiCoop - co-op mod
======================

This zip contains ONE folder: "KenshiCoop". That folder IS the mod.

INSTALL (both players)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     Keep KenshiCoop.dll and KenshiCoopUI.dll together from the same zip.
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (both players)
----------------------------
  1. Kenshi 1.0.65 (Steam).
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. For the Steam transport (recommended): Steam RUNNING and ONLINE on both
     machines. No port forwarding, no IPs, no config editing - you swap Steam
     IDs in-game (see PLAY below).

PLAY (Steam - recommended)
--------------------------
  1. Press F2 to open the Co-op panel. It works at the MAIN MENU (before loading
     a game) as well as in-game.
  2. Type your nick (Cyrillic/editing/Ctrl+V supported), or click "Вставить".
     Tab moves between fields; Enter starts. Invalid fields block the start.
     Choose Host or Client; game controls are suppressed while editing.
  3. HOST: select Steam, click "Копировать" beside your Steam ID, and send the ID to clients.
     Click "Создать сессию", then load a save or start a new game.
  4. CLIENT: select Steam and type or paste the host's Steam ID into its field.
     Click "Подключиться" from the MAIN MENU; no local save is required.
     The host transfers its world, or an identical local copy is reused.
  5. The panel shows connection, world-transfer progress and player readiness.
     "Диагностика" expands details; "Копировать отчёт" copies the complete report.
     F2, Esc, the close button and "Скрыть (F2)" only hide the panel.
     Use "Остановить сессию" (host) or "Отключиться" (client) to stop networking.

PLAY (LAN / direct UDP - advanced)
----------------------------------
  Enter your nick and choose "Прямой IP (UDP)" in F2; no Steam IDs are needed.
  HOST: enter a port if different from 27800, then click "Создать сессию".
  CLIENT: enter the host's ip:port (for example 192.168.1.10:27800), then
  click "Подключиться". Internet play requires the host's UDP port to be reachable.
  The UDP port is shared between roles and remembered without resetting on a switch.
  Mod-update messages are separate from connection status. If diagnostics overflow,
  use "Копировать отчёт" for the complete report.
  Role, transport and endpoint are locked while the session is active.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE. The client
    must paste the HOST's Steam ID, not a lobby ID. The host has no peer-ID field.
    If pasting fails, have the host re-copy the ID using "Копировать мой Steam ID".
    Look for '[steam] session ... active=1' in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
'@ | Set-Content (Join-Path $kitDir "README.txt") -Encoding UTF8

# Provenance: assert the PACKAGED DLLs are byte-identical to the canonical pair,
# then record the hashes next to the kit so the release artifact is verifiable.
$packagedSha = (Get-FileHash -Algorithm SHA256 (Join-Path $modDir "KenshiCoop.dll")).Hash
if ($packagedSha -ne $canonSha) {
    throw "packaged DLL hash ($packagedSha) != canonical Release DLL hash ($canonSha)"
}
$packagedUiSha = (Get-FileHash -Algorithm SHA256 (Join-Path $modDir "KenshiCoopUI.dll")).Hash
if ($packagedUiSha -ne $canonUiSha) {
    throw "packaged UI DLL hash ($packagedUiSha) != canonical Release UI DLL hash ($canonUiSha)"
}
# Release uses the public branch, last in Wire.h; the private NET_DIAG branch is first.
$protoLine = Select-String -Path (Join-Path $repoRoot "src\netproto\Wire.h") `
    -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' | Select-Object -Last 1
$proto = if ($protoLine) { $protoLine.Matches[0].Groups[1].Value } else { "?" }
@{
    dllSha256       = $canonSha
    uiDllSha256     = $canonUiSha
    protocolVersion = $proto
    builtUtc        = (Get-Date).ToUniversalTime().ToString("o")
    config          = "Release"
} | ConvertTo-Json | Set-Content (Join-Path $kitDir "PROVENANCE.json") -Encoding UTF8
Write-Host "Packaged DLL pair SHA-256 verified == canonical."

# Zip: the archive contains the KenshiCoop\ folder + README.txt + PROVENANCE.json.
$zip = Join-Path $repoRoot "dist\KenshiCoop-kit.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $kitDir "*") -DestinationPath $zip

Write-Host ""
Write-Host "Mod folder: $modDir"
Write-Host "Kit zipped: $zip"
Get-ChildItem -Recurse $kitDir | ForEach-Object {
    Write-Host ("  " + $_.FullName.Substring($kitDir.Length + 1))
}
