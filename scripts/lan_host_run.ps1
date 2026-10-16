<#
.SYNOPSIS
  Interactive remote owner for a uniquely admitted LAN run. Staged files never
  replace user runtime/saves until the owning snapshot is complete. done.json is
  published only after cleanup/restoration and carries the exact request token.
#>
$ErrorActionPreference = 'Stop'
$dropDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $dropDir 'kit_preflight.ps1')
Import-Module (Join-Path $dropDir 'CoopHarness.psm1') -Force
Import-Module (Join-Path $dropDir 'CoopOracles.psm1') -Force
$runArgs = Get-Content -LiteralPath (Join-Path $dropDir 'run_args.json') -Raw | ConvertFrom-Json
$runId = "$($runArgs.runId)"
if ($runId -notmatch '^[a-fA-F0-9]{32}$') { throw 'Invalid LAN request token.' }
$lockDir = Join-Path $dropDir 'owner.lock'
if ((Get-Content -LiteralPath (Join-Path $lockDir 'token.txt') -Raw).Trim() -cne $runId) { throw 'This request does not own the remote admission lock.' }
$resultsDir = Join-Path $dropDir "results\$runId"
if (Test-Path -LiteralPath $resultsDir) { throw 'LAN request token has already been used.' }
New-Item -ItemType Directory -Path $resultsDir | Out-Null
$hostLog = Join-Path $resultsDir 'host.log'
$runState = $null
$failure = $null
try {
    $kenshiDir = [IO.Path]::GetFullPath($runArgs.kenshiDir)
    $save = "$($runArgs.env.KENSHICOOP_SAVE)"
    Assert-CoopKit -KitDir $dropDir -Metadata $runArgs
    Test-CoopPrereqs -KenshiDir $kenshiDir -UseSteam $false
    if (-not (Test-Path -LiteralPath (Join-Path $dropDir "save\$save\quick.save"))) { throw 'Staged fixture is incomplete.' }
    $runState = New-CoopRunState -GameDirs @($kenshiDir) -SaveNames @($save,'coopresume')
    Clear-CoopRunEnvironment -State $runState
    New-Item -ItemType Directory -Path (Join-Path $kenshiDir 'mods\KenshiCoop') -Force | Out-Null
    foreach ($name in @('KenshiCoop.dll','KenshiCoopUI.dll','KenshiCoopNetSteam.dll','KenshiCoopNetUDP.dll','KenshiCoop.mod','RE_Kenshi.json')) {
        Copy-Item -LiteralPath (Join-Path "$dropDir\mod" $name) -Destination (Join-Path $kenshiDir 'mods\KenshiCoop') -Force
    }
    foreach ($root in @((Join-Path $env:LOCALAPPDATA 'kenshi\save'),(Join-Path $kenshiDir 'save'))) {
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $destination = Join-Path $root $save
        if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
        Copy-Item -LiteralPath (Join-Path $dropDir "save\$save") -Destination $destination -Recurse
    }
    # JSON null means removal. A present empty string is not a removal on PS7.5+.
    [void](Set-CoopDiagEnv -Entry $null)
    foreach ($property in $runArgs.env.PSObject.Properties) {
        if ($property.Name -notlike 'KENSHICOOP_*') { throw 'Unexpected remote environment key.' }
        $value = if ($null -eq $property.Value) { $null } else { [string]$property.Value }
        [Environment]::SetEnvironmentVariable($property.Name,$value,'Process')
    }
    $env:KENSHICOOP_LOG = $hostLog
    Set-CoopPrivateCredentials -Mode host -UseSteam $false -GameDirs @($kenshiDir,$dropDir) -ProfilePath "$($runArgs.udpProfilePath)"
    $out = Start-CoopOwnedGame -State $runState -ScriptDir $dropDir -ExePath (Join-Path $kenshiDir 'kenshi_x64.exe') -WorkDir $kenshiDir -TimeoutSec ([int]$runArgs.startTimeoutSec)
    $out | ForEach-Object { Add-Content -LiteralPath (Join-Path $resultsDir 'launcher.log') -Value "$_" }
    $line = $out | Where-Object { "$_" -match 'GAMEPID=(\d+)' } | Select-Object -First 1
    if (-not $line -or "$line" -notmatch 'GAMEPID=(\d+)') { throw 'No final owned game process.' }
    $game = $runState.Processes[[int]$Matches[1]]
    $deadline = Get-CoopRunDeadline -Process $game -Seconds ([int]$runArgs.seconds) -StartTimeoutSec ([int]$runArgs.startTimeoutSec) -ArmTimeoutMs ([int]$runArgs.armTimeoutMs) -KillGraceSec ([int]$runArgs.killGraceSec)
    @{ runToken=$runId; pid=$game.Id; startTicks=$game.StartTime.ToUniversalTime().Ticks; deadlineUtc=$deadline.ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultsDir 'started.json') -Encoding UTF8
    if ($runArgs.anchor -and (Wait-CoopOwnedLogLine -Process $game -Deadline $deadline -File $hostLog -Pattern $runArgs.anchor -TimeoutSec ([int]$runArgs.anchorTimeoutSec))) {
        if ([DateTime]::UtcNow.AddSeconds([int]$runArgs.shotDelaySec) -lt $deadline -and -not $game.HasExited) {
            Start-Sleep -Seconds ([int]$runArgs.shotDelaySec)
            try {
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dropDir 'screenshot.ps1') -ProcessId $game.Id -ProcessStartTicks ($game.StartTime.ToUniversalTime().Ticks) -Out (Join-Path $resultsDir 'host.png') -Frames 5 -IntervalMs 16
                if ($LASTEXITCODE -ne 0) { throw 'Screenshot helper failed.' }
            } catch {
                if ($runArgs.shotRequired) { throw }
                Write-Warning "Screenshot evidence unavailable: $($_.Exception.Message)"
            }
        }
    }
    Wait-CoopOwnedExit -Process $game -Deadline $deadline
    if (-not (Test-Path -LiteralPath $hostLog) -or -not (Select-String -LiteralPath $hostLog -Pattern 'gameplay started' -Quiet)) { throw 'No current-run gameplay evidence.' }
    if ($runArgs.requireResult -and (Test-ScenarioResultPass -File $hostLog -Label host -Required $true -Scenario $runArgs.env.KENSHICOOP_SCENARIO) -ne 'PASS') { throw 'Scenario incomplete or failed; exit alone is not success.' }
    if ($runArgs.shotRequired -and -not (Test-Path -LiteralPath (Join-Path $resultsDir 'host.png'))) { throw 'Required scenario capture is missing.' }
} catch { $failure = $_ }
finally {
    try {
        if ($runState) {
            $engine = Join-Path $kenshiDir 'kenshi_info.log'
            if (Test-Path -LiteralPath $engine) { Copy-Item -LiteralPath $engine -Destination (Join-Path $resultsDir 'host_engine.log') }
        }
    } catch { if (-not $failure) { $failure = $_ } }
    $restored = $false
    try { Restore-CoopRunState $runState; $restored = $true } catch { $failure = $_ }
    if ($restored) {
        try {
            if ((Get-Content -LiteralPath (Join-Path $lockDir 'token.txt') -Raw).Trim() -cne $runId) { throw 'Remote admission ownership changed unexpectedly.' }
            Remove-Item -LiteralPath $lockDir -Recurse -Force
        } catch { if (-not $failure) { $failure = $_ } }
    }
    $status = @{ runToken=$runId; success=($null -eq $failure); restored=$restored; error=$(if ($failure) { "$failure" } else { $null }) }
    $done = Join-Path $resultsDir 'done.json'
    $status | ConvertTo-Json | Set-Content -LiteralPath "$done.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$done.tmp" -Destination $done
}
if ($failure) { throw $failure }
