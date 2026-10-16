[CmdletBinding()]
param([string]$OracleRoot = '')
if (-not $OracleRoot) { $OracleRoot = Split-Path -Parent $PSScriptRoot }
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $OracleRoot 'CoopOracles.psm1') -Force
$script:failed = 0
function Check([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  ok $Name" } else { Write-Host "  FAIL $Name"; $script:failed++ }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('coop-oracle-fixture-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
function Log([string]$Name, [string[]]$Lines) {
    $path = Join-Path $root $Name
    [IO.File]::WriteAllLines($path, $Lines, [Text.UTF8Encoding]::new($false))
    return $path
}
function Run-Report([string]$History, [string]$Culture = 'en-US') {
    $scriptPath = (Join-Path $OracleRoot 'report_history.ps1').Replace("'","''")
    $historyPath = $History.Replace("'","''")
    $command = "[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('$Culture'); & '$scriptPath' -HistoryFile '$historyPath'; exit `$LASTEXITCODE"
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = (Get-Process -Id $PID).Path
    $info.Arguments = '-NoProfile -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $info.WorkingDirectory = $PSScriptRoot
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return @{ Code = $process.ExitCode; Text = ($stdout.Result + $stderr.Result) }
    } finally { $process.Dispose() }
}
function Ambient-Rows([string]$Side, [int]$ClockShift = 0, [switch]$Rotate, [switch]$LateHold, [string]$Subject = '6.7.8.9.10') {
    $rows = @()
    if ($ClockShift) { $rows += '[10:00:00.000] CLOCKSYNC offset=-2000 rtt=2' }
    foreach ($i in 0..4) {
        $yaw = if ($Rotate) { [string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0:F2}',($i*0.2)) } else { '0.00' }
        $rows += ("[10:00:{0:00}.000] [poseobserve] side=$Side wire=1.2.3.4.5 local=1.2.3.4.5 raw=221 task=221 native=221 wireSub=$Subject localSub=$Subject subjectRead=1 resolved=1 sid='fixture' fixture=1,2,3 descriptor=1 yaw=0 owner='владелец' pos=1,2,3 actorYaw=$yaw positioned=1 rendered=1 pelvis=3.1 idle=1 crouch=0 floor=1 outside=0 insideSid='室' townSid='town' class=7 inv=0 complete=1 ownerPresent=1 inside=2.3.4.5.6 insidePos=10,2,30 town=3.4.5.6.7 townPos=10,2,30" -f ($i+$ClockShift))
    }
    $end = if ($LateHold) { 20 } else { 4 }
    $rows += ("[10:00:{0:00}.000] SCENARIO AMBIENT NATURAL hand=1.2.3.4.5 name='NPC' t=4000 task=221 subject=$Subject sid='fixture' floor=1 outside=0 pelvis=3.1 heldMs=4000 held=1 actorYaw=0 yaw=0 fixture=1,2,3 ownerPresent=1 owner='владелец' inside=2.3.4.5.6 insideSid='室' insidePos=10,2,30 town=3.4.5.6.7 townSid='town' townPos=10,2,30" -f ($end+$ClockShift))
    return $rows
}
try {
    $enemyHostRows = @(
        '[10:00:01.000] SCENARIO TRAVELFIGHT enemy spawned n=1 parked=1 dist=450',
        '[10:00:02.000] SCENARIO TRAVELFIGHT enemy closed n=1 dist=12'
    )
    $enemyJoinRows = @('[10:00:03.000] [spawn] proxy BOUND hand=1,2,3,4,5')
    foreach ($second in 10..17) {
        $row = "[10:00:$second.000] SCENARIO TRAVELFIGHT enemystate canon=4,5 fight=1 read=1 tgt=8,9 canon5=1,2,3,4,5 target5=1,6,7,8,9"
        $enemyHostRows += $row
        $enemyJoinRows += $row
    }
    $enemyHostLog = Log 'enemy-host.log' $enemyHostRows
    $enemyJoinLog = Log 'enemy-join.log' $enemyJoinRows
    Check 'native enemy agreement requires a readable shared full identity and actual combat' ((Invoke-OneOracle -Id mint_aggro -HostLog $enemyHostLog -JoinLog $enemyJoinLog) -eq 'PASS')
    foreach ($mutation in @(
        @{Name='different object type'; Old='canon5=1,2,3,4,5'; New='canon5=2,2,3,4,5'},
        @{Name='different container'; Old='canon5=1,2,3,4,5'; New='canon5=1,22,3,4,5'},
        @{Name='different container generation'; Old='canon5=1,2,3,4,5'; New='canon5=1,2,33,4,5'},
        @{Name='missing full canonical identity'; Old=' canon5=1,2,3,4,5'; New=''},
        @{Name='contradictory short and full identity'; Old='canon5=1,2,3,4,5'; New='canon5=1,2,3,40,50'},
        @{Name='unreadable native enemy state'; Old='read=1'; New='read=0'}
    )) {
        $changed = @($enemyJoinRows | ForEach-Object { $_.Replace($mutation.Old,$mutation.New) })
        $enemyJoinLog = Log 'enemy-mutated.log' $changed
        Check ("native enemy parity rejects " + $mutation.Name) ((Invoke-OneOracle -Id mint_aggro -HostLog $enemyHostLog -JoinLog $enemyJoinLog) -eq 'FAIL')
    }
    $peaceHost = Log 'enemy-peace-host.log' @($enemyHostRows | ForEach-Object { $_.Replace('fight=1','fight=0') })
    $peaceJoin = Log 'enemy-peace-join.log' @($enemyJoinRows | ForEach-Object { $_.Replace('fight=1','fight=0') })
    Check 'inert enemies on both screens do not prove combat synchronization' ((Invoke-OneOracle -Id mint_aggro -HostLog $peaceHost -JoinLog $peaceJoin) -eq 'FAIL')
    $singleHost = Log 'enemy-single-host.log' @($enemyHostRows[0],$enemyHostRows[1],$enemyHostRows[2])
    $repeatedJoin = Log 'enemy-repeated-join.log' @($enemyJoinRows[0]; 1..8 | ForEach-Object { $enemyJoinRows[1] })
    Check 'one host observation cannot satisfy eight join observations' ((Invoke-OneOracle -Id mint_aggro -HostLog $singleHost -JoinLog $repeatedJoin) -eq 'FAIL')
    $missingEnemyHost = Log 'enemy-missing-host.log' @($enemyHostRows | ForEach-Object { $_.Replace('n=1','n=2') })
    $enemyJoinLog = Log 'enemy-join-complete.log' $enemyJoinRows
    Check 'one shared enemy cannot certify a two-enemy squad' ((Invoke-OneOracle -Id mint_aggro -HostLog $missingEnemyHost -JoinLog $enemyJoinLog) -eq 'FAIL')
    # Each case goes through the public gate: eight is still the minimum, and
    # timestamps (not combat-state similarity) determine the one-use pairing.
    $temporalCase = 0
    foreach ($case in @(
        @{Name='JOIN 900ms later retains all eight observations'; HostTimes=@(0,1000,2000,3000,4000,5000,6000,7000); JoinTimes=@(900,1900,2900,3900,4900,5900,6900,7900); Want='PASS'; Transition=$true},
        @{Name='HOST 900ms later retains all eight observations'; HostTimes=@(900,1900,2900,3900,4900,5900,6900,7900); JoinTimes=@(0,1000,2000,3000,4000,5000,6000,7000); Want='PASS'; Transition=$true},
        @{Name='positive tolerance endpoint is inclusive'; HostTimes=@(0,1000,2000,3000,4000,5000,6000,7000); JoinTimes=@(1500,2500,3500,4500,5500,6500,7500,8500); Want='PASS'},
        @{Name='negative tolerance endpoint is inclusive'; HostTimes=@(1500,2500,3500,4500,5500,6500,7500,8500); JoinTimes=@(0,1000,2000,3000,4000,5000,6000,7000); Want='PASS'},
        @{Name='positive out-of-tolerance endpoint cannot supply eight pairs'; HostTimes=@(0,1000,2000,3000,4000,5000,6000,7000); JoinTimes=@(1501,2501,3501,4501,5501,6501,7501,8501); Want='FAIL'},
        @{Name='negative out-of-tolerance endpoint cannot supply eight pairs'; HostTimes=@(1501,2501,3501,4501,5501,6501,7501,8501); JoinTimes=@(0,1000,2000,3000,4000,5000,6000,7000); Want='FAIL'},
        @{Name='denser HOST cadence preserves eight feasible pairs'; HostTimes=@(-3000,0,500,1000,1500,2000,2500,3000,3500,4000,4500,5000,5500,6000,6500,7000); JoinTimes=@(900,1900,2900,3900,4900,5900,6900,7900); Want='PASS'},
        @{Name='denser JOIN cadence preserves eight feasible pairs'; HostTimes=@(900,1900,2900,3900,4900,5900,6900,7900); JoinTimes=@(-3000,0,500,1000,1500,2000,2500,3000,3500,4000,4500,5000,5500,6000,6500,7000); Want='PASS'},
        @{Name='distinct observations at duplicate timestamps remain usable'; HostTimes=@(0,0,0,0,0,0,0,0); JoinTimes=@(0,0,0,0,0,0,0,0); Want='PASS'},
        @{Name='duplicate timestamps cannot reuse a seventh HOST observation'; HostTimes=@(0,0,0,0,0,0,0); JoinTimes=@(0,0,0,0,0,0,0,0); Want='FAIL'},
        @{Name='phase-shifted combat transitions cannot select matching states'; HostTimes=@(0,1000,2000,3000,4000,5000,6000,7000); JoinTimes=@(900,1900,2900,3900,4900,5900,6900,7900); Want='FAIL'; Transition=$true; Invert=$true},
        @{Name='HOST-only fighting across a transition stays rejected'; HostTimes=@(0,1000,2000,3000,4000,5000,6000,7000); JoinTimes=@(900,1900,2900,3900,4900,5900,6900,7900); Want='FAIL'; PeaceSide='Join'},
        @{Name='JOIN-only fighting across a transition stays rejected'; HostTimes=@(900,1900,2900,3900,4900,5900,6900,7900); JoinTimes=@(0,1000,2000,3000,4000,5000,6000,7000); Want='FAIL'; PeaceSide='Host'}
    )) {
        $temporalCase++
        $caseHost = @($enemyHostRows[0],$enemyHostRows[1])
        $caseJoin = @($enemyJoinRows[0])
        foreach ($side in @('Host','Join')) {
            $times = $case[$side + 'Times']
            for ($i = 0; $i -lt $times.Count; $i++) {
                $stamp = ([datetime]'2000-01-01T10:00:10').AddMilliseconds($times[$i]).ToString('HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture)
                $fight = if ($case.Transition) { $i % 2 } else { 1 }
                if ($side -eq 'Join' -and $case.Invert) { $fight = 1 - $fight }
                if ($case.PeaceSide -eq $side -and $i -ge 4) { $fight = 0 }
                $row = "[$stamp] SCENARIO TRAVELFIGHT enemystate canon=4,5 fight=$fight read=1 tgt=8,9 canon5=1,2,3,4,5 target5=1,6,7,8,9"
                if ($side -eq 'Host') { $caseHost += $row } else { $caseJoin += $row }
            }
        }
        $caseHostLog = Log "enemy-temporal-$temporalCase-host.log" $caseHost
        $caseJoinLog = Log "enemy-temporal-$temporalCase-join.log" $caseJoin
        Check ("native enemy pairing: " + $case.Name) ((Invoke-OneOracle -Id mint_aggro -HostLog $caseHostLog -JoinLog $caseJoinLog) -eq $case.Want)
    }
    $pickupHost = @(
        '[10:00:00.000] WPMIRROR start role=host have=1 hand=11,12,13,14,15',
        "[10:00:01.000] [wi] SEND netId=42 sid='material' qty=1 pos=1,2,3 hash=7 owner=1 type=3 hand=1,2,3,4,5",
        "[10:00:03.000] [wi] CLAIM-APPLY netId=42 from=2 sid='material' destroyed=1 author=1 type=3 itemhand=1,2,3,4,5",
        "[10:00:26.000] WPMIRROR verdict role=host pass=1 sid='material' dropped=1 grndPeak=1 grndFinal=0 pickedUp=0 type=3 bagBase=1 bagFinal=0 groundBase=0 groundFinal=0 stable=1 complete=1 stableMs=2000 sampleCount=5 itemHand=1,2,3,4,5 dropOwner=1 dropId=42 claimOwner=1 afterItemHand=1,2,3,4,5"
    )
    $pickupJoin = @(
        '[10:00:00.000] WPMIRROR start role=join have=1 hand=21,22,23,24,25',
        "[10:00:01.100] [wi] SPAWN owner=1 netId=42 ok=1 sid='material' pos=1,2,3 hand=2,3,4,5,6 type=3 qty=1",
        '[10:00:02.000] [wi] CLAIM author=1 netId=42 (proxy consumed locally) claimOwner=2 type=3 itemhand=2,3,4,5,6 complete=1 afterItemhand=2,3,4,50,60 recipient=21,22,23,24,25',
        "[10:00:22.000] WPMIRROR verdict role=join pass=1 sid='material' dropped=0 grndPeak=1 grndFinal=0 pickedUp=1 type=3 bagBase=0 bagFinal=1 groundBase=0 groundFinal=0 stable=1 complete=1 stableMs=2000 sampleCount=5 itemHand=2,3,4,5,6 dropOwner=1 dropId=42 claimOwner=2 afterItemHand=2,3,4,50,60"
    )
    $hostPickupLog = Log 'physical-pickup-host.log' $pickupHost
    $joinPickupLog = Log 'physical-pickup-join.log' $pickupJoin
    Check 'exact physical pickup permits distinct local handles and recorded native rekey' ((Test-WorldPickupMirror -HostFile $hostPickupLog -JoinFile $joinPickupLog) -eq 'PASS')
    foreach ($mutation in @(
        @{Name='wrong full claimant handle'; Old='itemhand=2,3,4,5,6 complete'; New='itemhand=2,99,4,5,6 complete'},
        @{Name='wrong recipient container'; Old='recipient=21,22,23,24,25'; New='recipient=21,99,23,24,25'},
        @{Name='unrelated after-pickup handle'; Old='afterItemhand=2,3,4,50,60 recipient'; New='afterItemhand=2,3,4,51,60 recipient'},
        @{Name='wrong item type'; Old='type=3 qty=1'; New='type=8 qty=1'},
        @{Name='duplicated spawn quantity'; Old='type=3 qty=1'; New='type=3 qty=2'},
        @{Name='extra bag item'; Old='bagBase=0 bagFinal=1'; New='bagBase=0 bagFinal=2'},
        @{Name='incomplete native claim'; Old='complete=1 afterItemhand'; New='complete=0 afterItemhand'},
        @{Name='wrong claim owner'; Old='claimOwner=2 type'; New='claimOwner=3 type'}
    )) {
        $changed = @($pickupJoin | ForEach-Object { $_.Replace($mutation.Old,$mutation.New) })
        $joinPickupLog = Log 'physical-pickup-mutated.log' $changed
        Check ("physical pickup rejects " + $mutation.Name) ((Test-WorldPickupMirror -HostFile $hostPickupLog -JoinFile $joinPickupLog) -eq 'FAIL')
    }
    $gearHost = @(
        "[10:00:00.000] WGRP start role=host have=1 sid='gear' type=8 bagBase=0 r0=11,12,13,14,15 r1=21,22,23,24,25",
        "[10:00:08.000] [wd] DROP id=42 sid='gear' owner=11,12,13,14,15 pos=1,2,3 tracked=1 level=1 equipped=0 slot=0 section=0 native=1 dropOwner=1 type=8 itemhand=1,2,3,4,5",
        "[10:00:16.100] [wd] PICKUP-APPLY id=55 sid='gear' owner=21,22,23,24,25 ref=1/42 moved=1 why=ok trackedLeft=0 pickupOwner=2 type=8 target=21,22,23,24,25 beforeItemhand=1,2,3,4,5 afterItemhand=1,2,3,40,50",
        "[10:00:40.000] WGRP verdict role=host pass=1 sid='gear' dropped=1 grndPeak=1 grndFinal=0 bagR1=1 bagBase=0 pickedUp=0 type=8 bagFinal=1 sourceBase=1 sourceFinal=0 groundBase=0 groundFinal=0 stable=1 complete=1 stableMs=2000 sampleCount=5 sourceHand=11,12,13,14,15 itemHand=1,2,3,4,5 dropOwner=1 dropId=42 afterItemHand=1,2,3,40,50"
    )
    $gearJoin = @(
        "[10:00:00.000] WGRP start role=join have=1 sid='gear' type=8 bagBase=0 r0=11,12,13,14,15 r1=21,22,23,24,25",
        "[10:00:08.100] [wd] APPLY id=42 sid='gear' owner=11,12,13,14,15 moved=1 pos=1,2,3 tracked=1 dropOwner=1 type=8 itemhand=2,3,4,5,6",
        "[10:00:16.000] [wd] PICKUP id=55 sid='gear' owner=21,22,23,24,25 ref=1/42 native=1 pickupOwner=2 type=8 beforeItemhand=2,3,4,5,6 afterItemhand=2,3,4,50,60",
        "[10:00:30.000] WGRP verdict role=join pass=1 sid='gear' dropped=0 grndPeak=1 grndFinal=0 bagR1=1 bagBase=0 pickedUp=1 type=8 bagFinal=1 sourceBase=1 sourceFinal=0 groundBase=0 groundFinal=0 stable=1 complete=1 stableMs=2000 sampleCount=5 sourceHand=11,12,13,14,15 itemHand=2,3,4,5,6 dropOwner=1 dropId=42 afterItemHand=2,3,4,50,60"
    )
    $hostGearLog = Log 'physical-gear-host.log' $gearHost
    $joinGearLog = Log 'physical-gear-join.log' $gearJoin
    Check 'exact gear pickup uses physical quantities and both native transitions' ((Test-GearRepickup -HostFile $hostGearLog -JoinFile $joinGearLog) -eq 'PASS')
    foreach ($mutation in @(
        @{Name='wrong target full handle'; Old='target=21,22,23,24,25'; New='target=21,99,23,24,25'},
        @{Name='wrong host physical ground incarnation'; Old='beforeItemhand=1,2,3,4,5'; New='beforeItemhand=1,99,3,4,5'},
        @{Name='wrong host bag incarnation'; Old='afterItemhand=1,2,3,40,50'; New='afterItemhand=1,2,3,41,50'},
        @{Name='wrong pickup occurrence'; Old='PICKUP-APPLY id=55'; New='PICKUP-APPLY id=56'},
        @{Name='wrong physical type'; Old='pickupOwner=2 type=8'; New='pickupOwner=2 type=3'},
        @{Name='unconsumed source quantity'; Old='sourceFinal=0'; New='sourceFinal=1'}
    )) {
        $changed = @($gearHost | ForEach-Object { $_.Replace($mutation.Old,$mutation.New) })
        $hostGearLog = Log 'physical-gear-mutated.log' $changed
        Check ("gear pickup rejects " + $mutation.Name) ((Test-GearRepickup -HostFile $hostGearLog -JoinFile $joinGearLog) -eq 'FAIL')
    }
    $retryHost = @($gearHost[0],$gearHost[1],
        "[10:00:16.050] [wd] REHOME-REFUSE-INJECTED (test lever: re-home refused) sid='gear' ref=1/42",
        "[10:00:16.100] [wd] PICKUP-APPLY id=55 sid='gear' owner=21,22,23,24,25 ref=1/42 moved=0 why=refused trackedLeft=1 pickupOwner=2 type=8 target=21,22,23,24,25 beforeItemhand=1,2,3,4,5 afterItemhand=0,0,0,0,0",
        "[10:00:17.000] [wd] PICKUP-RETRY-OK sid='gear' ref=1/42 via=track afterMs=900 type=8 target=21,22,23,24,25 beforeItemhand=1,2,3,4,5 afterItemhand=1,2,3,40,50",
        $gearHost[3])
    $hostGearLog = Log 'physical-gear-retry.log' $retryHost
    Check 'refused exact physical pickup later rehomes the same instance' ((Test-GearRepickup -HostFile $hostGearLog -JoinFile $joinGearLog -RequireRetry) -eq 'PASS')
    $hostGearLog = Log 'physical-gear-retry-before-refusal.log' @($retryHost | ForEach-Object { $_.Replace('[10:00:16.050]','[10:00:18.000]') })
    Check 'retry cannot precede its refusal even if log line order agrees' ((Test-GearRepickup -HostFile $hostGearLog -JoinFile $joinGearLog -RequireRetry) -eq 'FAIL')
    $hostZero = Log 'physical-gear-host-zero.log' @($gearHost | ForEach-Object { $_.Replace('dropOwner=1','dropOwner=0').Replace('ref=1/42','ref=0/42') })
    $joinZero = Log 'physical-gear-join-zero.log' @($gearJoin | ForEach-Object { $_.Replace('dropOwner=1','dropOwner=0').Replace('ref=1/42','ref=0/42') })
    Check 'authenticated host seat zero is a valid exact drop owner' ((Test-GearRepickup -HostFile $hostZero -JoinFile $joinZero) -eq 'PASS')
    $valid = Log 'valid.log' @('[10:00:00.000] [HOST] SCENARIO RESULT PASS')
    Check 'one exact successful terminal record' ((Test-ScenarioResultPass -File $valid -Label host -Required $true) -eq 'PASS')
    $conflict = Log 'conflict.log' @('[10:00:00.000] [HOST] SCENARIO RESULT PASS','[10:00:01.000] [HOST] SCENARIO RESULT FAIL')
    Check 'conflicting completions cannot pass' ((Test-ScenarioResultPass -File $conflict -Label host -Required $true) -eq 'FAIL')
    $substring = Log 'substring.log' @('[10:00:00.000] diagnostic expected SCENARIO RESULT PASS but never completed')
    Check 'diagnostic PASS substring is not completion' ((Test-ScenarioResultPass -File $substring -Label host -Required $true) -eq 'FAIL')
    $pending = Log 'pending.log' @('[10:00:00.000] [coop-ui] CONNECT role=JOIN transport=udp')
    Check 'attempted unresolved panel action fails' ((Test-PanelConfig -File $pending) -eq 'FAIL')
    $panel = Log 'panel.log' @('[10:00:00.000] [coop-ui] CONNECT role=JOIN transport=udp','[10:00:00.100] [coop-ui] connect: role=JOIN transport=udp peer=0 ownRanks={1} src=role')
    Check 'ordered panel resolution passes' ((Test-PanelConfig -File $panel) -eq 'PASS')
    $old = Log 'old-panel.log' @('[10:00:00.000] [coop-ui] connect: role=JOIN transport=udp peer=0 ownRanks={1} src=role','[10:00:01.000] [coop-ui] CONNECT role=JOIN transport=udp')
    Check 'old resolution cannot satisfy later intent' ((Test-PanelConfig -File $old) -eq 'FAIL')
    $unused = Log 'unused-panel.log' @('[10:00:00.000] gameplay started')
    Check 'unused panel remains inconclusive' ((Test-PanelConfig -File $unused) -eq 'SKIP')
    $hostLog = Log 'midnight-host.log' @('[23:59:59.500] SCENARIO MEMBER hand=1,2,3,4,5 pos=1.5,2,3','[00:00:00.500] SCENARIO MEMBER hand=1,2,3,4,5 pos=2.5,2,3')
    $join = Log 'midnight-join.log' @('[00:00:01.500] CLOCKSYNC offset=-2000 rtt=2','[00:00:01.500] SCENARIO RECV hand=1,2,3,4,5 pos=1.5,2,3','[00:00:02.500] SCENARIO RECV hand=1,2,3,4,5 pos=2.5,2,3')
    Reset-GateResults
    $h = (Get-ScenarioSeries -File $hostLog -Kind MEMBER)['1,2,3,4,5']
    $j = (Get-ScenarioSeries -File $join -Kind RECV)['1,2,3,4,5']
    Check 'midnight chronology advances by one second' (($h[1].t-$h[0].t) -eq 1000)
    Check 'clock offset joins opposite midnight sides' (($h[0].t -eq $j[0].t) -and ($h[1].t -eq $j[1].t))
    Check 'odd median chooses middle observation' ((& (Get-Module CoopOracles) { Get-Median @(1,2,100) }) -eq 2)
    Check 'even median averages middle observations' ((& (Get-Module CoopOracles) { Get-Median @(1,3,7,9) }) -eq 5)
    $hostLog = Log 'single-host.log' @('SCENARIO INVE r=0 OWN t=1 count=5 eq=1 hash=7','SCENARIO INVE r=1 PEER t=1 count=5 eq=1 hash=7','SCENARIO INVE UNEQUIP r=0 n=1')
    $join = Log 'single-join.log' @('SCENARIO INVE r=1 OWN t=1 count=5 eq=1 hash=7','SCENARIO INVE r=0 PEER t=1 count=5 eq=1 hash=7','SCENARIO INVE UNEQUIP r=1 n=1')
    Check 'unchanged singleton equipment cannot prove removal' ((Test-InventoryEquip -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $hostLines = @(); $joinLines = @()
    foreach ($i in 0..5) {
        $hostLines += ('[10:00:{0:00}.000] SCENARIO GTIME hours=1 hourLen=60 fsm=1 paused=0 ok=1 t={1}' -f $i, ($i*1000))
        $joinLines += ('[11:00:{0:00}.000] SCENARIO GTIME hours=1 hourLen=60 fsm=1 paused=0 ok=1 t={1}' -f $i, ($i*1000))
    }
    $hostLog = Log 'time-host.log' $hostLines; $join = Log 'time-join.log' $joinLines
    Check 'no aligned game-clock pair cannot pass' ((Test-TimeSync -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $join = Log 'time-aligned.log' $hostLines
    Check 'aligned readable clock tail passes' ((Test-TimeSync -HostFile $hostLog -JoinFile $join) -eq 'PASS')
    $hostLog = Log 'event-host.log' @('[10:00:01.000] [event] SEND id=10 ev=2 hand=3,4,5,1,2 actor=7,8 bs 0->4 owner=1 actor5=9,10,11,7,8')
    $join = Log 'event-join.log' @('[10:00:01.100] [event] RECV id=10 ev=2 owner=1 hand=3,4,5,1,2 actor=7,8 actor5=9,10,11,7,8')
    Check 'same reliable death occurrence passes' ((Test-DeathOrder -HostFile $hostLog -JoinFile $join) -eq 'PASS')
    $join = Log 'event-wrong.log' @('[10:00:01.100] [event] RECV id=11 ev=2 owner=1 hand=3,4,5,1,2 actor=7,8 actor5=9,10,11,7,8')
    Check 'different event occurrence cannot satisfy receipt' ((Test-DeathOrder -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $join = Log 'event-early.log' @('[10:00:00.900] [event] RECV id=10 ev=2 owner=1 hand=3,4,5,1,2 actor=7,8 actor5=9,10,11,7,8')
    Check 'receipt before send cannot prove delivery' ((Test-DeathOrder -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $join = Log 'event-actor.log' @('[10:00:01.100] [event] RECV id=10 ev=2 owner=1 hand=3,4,5,1,2 actor=7,8 actor5=9,99,11,7,8')
    Check 'full actor collision cannot satisfy receipt' ((Test-DeathOrder -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $join = Log 'event-body.log' @('[10:00:01.100] [event] RECV id=10 ev=2 owner=1 hand=3,99,5,1,2 actor=7,8 actor5=9,10,11,7,8')
    Check 'same short victim in different container cannot match' ((Test-DeathParity -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $hostLog = Log 'two-events.log' @('[10:00:01.000] [event] SEND id=10 ev=2 hand=3,4,5,1,2 actor=7,8 bs 0->4 owner=1 actor5=9,10,11,7,8','[10:00:02.000] [event] SEND id=11 ev=2 hand=3,4,5,1,2 actor=7,8 bs 0->4 owner=1 actor5=9,10,11,7,8')
    $join = Log 'one-receipt.log' @('[10:00:02.100] [event] RECV id=10 ev=2 owner=1 hand=3,4,5,1,2 actor=7,8 actor5=9,10,11,7,8')
    Check 'one receipt cannot satisfy two death occurrences' ((Test-DeathParity -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    Reset-GateResults
    $hostLog = Log 'ambient-host.log' (Ambient-Rows SRC)
    $join = Log 'ambient-join.log' (Ambient-Rows DRIVE -ClockShift 2)
    Check 'independent natural hold with skewed driven clock passes' ((Invoke-OneOracle -Id ambient_pose -HostLog $hostLog -JoinLog $join -Tolerance 3.0) -eq 'PASS')
    $hostLog = Log 'ambient-late.log' (Ambient-Rows SRC -LateHold)
    Check 'unrelated later native hold cannot qualify pose window' ((Invoke-OneOracle -Id ambient_pose -HostLog $hostLog -JoinLog $join -Tolerance 3.0) -eq 'FAIL')
    $hostLog = Log 'ambient-rotate-host.log' (Ambient-Rows SRC -Rotate)
    $join = Log 'ambient-rotate-join.log' (Ambient-Rows DRIVE -ClockShift 2 -Rotate)
    Check 'jointly rotating actors have not held facing' ((Invoke-OneOracle -Id ambient_pose -HostLog $hostLog -JoinLog $join -Tolerance 3.0) -eq 'FAIL')
    $hostLog = Log 'ambient-good.log' (Ambient-Rows SRC)
    $join = Log 'ambient-other-subject.log' (Ambient-Rows DRIVE -Subject '6.7.8.99.10')
    Check 'different full fixture identity cannot satisfy pose' ((Invoke-OneOracle -Id ambient_pose -HostLog $hostLog -JoinLog $join -Tolerance 3.0) -eq 'FAIL')
    $hostLines = @(); $joinLines = @()
    foreach ($i in 0..6) {
        $stamp = '[10:01:{0:00}.000]' -f ($i*5)
        $hostLines += "$stamp SCENARIO WNPC hand=1,2,3,4,5 pos=0,0,0 cls=pc name='A' task=0 pelvis=3 mv=0 carry=0"
        $hostLines += "$stamp SCENARIO WNPC hand=1,2,3,6,7 pos=10,0,0 cls=pc name='B' task=0 pelvis=3 mv=0 carry=0"
    }
    $hostLog = Log 'roster-full.log' $hostLines; $join = Log 'roster-full-peer.log' $hostLines
    Check 'two full bodies sharing short pair remain distinct' ((Test-PcDuplicates -HostFile $hostLog -JoinFile $join -GraceMs 0) -eq 'PASS')
    $joinLines = @($hostLines | ForEach-Object { $_.Replace('1,2,3,4,5','1,2,3,40,50').Replace('1,2,3,6,7','1,2,3,60,70') })
    $join = Log 'roster-collision.log' $joinLines
    Check 'independently minted short collisions cannot prove roster parity' ((Test-PcDuplicates -HostFile $hostLog -JoinFile $join -GraceMs 0) -eq 'FAIL')
    $manifest = Log 'manifest.psd1' @("@{ Scenarios=@{ fixture=@{Gating=@(); Advisory=@(); PrimaryGate=''} } }")
    $hostLog = Log 'complete-host.log' @('[10:00:00.000] [HOST] INFO: gameplay started','[10:00:01.000] [HOST] INFO: SCENARIO fixture start','[10:00:02.000] [HOST] INFO: SCENARIO RESULT PASS')
    $join = Log 'complete-join.log' @('[10:00:00.000] [JOIN] INFO: gameplay started','[10:00:01.000] [JOIN] INFO: SCENARIO fixture start','[10:00:02.000] [JOIN] INFO: SCENARIO RESULT PASS')
    $v = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $join -Scenario fixture -ManifestPath $manifest -RunInfo @{ ownedExitFailures=@() }
    Check 'complete diagnostic capture never certifies gameplay' ($v.captureOk -and $v.diagnostic -and -not $v.pass)
    $v = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $join -Scenario fixture -ManifestPath $manifest -RunInfo @{ ownedExitFailures=@('host forced termination') }
    Check 'successful log cannot hide forced process termination' (-not $v.captureOk -and -not $v.pass)
    $v = Invoke-RunAnalysis -HostLog $hostLog -Scenario fixture -ManifestPath $manifest -JoinExpected $false
    Check 'omitted required participant cannot certify capture' (-not $v.captureOk -and -not $v.pass)
    $required = Log 'engine-manifest.psd1' @("@{ Scenarios=@{ fixture=@{Gating=@('engine_integrity'); Advisory=@(); PrimaryGate=''} } }")
    $v = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $join -Scenario fixture -ManifestPath $required
    Check 'required engine integrity cannot skip to green' (-not $v.pass)
    [void](Log 'host_engine.log' @('Item Steel bars alredy has destroy reason addItem (addItem)'))
    [void](Log 'join_engine.log' @('normal native startup'))
    Check 'untagged native double destruction cannot certify integrity' ((Test-EngineIntegrity -OutDir $root) -eq 'FAIL')
    [void](Log 'host_engine.log' @('Steel bars was not removed from any zones'))
    Check 'unattributed dangling zone entry cannot certify integrity' ((Test-EngineIntegrity -OutDir $root) -eq 'FAIL')
    [void](Log 'host_engine.log' @('normal native startup'))
    Check 'complete clean native lifetime archives certify integrity' ((Test-EngineIntegrity -OutDir $root) -eq 'PASS')
    $malformed = Log 'malformed.jsonl' @('{not json')
    $report = Run-Report $malformed
    Check 'malformed-only history is not healthy' ($report.Code -ne 0)
    $historyLines = @()
    foreach ($i in 0..3) { $historyLines += ('{"timestamp":"2020-01-0'+($i+1)+'T00:00:00Z","scenario":"fixture","variant":"clean","pass":true,"flaky":false,"gates":[{"gate":"march","status":"PASS","metrics":{"idleFrac":' + $(if ($i -eq 3) { '0.1' } else { '0' }) + '}}]}') }
    $history = Log 'zero-history.jsonl' $historyLines
    $report = Run-Report $history
    Check 'zero to nonzero error metric is degradation' ($report.Code -eq 2)
    $historyLines = @()
    foreach ($i in 0..3) { $historyLines += ('{"timestamp":"2020-01-0'+($i+1)+'T00:00:00Z","scenario":"fixture","variant":"clean","pass":true,"flaky":false,"gates":[{"gate":"motion","status":"PASS","metrics":{"worstMedian":' + $(if ($i -eq 3) { '1.8' } else { '1.5' }) + '}}]}') }
    $history = Log 'culture-history.jsonl' $historyLines
    foreach ($culture in @('en-US','de-DE','fr-FR')) {
        $report = Run-Report $history $culture
        Check "numeric history has invariant trend under $culture" ($report.Code -eq 0)
    }
    $historyLines[-1] = '{"timestamp":"2020-01-04T00:00:00Z","scenario":"fixture","variant":"clean","pass":true,"flaky":false,"gates":[]}'
    $history = Log 'missing-latest.jsonl' $historyLines
    $report = Run-Report $history
    Check 'older last available metric cannot substitute for latest run' ($report.Code -eq 2)
    $hostLog = Log 'pickup-unrelated-host.log' @("WPMIRROR verdict role=host pass=1 sid='itemA' dropped=1 grndPeak=1 grndFinal=0")
    $join = Log 'pickup-unrelated-join.log' @("WPMIRROR verdict role=join pass=1 sid='itemB' pickedUp=1")
    Check 'wrong pickup template cannot prove physical conservation' ((Test-WorldPickupMirror -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $hostLog = Log 'repickup-unrelated-host.log' @("WGRP verdict role=host pass=1 sid='itemA' dropped=1 grndPeak=1 grndFinal=0 bagR1=1 bagBase=0")
    $join = Log 'repickup-unrelated-join.log' @("WGRP verdict role=join pass=1 sid='itemB' pickedUp=1 tries=1 bagR1=1 bagBase=0")
    Check 'different repickup templates cannot prove physical transfer' ((Test-GearRepickup -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $hostLog = Log 'drop-transient-host.log' @("SCENARIO INVDF verdict role=host pass=1 sid='itemA' invBase=1 invAfter=0 grndAfter=1 churns=5","[wd] DROP id=42")
    $join = Log 'drop-transient-join.log' @("SCENARIO INVDF verdict role=join pass=1 sid='itemA' invBase=1 invMin=0 grndMax=1 conserved=1","[wd] APPLY id=42 moved=1")
    Check 'non-simultaneous inventory extrema are not conservation proof' ((Test-InventoryDropFull -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $hostLog = Log 'shackle-host.log' @('[10:00:00.000] SCENARIO SHACKLE hand=1,2 t=1 chained=1 shackleItem=1 lock=1 owner=1,2')
    $join = Log 'shackle-other.log' @('[10:00:00.000] SCENARIO SHACKLE hand=3,4 t=1 chained=1 shackleItem=1 lock=1 owner=3,4')
    Check 'no shared prisoner cannot prove shackle parity' ((Test-ShackleSync -HostFile $hostLog -JoinFile $join) -eq 'FAIL')
    $savedAppData = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = Join-Path $root 'appdata'
        $saveDir = Join-Path $env:LOCALAPPDATA 'kenshi\save\coopresume'
        [void][IO.Directory]::CreateDirectory($saveDir)
        [IO.File]::WriteAllBytes((Join-Path $saveDir 'portraits_texture.png'), [byte[]]@(1))
        $hostLines = @("SCENARIO LSBUILD rc=1 ok=1 sid='runtime-site' hand=1.2.3.4.5",'SCENARIO LSACK ok=1',"[load] GO->join id=42 name='coopresume' fp=ab",'[load] session reset','SCENARIO LSSWAPDONE',"SCENARIO LSSITE hand=1.2.3.4.5 sid='runtime-site' prog=0.5 complete=0")
        $joinLines = @("[load] GO id=42 name='coopresume' fp=ab MATCH -> loading",'[load] session reset','SCENARIO LSSWAPDONE',"SCENARIO LSSITE hand=1.2.3.4.5 sid='runtime-site' prog=0.5 complete=0")
        $hostLog = Log 'load-host.log' $hostLines; $join = Log 'load-join.log' $joinLines
        Check 'matching load transaction preserves actual runtime site' ((Invoke-OneOracle -Id load_sync -HostLog $hostLog -JoinLog $join) -eq 'PASS')
        $joinLines[0] = "[load] GO id=43 name='coopresume' fp=ab MATCH -> loading"
        $join = Log 'load-wrong-id.log' $joinLines
        Check 'different load transaction cannot satisfy swap proof' ((Invoke-OneOracle -Id load_sync -HostLog $hostLog -JoinLog $join) -eq 'FAIL')
        $joinLines[0] = "[load] GO id=42 name='coopresume' fp=ac MATCH -> loading"
        $join = Log 'load-wrong-fp.log' $joinLines
        Check 'different save fingerprint cannot prove coordinated load' ((Invoke-OneOracle -Id load_sync -HostLog $hostLog -JoinLog $join) -eq 'FAIL')
        $joinLines = @("SCENARIO LSSITE hand=1.2.3.4.5 sid='runtime-site' prog=0.5 complete=0","[load] GO id=42 name='coopresume' fp=ab MATCH -> loading",'[load] session reset','SCENARIO LSSWAPDONE')
        $join = Log 'load-old-site.log' $joinLines
        Check 'pre-swap site cannot prove post-load identity' ((Invoke-OneOracle -Id load_sync -HostLog $hostLog -JoinLog $join) -eq 'FAIL')
    } finally { $env:LOCALAPPDATA = $savedAppData }
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
exit $script:failed
