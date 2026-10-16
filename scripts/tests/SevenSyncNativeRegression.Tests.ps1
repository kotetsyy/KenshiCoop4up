$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../oracles/EnvironmentItems.ps1')
# Exercise the actual consumer: complete membership -> real removal -> sustained paired absence.
function New-EnvironmentEvidence([string]$Side){
    $load=if($Side -eq 'HOST'){3}else{7}
    $scope=if($Side -eq 'HOST'){2}else{4}
    "SCENARIO ENVIRONMENT ITEM side=$Side stage=0 t=5000 world=99 load=$load scope=$scope region=22,32 epoch=5 id=42 bound=1 native=1234abcd hand=1,2,3,4,5 sid='food' type=1 qty=2 condition=1 charges=0/0 manufacturer='' material='' grade=255 pos=1,2,3 rot=1,0,0,0 floor=0 flags=1 parent='' parentSID='' owner='' persistent='' ownerSID='' ownerStream=0 ownerKey=0,0,0,0,0 persistentSID='' persistentStream=0 persistentKey=0,0,0,0,0"
    "SCENARIO ENVIRONMENT SAMPLE side=$Side stage=0 t=5000 world=99 load=$load scope=$scope region=22,32 room='shop' epoch=5 count=1 nativeCount=1 digest=1234abcd nativeDigest=1234abcd ready=1 protected=1 preserved=1"
    if($Side -eq 'HOST'){'SCENARIO ENVIRONMENT MUTATE side=HOST t=10000 world=99 load=3 scope=2 region=22,32 epoch=5 id=42 nativeRemoved=1'}
    for($i=0;$i -lt 8;$i++){
        $t=12000+$i*500
        "SCENARIO ENVIRONMENT SAMPLE side=$Side stage=1 t=$t world=99 load=$load scope=$scope region=22,32 room='shop' epoch=6 count=0 nativeCount=0 digest=00000000 nativeDigest=00000000 ready=1 protected=1 preserved=1"
    }
}
function Assert-EnvironmentGate($H,$J,[bool]$Expected,[string]$Case){
    $r=Test-EnvironmentItemsNativePair -HostLines $H -JoinLines $J
    if($r.Passed -ne $Expected){throw "$Case expected $Expected; errors: $($r.Errors -join '; ')"}
}
$h=@(New-EnvironmentEvidence HOST);$j=@(New-EnvironmentEvidence JOIN)
Assert-EnvironmentGate $h $j $true 'complete native category and removal convergence'
Assert-EnvironmentGate $h @() $false 'missing JOIN cannot pass'
Assert-EnvironmentGate $h @($j|Where-Object {$_ -notmatch ' ITEM '}) $false 'catalog/ACK alone cannot prove native membership'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('protected=1','protected=0')}) $false 'unknown saved sentinel must be present'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('preserved=1','preserved=0')}) $false 'unknown sentinel mutation fails'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('world=99','world=98')}) $false 'stale world cannot pair'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('scope=4','scope=0')}) $false 'no admitted region scope fails'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('qty=2','qty=3')}) $false 'same digest cannot hide native quantity divergence'
Assert-EnvironmentGate $h @($j|ForEach-Object {$_.Replace('epoch=6','epoch=5')}) $false 'removal must advance epoch'
Assert-EnvironmentGate @($h|Where-Object {$_ -notmatch ' MUTATE '}) $j $false 'static agreement is not lifecycle mutation proof'
Assert-EnvironmentGate $h @($j|Where-Object {$_ -notmatch 't=15500 '}) $false 'insufficient post-removal membership observations'
$r=Test-EnvironmentItemsNativePair -HostLines $h -JoinLines $j -Lifecycle
if($r.Passed){throw 'baseline/removal alone must not pass unload/reconnect lifecycle'}
Import-Module (Join-Path $PSScriptRoot '../CoopOracles.psm1') -Force
. (Join-Path $PSScriptRoot '../oracles/ProceduralBeds.ps1')
. (Join-Path $PSScriptRoot '../oracles/SevenSyncNative.ps1')
function New-CarryEvidence([string]$Side,[string]$Fixture){
    $identity='passenger=1,2,3,4,5 carrier=6,7,8,9,10 world=99 load=3'
    foreach($action in $(if($Side -eq 'owner'){@('exit')}else{@('pickup','placement')})){
        $t=if($action -eq 'exit'){42000}elseif($action -eq 'pickup'){16000}else{20000}
        "SCENARIO CARRYBED ACTION side=$Side ms=$t action=$action ok=1 $identity"
    }
    for($t=18000;$t -lt 19500;$t+=250){"SCENARIO CARRYBED side=$Side ms=$t $identity read=1 kind=0 furn=0,0,0,0,0 carried=1 failed=0 exactCarry=1 moved=0"}
    for($t=25000;$t -lt 40000;$t+=250){
        "SCENARIO CARRYBED side=$Side ms=$t $identity read=1 kind=1 furn=$Fixture carried=0 failed=0 exactCarry=0 moved=0"
        "SCENARIO CARRYBED FIXTURE side=$Side ms=$t world=99 load=3 descriptor=1 sid='sleeping-bag' pos=1,2,3 heading=0 floor=0 class=1 owner='' inside='' insidePos=0,0,0 town='hub' townPos=4,5,6"
    }
    for($t=47000;$t -le 52000;$t+=250){"SCENARIO CARRYBED side=$Side ms=$t $identity read=1 kind=0 furn=0,0,0,0,0 carried=0 failed=0 exactCarry=0 moved=1"}
    for($t=18000;$t -le 52000;$t+=250){
        "SCENARIO CARRYBED CLAIM side=$Side ms=$t passengerOwner=0 passengerCanonical=1,2,3,4,5 carrierOwner=1 carrierCanonical=6,7,8,9,10 world=99 load=3"
    }
}
$temp=Join-Path ([IO.Path]::GetTempPath()) ('SevenSyncOracle-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temp)
try{
    $hostPath=Join-Path $temp 'host.log';$joinPath=Join-Path $temp 'join.log'
    function Assert-CarryGate($H,$J,[string]$Want,[string]$Case){
        $H|Set-Content -LiteralPath $hostPath;$J|Set-Content -LiteralPath $joinPath
        Reset-GateResults
        [void](Test-CarryBedNative -HostFile $hostPath -JoinFile $joinPath)
        $gate=@(Get-GateResults)[-1]
        if($gate.status -ne $Want){throw "$Case expected $Want; got $($gate.status): $($gate.detail)"}
    }
    $hc=@(New-CarryEvidence owner '4,3,2,1,9');$jc=@(New-CarryEvidence carrier '9,8,7,6,5')
    Assert-CarryGate $hc $jc PASS 'native fixture handles differ but canonical descriptor agrees'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace('passenger=1,2,3,4,5','passenger=9,2,3,4,6')}) PASS 'peer-local native handles may differ while admitted canonical actor agrees'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace("pos=1,2,3 ","pos=8,2,3 ")}) FAIL 'same sleeping bag template at different position cannot pass'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace('exactCarry=1','exactCarry=0')}) FAIL 'bed occupancy alone does not prove actual cross-owner carry'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace('passengerCanonical=1,2,3,4,5','passengerCanonical=1,2,3,4,6')}) FAIL 'canonical serial reuse cannot impersonate passenger'
    Assert-CarryGate @($hc|ForEach-Object {$_.Replace('moved=1','moved=0')}) $jc FAIL 'voluntary owner exit requires actual movement'
    Assert-CarryGate $hc @($jc|Where-Object {$_ -notmatch 'action=placement'}) FAIL 'native one-shot placement must exist'
    Assert-CarryGate $hc @($jc|Where-Object {$_ -notmatch 'ms=3[0-9][0-9][0-9][0-9]'}) FAIL 'sample gap cannot count as sustained occupancy'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace('passengerOwner=0','passengerOwner=1')}) FAIL 'separate squad tabs are not authenticated ownership claims'
    Assert-CarryGate $hc @($jc|ForEach-Object {$_.Replace('world=99','world=98')}) FAIL 'stale world cannot prove current carry occupancy'
    function New-TransactionEvidence([string]$Role,[switch]$Extraction,[switch]$SingleStock){
        $actor=if($Role -eq 'HOST'){'1,2,3,4,5'}else{'6,7,8,9,10'}
        $peer=if($Role -eq 'HOST'){'6,7,8,9,10'}else{'1,2,3,4,5'}
        $owner=if($Role -eq 'HOST'){0}else{1};$peerOwner=1-$owner
        for($t=0;$t -le 90000;$t+=500){
            $stock=10;$hu=1;$ju=2;$wallet=100;$revision=1
            if($t -ge 5000){$stock=8;$hu=3;$wallet=80;$revision=2}
            if($t -ge 6000){$stock=10;$hu=1;$wallet=90;$revision=3}
            if($t -ge 30000){$stock=8;$ju=4;$wallet=70;$revision=4}
            if($t -ge 31000){$stock=10;$ju=2;$wallet=80;$revision=5}
            $extractionFlag=0;$output=-1
            if($Extraction){
                $extractionFlag=1;$stock=10;$hu=1;$ju=2;$wallet=100;$revision=1
                if($t -ge 5000){$stock=8;$hu=3;$revision=2}
                if($t -ge 30000){$stock=6;$ju=4;$revision=3}
                $output=0
                if($SingleStock){
                    $stock=2;$hu=1;$ju=2;$wallet=100;$revision=1
                    if($t -ge 5000){$stock=0;$hu=3;$revision=2}
                }
            }
            $units=if($Role -eq 'HOST'){$hu}else{$ju};$peerUnits=if($Role -eq 'HOST'){$ju}else{$hu}
            $at=if($Role -eq 'HOST'){5000}else{30000};$done=$at+1000
            $step=if($t -lt $at){0}elseif($t -lt $done){1}else{3}
            $invoked=if($step){1}else{0}
            $attempted=if($step){1}else{0}
            if($SingleStock -and $Role -eq 'JOIN'){$invoked=0}
            $nativeHand=if($Role -eq 'HOST'){'4,4,4,4,4'}else{'4,4,4,40,400'}
            if($Role -eq 'JOIN'){$revision+=100}
            $sampleMs=100000+$t
            "NATIVE_TX WATCH world=99 load=3 role=$Role extraction=$extractionFlag t=$t sampleMs=$sampleMs preparedAt=0 step=$step attempted=$attempted invoked=$invoked hand=$nativeHand sourceCanonical=4,4,4,4,4 actor=$actor sid=food qty=2 peerActor=$peer peerUnits=$peerUnits peerHash=$peerUnits actorOwner=$owner actorCanonical=$actor peerOwner=$peerOwner peerCanonical=$peer revision=$revision hash=$stock actorHash=$units stock=$stock actorUnits=$units wallet=$wallet output=$output complete=1"
        }
    }
    function Assert-TransactionGate($H,$J,[string]$Want,[string]$Case,[switch]$Extraction,[switch]$Race){
        $H|Set-Content -LiteralPath $hostPath;$J|Set-Content -LiteralPath $joinPath
        Reset-GateResults
        [void](Test-NativeTransactionWatch -HostFile $hostPath -JoinFile $joinPath -Extraction:$Extraction -Race:$Race)
        $gate=@(Get-GateResults)[-1]
        if($gate.status -ne $Want){throw "$Case expected $Want; got $($gate.status): $($gate.detail)"}
    }
    $ht=@(New-TransactionEvidence HOST);$jt=@(New-TransactionEvidence JOIN)
    Assert-TransactionGate $ht $jt PASS 'both native buy/debit/sale/credit arcs converge at source and both actors'
    $joinPreCall=@($jt|ForEach-Object {
        $f=ConvertFrom-EnvironmentFields $_
        if($f.t -eq '30000'){
            $_.Replace('step=1','step=0').Replace('attempted=1','attempted=0').Replace('invoked=1','invoked=0').Replace('stock=8','stock=10').Replace('hash=8','hash=10').Replace('actorUnits=4','actorUnits=2').Replace('actorHash=4','actorHash=2').Replace('wallet=70','wallet=90').Replace('revision=104','revision=103')
        }else{$_}
    })
    Assert-TransactionGate $ht $joinPreCall PASS 'HOST completion window ends at the actual JOIN pre-call sample, not its later attempted observation'
    $hostLost=@($ht|ForEach-Object {if((ConvertFrom-EnvironmentFields $_).t -eq '20000'){$_.Replace('stock=10','stock=9').Replace('hash=10','hash=9')}else{$_}})
    $joinLost=@($joinPreCall|ForEach-Object {if((ConvertFrom-EnvironmentFields $_).t -eq '20000'){$_.Replace('stock=10','stock=9').Replace('hash=10','hash=9')}else{$_}})
    Assert-TransactionGate $hostLost $joinLost FAIL 'a real conserved-stock loss before the next participant acts still fails'
    $walletDiverged=@($jt|ForEach-Object {$f=ConvertFrom-EnvironmentFields $_;$_.Replace("wallet=$($f.wallet)","wallet=$([long]$f.wallet+1)")})
    Assert-TransactionGate $ht $walletDiverged FAIL 'equal goods cannot certify different native shared wallets'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('world=99','world=98')}) FAIL 'stale native transaction world'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('complete=1','complete=0')}) FAIL 'partial native inventory cannot prove conserved transfer'
    Assert-TransactionGate @($ht|ForEach-Object {$_.Replace('wallet=80','wallet=100')}) @($jt|ForEach-Object {$_.Replace('wallet=80','wallet=100')}) FAIL 'stock decrease alone without native HOST payment'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('step=1','step=0')}) FAIL 'local completion flag cannot replace paired observed before and debit'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('peerHash=3','peerHash=99')}) FAIL 'source convergence cannot hide peer actor contents divergence'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('sourceCanonical=4,4,4,4,4','sourceCanonical=4,4,4,4,5')}) FAIL 'same template in another canonical source is not shared stock'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('load=3','load=4')}) FAIL 'another admitted load cannot certify the current transaction'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('preparedAt=0','preparedAt=broken')}) FAIL 'malformed native timing evidence fails the gate without crashing the runner'
    Assert-TransactionGate $ht @($jt|ForEach-Object {$_.Replace('revision=101','revision=999999999999999999999999')}) FAIL 'overflowed native revision is invalid evidence, not a runner exception'
    $he=@(New-TransactionEvidence HOST -Extraction);$je=@(New-TransactionEvidence JOIN -Extraction)
    Assert-TransactionGate $he $je PASS 'two actual conserved native output pickups converge with unchanged wallet' -Extraction
    Assert-TransactionGate $he @($je|ForEach-Object {$_.Replace('actorUnits=4','actorUnits=2')}) FAIL 'output decrease without actual JOIN resource grant is not extraction proof' -Extraction
    Assert-TransactionGate $he @($je|ForEach-Object {$_.Replace('output=0','output=0.2')}) FAIL 'native production progress must converge too' -Extraction
    $hs=@(New-TransactionEvidence HOST -Extraction -SingleStock)
    $js=@(New-TransactionEvidence JOIN -Extraction -SingleStock)
    Assert-TransactionGate $hs $js PASS 'one physical stack has one native winner; a stale JOIN retry grants nothing' -Extraction
    $earlierArmedJoin=@($js|ForEach-Object {
        $fields=ConvertFrom-EnvironmentFields $_
        $_.Replace(" t=$($fields.t) "," t=$([long]$fields.t+20000) ").Replace('preparedAt=0','preparedAt=20000')
    })
    Assert-TransactionGate $hs $earlierArmedJoin PASS 'different scenario arm times must pair observations at the same monotonic sample time' -Extraction
    Assert-TransactionGate $hs @($js|ForEach-Object {$_.Replace('sampleMs=100000','sampleMs=broken')}) FAIL 'malformed shared sample time is not usable native evidence' -Extraction
    Assert-TransactionGate $hs @($js|ForEach-Object {$_.Replace('sampleMs=100500','sampleMs=99999')}) FAIL 'native shared sample time cannot regress' -Extraction
    Assert-TransactionGate $hs @($js|ForEach-Object {$_.Replace('attempted=1','attempted=0')}) FAIL 'matching empty stock alone is not an attempted stale pickup' -Extraction
    $hostDuplicate=@($hs|ForEach-Object {
        if([long](ConvertFrom-EnvironmentFields $_).t -ge 30000){$_.Replace('peerUnits=2','peerUnits=4').Replace('peerHash=2','peerHash=4')}else{$_}
    })
    $joinDuplicate=@($js|ForEach-Object {
        if([long](ConvertFrom-EnvironmentFields $_).t -ge 30000){$_.Replace('actorUnits=2','actorUnits=4').Replace('actorHash=2','actorHash=4')}else{$_}
    })
    Assert-TransactionGate $hostDuplicate $joinDuplicate FAIL 'even paired matching inventories cannot certify two grants from one physical stack' -Extraction
    function New-RaceEvidence([string]$Role,[string]$Winner='HOST',[switch]$Duplicate,[switch]$Lost){
        foreach($line in New-TransactionEvidence $Role -Extraction -SingleStock){
            $f=ConvertFrom-EnvironmentFields $line
            $t=[long]$f.t
            $hu=1;$ju=2
            if($t -ge 5000){
                if(-not $Lost){
                    if($Winner -eq 'HOST' -or $Duplicate){$hu+=2}
                    if($Winner -eq 'JOIN' -or $Duplicate){$ju+=2}
                }
            }
            $units=if($Role -eq 'HOST'){$hu}else{$ju}
            $peer=if($Role -eq 'HOST'){$ju}else{$hu}
            $step=if($t -lt 5000){0}elseif($t -lt 6000){1}else{3}
            $attempt=if($step){1}else{0}
            $invoked=if($Role -eq $Winner -and $step){1}else{0}
            $line=$line -replace ' actorUnits=\d+', " actorUnits=$units" -replace ' actorHash=\d+', " actorHash=$units"
            $line=$line -replace ' peerUnits=\d+', " peerUnits=$peer" -replace ' peerHash=\d+', " peerHash=$peer"
            $line=$line -replace ' step=\d+', " step=$step" -replace ' attempted=\d+', " attempted=$attempt" -replace ' invoked=\d+', " invoked=$invoked"
            "$line race=1 takeAt=105000"
        }
        $hostRole=if($Role -eq 'HOST'){1}else{0}
        "NATIVE_TX ACTION host=$hostRole attempted=1 returned=1 stock=2 actor=0 peer=0 sampleMs=105000"
    }
    $hr=@(New-RaceEvidence HOST);$jr=@(New-RaceEvidence JOIN)
    Assert-TransactionGate $hr $jr PASS 'simultaneous real attempts have one HOST winner with conserved endpoints' -Extraction -Race
    Assert-TransactionGate @(New-RaceEvidence HOST JOIN) @(New-RaceEvidence JOIN JOIN) PASS 'simultaneous extraction may have JOIN as the sole winner' -Extraction -Race
    Assert-TransactionGate @(New-RaceEvidence HOST -Duplicate) @(New-RaceEvidence JOIN -Duplicate) FAIL 'matching paired endpoints cannot hide two grants from one contested stack' -Extraction -Race
    Assert-TransactionGate @(New-RaceEvidence HOST -Lost) @(New-RaceEvidence JOIN -Lost) FAIL 'matching empty source cannot hide a missing contested stack' -Extraction -Race
    Assert-TransactionGate $hr @($jr|Where-Object {$_ -notmatch 'NATIVE_TX ACTION'}) FAIL 'both actual native attempts must be observed' -Extraction -Race
    Assert-TransactionGate $hr @($jr|ForEach-Object {if($_ -match 'NATIVE_TX ACTION'){$_.Replace('sampleMs=105000','sampleMs=106000')}else{$_}}) FAIL 'widely separated attempts are not a simultaneous contention test' -Extraction -Race
    Assert-TransactionGate $hr @($jr|ForEach-Object {$_.Replace('takeAt=105000','takeAt=105001')}) FAIL 'participants must use the exact same contention deadline' -Extraction -Race
    function New-LockEvidence([int]$HostRole){
        "CRIT LOCK ANCHOR host=$HostRole have=1 containers=1 hand=1.2.3.4.5"
        for($t=6000;$t -le 120000;$t+=500){
            $locked=if($t -lt 8000 -or ($t -ge 60000 -and $t -lt 65000)){1}else{0}
            "CRIT LOCK host=$HostRole t=$t locked=$locked initial=1 client=1 hostLeg=1 final=1"
        }
        if($HostRole -eq 1){'CRIT LOCK NATIVE host=1 t=120000 order=1/1 task=1 subject=4.5 target=4.5'}
    }
    function Assert-LockGate($H,$J,[string]$Want,[string]$Case){
        $H|Set-Content -LiteralPath $hostPath;$J|Set-Content -LiteralPath $joinPath
        Reset-GateResults
        [void](Test-ChestLockNative -HostFile $hostPath -JoinFile $joinPath)
        $gate=@(Get-GateResults)[-1]
        if($gate.status -ne $Want){throw "$Case expected $Want; got $($gate.status): $($gate.detail)"}
    }
    $hl=@(New-LockEvidence 1);$jl=@(New-LockEvidence 0)
    Assert-LockGate $hl $jl PASS 'four complete paired native lock windows and two native pick orders'
    Assert-LockGate $hl @($jl|ForEach-Object {$_.Replace('locked=0','locked=1')}) FAIL 'latched success flags cannot hide native JOIN lock mismatch'
    Assert-LockGate @($hl|Where-Object {$_ -notmatch ' LOCK NATIVE '}) $jl FAIL 'state changes alone do not prove native lockpick orders'
    Assert-LockGate $hl @($jl|ForEach-Object {$_.Replace('hand=1.2.3.4.5','hand=1.2.3.4.6')}) FAIL 'same lock index with reused serial is another object'
    Assert-LockGate @($hl|Where-Object {$_ -notmatch 't=120000'}) @($jl|Where-Object {$_ -notmatch 't=120000'}) FAIL '100 second runner cannot certify 120 second native lifecycle'
}finally{Remove-Item -LiteralPath $temp -Recurse -Force}
Write-Output 'Seven-sync native oracle consumer regression cases passed'
