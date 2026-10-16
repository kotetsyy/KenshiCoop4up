[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Kit,
    [string]$GameRoot='D:\SteamLibrary\steamapps\common\Kenshi',
    [string]$NativeBin='',
    [switch]$AutoConnect,
    [ValidateRange(30,1800)][int]$Seconds=600,
    [string]$EvidenceRoot="$env:LOCALAPPDATA\KenshiCoop\native-surface-evidence"
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'kit_preflight.ps1')
. (Join-Path $PSScriptRoot 'RuntimeDistribution.ps1') -Configuration Harness -NativeBin $NativeBin
$Inputs=Get-CoopRuntimeFiles -Configuration Harness -NativeBin $NativeBin
Assert-CoopRuntimeZip -Zip $Kit -Files $Inputs
$ModRoot=Join-Path $GameRoot 'mods\KenshiCoop'
if (-not (Test-Path -LiteralPath (Join-Path $ModRoot 'RE_Kenshi.json')) -or -not (Test-Path -LiteralPath (Join-Path $GameRoot 'kenshi_x64.exe'))) { throw 'Sanctioned installed Kenshi/RE_Kenshi required.' }
$RunState=New-CoopRunState -GameDirs @($GameRoot)
$Evidence=Join-Path $EvidenceRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Originals=@($RunState.Backups | ForEach-Object {
    [PSCustomObject]@{Path=$_.Path;Existed=$_.Existed;Hash=$(if($_.Existed){(Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash}else{''})}
})
$Profile=Join-Path $Evidence 'udp-host.private'
$Ready=$false
try {
    Install-CoopRuntime -Files $Inputs -Installations @($GameRoot)
    foreach ($Variable in @(Get-ChildItem Env: | Where-Object Name -like 'KENSHICOOP_*')) { Remove-Item -LiteralPath "Env:$($Variable.Name)" }
    $env:KENSHICOOP_LOG=Join-Path $Evidence 'KenshiCoop_native_smoke.log'
    $env:KENSHICOOP_UDP_PROFILE_PATH=$Profile
    $env:KENSHICOOP_AUTOCONNECT=$(if($AutoConnect){'1'}else{'0'})
    $env:KENSHICOOP_ROLE='host'
    $env:KENSHICOOP_TRANSPORT='udp'
    $env:KENSHICOOP_PLAYER_NAME='NativeSmoke'
    $Config=@{role='host';transport='udp';ip='127.0.0.1';port=27800;playerName='NativeSmoke';autoConnect=[bool]$AutoConnect;updateEnabled=$false}
    [IO.File]::WriteAllText((Join-Path $ModRoot 'coop_config.json'),($Config | ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
    Start-CoopOwnedGame -State $RunState -ScriptDir $PSScriptRoot -ExePath (Join-Path $GameRoot 'kenshi_x64.exe') -WorkDir $GameRoot -TimeoutSec 180 -NativeOutput $NativeBin | ForEach-Object { Write-Output "$_" }
    $Actual=@($RunState.Processes.Values | Where-Object { -not $_.HasExited -and $_.ProcessName -ieq 'kenshi_x64' } | Sort-Object StartTime -Descending | Select-Object -First 1)[0]
    if (-not $Actual) { throw 'Launcher returned no owned actual game.' }
    $Deadline=(Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $Deadline -and -not $Actual.HasExited) {
        if ($RunState.Watcher -and $RunState.Watcher.HasExited) { throw 'Live non-primary monitor guard stopped.' }
        $Work=Get-CoopSecondaryWorkArea
        $Actual.Refresh()
        if ($Actual.MainWindowHandle -ne [IntPtr]::Zero -and -not $Ready) {
            [PSCustomObject]@{PID=$Actual.Id;Window=$Actual.MainWindowHandle.ToInt64();Monitor='non-primary';Work=$Work;Evidence=$Evidence} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Evidence 'window.json') -Encoding UTF8
            Write-Output "Native surface ready PID=$($Actual.Id) HWND=$($Actual.MainWindowHandle.ToInt64()) Evidence=$Evidence"
            $Ready=$true
        }
        if (Test-Path -LiteralPath (Join-Path $Evidence 'stop-smoke')) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $Ready) { throw 'No actual secondary-monitor window observed.' }
    if ($Actual.HasExited) { throw 'Owned game exited before controlled surface checks finished.' }
    @($Actual.Modules | Where-Object ModuleName -like 'KenshiCoop*.dll' | ForEach-Object {
        [PSCustomObject]@{Name=$_.ModuleName;Path=$_.FileName}
    }) | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'loaded-modules.json') -Encoding UTF8
    foreach ($Name in $Inputs.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $ModRoot $Name)).Hash -cne (Get-FileHash -LiteralPath $Inputs[$Name]).Hash) { throw 'Installed runtime changed during controlled native smoke.' }
    }
} finally {
    try { Restore-CoopRunState -State $RunState }
    finally { if (Test-Path -LiteralPath $Profile) { Remove-Item -LiteralPath $Profile -Force } }
    foreach ($Original in $Originals) {
        if ($Original.Existed) {
            if ((Get-FileHash -LiteralPath $Original.Path -Algorithm SHA256).Hash -cne $Original.Hash) { throw "Exact original runtime/config restore failed: $($Original.Path)" }
        } elseif (Test-Path -LiteralPath $Original.Path) { throw "Originally absent runtime/config survived restoration: $($Original.Path)" }
    }
    $Originals | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'restored-originals.json') -Encoding UTF8
    Write-Output "Owned native test tree closed; exact original runtime/config/display files restored. Evidence=$Evidence"
}
