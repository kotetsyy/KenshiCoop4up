$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../CoopOracles.psm1') -Force
. (Join-Path $PSScriptRoot '../oracles/ProceduralBeds.ps1')
# Removing census membership, full-row or native-occupancy validation breaks these cases.
function New-BedEvidence([string]$Role,[int]$Census=1,[string]$Region='2,-1',[switch]$EmptyRegion,[switch]$Preserved,[int]$Load=3) {
    $regions = if ($EmptyRegion) { 2 } else { 1 }
    $saved = if ($Preserved) { 1 } else { 0 }
    "[procworld] CENSUSBEGIN role=$Role world=99 load=$Load census=$Census count=$regions source=native"
    "[procworld] CATALOG role=$Role world=99 load=$Load census=$Census region=$Region epoch=7 complete=1 count=1 preserved=$saved digest=1234abcd source=native"
    "[procworld] OBJECT role=$Role world=99 load=$Load census=$Census region=$Region epoch=7 id=42 category=1 sid='camp' pos=1,2,3 yaw=0 rotationX=0 rotationY=0 rotationZ=0 rotationW=1 floor=0 flags=10 function=5 state=9 progress=1 owner='' town='hub' townPos=4,5,6 inside='' insidePos=0,0,0 source=native"
    if ($Preserved) {
        "[procworld] PRESERVED role=$Role world=99 load=$Load census=$Census region=$Region epoch=7 hand=6.7.8.9.10 category=1 sid='saved-bed' pos=7,8,9 yaw=0 rotationX=0 rotationY=0 rotationZ=0 rotationW=1 floor=1 flags=15 function=5 state=9 progress=1 owner='player' town='hub' townPos=4,5,6 inside='house' insidePos=10,11,12 source=native"
    }
    "[procworld] REGIONEND role=$Role world=99 load=$Load census=$Census region=$Region epoch=7 count=1 preserved=$saved source=native"
    if ($EmptyRegion) {
        "[procworld] CATALOG role=$Role world=99 load=$Load census=$Census region=9,9 epoch=8 complete=1 count=0 preserved=0 digest=00000000 source=native"
        "[procworld] REGIONEND role=$Role world=99 load=$Load census=$Census region=9,9 epoch=8 count=0 preserved=0 source=native"
    }
    "[procworld] CENSUSEND role=$Role world=99 load=$Load census=$Census complete=1 count=$regions beds=1 preserved=$saved source=native"
    "SCENARIO PROCBEDUSE role=$Role world=99 load=$Load census=$Census region=$Region epoch=7 id=42 actor=1.2.3.4.5 actorPos=1,2,3 bedPos=1,2,3 inBed=1 pose=17 occupant=1.2.3.4.5 ok=1 t=20000"
}
function Assert-BedGate([string[]]$H,[string[]]$J,[string]$Want,[string]$Case,[hashtable]$Metrics=@{}) {
    Reset-GateResults
    [void](Test-ProceduralBeds -HostLines $H -JoinLines $J)
    $gate = @(Get-GateResults)[-1]
    if ($gate.status -ne $Want) { throw "$Case expected $Want, got $($gate.status): $($gate.detail)" }
    foreach ($key in $Metrics.Keys) { if ($gate.metrics[$key] -cne $Metrics[$key]) { throw "$Case expected metric $key=$($Metrics[$key]), got $($gate.metrics[$key])" } }
}
$h = @(New-BedEvidence HOST); $j = @(New-BedEvidence JOIN)
Assert-BedGate $h $j PASS 'fully delimited current native census and same occupant'
# These deliberately keep digest and heading equal: full native orientation must
# be checked independently of the catalog digest and legacy yaw.
$identity = 'rotationX=0 rotationY=0 rotationZ=0 rotationW=1'
foreach ($orientation in @(
    'rotationX=0.707106781 rotationY=0 rotationZ=0 rotationW=0.707106781',
    'rotationX=0 rotationY=0 rotationZ=0.707106781 rotationW=0.707106781'
)) {
    Assert-BedGate $h @($j | ForEach-Object { $_.Replace($identity,$orientation) }) FAIL 'pitch/roll mismatch with matching heading and digest'
}
Assert-BedGate $h @($j | ForEach-Object { $_.Replace($identity,'rotationX=0 rotationY=0 rotationZ=0 rotationW=2') }) PASS 'quaternion is normalized before comparison'
Assert-BedGate $h @($j | ForEach-Object { $_.Replace($identity,'rotationX=0 rotationY=0 rotationZ=0 rotationW=-1') }) PASS 'identity and negative identity describe the same orientation'
$qh = @($h | ForEach-Object { $_.Replace($identity,'rotationX=0.5 rotationY=0.5 rotationZ=0.5 rotationW=0.5') })
$qj = @($j | ForEach-Object { $_.Replace($identity,'rotationX=-0.5 rotationY=-0.5 rotationZ=-0.5 rotationW=-0.5') })
Assert-BedGate $qh $qj PASS 'q and negative q agree for full pitch yaw roll'
$piHost = @($h | ForEach-Object { $_.Replace($identity,'rotationX=-1 rotationY=0 rotationZ=0 rotationW=0.000001') })
$piJoin = @($j | ForEach-Object { $_.Replace($identity,'rotationX=-1 rotationY=0 rotationZ=0 rotationW=-0.000001') })
Assert-BedGate $piHost $piJoin PASS 'near pi sign is chosen after quantization in W X Y Z order'
foreach ($field in @('rotationX=0 ','rotationY=0 ','rotationZ=0 ','rotationW=1 ')) {
    Assert-BedGate @($h | ForEach-Object { $_.Replace($field,'') }) @($j | ForEach-Object { $_.Replace($field,'') }) FAIL "both peers missing required $field"
}
foreach ($orientation in @(
    'rotationX=0 rotationY=0 rotationZ=0 rotationW=0',
    'rotationX=bad rotationY=0 rotationZ=0 rotationW=1',
    'rotationX=1,2 rotationY=0 rotationZ=0 rotationW=1',
    'rotationX=NaN rotationY=0 rotationZ=0 rotationW=1',
    'rotationX=0 rotationY=Infinity rotationZ=0 rotationW=1',
    'rotationX=0 rotationY=0 rotationZ=-Infinity rotationW=1',
    'rotationX=0 rotationY=0 rotationZ=0 rotationW=NaN'
)) {
    Assert-BedGate @($h | ForEach-Object { $_.Replace($identity,$orientation) }) @($j | ForEach-Object { $_.Replace($identity,$orientation) }) FAIL "invalid quaternion on both peers: $orientation"
}
Assert-BedGate @($h | ForEach-Object { $_.Replace('world=99','world=0').Replace('load=3','load=0').Replace('epoch=7','epoch=0') }) @($j | ForEach-Object { $_.Replace('world=99','world=0').Replace('load=3','load=0').Replace('epoch=7','epoch=0') }) FAIL 'blocked initialization never supplies valid native scope'
foreach ($change in @(
    @('complete=1','complete=0'), @('pos=1,2,3','pos=1,2,4'),
    @("town='hub'","town='other'"), @('townPos=4,5,6','townPos=4,5,7'),
    @('floor=0','floor=1'), @('category=1','category=2'),
    @('progress=1','progress=0.5'), @('state=9','state=1'),
    @('epoch=7','epoch=6'), @('world=99','world=98'), @('load=3','load=4'),
    @('occupant=1.2.3.4.5','occupant=1.2.3.4.6'), @('inBed=1','inBed=0'),
    @('source=native','source=wire')
)) {
    $changed = @($j | ForEach-Object { $_.Replace($change[0],$change[1]) })
    Assert-BedGate $h $changed FAIL ($change -join ' -> ')
}
Assert-BedGate $h @($j | Where-Object { $_ -notmatch '\] OBJECT ' }) FAIL 'partial rows despite matching digest'
Assert-BedGate $h @($j[0..2] + $j[2] + $j[3..5]) FAIL 'duplicate logical id'
Assert-BedGate $h @($j | ForEach-Object { if ($_ -match '\] OBJECT ') { $_.Replace('epoch=7','epoch=6') } else { $_ } }) FAIL 'wrong row epoch'
Assert-BedGate $h $j[0..4] FAIL 'same-bed use was not performed'
Assert-BedGate $h @($j + '[procworld] CENSUSBEGIN role=JOIN world=99 load=3 census=2 count=1 source=native') FAIL 'newer partial census cannot reuse previous complete census'
Assert-BedGate $h @($j | Where-Object { $_ -notmatch 'REGIONEND' }) FAIL 'missing region boundary'
Assert-BedGate $h @($j[0..3] + $j[1..3] + $j[4..5]) FAIL 'duplicate region boundary'
Assert-BedGate $h @($j | ForEach-Object { $_.Replace('pos=1,2,3','pos=1.004,2,3') }) PASS 'canonical sub-centimetre geometry agrees'
Assert-BedGate $h @(New-BedEvidence JOIN 17) PASS 'census IDs are local boundaries, not synchronized peer clocks'
Assert-BedGate $h @($j | ForEach-Object { if ($_ -match 'CENSUSEND') { $_.Replace('beds=1','beds=2') } else { $_ } }) FAIL 'census total must cover every row'
Assert-BedGate $h @($j | ForEach-Object { if ($_ -match 'REGIONEND') { $_.Replace('count=1','count=0') } else { $_ } }) FAIL 'region end must confirm full row count'
$he = @(New-BedEvidence HOST -EmptyRegion); $je = @(New-BedEvidence JOIN -EmptyRegion)
Assert-BedGate $he $je PASS 'empty region is explicit current membership'
$duplicateRegions = @($j[0].Replace('count=1','count=2')) + $j[1..3] + @($j[1..3] | ForEach-Object { $_.Replace('region=2,-1','region=3,-1') }) + @($j[4].Replace('count=1','count=2').Replace('beds=1','beds=2'),$j[5])
Assert-BedGate @($duplicateRegions | ForEach-Object { $_.Replace('role=JOIN','role=HOST') }) $duplicateRegions FAIL 'logical ID duplicated across complete distinct regions'
$twoBeds = @($duplicateRegions | ForEach-Object { if ($_ -match 'region=3,-1') { $_.Replace('id=42','id=43') } else { $_ } })
$twoHost = @($twoBeds | ForEach-Object { $_.Replace('role=JOIN','role=HOST') })
$onlyB = @(New-BedEvidence JOIN 2 '3,-1' | ForEach-Object { $_.Replace('id=42','id=43') })
Assert-BedGate @($twoHost + @($twoHost | ForEach-Object { $_.Replace('census=1','census=2') })) @($twoBeds + $onlyB) FAIL 'historical populated A cannot survive JOIN unload while both enumerate B'
# Both used to contain A (the empty region), then only JOIN unloads A; both still log B.
Assert-BedGate @($he + @(New-BedEvidence HOST 2 -EmptyRegion)) @($je + @(New-BedEvidence JOIN 2)) FAIL 'historical matching A must not hide current unload while both retain B'
Assert-BedGate @($he + @(New-BedEvidence HOST 2)) @($je + @(New-BedEvidence JOIN 2)) PASS 'historical membership is discarded on both peers'
Assert-BedGate $h @($j + @(New-BedEvidence JOIN 2)[0..3]) FAIL 'newer census missing end cannot green'
Assert-BedGate @($h + @(New-BedEvidence HOST 2)[0..4]) @($j + @(New-BedEvidence JOIN 2)[0..4]) FAIL 'old use does not prove latest census occupancy'
# A matching desired/generated set is insufficient: every actual unmanaged bed
# needs its own complete, native, hand-keyed descriptor and count.
$hp = @(New-BedEvidence HOST -Preserved); $jp = @(New-BedEvidence JOIN -Preserved)
Assert-BedGate $hp $jp PASS 'generated plus saved full native bed set' @{hostBeds=1;joinBeds=1;fullHostBeds=2;fullJoinBeds=2;preservedBeds=1}
$hpRaw = @($hp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('progress=1 ','progress=5 ') } else { $_ } })
$jpRaw = @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('progress=1 ','progress=5 ') } else { $_ } })
Assert-BedGate $hpRaw $jpRaw PASS 'native saved beds retain raw construction progress above one'
Assert-BedGate $hpRaw $jp FAIL 'native saved construction progress still participates in full-set comparison'
foreach ($bad in @('-1','NaN','Infinity')) {
    Assert-BedGate $hpRaw @($jpRaw | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('progress=5 ',"progress=$bad ") } else { $_ } }) FAIL "saved construction progress rejects $bad"
}
Assert-BedGate $hp @($jp | Where-Object { $_ -notmatch '\] PRESERVED ' }) FAIL 'missing saved bed despite declared preserved count'
Assert-BedGate $h $jp FAIL 'extra saved bed cannot hide behind matching generated digest'
Assert-BedGate $hp $j FAIL 'missing saved set despite internally consistent saved count'
$savedRow = @($jp | Where-Object { $_ -match '\] PRESERVED ' })[0]
Assert-BedGate $hp @($jp | ForEach-Object { $_; if ($_ -match '\] PRESERVED ') { $_ } }) FAIL 'duplicate full native saved hand'
$savedAcrossRegions=@($jp[0].Replace('count=1','count=2')) + $jp[1..4] + @($jp[1..4] | ForEach-Object { $_.Replace('region=2,-1','region=3,-1').Replace('id=42','id=43') }) + @($jp[5].Replace('count=1','count=2').Replace('beds=1','beds=2').Replace('preserved=1','preserved=2'),$jp[6])
Assert-BedGate @($savedAcrossRegions | ForEach-Object { $_.Replace('role=JOIN','role=HOST') }) $savedAcrossRegions FAIL 'one full native saved hand cannot exist in two complete current regions'
Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('source=native','source=wire') } else { $_ } }) FAIL 'unknown source cannot supply saved rows'
Assert-BedGate $hp @($jp + $savedRow) FAIL 'saved row outside completed census'
Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('hand=6.7.8.9.10','hand=6.7.8.9.11') } else { $_ } }) FAIL 'all five native hand components identify saved bed'
Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('hand=6.7.8.9.10','hand=06.07.08.09.010') } else { $_ } }) PASS 'all native hand components canonicalize numerically'
Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('floor=1','floor=01').Replace('flags=15','flags=015').Replace('function=5','function=05').Replace('state=9','state=09') } else { $_ } }) PASS 'saved integer descriptor fields canonicalize numerically'
Assert-BedGate @($hp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('flags=15','flags=14') } else { $_ } }) @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('flags=15','flags=14') } else { $_ } }) FAIL 'matching saved owner descriptors still require consistent native parent flags'
foreach ($badHand in @('0.0.0.0.0','0.0.0.0.00','1.2.3.4','1.2.3.4.4294967296','1.2.3.4.-1')) {
    Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace('hand=6.7.8.9.10',"hand=$badHand") } else { $_ } }) FAIL "invalid full native saved hand $badHand"
}
foreach ($change in @(
    @('category=1','category=2'), @("sid='saved-bed'","sid='other-bed'"),
    @('pos=7,8,9','pos=7,8,10'), @('yaw=0','yaw=0.5'),
    @('rotationX=0 rotationY=0 rotationZ=0 rotationW=1','rotationX=0.707106781 rotationY=0 rotationZ=0 rotationW=0.707106781'),
    @('floor=1','floor=2'), @('flags=15','flags=7'), @('function=5','function=6'),
    @('state=9','state=1'), @('progress=1','progress=0.5'),
    @("owner='player'","owner='other'"), @("town='hub'","town='other'"),
    @('townPos=4,5,6','townPos=4,5,7'), @("inside='house'","inside='other'"),
    @('insidePos=10,11,12','insidePos=10,11,13')
)) {
    Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace($change[0],$change[1]) } else { $_ } }) FAIL ('saved descriptor ' + ($change -join ' -> '))
}
Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match '\] PRESERVED ') { $_.Replace($identity,'rotationX=0 rotationY=0 rotationZ=0 rotationW=-2') } else { $_ } }) PASS 'saved quaternion normalizes sign and length'
foreach ($boundary in @('CATALOG','REGIONEND','CENSUSEND')) {
    foreach ($bad in @('', 'preserved=-1 ', 'preserved=bad ', 'preserved=2 ')) {
        Assert-BedGate $hp @($jp | ForEach-Object { if ($_ -match "\] $boundary ") { $_.Replace('preserved=1 ',$bad) } else { $_ } }) FAIL "saved count required and exact at $boundary : $bad"
    }
}
Assert-BedGate $hp @($jp + @(New-BedEvidence JOIN 2 -Preserved)[0..3]) FAIL 'newer truncated saved census invalidates complete older saved rows'
foreach ($field in @('actorPos','bedPos')) {
    Assert-BedGate $h @($j | ForEach-Object { $_.Replace("$field=1,2,3 ","") }) FAIL "native use requires $field"
    foreach ($bad in @('1,2,3.051','NaN,2,3','1,Infinity,3','1,2,-Infinity','1,2','bad,2,3')) {
        Assert-BedGate $h @($j | ForEach-Object { $_.Replace("$field=1,2,3","$field=$bad") }) FAIL "native use rejects mismatching/nonfinite $field=$bad"
    }
    Assert-BedGate $h @($j | ForEach-Object { $_.Replace("$field=1,2,3","$field=1.02,2.02,3.02") }) PASS "native $field within physical tolerance"
    Assert-BedGate $h @($j | ForEach-Object { $_.Replace("$field=1,2,3","$field=1.04,2.04,3") }) FAIL "native $field uses 3D distance, not per-axis allowance"
    Assert-BedGate $h @($j | ForEach-Object { $_.Replace("$field=1,2,3","$field=1.05,2,3") }) PASS "native $field includes exactly five centimetres"
}
# The lifecycle marker opts into mandatory native history; no missing stage may
# be described as NOT_PERFORMED or inferred from a final ready state.
function New-BedLifecycleEvidence([string]$Role) {
    "SCENARIO PROCLIFE role=$Role phase=required ok=1 world=99 load=3 region=2,-1"
    New-BedEvidence $Role 1 -Preserved
    "[procworld] CENSUSBEGIN role=$Role world=99 load=3 census=2 count=0 source=native"
    "[procworld] CENSUSEND role=$Role world=99 load=3 census=2 complete=1 count=0 beds=0 preserved=0 source=native"
    "SCENARIO PROCLIFE role=$Role phase=unloaded ok=1 world=99 load=3 census=2 region=2,-1"
    New-BedEvidence $Role 3 -Preserved
    "SCENARIO PROCLIFE role=$Role phase=reloaded ok=1 world=99 load=3 census=3 region=2,-1"
    if ($Role -eq 'JOIN') { "SCENARIO PROCLIFE role=$Role phase=reconnect_requested ok=1 world=99 load=3 oldLoad=3 census=3 region=2,-1" }
    $current = @(New-BedEvidence $Role 4 -Preserved -Load 4)
    $current[0..($current.Count-2)]
    "SCENARIO PROCLIFE role=$Role phase=reconnected ok=1 world=99 load=4 oldLoad=3 census=4 region=2,-1"
    $current[-1]
    "SCENARIO PROCLIFE role=$Role phase=host_use ok=1 world=99 load=4 census=4 region=2,-1 id=42 actor=1.2.3.4.5"
    "SCENARIO PROCLIFE role=$Role phase=preserved ok=1 world=99 load=4 census=4 region=2,-1"
    $current[-1].Replace('actor=1.2.3.4.5','actor=1.2.3.6.7').Replace('occupant=1.2.3.4.5','occupant=1.2.3.6.7')
    "SCENARIO PROCLIFE role=$Role phase=post_reconnect_use ok=1 world=99 load=4 census=4 region=2,-1"
}
$hl = @(New-BedLifecycleEvidence HOST); $jl = @(New-BedLifecycleEvidence JOIN)
Assert-BedGate $hl $jl PASS 'complete chronological native lifecycle and post-reconnect use' @{reconnect='PASS';unload='PASS';lifecycle='PASS'}
Assert-BedGate @($hl | ForEach-Object { $_.Replace('region=2,-1','region=20,32') }) @($jl | ForEach-Object { $_.Replace('region=2,-1','region=20,32') }) PASS 'existing Hub lifecycle remains valid'
Assert-BedGate @($hl | ForEach-Object { $_.Replace('region=2,-1','region=20,32') + ' outside=1' }) @($jl | ForEach-Object { $_.Replace('region=2,-1','region=20,32') + ' outside=1' }) FAIL 'outside variant cannot be certified with Hub-only native lifecycle'
Assert-BedGate @($hl | ForEach-Object { $_.Replace('region=2,-1','region=22,32') + ' outside=1' }) @($jl | ForEach-Object { $_.Replace('region=2,-1','region=22,32') + ' outside=1' }) PASS 'outside variant accepts complete native failing-cell lifecycle'
foreach ($phase in @('required','unloaded','reloaded','reconnected','host_use','preserved','post_reconnect_use')) {
    Assert-BedGate @($hl | Where-Object { $_ -notmatch "phase=$phase " }) $jl FAIL "HOST lifecycle missing $phase" @{lifecycle='FAIL'}
    Assert-BedGate $hl @($jl | Where-Object { $_ -notmatch "phase=$phase " }) FAIL "JOIN lifecycle missing $phase" @{lifecycle='FAIL'}
    Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match "phase=$phase ") { $_.Replace('ok=1','ok=0') } else { $_ } }) FAIL "JOIN lifecycle rejects false $phase"
}
Assert-BedGate $hl @($jl | Where-Object { $_ -notmatch 'phase=reconnect_requested ' }) FAIL 'JOIN reconnect must actually be accepted'
Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match 'phase=reconnect_requested ') { $_.Replace('ok=1','ok=0') } else { $_ } }) FAIL 'rejected JOIN reconnect is not a request'
Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match 'phase=reloaded ') { @($jl | Where-Object { $_ -match 'phase=unloaded ' })[0] }; if ($_ -notmatch 'phase=unloaded ') { $_ } }) FAIL 'unload and reload markers must be chronological'
Assert-BedGate $hl @($jl | ForEach-Object { $_; if ($_ -match 'phase=reconnected ') { $_ } }) FAIL 'duplicate reconnect phases cannot be cherry-picked'
Assert-BedGate @($hl | ForEach-Object { if ($_ -match 'phase=required ') { $_.Replace('world=99','world=0').Replace('load=3','load=0') } else { $_ } }) @($jl | ForEach-Object { if ($_ -match 'phase=required ') { $_.Replace('world=99','world=0').Replace('load=3','load=0') } else { $_ } }) PASS 'required onStart may precede native scope initialization'
Assert-BedGate $hl @($jl | ForEach-Object { $_.Replace('load=4','load=3') }) FAIL 'stale native load cannot prove reconnect'
Assert-BedGate @($hl | ForEach-Object { $_.Replace('load=4','load=0') }) @($jl | ForEach-Object { $_.Replace('load=4','load=0') }) FAIL 'zero post-reconnect scope cannot prove lifecycle'
Assert-BedGate $hl @($jl | ForEach-Object { $_.Replace('oldLoad=3','oldLoad=2') }) FAIL 'explicit old load must match actual pre-reconnect load'
Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match 'phase=reconnected ') { $_.Replace('world=99','world=98') } else { $_ } }) FAIL 'reconnect must retain same host world'
Assert-BedGate $hl @($jl | Where-Object { $_ -notmatch 'census=2 .*source=native' }) FAIL 'unloaded marker without complete absent-region census is not native proof'
Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match 'phase=unloaded ') { $_.Replace('census=2','census=1') } else { $_ } }) FAIL 'unloaded phase cannot point to loaded-region census'
Assert-BedGate $hl @($jl | Where-Object { $_ -notmatch 'PROCBEDUSE .*load=4' }) FAIL 'historical same-bed interaction cannot prove post-reconnect use'
Assert-BedGate $hl @($jl + '[procworld] CENSUSBEGIN role=JOIN world=99 load=4 census=5 count=1 source=native') FAIL 'post-reconnect latest truncated census still invalidates all old proof'
$changedAcrossReload = @($jl | ForEach-Object { if ($_ -match '\] PRESERVED .*load=4') { $_.Replace("inside='house'","inside='other'") } else { $_ } })
Assert-BedGate @($hl | ForEach-Object { if ($_ -match '\] PRESERVED .*load=4') { $_.Replace("inside='house'","inside='other'") } else { $_ } }) $changedAcrossReload FAIL 'matching final peers cannot hide saved-bed corruption across lifecycle'
Assert-BedGate $hl @($jl + 'SCENARIO PROCLIFE role=JOIN phase=reload_preservation ok=0 world=99 load=4 census=4 region=2,-1') FAIL 'explicit native lifecycle failure cannot be hidden by successful required phases'
foreach ($role in @('HOST','JOIN')) {
    $withoutHostUse=@($(if ($role -eq 'HOST') { $hl } else { $jl }) | Where-Object { $_ -notmatch 'PROCBEDUSE .*load=4 .*actor=1\.2\.3\.4\.5 ' })
    Assert-BedGate $(if ($role -eq 'HOST') { $withoutHostUse } else { $hl }) $(if ($role -eq 'JOIN') { $withoutHostUse } else { $jl }) FAIL "$role first participant needs actual native occupancy, not just host_use marker"
}
Assert-BedGate $hl @($jl | ForEach-Object { if ($_ -match 'phase=host_use ') { $_.Replace('actor=1.2.3.4.5','actor=1.2.3.6.7') } else { $_ } }) FAIL 'first participant proof precedes its marker'
Assert-BedGate $hl @($jl | ForEach-Object { $_.Replace('actor=1.2.3.6.7','actor=1.2.3.4.5').Replace('occupant=1.2.3.6.7','occupant=1.2.3.4.5') }) FAIL 'one participant cannot certify two-player use'
function Set-NewerGeneratedLayout([string[]]$Lines,[switch]$KeepEpoch,[switch]$OnlyRenumber) {
    foreach ($line in $Lines) {
        if ($line -notmatch 'load=4 ') { $line; continue }
        $changed=$line.Replace('id=42','id=43')
        if (-not $KeepEpoch) { $changed=$changed.Replace('epoch=7','epoch=8') }
        if (-not $OnlyRenumber) {
            $changed=$changed.Replace('pos=1,2,3','pos=1,2,4').Replace('actorPos=1,2,3','actorPos=1,2,4').Replace('bedPos=1,2,3','bedPos=1,2,4').Replace('digest=1234abcd','digest=abcd1234')
        }
        $changed
    }
}
Assert-BedGate @(Set-NewerGeneratedLayout $hl) @(Set-NewerGeneratedLayout $jl) PASS 'new authoritative generated layout at newer epoch retains exact current full peer parity'
Assert-BedGate @(Set-NewerGeneratedLayout $hl -KeepEpoch) @(Set-NewerGeneratedLayout $jl -KeepEpoch) FAIL 'changed authoritative layout requires strictly newer epoch'
Assert-BedGate @(Set-NewerGeneratedLayout $hl -OnlyRenumber) @(Set-NewerGeneratedLayout $jl -OnlyRenumber) FAIL 'unchanged surviving descriptor keeps its original logical ID even at newer epoch'
Assert-BedGate @($hl | ForEach-Object { if ($_ -match 'census=3 ') { $_.Replace('epoch=7','epoch=8') } else { $_ } }) @($jl | ForEach-Object { if ($_ -match 'census=3 ') { $_.Replace('epoch=7','epoch=8') } else { $_ } }) FAIL 'new reconnect load does not authorize an epoch reset after real reload'
Reset-GateResults
[void](Test-ProceduralBeds -HostLines $h -JoinLines $j)
$gate = @(Get-GateResults)[-1]
if ($gate.metrics.reconnect -ne 'NOT_PERFORMED' -or $gate.metrics.unload -ne 'NOT_PERFORMED') { throw 'Absent lifecycle proof must not be called tested' }
Write-Output 'Procedural beds native-set oracle behavioral assertions passed'
