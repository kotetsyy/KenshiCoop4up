[CmdletBinding()]
param([string]$OracleRoot = '')
if (-not $OracleRoot) { $OracleRoot = Split-Path -Parent $PSScriptRoot }
$ErrorActionPreference = 'Stop'
. (Join-Path $OracleRoot 'kit_preflight.ps1')
Import-Module (Join-Path $OracleRoot 'CoopOracles.psm1') -Force
Import-Module (Join-Path $OracleRoot 'CoopHarness.psm1') -Force
function Get-SessionAst([string]$Name) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $OracleRoot $Name),[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw "$Name contains parse errors: $errors" }
    return $ast
}
function Get-SessionStatement($Ast,[string]$Pattern,[type]$Type = [Management.Automation.Language.StatementAst]) {
    $nodes=@($Ast.FindAll({param($n) $n -is $Type -and $n.Extent.Text -match $Pattern}.GetNewClosure(),$true))
    if ($nodes.Count -ne 1) { throw "Expected one production boundary for $Pattern, found $($nodes.Count)." }
    return [scriptblock]::Create($nodes[0].Extent.Text)
}
function Get-SessionRange($Ast,[string]$Start,[string]$End) {
    $first=@($Ast.FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Extent.Text -match $Start}.GetNewClosure(),$true))
    $last=@($Ast.FindAll({param($n) $n -is [Management.Automation.Language.PipelineAst] -and $n.Extent.Text -match $End}.GetNewClosure(),$true))
    if($first.Count -ne 1 -or $last.Count -ne 1){throw "Ambiguous production range $Start / $End"}
    $offset=$first[0].Extent.StartOffset
    return [scriptblock]::Create($Ast.Extent.Text.Substring($offset,$last[0].Extent.EndOffset-$offset))
}
function Assert-Rejected([scriptblock]$Action,[string]$Label) {
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    if (-not $rejected) { throw "Accepted $Label" }
}
$root=Join-Path $env:TEMP ('SessionNetworkRegression-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$originalEnvironment=@{}
Get-ChildItem Env: | Where-Object {$_.Name -like 'KENSHICOOP_*'} | ForEach-Object {$originalEnvironment[$_.Name]=$_.Value}
try {
    $log=Join-Path $root 'current.log'
    $hostLog=$log; $joinLog=$log; $Scenario='xfer_block'
    foreach ($name in @('host_session.ps1','join_session.ps1','friend_host.ps1','friend_join.ps1','lan_host_run.ps1')) {
        $ast=Get-SessionAst $name
        $runArgs=[pscustomobject]@{requireResult=$true;env=[pscustomobject]@{KENSHICOOP_SCENARIO=$Scenario}}
        if ($name -ne 'lan_host_run.ps1') {
            $entry=@{PrimaryGate='';Gating=@()}
            . (Get-SessionStatement $ast '^\$requireResult\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
            if (-not $requireResult) { throw "$name exempts compiled diagnostic completion." }
        }
        $boundary=Get-SessionStatement $ast '^if\s*\(\$(?:requireResult|runArgs\.requireResult)\b' ([Management.Automation.Language.IfStatementAst])
        $role=if ($name -match 'host') {'HOST'} else {'JOIN'}
        $good="[00:00:00.000] [$role] SCENARIO xfer_block start`n[00:00:01.000] [$role] SCENARIO RESULT PASS`n"
        [IO.File]::WriteAllText($log,$good)
        . $boundary
        foreach ($bad in @(
            $good.Replace('RESULT PASS','RESULT FAIL'),
            "[00:00:00.000] [$role] SCENARIO xfer_block start`n",
            ($good+"[00:00:02.000] [$role] SCENARIO RESULT FAIL`n"),
            $good.Replace('SCENARIO RESULT PASS','diagnostic says SCENARIO RESULT PASS'),
            $good.Replace('xfer_block start','other start'),
            $good.Replace("[$role] SCENARIO RESULT",'['+$(if($role -eq 'HOST'){'JOIN'}else{'HOST'})+'] SCENARIO RESULT')
        )) {
            [IO.File]::WriteAllText($log,$bad)
            Assert-Rejected { . $boundary } "$name invalid native completion"
        }
        if ($name -ne 'lan_host_run.ps1') {
            $entry=$null; $Scenario=''
            . (Get-SessionStatement $ast '^\$requireResult\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
            . $boundary
            $Scenario='bootstrap_stream'; $entry=@{PrimaryGate='';Gating=@()}
            $runnerOnly=Get-SessionStatement $ast "^if\s*\(\`$Scenario -eq 'bootstrap_stream'\)" ([Management.Automation.Language.IfStatementAst])
            Assert-Rejected { . $runnerOnly } "$name unsupported runner-only lifecycle"
            $Scenario='xfer_block'
            $diag=Get-SessionStatement $ast '^if\s*\(\$entry\)\s*\{\s*\[void\]\(Set-CoopDiagEnv' ([Management.Automation.Language.IfStatementAst])
            [Environment]::SetEnvironmentVariable('KENSHICOOP_CELL_AUTH',[NullString]::Value,'Process'); $entry=$null
            . $diag
            if ($null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_CELL_AUTH','Process')) { throw "$name changes free-play authority." }
            $entry=@{}; . $diag
            if ($env:KENSHICOOP_CELL_AUTH -ne '0') { throw "$name omits scenario isolation." }
            $entry=@{DiagEnv=@{KENSHICOOP_CELL_AUTH='1'}}; . $diag
            if ($env:KENSHICOOP_CELL_AUTH -ne '1') { throw "$name loses manifest override." }
        }
    }
    # Hold a real disposable exited shell and its final native-shaped log. The
    # actual log observer must retain that evidence; optional capture loss must
    # not override exact native success, while required capture still rejects.
    $shell=(Get-Process -Id $PID).Path
    $exitedCaptureProcess=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-NonInteractive','-Command','Start-Sleep -Milliseconds 200; exit 0') -WindowStyle Hidden -PassThru
    $null=$exitedCaptureProcess.Handle
    $exitedCaptureProcess.WaitForExit()
    $game=$exitedCaptureProcess
    $shotDelay=2; $runDeadline=[DateTime]::UtcNow.AddSeconds(30)
    foreach ($name in @('friend_host.ps1','friend_join.ps1','join_session.ps1')) {
        $role=if($name -match 'host'){'HOST'}else{'JOIN'}
        [IO.File]::WriteAllText($log,"[00:00:00.000] [$role] gameplay started`n[00:00:00.100] [$role] SCENARIO xfer_block start`n[00:00:00.200] [$role] SCENARIO RECV`n[00:00:01.000] [$role] SCENARIO RESULT PASS`n")
        if(-not (Wait-CoopOwnedLogLine -Process $game -Deadline $runDeadline -File $log -Pattern 'SCENARIO RECV' -TimeoutSec 10)){throw 'Exited-process final anchor was discarded.'}
        $ast=Get-SessionAst $name
        $capture=Get-SessionStatement $ast '^try\s*\{\s*if\s*\(\[DateTime\]::UtcNow\.AddSeconds\(\$shotDelay\)' ([Management.Automation.Language.TryStatementAst])
        $shotRequired=$false; . $capture
        $requireResult=$true; $Scenario='xfer_block'
        . (Get-SessionStatement $ast '^if\s*\(\$requireResult\b' ([Management.Automation.Language.IfStatementAst]))
        $shotRequired=$true; Assert-Rejected { . $capture } "$name missing required capture"
    }
    $exitedCaptureProcess.Dispose()
    $exitedCaptureProcess=$null
    # Real cmd.exe writes, not an imitation of cmd redirection. Every final hex
    # digit must survive the exact production admission expression.
    function Invoke-LanSsh([string]$Cmd) { & cmd.exe /d /c $Cmd | Out-Null; if($LASTEXITCODE -ne 0){throw 'Local admission command failed.'}; return @{exit=0;out='RESERVED'} }
    foreach ($name in @('run_lan_test.ps1','setup_lan_host.ps1')) {
        $admit=Get-SessionStatement (Get-SessionAst $name) '^\$r\s*=\s*Invoke-LanSsh "mkdir .*RESERVED"$' ([Management.Automation.Language.AssignmentStatementAst])
        foreach($digit in '0123456789abcdef'.ToCharArray()) {
            $cfg=@{dropDir=(Join-Path $root ('admit-'+$digit))}; New-Item -ItemType Directory -Path $cfg.dropDir -Force | Out-Null
            $runId=('a'*31)+$digit; $setupToken=$runId
            . $admit
            $lock=Join-Path $cfg.dropDir 'owner.lock'
            if([IO.File]::ReadAllText((Join-Path $lock 'token.txt')).Trim() -cne $runId){throw "$name corrupts token ending $digit"}
            Remove-Item -LiteralPath $lock -Recurse -Force
        }
    }
    # Same-install rejection executes before the owning snapshot is created.
    $same=Get-SessionStatement (Get-SessionAst 'rehearse_remote_kit.ps1') '^if\s*\(\$HostDir\.TrimEnd' ([Management.Automation.Language.IfStatementAst])
    $HostDir=Join-Path $root 'game'; $JoinDir=$HostDir.ToUpperInvariant()+'\'
    Assert-Rejected { . $same } 'same-install rehearsal'
    foreach($name in @('rehearse_remote_kit.ps1','rehearse_host_kit.ps1','run_lan_test.ps1')) {
        $ast=Get-SessionAst $name
        $argsBoundary=Get-SessionStatement $ast '^\$wanArgs\s*=' ([Management.Automation.Language.AssignmentStatementAst])
        $stall=Get-SessionStatement $ast '^if\s*\(\$wp\.ContainsKey\(' ([Management.Automation.Language.IfStatementAst])
        $proxyPort=27901; $Port=27900; $cfg=@{host='192.0.2.1'}; $wanSeed=123
        $wp=@{DelayMs=20;JitterMs=3;LossPct=1}; . $argsBoundary; . $stall
        if($wanArgs.Count -ne 6){throw "$name changes normal WAN arguments."}
        $wp.StallAtS=30; $wp.StallForS=4; . $argsBoundary; . $stall
        if($wanArgs.Count -ne 9 -or $wanArgs[6] -ne '123' -or $wanArgs[7] -ne '30' -or $wanArgs[8] -ne '4'){throw "$name omits native outage contract."}
    }
    # Exercise real rehearsal dispatch plus the manifest WAN overlay, not a
    # mocked analysis call or merely RunInfo.wan. Missing gameplay samples still
    # fail the actual domain gates; this fixture never calls that a certificate.
    $manifest=Get-ScenarioManifest
    $Scenario='split_interest'; $stampDir=$root; $kitDir=$root; $Wan='bad'
    $hostLog=Join-Path $root 'wan-host.log'; $joinLog=Join-Path $root 'wan-join.log'
    foreach($role in @('HOST','JOIN')) {
        [IO.File]::WriteAllText($(if($role -eq 'HOST'){$hostLog}else{$joinLog}),"[00:00:00.000] [$role] gameplay started`n[00:00:00.100] [$role] SCENARIO split_interest start`n[00:00:01.000] [$role] SCENARIO RESULT PASS`n")
    }
    foreach($name in @('rehearse_remote_kit.ps1','rehearse_host_kit.ps1')) {
        . (Get-SessionStatement (Get-SessionAst $name) '^\$verdict\s*=\s*Invoke-RunAnalysis' ([Management.Automation.Language.AssignmentStatementAst]))
        if($verdict.tolerance -ne $manifest.Scenarios[$Scenario].WanTolerance -or $verdict.tolerance -eq $manifest.Scenarios[$Scenario].Tolerance){throw "$name bypassed the actual WAN tolerance overlay."}
    }
    # Actual argument rendering is parsed as a PowerShell invocation, proving
    # that spaces/apostrophes survive rather than just checking a text fragment.
    $kitAst=Get-SessionAst 'make_remote_kit.ps1'
    foreach($Scenario in @('','coop_presence')) {
        $Port=27900; $Save="friend's fixture"
        . (Get-SessionStatement $kitAst '^\$sessionArgs\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
        $tokens=$null;$errors=$null
        $command=[Management.Automation.Language.Parser]::ParseInput(("& ./join_session.ps1 "+$sessionArgs),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw 'Generated counterpart arguments do not parse.'}
        $call=$command.Find({param($n) $n -is [Management.Automation.Language.CommandAst]},$true)
        $args=@($call.CommandElements | Select-Object -Skip 1 | ForEach-Object {$_.Extent.Text})
        if($args[0] -cne '-Port' -or $call.CommandElements[2].SafeGetValue() -ne 27900 -or $call.CommandElements[4].SafeGetValue() -cne $Save -or $call.CommandElements[6].SafeGetValue() -cne $(if($Scenario){$Scenario}else{'free'})){throw 'Generated counterpart differs from kit port/save/scenario.'}
    }
    foreach($Role in @('host','join')) {
        . (Get-SessionStatement $kitAst '^\$friendScript\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
        . (Get-SessionStatement $kitAst '^\$requiredScripts\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
        . (Get-SessionStatement $kitAst '^\$udpPreflight\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
        if($Role -eq 'host') {
            if($requiredScripts -notcontains 'friend_host.ps1' -or $udpPreflight -notmatch 'friend_host.ps1 -CheckOnly'){throw 'HOST preflight is not packaged.'}
        } elseif($udpPreflight -match 'friend_host.ps1'){throw 'JOIN preflight invokes an absent host script.'}
    }
    # Trigger rejection differs from a lost SSH response. Execute the real
    # response decision and exact-owner release program against disposable paths.
    $lanAst=Get-SessionAst 'run_lan_test.ps1'
    $trigger=Get-SessionStatement $lanAst '^if\s*\(\$r.exit -ne 0\)\s*\{\s*if\s*\(\$r.exit -ne 255\)' ([Management.Automation.Language.IfStatementAst])
    foreach($code in @(0,1,255)) {
        $remoteTriggered=$true; $r=@{exit=$code;out='fixture task response'}
        try { . $trigger } catch {}
        if($remoteTriggered -ne ($code -ne 1)){throw 'Task rejection/uncertainty handoff is incorrect.'}
    }
    $cfg=@{dropDir=(Join-Path $root 'remote-drop');host='192.0.2.1'}; New-Item -ItemType Directory -Path $cfg.dropDir | Out-Null
    $runId='0123456789abcdef0123456789abcdef'
    . (Get-SessionStatement $lanAst '^\$release\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
    function Get-ScheduledTask { param($ErrorAction) return @() }
    $releaseAction=[scriptblock]::Create($release)
    foreach($accepted in @($false,$true)) {
        $lock=Join-Path $cfg.dropDir 'owner.lock'; New-Item -ItemType Directory -Path $lock -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $lock 'token.txt'),$runId)
        if($accepted){New-Item -ItemType Directory -Path (Join-Path $cfg.dropDir "results\$runId") -Force | Out-Null}
        if($accepted){Assert-Rejected { . $releaseAction } 'accepted remote owner release'}
        else{. $releaseAction; if(Test-Path -LiteralPath $lock){throw 'Confirmed task rejection stranded admission.'}}
    }
    Remove-Item Function:Get-ScheduledTask
    # Real owned shell exits, a real damaged disposable restore catalog, and
    # native remote receipt values cross the actual LAN failure/analysis boundary.
    $exitBoundary=Get-SessionStatement $lanAst '^try\s*\{\s*Wait-CoopOwnedExit\b' ([Management.Automation.Language.TryStatementAst])
    $remoteBoundary=Get-SessionStatement $lanAst '^if\s*\(-not \$remoteDone.success' ([Management.Automation.Language.IfStatementAst])
    $restoreBoundary=Get-SessionStatement $lanAst '^try\s*\{\s*Restore-CoopHarnessSession\b' ([Management.Automation.Language.TryStatementAst])
    $infoBoundary=Get-SessionStatement $lanAst '^\$runInfo\s*=' ([Management.Automation.Language.AssignmentStatementAst])
    $analysis=Get-SessionStatement $lanAst '^\$verdict\s*=\s*Invoke-RunAnalysis' ([Management.Automation.Language.AssignmentStatementAst])
    $Scenario='coop_presence'; $Save='fixture'; $Setup=''; $Profile='remote'; $Wan=''; $Tolerance=1; $FakeClockSkewMs=0; $expectedSkew=$null
    $hostLog=Join-Path $root 'host.log'; $joinLog=Join-Path $root 'join.log'
    foreach($role in @('HOST','JOIN')) {
        [IO.File]::WriteAllText($(if($role -eq 'HOST'){$hostLog}else{$joinLog}),"[00:00:00.000] [$role] gameplay started`n[00:00:00.100] [$role] SCENARIO coop_presence start`n[00:00:01.000] [$role] SCENARIO RESULT PASS`n")
    }
    foreach($failure in @('nonzero exit 7','forced timeout','remote failure','remote restore','local restore')) {
        $ownedExitFailures=@(); $OutDir=Join-Path $root ($failure -replace ' ','-'); New-Item -ItemType Directory -Path $OutDir | Out-Null
        switch($failure) {
            'remote failure' {$remoteDone=@{success=$false;restored=$true;error='host exit 9'}; . $remoteBoundary}
            'remote restore' {$remoteDone=@{success=$true;restored=$false;error='restoration failed'}; . $remoteBoundary}
            'local restore' {
                $restoreRoot=Join-Path $root 'damaged-restore'; New-Item -ItemType Directory -Path $restoreRoot | Out-Null
                $restoreFile=Join-Path $root 'restore-fixture'; [IO.File]::WriteAllText($restoreFile,'user fixture')
                $runState=@{Root=$restoreRoot;Backups=@();Processes=@{};Environment=$originalEnvironment}
                Add-CoopRunBackup -State $runState -Path $restoreFile
                Remove-Item -LiteralPath $runState.Backups[0].Backup -Force
                $wanProc=$null
                . $restoreBoundary
            }
            default {
                $childCommand=if($failure -eq 'forced timeout'){'Start-Sleep -Seconds 60'}else{'Start-Sleep -Milliseconds 200; exit 7'}
                $game=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-NonInteractive','-Command',$childCommand) -WindowStyle Hidden -PassThru
                try {
                    $null=$game.Handle
                    if($failure -eq 'forced timeout'){$runDeadline=[DateTime]::UtcNow.AddSeconds(-1)}
                    else{$game.WaitForExit();$runDeadline=[DateTime]::UtcNow.AddSeconds(30)}
                    . $exitBoundary
                } finally {
                    if(-not $game.HasExited){$game.Kill();$game.WaitForExit()}
                    $game.Dispose()
                }
            }
        }
        . $infoBoundary; . $analysis
        $saved=Get-Content -LiteralPath (Join-Path $OutDir 'verdict.json') -Raw | ConvertFrom-Json
        $gate=@($saved.gates | Where-Object {$_.gate -eq 'process_exit'})
        if($verdict.pass -or $gate.Count -ne 1 -or $gate[0].status -cne 'FAIL' -or -not @($runInfo.ownedExitFailures).Count){throw "LAN $failure bypassed process_exit evidence."}
    }
    # Cleanup is authorized only by a matching successful restored receipt.
    $hostAst=Get-SessionAst 'rehearse_host_kit.ps1'
    $runToken='abcdef0123456789abcdef0123456789'
    $zipName="KenshiCoop-results-$runToken.zip"
    $receipt=Get-SessionStatement $hostAst '^if\s*\(\$status.runToken -cne' ([Management.Automation.Language.IfStatementAst])
    foreach($restored in @($false,$true)) {
        $status=@{runToken=$runToken;success=$true;restored=$restored;zip=$zipName}
        if(-not $restored){Assert-Rejected { . $receipt } 'unrestored cleanup authorization'}else{. $receipt}
    }
    $kitTestDir=Join-Path $root 'remote-kit'; $taskName="KenshiCoopKitHost-$runToken"
    $unrelatedPrivate=Join-Path $root 'other-private'
    New-Item -ItemType Directory -Path $kitTestDir,$unrelatedPrivate | Out-Null
    . (Get-SessionStatement $hostAst '^\$cleanup\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
    function schtasks { if(($args -join ' ') -cne "/delete /tn $taskName /f"){throw 'Wrong run task cleanup.'}; $global:LASTEXITCODE=0 }
    . ([scriptblock]::Create($cleanup))
    Remove-Item Function:schtasks
    if((Test-Path -LiteralPath $kitTestDir) -or -not (Test-Path -LiteralPath $unrelatedPrivate)){throw 'Remote cleanup leaked its kit or deleted unrelated private files.'}
    # Execute the actual three fixture preparation boundaries, with AppData,
    # package and stale install catalogs deliberately different.
    $Save='fixture'; $kitDir=Join-Path $root 'immutable-kit'
    New-Item -ItemType Directory -Path (Join-Path $kitDir "save\$Save") -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $kitDir "save\$Save\quick.save"),'packaged fixture')
    $kitZipPath=Join-Path $root 'fixture-kit.zip'
    Compress-Archive -Path (Join-Path $kitDir '*') -DestinationPath $kitZipPath
    $kitZip=Get-Item -LiteralPath $kitZipPath
    $saveRoot=Join-Path $root 'appdata'
    New-Item -ItemType Directory -Path (Join-Path $saveRoot $Save) -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $saveRoot "$Save\quick.save"),'AppData fixture')
    foreach($name in @('run_lan_test.ps1','rehearse_remote_kit.ps1','rehearse_host_kit.ps1')) {
        $ast=Get-SessionAst $name
        $start=if($name -eq 'rehearse_remote_kit.ps1'){'^\$hostFixture\s*='}else{'^\$joinFixture\s*='}
        $prepare=Get-SessionRange $ast $start '^Set-CoopSaveLocation\b'
        foreach($location in @(0,1)) {
            $gameDir=Join-Path $root ($name+"-$location")
            $HostDir=$gameDir; $JoinDir=$gameDir
            $destination=Join-Path $gameDir "save\$Save"; $settings=Join-Path $gameDir 'settings.cfg'
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            [IO.File]::WriteAllText((Join-Path $destination 'quick.save'),'old installation fixture')
            [IO.File]::WriteAllText($settings,"User save location=$location`r`nUnrelated=retained`r`n")
            $settingsHash=(Get-FileHash -LiteralPath $settings).Hash
            $backup=Join-Path $root ([Guid]::NewGuid().ToString('N')); New-Item -ItemType Directory -Path $backup | Out-Null
            $fixtureState=@{Root=$backup;Backups=@();Processes=@{};Environment=$originalEnvironment}
            Add-CoopRunBackup -State $fixtureState -Path $destination
            Add-CoopRunBackup -State $fixtureState -Path $settings
            try {
                . $prepare
                $expected=if($name -eq 'run_lan_test.ps1'){'AppData fixture'}else{'packaged fixture'}
                if([IO.File]::ReadAllText((Join-Path $destination 'quick.save')) -cne $expected -or [IO.File]::ReadAllText($settings) -notmatch 'User save location=0'){throw "$name native lookup uses a different fixture."}
            } finally {Restore-CoopRunState $fixtureState}
            if([IO.File]::ReadAllText((Join-Path $destination 'quick.save')) -cne 'old installation fixture' -or (Get-FileHash -LiteralPath $settings).Hash -ne $settingsHash){throw "$name preparation is not exactly reversible."}
        }
    }
    # Offline router transport below operates an in-memory table, never COM,
    # HTTP, SSDP, firewall, SSH, scheduled tasks or a game.
    . (Join-Path $OracleRoot 'upnp_portmap.ps1')
    $soapTransport=${function:Invoke-UpnpSoap}
    if (-not ('SessionNetworkRouterCollection' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Generic;
public sealed class SessionNetworkRouterEntry {
    public int ExternalPort, InternalPort;
    public string Protocol, InternalClient, Description, ExternalIPAddress = "198.51.100.1";
    public bool Enabled;
}
public sealed class SessionNetworkRouterCollection : IEnumerable {
    public readonly List<SessionNetworkRouterEntry> Entries = new List<SessionNetworkRouterEntry>();
    public bool FailAdd, FailQuery, FailDelete, FailQueryAfterAdd;
    public object Add(int external, string protocol, int port, string client, bool enabled, string description) {
        Entries.Add(new SessionNetworkRouterEntry { ExternalPort=external, Protocol=protocol,
            InternalPort=port, InternalClient=client, Enabled=enabled, Description=description });
        if (FailQueryAfterAdd) FailQuery=true;
        if (FailAdd) throw new InvalidOperationException("COM add response lost");
        return Entries[Entries.Count-1];
    }
    public object Item(int port, string protocol) {
        if (FailQuery) throw new InvalidOperationException("COM query unavailable");
        return Entries.Find(e => e.ExternalPort == port && e.Protocol == protocol);
    }
    public void Remove(int port, string protocol) {
        if (FailDelete) throw new InvalidOperationException("COM delete unavailable");
        Entries.RemoveAll(e => e.ExternalPort == port && e.Protocol == protocol);
    }
    public IEnumerator GetEnumerator() {
        if (FailQuery) throw new InvalidOperationException("COM enumeration unavailable");
        return Entries.GetEnumerator();
    }
}
public sealed class SessionNetworkStreamResponse {
    private readonly string body;
    public SessionNetworkStreamResponse(string text) { body=text; }
    public System.IO.Stream GetResponseStream() {
        return new System.IO.MemoryStream(System.Text.Encoding.UTF8.GetBytes(body));
    }
}
public sealed class SessionNetworkHttpFailure : Exception {
    public object Response { get; private set; }
    public SessionNetworkHttpFailure(object response) : base("fixture HTTP failure") { Response=response; }
}
'@
    }
    function Get-LanIPv4 { return '192.0.2.10' }
    $script:nat=$null
    function New-Object {
        param([string]$TypeName,[object[]]$ArgumentList,[string]$ComObject)
        if($ComObject){if($script:nat){return $script:nat};throw 'Offline fixture: COM unavailable'}
        $parameters=@{TypeName=$TypeName}
        if($PSBoundParameters.ContainsKey('ArgumentList')){$parameters.ArgumentList=$ArgumentList}
        Microsoft.PowerShell.Utility\New-Object @parameters
    }
    $script:entry=$null; $script:failAdd=$false; $script:failQuery=$false; $script:failQueryAfterAdd=$false; $script:failDelete=$false; $script:deleteRace=$false; $script:discoveries=0
    function Find-UpnpIgd { $script:discoveries++; return @{ControlUrl='http://original.invalid/control';ServiceType='fixture'} }
    function Invoke-UpnpSoap {
        param($ControlUrl,$ServiceType,$Action,$Arguments)
        if($ControlUrl -cne 'http://original.invalid/control'){throw 'Router affinity lost.'}
        switch($Action) {
            'GetSpecificPortMappingEntry' {
                if($script:failQuery){throw 'Query unavailable'}
                if(-not $script:entry){$e=New-Object InvalidOperationException('absent');$e.Data['UPnPErrorCode']=714;throw $e}
                return [xml]("<r><NewInternalClient>"+$script:entry.Client+'</NewInternalClient><NewInternalPort>'+$script:entry.Port+'</NewInternalPort><NewEnabled>'+$script:entry.Enabled+'</NewEnabled><NewPortMappingDescription>'+$script:entry.Description+'</NewPortMappingDescription></r>')
            }
            'AddPortMapping' {
                $script:entry=@{Client=$Arguments.NewInternalClient;Port=$Arguments.NewInternalPort;Enabled='true';Description=$Arguments.NewPortMappingDescription}
                if($script:failQueryAfterAdd){$script:failQuery=$true}
                if($script:failAdd){throw 'Lost add response'}
                return [xml]'<r/>'
            }
            'DeletePortMapping' {
                if($script:failDelete){throw 'Delete unavailable'}
                $script:entry=$null
                if($script:deleteRace){$e=New-Object InvalidOperationException('already removed');$e.Data['UPnPErrorCode']=714;throw $e}
                return [xml]'<r/>'
            }
            'GetExternalIPAddress' { return [xml]'<r><NewExternalIPAddress>198.51.100.1</NewExternalIPAddress></r>' }
            default {throw 'Unexpected router action'}
        }
    }
    foreach($value in @('1','true','yes','0','false','no','garbage')) {
        $script:entry=@{Client='192.0.2.10';Port=27900;Enabled=$value;Description='existing'}
        if($value -eq 'garbage'){Assert-Rejected {Get-UpnpSoapMapping @{ControlUrl='http://original.invalid/control';ServiceType='fixture'} 27900} 'malformed UPnP boolean';continue}
        $entry=Get-UpnpSoapMapping @{ControlUrl='http://original.invalid/control';ServiceType='fixture'} 27900
        if($entry.Enabled -ne ($value -in @('1','true','yes'))){throw "Wrong UPnP boolean $value"}
    }
    $script:entry=$null; $script:failAdd=$true
    $mapping=Add-UpnpMapping -Port 27900
    if(-not $mapping.Owned -or -not $mapping.Ok -or $script:discoveries -ne 1){throw 'Lost add response abandoned verified nonce ownership.'}
    $script:failDelete=$true
    Assert-Rejected { Remove-UpnpMapping -Mapping $mapping } 'failed owned mapping cleanup'
    $script:failDelete=$false
    $script:entry.Description='another-owner'
    Assert-Rejected { Remove-UpnpMapping -Mapping $mapping } 'changed nonce cleanup'
    if(-not $script:entry){throw 'Deleted another owner mapping.'}
    $script:entry.Description=$mapping.Description
    Remove-UpnpMapping -Mapping $mapping
    Remove-UpnpMapping -Mapping $mapping
    if($script:entry){throw 'Owned mapping remains.'}
    # Existing compatible mappings are borrowed, never owned or deleted.
    $script:entry=@{Client='192.0.2.10';Port=27900;Enabled='yes';Description='user-existing'}
    $borrowed=Add-UpnpMapping -Port 27900
    if(-not $borrowed.Ok -or $borrowed.Owned -or $borrowed.Unresolved){throw 'Compatible mapping ownership was stolen.'}
    Remove-UpnpMapping -Mapping $borrowed
    if(-not $script:entry){throw 'Borrowed mapping was deleted.'}
    $script:entry.Client='192.0.2.99'
    $conflict=Add-UpnpMapping -Port 27900
    if($conflict.Ok -or $conflict.Owned -or $script:entry.Client -cne '192.0.2.99'){throw 'Conflicting mapping was overwritten.'}
    $script:entry=$null; $script:failAdd=$false
    $mapping=Add-UpnpMapping -Port 27900
    $script:failQuery=$true
    Assert-Rejected {Remove-UpnpMapping -Mapping $mapping} 'unreachable original router cleanup'
    $script:failQuery=$false; $script:deleteRace=$true
    Remove-UpnpMapping -Mapping $mapping
    $script:deleteRace=$false
    $script:entry=$null; $script:failAdd=$true; $script:failQueryAfterAdd=$true
    $mapping=Add-UpnpMapping -Port 27900
    if($mapping.Ok -or $mapping.Owned -or -not $mapping.Unresolved -or $mapping.ControlUrl -cne 'http://original.invalid/control'){throw 'Unreachable add reconciliation discarded its original-target receipt.'}
    Assert-Rejected {Remove-UpnpMapping -Mapping $mapping} 'unresolved SOAP mutation cleanup'
    $script:failQuery=$false; $script:failQueryAfterAdd=$false; $script:failAdd=$false
    Remove-UpnpMapping -Mapping $mapping
    # The real friend receipt boundary must propagate router cleanup failure,
    # while its nested finally still restores actual local snapshot bytes.
    $mapping=Add-UpnpMapping -Port 27900
    $upnp=$mapping; $script:failDelete=$true; $failure=$null; $restored=$false
    $restoreFile=Join-Path $root 'local-owned-settings'
    [IO.File]::WriteAllText($restoreFile,'user settings')
    $restoreRoot=Join-Path $root 'router-caller-backup'; New-Item -ItemType Directory -Path $restoreRoot | Out-Null
    $runState=@{Root=$restoreRoot;Backups=@();Processes=@{};Environment=$originalEnvironment}
    Add-CoopRunBackup -State $runState -Path $restoreFile
    [IO.File]::WriteAllText($restoreFile,'session settings')
    $hostAst=Get-SessionAst 'friend_host.ps1'
    . (Get-SessionStatement $hostAst '^try\s*\{\s*if\s*\(\$upnp' ([Management.Automation.Language.TryStatementAst]))
    . (Get-SessionStatement $hostAst '^\$status\s*=' ([Management.Automation.Language.AssignmentStatementAst]))
    if($status.success -or -not $status.restored -or -not $status.error -or [IO.File]::ReadAllText($restoreFile) -cne 'user settings'){throw 'Router cleanup failure published success or bypassed local restoration.'}
    $check=$hostAst.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Clauses[0].Item1.Extent.Text -ceq '$CheckOnly'},$true)
    function Test-UpnpHairpin { return 'inconclusive' }
    $publicIp='198.51.100.1'
    $checkRouter=Get-SessionStatement $check.Clauses[0].Item2 '^try\s*\{\s*\$hairpin' ([Management.Automation.Language.TryStatementAst])
    Assert-Rejected { . $checkRouter } 'CheckOnly success despite cleanup failure'
    $script:failDelete=$false
    Remove-UpnpMapping -Mapping $mapping
    # COM mutation then exception must reconcile on A; swapping acquisition to
    # router B before cleanup must not query or delete B.
    $routerA=New-Object SessionNetworkRouterCollection
    $routerA.FailAdd=$true
    $script:nat=[pscustomobject]@{StaticPortMappingCollection=$routerA}
    $discoveriesBefore=$script:discoveries
    $mapping=Add-UpnpMapping -Port 27900
    if(-not $mapping.Ok -or -not $mapping.Owned -or $script:discoveries -ne $discoveriesBefore){throw 'COM lost response fell back to discovery instead of original-router reconciliation.'}
    $routerB=New-Object SessionNetworkRouterCollection
    $script:nat=[pscustomobject]@{StaticPortMappingCollection=$routerB}
    $routerA.FailDelete=$true
    Assert-Rejected {Remove-UpnpMapping -Mapping $mapping} 'COM original router cleanup failure'
    $routerA.FailDelete=$false
    Remove-UpnpMapping -Mapping $mapping
    if($routerA.Entries.Count -or $routerB.Entries.Count){throw 'COM cleanup lost original-router affinity.'}
    # Preserve an explicit unresolved receipt when reconciliation cannot query.
    $routerA.FailQuery=$false; $routerA.FailAdd=$true; $routerA.FailQueryAfterAdd=$true
    $script:nat=[pscustomobject]@{StaticPortMappingCollection=$routerA}
    $mapping=Add-UpnpMapping -Port 27900
    if($mapping.Ok -or $mapping.Owned -or -not $mapping.Unresolved -or -not [object]::ReferenceEquals($mapping.Collection,$routerA)){throw 'Unresolved COM mutation discarded original collection.'}
    Assert-Rejected {Remove-UpnpMapping -Mapping $mapping} 'COM query failure reported as absence'
    $routerA.FailQuery=$false
    $routerA.FailQueryAfterAdd=$false
    Remove-UpnpMapping -Mapping $mapping
    # Exercise the actual CheckOnly router block without running its permanent
    # firewall/public-IP provisioning or its terminal exit statement.
    function Add-UpnpMapping { throw 'CheckOnly NoUpnp attempted router Add.' }
    function Test-UpnpHairpin { throw 'CheckOnly NoUpnp attempted hairpin.' }
    $hostAst=Get-SessionAst 'friend_host.ps1'
    $check=$hostAst.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Clauses[0].Item1.Extent.Text -ceq '$CheckOnly'},$true)
    if(-not $check){throw 'CheckOnly boundary missing.'}
    $NoUpnp=$true; $upnp=$null
    . (Get-SessionStatement $check.Clauses[0].Item2 '^if\s*\(-not \$NoUpnp\)' ([Management.Automation.Language.IfStatementAst]))
    . (Get-SessionStatement $check.Clauses[0].Item2 '^try\s*\{\s*\$hairpin' ([Management.Automation.Language.TryStatementAst]))
    if($upnp -or $hairpin -cne 'skipped'){throw 'NoUpnp fabricated automatic readiness.'}
    # SOAP fault decoding uses actual stream/HttpResponseMessage bodies and
    # preserves both absence 714 and real failure 718 through exception wrapping.
    function Invoke-WebRequest { throw $script:httpFailure }
    foreach($code in @(714,718)) {
        $body="<Envelope><errorCode>$code</errorCode><errorDescription>fixture</errorDescription></Envelope>"
        $responses=@((New-Object SessionNetworkStreamResponse($body)))
        if($PSVersionTable.PSVersion.Major -ge 7) {
            $response=New-Object Net.Http.HttpResponseMessage
            $response.Content=New-Object Net.Http.StringContent($body)
            $responses+=,$response
        }
        foreach($response in $responses) {
            $script:httpFailure=New-Object SessionNetworkHttpFailure($response)
            $observed=$null
            try {& $soapTransport -ControlUrl 'http://offline.invalid' -ServiceType 'fixture' -Action 'GetSpecificPortMappingEntry' -Arguments $null}
            catch {$observed=$_.Exception.Data['UPnPErrorCode']}
            if($observed -ne $code){throw "SOAP lost error code $code through response wrapping."}
            if($response -is [IDisposable]){$response.Dispose()}
        }
    }
    Write-Host 'PASS: production session terminal/capture/environment boundaries, admission bytes, WAN arguments, and offline router ownership.'
} finally {
    if($exitedCaptureProcess){$exitedCaptureProcess.Dispose()}
    Get-ChildItem Env: | Where-Object {$_.Name -like 'KENSHICOOP_*'} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,[NullString]::Value,'Process')}
    foreach($key in $originalEnvironment.Keys){[Environment]::SetEnvironmentVariable($key,$originalEnvironment[$key],'Process')}
    Remove-Item -LiteralPath $root -Recurse -Force
}
