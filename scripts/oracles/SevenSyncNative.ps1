# These gates consume native observations, never transport ACKs or desired-state echoes.
function Test-EnvironmentItemsGate {
    param([string]$HostFile,[string]$JoinFile,[switch]$Lifecycle)
    $name=if($Lifecycle){'environment_items_lifecycle'}else{'environment_items'}
    if(-not(Test-Path -LiteralPath $HostFile) -or -not(Test-Path -LiteralPath $JoinFile)){
        return (Add-GateResult -Name $name -Status FAIL -Metrics @{} -Detail 'Both native logs are required')
    }
    $r=Test-EnvironmentItemsNativePair -HostLines @(Get-Content -LiteralPath $HostFile) -JoinLines @(Get-Content -LiteralPath $JoinFile) -Lifecycle:$Lifecycle
    return (Add-GateResult -Name $name -Status $(if($r.Passed){'PASS'}else{'FAIL'}) -Metrics @{hostSamples=$r.HostSamples;joinSamples=$r.JoinSamples;pairs=$r.Pairs.Count;savedItemGroupProvenance='UNVERIFIED unless positive native rows exist'} -Detail ($r.Errors -join '; '))
}
function Test-NativeTransactionWatch {
    param([string]$HostFile,[string]$JoinFile,[switch]$Extraction,[switch]$Race)
    $name=if($Race){'native_extraction_race_watch'}elseif($Extraction){'native_extraction_watch'}else{'native_trade_watch'}
    $errors=@();$sides=@();$actionSides=@();$expected=if($Extraction){'1'}else{'0'}
    if($Race -and -not $Extraction){$errors+='contention requires the extraction contract'}
    foreach($spec in @(@($HostFile,'HOST'),@($JoinFile,'JOIN'))){
        $rows=@();$actions=@()
        if(Test-Path -LiteralPath $spec[0]){foreach($line in Get-Content -LiteralPath $spec[0]){
            if($Race -and $line -match 'NATIVE_TX ACTION '){$actions+=,(ConvertFrom-EnvironmentFields $line)}
            if($line -notmatch 'NATIVE_TX WATCH world='){continue}
            $f=ConvertFrom-EnvironmentFields $line
            if($f.complete -eq '0' -and $f.step -eq '0'){continue}
            $valid=$f.role -ceq $spec[1] -and $f.extraction -ceq $expected -and $f.complete -eq '1' -and $f.world -match '^[1-9][0-9]*$' -and $f.load -match '^[1-9][0-9]*$'
            foreach($key in @('hand','sourceCanonical','actor','peerActor','actorCanonical','peerCanonical')){if($f[$key] -notmatch '^\d+,\d+,\d+,\d+,\d+$' -or $f[$key] -eq '0,0,0,0,0'){$valid=$false}}
            foreach($key in @('t','preparedAt','load','step','qty','peerUnits','peerHash','revision','hash','actorHash','stock','actorUnits')){
                [uint32]$number=0
                if(-not [uint32]::TryParse([string]$f[$key],[ref]$number)){$valid=$false}
            }
            [uint64]$world=0;[uint64]$sampleMs=0;[int]$wallet=0
            if(-not [uint64]::TryParse([string]$f.world,[ref]$world) -or
                -not [uint64]::TryParse([string]$f.sampleMs,[ref]$sampleMs) -or $sampleMs -gt [long]::MaxValue -or
                -not [int]::TryParse([string]$f.wallet,[ref]$wallet) -or $wallet -lt 0 -or
                $f.step -notmatch '^[0-3]$' -or $f.invoked -notmatch '^[01]$' -or $f.attempted -notmatch '^[01]$'){$valid=$false}
            if($valid -and ([long]$f.preparedAt -gt [long]$f.t -or ($f.step -ne '0' -and $f.attempted -ne '1') -or
                ($f.invoked -eq '1' -and $f.attempted -ne '1'))){$valid=$false}
            if($Extraction -and $f.output -notmatch '^\d+(\.\d+)?$'){$valid=$false}
            if($Race){
                [uint64]$takeAt=0
                if($f.race -cne '1' -or -not [uint64]::TryParse([string]$f.takeAt,[ref]$takeAt) -or
                    $takeAt -eq 0 -or $takeAt -gt [long]::MaxValue){$valid=$false}
            }
            $owner=if($spec[1] -eq 'HOST'){'0'}else{'1'}
            $peerOwner=if($spec[1] -eq 'HOST'){'1'}else{'0'}
            if($f.actorOwner -cne $owner -or $f.peerOwner -cne $peerOwner -or $f.actorCanonical -ceq $f.peerCanonical){$valid=$false}
            if(-not $valid -or -not $f.sid -or $f.qty -eq '0' -or $f.revision -eq '0'){$errors+='invalid/incomplete native transaction observation';continue}
            $rows+=,$f
        }}
        if($rows.Count -lt 20){$errors+="missing sustained $($spec[1]) native samples"}
        foreach($key in @('hand','sourceCanonical','actor','peerActor','actorOwner','peerOwner','actorCanonical','peerCanonical','sid','qty','world','load','preparedAt')){
            if(@($rows|ForEach-Object {$_[$key]}|Select-Object -Unique).Count -ne 1){$errors+="unstable transaction identity/scope $key"}
        }
        for($i=1;$i -lt $rows.Count;$i++){
            if([long]$rows[$i].t -le [long]$rows[$i-1].t -or
                [long]$rows[$i].sampleMs -le [long]$rows[$i-1].sampleMs -or
                [uint32]$rows[$i].revision -lt [uint32]$rows[$i-1].revision){
                $errors+='native observation time/revision regressed'
            }
        }
        if($Race -and @($rows|ForEach-Object {$_.takeAt}|Select-Object -Unique).Count -ne 1){$errors+='unstable contention deadline'}
        $actionSides+=,@($actions)
        $sides+=,@($rows)
    }
    $paired=@(@(),@())
    $hostWindowEnd=0L
    $joinAttempt=@($sides[1]|Where-Object {$_.attempted -eq '1'}|Select-Object -First 1)
    if($joinAttempt.Count){
        # WATCH is emitted before the native call. Its first attempted=1 row is
        # a later observation, after HOST may already have committed that call.
        $joinBefore=@($sides[1]|Where-Object {$_.step -eq '0' -and
            [long]$_.sampleMs -lt [long]$joinAttempt[0].sampleMs}|Select-Object -Last 1)
        if($joinBefore.Count){$hostWindowEnd=[long]$joinBefore[0].sampleMs}
    }
    for($side=0;$side -lt 2;$side++){
        foreach($r in $sides[$side]){foreach($peer in $sides[1-$side]){
            # Local handles/revision counters belong to different engine heaps.
            # Pair the actual contents through the proven full canonical root.
            # run_test launches both native processes on this Windows host.
            # sampleMs is the shared boot clock, not each scenario's arm clock.
            if($r.world -ceq $peer.world -and $r.load -ceq $peer.load -and
                $r.sourceCanonical -ceq $peer.sourceCanonical -and $r.sid -ceq $peer.sid -and $r.qty -ceq $peer.qty -and
                $r.actorCanonical -ceq $peer.peerCanonical -and $r.peerCanonical -ceq $peer.actorCanonical -and
                $r.actorCanonical -cne $r.peerCanonical -and $r.hash -ceq $peer.hash -and $r.stock -ceq $peer.stock -and $r.actorHash -ceq $peer.peerHash -and
                $r.peerHash -ceq $peer.actorHash -and $r.actorUnits -ceq $peer.peerUnits -and $r.peerUnits -ceq $peer.actorUnits -and $r.wallet -ceq $peer.wallet -and
                [Math]::Abs([long]$r.sampleMs-[long]$peer.sampleMs) -le 1500 -and
                (-not $Race -or $r.takeAt -ceq $peer.takeAt) -and (-not $Extraction -or $r.output -ceq $peer.output)){
                $r['pairedPeerStep']=$peer.step
                $paired[$side]+=,$r;break
            }
        }}
        $singleStack=$Extraction -and $paired[$side].Count -gt 0 -and $paired[$side][0].stock -ceq $paired[$side][0].qty
        if($Race){
            $before=@($paired[$side]|Where-Object {$_.step -eq '0' -and $_.pairedPeerStep -eq '0' -and
                [long]$_.sampleMs -lt [long]$_.takeAt}|Select-Object -Last 1)
            $after=@($paired[$side]|Where-Object {$_.step -eq '3' -and $_.pairedPeerStep -eq '3' -and
                $_.attempted -eq '1' -and [long]$_.sampleMs -ge [long]$_.takeAt})
            if($before.Count -ne 1 -or $after.Count -lt 8){
                $errors+='missing paired contention baseline or sustained single-winner result';continue
            }
            $b=$before[0];$winner=$null
            if($b.stock -cne $b.qty){$errors+='contention requires one complete physical source stack'}
            $actions=@($actionSides[$side])
            [uint64]$actionMs=0
            $hostFlag=if($side -eq 0){'1'}else{'0'}
            if($actions.Count -ne 1 -or $actions[0].host -cne $hostFlag -or
                $actions[0].attempted -cne '1' -or
                -not [uint64]::TryParse([string]$actions[0].sampleMs,[ref]$actionMs) -or
                $actionMs -lt [uint64]$b.takeAt -or $actionMs-[uint64]$b.takeAt -gt 250){
                $errors+='missing or late actual native contention attempt'
            }
            foreach($r in $after){
                $selfWon=[long]$r.actorUnits -eq [long]$b.actorUnits+[long]$b.qty -and $r.peerUnits -ceq $b.peerUnits
                $peerWon=[long]$r.peerUnits -eq [long]$b.peerUnits+[long]$b.qty -and $r.actorUnits -ceq $b.actorUnits
                if([long]$r.stock -ne 0 -or (-not $selfWon -and -not $peerWon) -or
                    $r.wallet -cne $b.wallet -or [uint32]$r.revision -le [uint32]$b.revision){
                    $errors+='contested stack duplicated, lost, refilled or changed the wallet'
                }
                if($null -eq $winner){$winner=$selfWon}elseif($winner -ne $selfWon){$errors+='contested grant changed winner after completion'}
            }
            continue
        }
        $before=@($paired[$side]|Where-Object {$_.step -eq '0' -and (-not $singleStack -or $_.pairedPeerStep -eq '0')}|Select-Object -Last 1)
        $taken=@($paired[$side]|Where-Object {$_.step -eq '1' -or $_.step -eq '2'})
        $after=@($paired[$side]|Where-Object {$_.step -eq '3' -and $_.attempted -eq '1' -and
            ($_.invoked -eq '1' -or ($singleStack -and $side -eq 1)) -and
            ($side -ne 0 -or [long]$_.sampleMs -lt $hostWindowEnd)})
        if($before.Count -ne 1 -or $after.Count -lt 8){$errors+='missing paired native before and sustained complete transaction';continue}
        $b=$before[0];$debit=$null
        if($singleStack -and $side -eq 1){
            # One native stack was already taken by HOST. JOIN's stale attempt
            # must leave no second grant, regardless of a local entrypoint return.
            $observed=@($taken|Where-Object {$_.attempted -eq '1' -and
                [long]$_.t -ge [long]$_.preparedAt+30000 -and
                [long]$_.stock -eq 0 -and [long]$_.actorUnits -eq [long]$b.actorUnits -and
                [long]$_.peerUnits -eq [long]$b.peerUnits+[long]$b.qty -and
                $_.wallet -ceq $b.wallet -and [uint32]$_.revision -gt [uint32]$b.revision})
            if(-not $observed.Count){$errors+='no paired native stale extraction attempt with a single conserved winner';continue}
            foreach($r in $after){
                if([long]$r.t -le [long]$observed[0].t -or [long]$r.stock -ne 0 -or
                    [long]$r.actorUnits -ne [long]$b.actorUnits -or
                    [long]$r.peerUnits -ne [long]$b.peerUnits+[long]$b.qty -or $r.wallet -cne $b.wallet){
                    $errors+='single-stack extraction duplicated, refilled or changed the wallet'
                }
            }
            continue
        }
        foreach($r in $taken){
            if([long]$r.t -le [long]$b.t -or [long]$r.stock -ne [long]$b.stock-[long]$r.qty -or [long]$r.actorUnits -ne [long]$b.actorUnits+[long]$r.qty -or [uint32]$r.revision -le [uint32]$b.revision){continue}
            if(($Extraction -and $r.wallet -ceq $b.wallet) -or (-not $Extraction -and [long]$r.wallet -lt [long]$b.wallet)){$debit=$r;break}
        }
        if(-not $debit){$errors+='no paired native conserved pickup/purchase with actual wallet result';continue}
        foreach($r in $after){
            if([long]$r.t -le [long]$debit.t){$errors+='completion predates observed native transaction';continue}
            if($Extraction){
                if([long]$r.stock -ne [long]$b.stock-[long]$r.qty -or [long]$r.actorUnits -ne [long]$b.actorUnits+[long]$r.qty -or $r.wallet -cne $b.wallet){$errors+='extraction source/actor conservation or wallet not retained'}
            }elseif([long]$r.stock -ne [long]$b.stock -or [long]$r.actorUnits -ne [long]$b.actorUnits -or [long]$r.wallet -le [long]$debit.wallet){
                $errors+='native sale did not restore both quantities and credit actual wallet'
            }
        }
    }
    return (Add-GateResult -Name $name -Status $(if($errors.Count){'FAIL'}else{'PASS'}) -Metrics @{hostPairedSamples=$paired[0].Count;joinPairedSamples=$paired[1].Count;pairedNativeTransaction=($errors.Count -eq 0);atomicityProof=$false;reconnectProof=$false} -Detail (($errors+@('Native paired endpoint/payment regression; replay/atomicity/reconnect require separate evidence')) -join '; '))
}
function Test-ChestLockNative {
    param([string]$HostFile,[string]$JoinFile)
    $errors=@();$hands=@()
    foreach($spec in @(@($HostFile,'1'),@($JoinFile,'0'))){
        $rows=@();$anchors=@();$native=@()
        if(Test-Path -LiteralPath $spec[0]){foreach($line in Get-Content -LiteralPath $spec[0]){
            if($line -match 'CRIT LOCK ANCHOR '){$anchors+=,(ConvertFrom-EnvironmentFields $line)}
            elseif($line -match 'CRIT LOCK NATIVE '){$native+=,(ConvertFrom-EnvironmentFields $line)}
            elseif($line -match 'CRIT LOCK host='){$rows+=,(ConvertFrom-EnvironmentFields $line)}
        }}
        $a=@($anchors|Where-Object {$_.host -ceq $spec[1] -and $_.have -eq '1' -and $_.hand -match '^\d+\.\d+\.\d+\.\d+\.\d+$'})
        if($a.Count -ne 1){$errors+='missing unique full native lock anchor'}else{$hands+=,$a[0].hand}
        foreach($window in @(@(6000,8000,'1'),@(8000,60000,'0'),@(63000,65000,'1'),@(65000,120001,'0'))){
            $valid=@($rows|Where-Object {$_.host -ceq $spec[1] -and [long]$_.t -ge $window[0] -and [long]$_.t -lt $window[1] -and $_.locked -ceq $window[2]})
            if($valid.Count -lt 2){$errors+="missing paired native lock window $($window[0])-$($window[1])"}
            if($valid.Count){
                $start=if($window[2] -eq '1'){$window[0]}else{[long]$valid[0].t}
                if(@($rows|Where-Object {$_.host -ceq $spec[1] -and [long]$_.t -ge $start -and [long]$_.t -lt $window[1] -and $_.locked -cne $window[2]}).Count){$errors+='native lock state regressed within authoritative window'}
            }
        }
        if(@($rows|Where-Object {[long]$_.t -ge 120000}).Count -eq 0){$errors+='scenario stopped before 120s'}
        if($spec[1] -eq '1' -and @($native|Where-Object {$_.host -eq '1' -and $_.order -eq '1/1' -and [long]$_.t -ge 65000}).Count -eq 0){$errors+='missing both real native lockpick orders'}
    }
    if($hands.Count -ne 2 -or $hands[0] -cne $hands[1]){$errors+='HOST/JOIN full lock identities differ'}
    return (Add-GateResult -Name chest_lock_native -Status $(if($errors.Count){'FAIL'}else{'PASS'}) -Metrics @{windows=4;nativeLockpickRequired=$true} -Detail ($errors -join '; '))
}
function Test-ProceduralOutsideLifecycle {
    param([string]$HostFile,[string]$JoinFile)
    [void](Test-ProceduralBeds -HostFile $HostFile -JoinFile $JoinFile)
    $base=@(Get-GateResults)[-1];$errors=@()
    if($base.status -ne 'PASS'){$errors+=$base.detail}
    foreach($spec in @(@($HostFile,'HOST'),@($JoinFile,'JOIN'))){
        $final=@()
        if(Test-Path -LiteralPath $spec[0]){foreach($line in Get-Content -LiteralPath $spec[0]){
            if($line -notmatch 'SCENARIO PROCLIFE '){continue}
            $f=ConvertFrom-EnvironmentFields $line
            if($f.role -ceq $spec[1] -and $f.outside -eq '1' -and $f.region -eq '22,32' -and $f.phase -eq 'preserved' -and $f.ok -eq '1' -and [long]$f.t -ge 340000){$final+=,$f}
        }}
        if($final.Count -eq 0){$errors+='missing native region 22,32 lifecycle through at least 340 seconds'}
    }
    return (Add-GateResult -Name procedural_beds_outside_lifecycle -Status $(if($errors.Count){'FAIL'}else{'PASS'}) -Metrics @{region='22,32';minimumDurationMs=340000} -Detail ($errors -join '; '))
}
function Test-CarryBedNative {
    param([string]$HostFile,[string]$JoinFile)
    $errors=@();$all=@();$fixtures=@();$identities=@();$worlds=@()
    foreach($spec in @(@($HostFile,'owner'),@($JoinFile,'carrier'))){
        $rows=@();$descriptors=@();$actions=@();$claims=@()
        if(Test-Path -LiteralPath $spec[0]){foreach($line in Get-Content -LiteralPath $spec[0]){
            if($line -notmatch 'SCENARIO CARRYBED '){continue}
            $f=ConvertFrom-EnvironmentFields $line
            if($f.side -cne $spec[1]){$errors+='wrong native actor role';continue}
            if($f.world -notmatch '^[1-9][0-9]*$' -or $f.load -notmatch '^[1-9][0-9]*$'){$errors+='missing actual admitted carry scope';continue}
            if($f.ms -notmatch '^[0-9]+$'){$errors+='invalid native observation time';continue}
            if($line -match 'CARRYBED ACTION '){$actions+=,$f}
            elseif($line -match 'CARRYBED CLAIM '){$claims+=,$f}
            elseif($line -match 'CARRYBED FIXTURE '){
                foreach($key in @('descriptor','sid','pos','heading','floor','class','owner','inside','insidePos','town','townPos')){if(-not $f.ContainsKey($key)){$errors+="missing native fixture field $key"}}
                $descriptors+=,$f
            }
            else{$rows+=,$f}
        }}
        $scope=@(@($rows)+@($actions)+@($descriptors)+@($claims)|ForEach-Object {"$($_.world)|$($_.load)"}|Select-Object -Unique)
        if($scope.Count -ne 1){$errors+='carry observations crossed admission scopes'}
        if($rows.Count){$worlds+=,$rows[0].world}
        if(@($rows|Where-Object {$_.failed -ne '0'}).Count){$errors+='native scenario recorded a failure'}
        $occupied=@($rows|Where-Object {[long]$_.ms -ge 25000 -and [long]$_.ms -lt 40000})
        if($occupied.Count -lt 40){$errors+='missing sustained 15s native occupancy'}
        else{
            if([long]$occupied[0].ms -gt 25500 -or [long]$occupied[-1].ms -lt 39500){$errors+='occupancy does not cover full 15s window'}
            for($i=0;$i -lt $occupied.Count;$i++){
                $r=$occupied[$i]
                if($r.read -ne '1' -or $r.kind -ne '1' -or $r.carried -ne '0' -or $r.furn -notmatch '^\d+,\d+,\d+,\d+,\d+$' -or $r.furn -eq '0,0,0,0,0'){$errors+='native bed occupancy lost'}
                if($i -and ([long]$r.ms -le [long]$occupied[$i-1].ms -or [long]$r.ms-[long]$occupied[$i-1].ms -gt 750 -or $r.furn -cne $occupied[0].furn)){$errors+='native bed changed or observation gap'}
            }
        }
        $pairs=@($rows|ForEach-Object {"$($_.passenger)|$($_.carrier)"}|Select-Object -Unique)
        if($pairs.Count -ne 1 -or $rows.Count -eq 0){$errors+='unstable full actor identities'}
        else{
            if($rows[0].passenger -ceq $rows[0].carrier -or $rows[0].passenger -notmatch '^\d+,\d+,\d+,\d+,\d+$' -or $rows[0].carrier -notmatch '^\d+,\d+,\d+,\d+,\d+$'){$errors+='two separate claimed actors required'}
        }
        $canonical=@($claims|ForEach-Object {"$($_.passengerCanonical)|$($_.carrierCanonical)"}|Select-Object -Unique)
        if($claims.Count -lt $rows.Count -or $canonical.Count -ne 1 -or @($claims|Where-Object {$_.passengerOwner -ne '0' -or $_.carrierOwner -ne '1' -or $_.passengerCanonical -ceq $_.carrierCanonical -or $_.passengerCanonical -notmatch '^\d+,\d+,\d+,\d+,\d+$' -or $_.carrierCanonical -notmatch '^\d+,\d+,\d+,\d+,\d+$'}).Count){
            $errors+='missing exact admitted HOST/JOIN actor claims'
        }else{$identities+=,$canonical[0]}
        foreach($r in $rows){
            if(@($claims|Where-Object {$_.ms -ceq $r.ms}).Count -ne 1){$errors+='native sample lacks its exact current actor-claim observation';break}
        }
        $carried=@($rows|Where-Object {[long]$_.ms -ge 18000 -and [long]$_.ms -lt 19500})
        if($carried.Count -lt 4 -or @($carried|Where-Object {$_.exactCarry -ne '1'}).Count){$errors+='missing exact native passenger/carrier relationship'}
        $exit=@($rows|Where-Object {[long]$_.ms -ge 47000})
        if($exit.Count -lt 10 -or @($exit|Where-Object {$_.read -ne '1' -or $_.kind -ne '0' -or $_.carried -ne '0'}).Count){$errors+='explicit exit did not cross'}
        if($spec[1] -eq 'owner' -and @($exit|Where-Object {$_.moved -eq '1'}).Count -eq 0){$errors+='no voluntary owner movement after exit'}
        foreach($action in $(if($spec[1] -eq 'owner'){@('exit')}else{@('pickup','placement')})){
            $a=@($actions|Where-Object {$_.action -ceq $action})
            $minimum=if($action -eq 'exit'){42000}elseif($action -eq 'pickup'){16000}else{20000}
            if($a.Count -ne 1 -or $a[0].ok -ne '1' -or [long]$a[0].ms -lt $minimum -or $pairs.Count -ne 1 -or "$($a[0].passenger)|$($a[0].carrier)" -cne $pairs[0]){$errors+="missing unique native $action action"}
        }
        $d=@($descriptors|Where-Object {[long]$_.ms -ge 25000 -and [long]$_.ms -lt 40000})
        if($d.Count -ne $occupied.Count -or @($d|Where-Object {$_.descriptor -ne '1' -or -not $_.sid}).Count){$errors+='missing actual native bed descriptors'}
        $all+=,@($occupied);$fixtures+=,@($d)
    }
    if($identities.Count -ne 2 -or $identities[0] -cne $identities[1]){$errors+='peers disagree on full claimed actor identities'}
    if($worlds.Count -ne 2 -or $worlds[0] -cne $worlds[1]){$errors+='HOST/JOIN carry world scope differs'}
    foreach($j in $fixtures[1]){
        $matching=@($fixtures[0]|Where-Object {[Math]::Abs([long]$_.ms-[long]$j.ms) -le 500})
        $same=$false
        foreach($h in $matching){
            $ok=$true
            foreach($field in @('sid','floor','class','owner','inside','town')){if($h[$field] -cne $j[$field]){$ok=$false}}
            foreach($field in @('pos','insidePos','townPos')){
                try{if(-not(Test-ProceduralPositionParity (ConvertTo-ProceduralPosition $h[$field]) (ConvertTo-ProceduralPosition $j[$field]))){$ok=$false}}catch{$ok=$false}
            }
            try{
                $ha=[double]::Parse($h.heading,[Globalization.CultureInfo]::InvariantCulture)
                $ja=[double]::Parse($j.heading,[Globalization.CultureInfo]::InvariantCulture)
                if([double]::IsNaN($ha) -or [double]::IsNaN($ja) -or [double]::IsInfinity($ha) -or [double]::IsInfinity($ja) -or [Math]::Abs($ha-$ja) -gt 0.01){$ok=$false}
            }catch{$ok=$false}
            if($ok){$same=$true;break}
        }
        if(-not $same){$errors+='paired native fixture descriptors differ';break}
    }
    return (Add-GateResult -Name carry_bed -Status $(if($errors.Count){'FAIL'}else{'PASS'}) -Metrics @{hostOccupancy=$all[0].Count;joinOccupancy=$all[1].Count;nativeHoldMs=15000;explicitExit=$true} -Detail ($errors -join '; '))
}
