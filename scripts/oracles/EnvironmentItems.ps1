# Real native scenario evidence only. ACK/desired-catalog agreement alone never passes.
function ConvertFrom-EnvironmentFields([string]$Line) {
    $result=@{}
    foreach ($m in [regex]::Matches($Line,"(?<key>[A-Za-z][A-Za-z0-9]*)=(?:'(?<quoted>[^']*)'|(?<plain>[^\s]+))")) {
        $key=$m.Groups['key'].Value
        if ($result.ContainsKey($key)) { throw "duplicate environment field: $key" }
        $result[$key]=if ($m.Groups['quoted'].Success) { $m.Groups['quoted'].Value } else { $m.Groups['plain'].Value }
    }
    return $result
}
function Get-EnvironmentNativeEvidence([string[]]$Lines,[string]$Side) {
    $open=@{}; $samples=@(); $phases=@(); $mutations=@(); $errors=@(); $seen=@{}; $lineNumber=0
    foreach ($line in $Lines) {
        $lineNumber++
        if ($line -notmatch 'SCENARIO ENVIRONMENT (ITEM|SAMPLE|PHASE|MUTATE) ') { continue }
        $kind=$Matches[1]
        try {
            $f=ConvertFrom-EnvironmentFields $line
            if ($f.side -cne $Side) { throw 'wrong native evidence side' }
            if ($kind -eq 'PHASE') { $phases+=,$f; continue }
            if ($kind -eq 'MUTATE') {
                if ($f.nativeRemoved -ne '1' -or $f.id -notmatch '^[1-9][0-9]*$' -or $f.world -notmatch '^[1-9][0-9]*$' -or $f.load -notmatch '^[1-9][0-9]*$' -or $f.scope -notmatch '^[1-9][0-9]*$') { throw 'unproven native mutation/scope' }
                $mutations+=,$f;continue
            }
            foreach ($key in @('stage','t','world','load','scope','region','epoch')) { if (-not $f.ContainsKey($key)) { throw "missing native scope $key" } }
            if ($f.stage -notmatch '^[0-4]$' -or $f.t -notmatch '^[0-9]+$' -or $f.world -notmatch '^[0-9]+$' -or $f.load -notmatch '^[0-9]+$' -or $f.scope -notmatch '^[0-9]+$' -or $f.region -notmatch '^-?[0-9]+,-?[0-9]+$' -or $f.epoch -notmatch '^[0-9]+$') { throw 'invalid native scope' }
            $key="$($f.stage)|$($f.t)|$($f.world)|$($f.load)|$($f.scope)|$($f.region)|$($f.epoch)"
            if ($kind -eq 'ITEM') {
                foreach ($field in @('id','bound','native','hand','sid','type','qty','condition','charges','manufacturer','material','grade','pos','rot','floor','flags','parent','parentSID','owner','persistent','ownerSID','ownerStream','ownerKey','persistentSID','persistentStream','persistentKey')) {
                    if (-not $f.ContainsKey($field)) { throw "missing native item field $field" }
                }
                if (-not $open.ContainsKey($key)) { $open[$key]=@() }
                $open[$key]+=,$f;continue
            }
            if ($seen.ContainsKey($key)) { throw 'duplicate native sample closure' }
            $seen[$key]=$true
            $rows=@(if ($open.ContainsKey($key)) { $open[$key] })
            [void]$open.Remove($key)
            if ($f.ready -ne '1') { continue }
            if ($f.world -eq '0' -or $f.load -eq '0' -or $f.scope -eq '0') { throw 'ready without admitted world/load/region scope' }
            foreach ($field in @('count','nativeCount','digest','nativeDigest','room','protected','preserved')) {
                if (-not $f.ContainsKey($field)) { throw "missing native sample field $field" }
            }
            if ($f.count -notmatch '^[0-9]+$' -or $f.nativeCount -notmatch '^[0-9]+$' -or $f.protected -notmatch '^[1-9][0-9]*$' -or $f.preserved -ne '1') { throw 'missing native preservation/count proof' }
            if ($rows.Count -ne [int]$f.count -or $rows.Count -ne [int]$f.nativeCount -or $f.epoch -eq '0') { throw 'partial native membership' }
            if ($f.digest -notmatch '^[0-9a-fA-F]{8}$' -or $f.nativeDigest -notmatch '^[0-9a-fA-F]{8}$') { throw 'invalid native digest' }
            $ids=@{}; $hands=@{}
            if ([int]$f.stage -eq 0 -and $rows.Count -eq 0) { throw 'no positively identified native managed category before mutation' }
            foreach ($row in $rows) {
                if ($row.bound -ne '1' -or $row.id -notmatch '^[1-9][0-9]*$' -or $row.native -notmatch '^[0-9a-fA-F]{8}$' -or $row.qty -notmatch '^[1-9][0-9]*$') { throw 'unbound/invalid native row' }
                if ($ids.ContainsKey($row.id) -or $hands.ContainsKey($row.hand)) { throw 'duplicate native/network identity' }
                $ids[$row.id]=$row;$hands[$row.hand]=$true
            }
            $samples+=,@{Fields=$f;Rows=$ids;Line=$lineNumber}
        } catch { $errors+="line ${lineNumber}: $($_.Exception.Message)" }
    }
    return @{Samples=$samples;Phases=$phases;Mutations=$mutations;Errors=$errors}
}
function Test-EnvironmentItemsNativePair {
    param([string[]]$HostLines,[string[]]$JoinLines,[switch]$Lifecycle)
    $hostEvidence=Get-EnvironmentNativeEvidence $HostLines 'HOST'
    $joinEvidence=Get-EnvironmentNativeEvidence $JoinLines 'JOIN'
    $errors=@($hostEvidence.Errors)+@($joinEvidence.Errors); $paired=@(); $mutation=$null
    if ($hostEvidence.Mutations.Count -ne 1) { $errors+='expected exactly one actual HOST native removal' } else { $mutation=$hostEvidence.Mutations[0] }
    foreach ($joinSample in $joinEvidence.Samples) {
        $j=$joinSample.Fields
        foreach ($hostSample in $hostEvidence.Samples) {
            $h=$hostSample.Fields
            if ($h.world -cne $j.world -or $h.stage -cne $j.stage -or $h.region -cne $j.region -or $h.room -cne $j.room -or $h.epoch -cne $j.epoch -or $h.digest -cne $j.digest) { continue }
            if ([Math]::Abs([long]$h.t-[long]$j.t) -gt 1500) { continue }
            if ($h.nativeDigest -cne $j.nativeDigest -or $h.nativeCount -cne $j.nativeCount) { continue }
            $same=$true
            foreach ($id in $joinSample.Rows.Keys) {
                if (-not $hostSample.Rows.ContainsKey($id)) { $same=$false;break }
                $a=$hostSample.Rows[$id];$b=$joinSample.Rows[$id]
                foreach ($field in @('native','sid','type','qty','condition','charges','pos','rot','manufacturer','material','grade','floor','flags','parent','parentSID','owner','persistent','ownerSID','ownerStream','ownerKey','persistentSID','persistentStream','persistentKey')) {
                    if ($a[$field] -cne $b[$field]) { $same=$false;break }
                }
                if (-not $same) { break }
            }
            if (-not $same) { continue }
            if ($mutation -and [int]$j.stage -eq 1 -and $j.world -cne $mutation.world) { continue }
            if ($mutation -and [int]$j.stage -eq 0 -and
                (-not $joinSample.Rows.ContainsKey($mutation.id) -or -not $hostSample.Rows.ContainsKey($mutation.id))) { continue }
            if ($mutation -and [int]$j.stage -gt 0 -and
                ([long]$h.t -le [long]$mutation.t -or [long]$j.t -le [long]$mutation.t)) { continue }
            if ($mutation -and [int]$j.stage -gt 0 -and $j.world -ceq $mutation.world -and $j.region -ceq $mutation.region -and
                ([uint32]$j.epoch -le [uint32]$mutation.epoch -or $joinSample.Rows.ContainsKey($mutation.id) -or $hostSample.Rows.ContainsKey($mutation.id))) { continue }
            $paired+=,@{Stage=[int]$j.stage;World=$j.world;HostLoad=$h.load;JoinLoad=$j.load;HostScope=$h.scope;JoinScope=$j.scope;Epoch=$j.epoch;Region=$j.region;HostLine=$hostSample.Line;JoinLine=$joinSample.Line;NativeCount=$j.nativeCount}
            break
        }
    }
    if (@($paired | Where-Object {$_.Stage -eq 0}).Count -lt 1) { $errors+='no complete paired native BEFORE snapshot' }
    if (@($paired | Where-Object {$_.Stage -eq 1}).Count -lt 8) { $errors+='fewer than eight complete paired native AFTER snapshots with removed ID absent' }
    if ($Lifecycle) {
        foreach ($evidence in @($hostEvidence,$joinEvidence)) {
            foreach ($name in @('travel_away','native_region_unloaded','travel_return')) {
                if (@($evidence.Phases | Where-Object {$_.name -ceq $name -and $_.ok -eq '1'}).Count -lt 1) { $errors+="missing real lifecycle phase: $name" }
            }
        }
        if (@($joinEvidence.Phases | Where-Object {$_.name -ceq 'native_reconnect_requested' -and $_.ok -eq '1'}).Count -ne 1) { $errors+='missing actual JOIN reconnect invocation' }
        if (@($paired | Where-Object {$_.Stage -eq 3}).Count -lt 8) { $errors+='missing paired native region-reload proof' }
        if (@($paired | Where-Object {$_.Stage -eq 4}).Count -lt 8) { $errors+='missing paired native reconnect proof' }
        $before=@($paired | Where-Object {$_.Stage -eq 0} | Select-Object -First 1)
        if ($before.Count) {
            if (@($paired | Where-Object {$_.Stage -eq 3 -and ([uint32]$_.HostScope -gt [uint32]$before[0].HostScope) -and ([uint32]$_.JoinScope -gt [uint32]$before[0].JoinScope)}).Count -lt 8) { $errors+='missing fresh per-region scope after native unload/reload' }
            if (@($paired | Where-Object {$_.Stage -eq 4 -and $_.JoinLoad -cne $before[0].JoinLoad}).Count -lt 8) { $errors+='reconnect did not enter a fresh JOIN load generation' }
        }
    }
    return [pscustomobject]@{Passed=($errors.Count -eq 0);Errors=$errors;Pairs=$paired;HostSamples=$hostEvidence.Samples.Count;JoinSamples=$joinEvidence.Samples.Count}
}
