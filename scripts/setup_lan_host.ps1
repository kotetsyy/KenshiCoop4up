<#
.SYNOPSIS
  One-time provisioning of the LAN host machine (machine 2) for remote-host
  validation runs: push the mod + fixture saves + runner scripts, open the UDP
  firewall port, and register the KenshiCoopHost scheduled task that launches
  the game in the interactive session.

.DESCRIPTION
  Connection settings come from scripts\lan.config.json (copy the .example).
  SSH key auth to the machine must already work (same setup as the VideoEditor
  GPU-PC flow: OpenSSH server + administrators_authorized_keys).

  Prerequisites on the LAN machine (verified where possible):
    * Kenshi installed (kenshiDir), RE_Kenshi 0.3.1+ installed
    * Windowed mode (Options > Video > Full Screen off) - screenshots + window
      management need it
    * A user logged into the desktop (the scheduled task runs interactively)
    * STEAM ACCOUNT NOTE: with one Steam account on both machines, put ONE
      client in OFFLINE mode (validated 2026-07-05: concurrent play then works
      and neither session is signed out).

  Safe to re-run (idempotent): re-pushes artifacts, re-registers the task.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup_lan_host.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup_lan_host.ps1 -Saves squad1,sync,duel1
#>
[CmdletBinding()]
param(
    # Fixture saves to push (defaults to every save the tiered manifest uses).
    [string[]]$Saves = @(),
    [int]$Port = 27800,
    [string]$NativeBin = "",
    [switch]$SkipSaves
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'NativeBuild.ps1')
. (Join-Path $scriptDir 'kit_preflight.ps1')
$runtime = Get-CoopHarnessRuntime -RepoRoot $repoRoot -NativeBin $NativeBin
$native = Get-NativeBuild -Configuration Harness -NativeBin $NativeBin

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

# ---- Config -------------------------------------------------------------------
$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (copy lan.config.example.json and fill in)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"
$dropFwd = $cfg.dropDir -replace '\\', '/'

function Invoke-LanSsh {
    param([string]$Cmd)
    # PowerShell 5.1 does NOT escape embedded double quotes when building the
    # native command line, so a remote cmd like: if exist "C:\Program Files..."
    # arrives with its quotes stripped and breaks on the space. Escape them.
    # Also tolerate stderr noise: under ErrorActionPreference=Stop a mere ssh
    # WARNING on stderr becomes a terminating NativeCommandError via 2>&1.
    $escaped = $Cmd -replace '"', '\"'
    $PSNativeCommandArgumentPassing = 'Legacy'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

Write-Host "== LAN host provisioning: $target =="

# ---- 1. Connectivity + prerequisites ----------------------------------------------
$r = Invoke-LanSsh "echo SSH-OK && hostname"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch "SSH-OK") { throw "SSH to $target failed: $($r.out)" }
Write-Host "  ssh: OK ($((@($r.out) | Select-Object -Last 1)))"

$r = Invoke-LanSsh "if exist `"$($cfg.kenshiDir)\kenshi_x64.exe`" (echo KENSHI-OK) else (echo KENSHI-MISSING)"
if ("$($r.out)" -notmatch "KENSHI-OK") { throw "Kenshi not found at $($cfg.kenshiDir) on $($cfg.host)" }
Write-Host "  kenshi: OK"

# RE_Kenshi presence (best-effort: its loader DLL in the install root).
$r = Invoke-LanSsh "if exist `"$($cfg.kenshiDir)\RE_Kenshi*`" (echo REK-OK) else (if exist `"$($cfg.kenshiDir)\dinput8.dll`" (echo REK-OK) else (echo REK-UNKNOWN))"
Write-Host ("  re_kenshi: " + $(if ("$($r.out)" -match "REK-OK") { "detected" } else { "NOT detected - verify manually (Nexus mod 847)" }))
if ("$($r.out)" -notmatch 'REK-OK') { throw 'RE_Kenshi must be installed before remote mutation.' }
$r = Invoke-LanSsh 'powershell -NoProfile -Command "if(Get-Process kenshi_x64 -ErrorAction SilentlyContinue){exit 1}; if(Get-ScheduledTask -TaskName KenshiCoopHost -ErrorAction SilentlyContinue | Where-Object {$_.State -eq ''Running''}){exit 1}; Add-Type -AssemblyName System.Windows.Forms; if(-not ([Windows.Forms.Screen]::AllScreens | Where-Object {-not $_.Primary})){exit 1}; Write-Output SAFE"'
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'SAFE') { throw 'Remote preflight requires no active scheduled owner, no user-owned game, and a non-primary monitor.' }

if (-not $SkipSaves) {
    if ($Saves.Count -eq 0) {
        $manifest = Get-ScenarioManifest
        $Saves = @($manifest.Scenarios.Values | Where-Object { $_.Tier -ne 'none' -and $_.Save } | ForEach-Object { $_.Save } | Sort-Object -Unique)
    }
    $saveRoot = Join-Path $env:LOCALAPPDATA 'kenshi\save'
    foreach ($s in $Saves) {
        if (-not $s -or $s -match '[\\/:]' -or $s -eq '.' -or $s -eq '..' -or -not (Test-Path -LiteralPath (Join-Path $saveRoot "$s\quick.save"))) {
            throw 'Every requested named fixture must be complete before remote mutation.'
        }
    }
}
# ---- 2. Drop dir + runner scripts ---------------------------------------------------
[void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)`" 2>nul & mkdir `"$($cfg.dropDir)\results`" 2>nul & echo DIR-OK")
$setupToken = [Guid]::NewGuid().ToString('N')
$r = Invoke-LanSsh "mkdir `"$($cfg.dropDir)\owner.lock`" && > `"$($cfg.dropDir)\owner.lock\token.txt`" echo $setupToken && echo RESERVED"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'RESERVED') { throw 'An active remote run owns this drop; provisioning must not overwrite its inputs.' }
try {
& scp -o BatchMode=yes `
    (Join-Path $scriptDir "lan_host_run.ps1") `
    (Join-Path $scriptDir "start_kenshi.ps1") `
    (Join-Path $scriptDir "screenshot.ps1") `
    (Join-Path $scriptDir "kit_preflight.ps1") `
    (Join-Path $scriptDir "CoopHarness.psm1") `
    (Join-Path $scriptDir "CoopOracles.psm1") `
    (Join-Path $scriptDir "set_video_mode.ps1") `
    "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw "scp of runner scripts failed" }
& scp -o BatchMode=yes -r (Join-Path $scriptDir 'oracles') "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw 'scp of native terminal oracle closure failed.' }
Write-Host "  runner scripts: pushed -> $($cfg.dropDir)"

# ---- 3. Mod deploy --------------------------------------------------------------------
$modDirFwd = "$dropFwd/mod"
[void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)\mod`" 2>nul & echo OK")
& scp -o BatchMode=yes @runtime "${target}:$modDirFwd/"
if ($LASTEXITCODE -ne 0) { throw "scp of staged mod failed" }
Write-Host "  mod: staged -> $($cfg.dropDir)\mod (installed/restored only by the owner)"

# Stage named fixtures outside the game/user save roots. Only the interactive
# owned runner copies a selected fixture, with encompassing snapshot/restoration.
if (-not $SkipSaves) {
    [void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)\save`" 2>nul & echo OK")
    foreach ($s in $Saves) {
        $r = Invoke-LanSsh "if exist `"$($cfg.dropDir)\save\$s`" rmdir /S /Q `"$($cfg.dropDir)\save\$s`""
        if ($r.exit -ne 0) { throw "Old staged fixture could not be removed: $s" }
        & scp -o BatchMode=yes -r (Join-Path $saveRoot $s) "${target}:$dropFwd/save/"
        if ($LASTEXITCODE -ne 0) { throw "Named fixture staging failed: $s" }
    }
}

# Firewall provisioning is permanent, but READY requires the actual admission.
$firewallScript = @"
`$ErrorActionPreference='Stop'
function Test-Admission {
    foreach(`$rule in @(Get-NetFirewallRule -DisplayName KenshiCoop -ErrorAction SilentlyContinue | Where-Object {`$_.Enabled -eq 'True' -and `$_.Direction -eq 'Inbound' -and `$_.Action -eq 'Allow'})){
        foreach(`$filter in @(`$rule | Get-NetFirewallPortFilter)){
            if("`$(`$filter.Protocol)" -in @('UDP','17') -and "`$(`$filter.LocalPort)" -eq '$Port'){return `$true}
        }
    }
    return `$false
}
if(-not (Test-Admission)){
    & netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$Port | Out-Null
    if(`$LASTEXITCODE -ne 0){throw 'Firewall provisioning failed.'}
}
if(-not (Test-Admission)){throw 'Required enabled inbound UDP rule not verified.'}
Write-Output 'FIREWALL-READY'
"@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($firewallScript))
$r = Invoke-LanSsh "powershell -NoProfile -EncodedCommand $encoded"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch 'FIREWALL-READY') { throw "Firewall admission failed: $($r.out)" }
# Encode the action to avoid nested /tr and -File path quotation across shells.
$action = "& '$($cfg.dropDir.Replace("'","''"))\lan_host_run.ps1'"
$actionEncoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($action))
$taskCmd = "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $actionEncoded"
$r = Invoke-LanSsh "schtasks /create /tn KenshiCoopHost /tr `"$taskCmd`" /sc once /st 00:00 /it /f"
if ($r.exit -ne 0) { throw "scheduled task registration failed: $($r.out)" }
Write-Host "  scheduled task: KenshiCoopHost registered (interactive)"

} finally {
    $release = "`$ErrorActionPreference='Stop'; if((Get-Content -LiteralPath '$($cfg.dropDir.Replace("'","''"))\owner.lock\token.txt' -Raw).Trim() -cne '$setupToken'){throw 'Provisioning admission ownership changed.'}; Remove-Item -LiteralPath '$($cfg.dropDir.Replace("'","''"))\owner.lock' -Recurse -Force"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($release))
    $r = Invoke-LanSsh "powershell -NoProfile -EncodedCommand $encoded"
    if ($r.exit -ne 0) { throw "Remote provisioning admission cleanup failed: $($r.out)" }
}
Write-Host ""
Write-Host "== Provisioning complete =="
Write-Host "Reminders for $($cfg.host):"
Write-Host "  * Keep a user logged into the desktop (the task launches interactively)."
Write-Host "  * Windowed mode must be on (Full Screen=No)."
Write-Host "  * Same Steam account on both machines: keep ONE client in OFFLINE mode."
Write-Host "  * Runtime and selected fixtures are staged, then restored by each owner."
Write-Host ""
Write-Host "Next: powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario coop_presence"
