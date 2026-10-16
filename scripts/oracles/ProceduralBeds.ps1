# Native physical evidence only; coordinator desired catalogs/ACKs are not proof.
function ConvertFrom-ProceduralEvidenceFields([string]$Line) {
    $fields = @{}
    foreach ($m in [regex]::Matches($Line, "(?<key>[A-Za-z]+)=(?:'(?<quoted>[^']*)'|(?<plain>[^\s]+))")) {
        $key = $m.Groups['key'].Value
        if ($fields.ContainsKey($key)) { throw "duplicate field $key" }
        $fields[$key] = if ($m.Groups['quoted'].Success) { $m.Groups['quoted'].Value } else { $m.Groups['plain'].Value }
    }
    return $fields
}
function ConvertTo-ProceduralPosition([string]$Text) {
    $parts=@($Text -split ',')
    if ($parts.Count -ne 3) { throw 'invalid physical position' }
    $values=@()
    foreach ($part in $parts) {
        $v=[double]::Parse($part,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture)
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { throw 'nonfinite physical position' }
        $values += $v
    }
    return ,$values
}
function Test-ProceduralPositionParity($Left,$Right) {
    $distanceSquared=0.0
    for ($i=0; $i -lt 3; $i++) { $delta=$Left[$i]-$Right[$i]; $distanceSquared += $delta*$delta }
    return $distanceSquared -le (0.05*0.05+1e-12)
}
function Get-ProceduralNativeEvidence([string[]]$Lines,[string]$Role) {
    $regions=@{}; $uses=@(); $errors=@(); $census=$null; $openRegion=$null; $seenCensuses=@{}
    $history=@(); $lifecycle=@(); $lifeErrors=@(); $required=$false; $lineNumber=0
    foreach ($line in $Lines) {
        $lineNumber++
        if ($line -notmatch '\[procworld\] (CENSUSBEGIN|CENSUSEND|CATALOG|OBJECT|PRESERVED|REGIONEND) ' -and $line -notmatch 'SCENARIO (PROCBEDUSE|PROCLIFE) ') { continue }
        $kind = if ($line -match '\[procworld\] (CENSUSBEGIN|CENSUSEND|CATALOG|OBJECT|PRESERVED|REGIONEND) ') { $Matches[1] } elseif ($line -match 'SCENARIO PROCLIFE ') { 'LIFE' } else { 'USE' }
        if ($kind -eq 'LIFE' -and $line -match '\bphase=required\b') { $required=$true }
        $f=$null
        try {
            $f = ConvertFrom-ProceduralEvidenceFields $line
            if ($kind -eq 'LIFE') {
                foreach ($key in @('role','phase','ok','world','load')) { if (-not $f.ContainsKey($key)) { throw "missing lifecycle $key" } }
                if ($f.role -cne $Role -or $f.ok -notmatch '^[01]$' -or $f.world -notmatch '^\d+$' -or $f.load -notmatch '^\d+$') { throw 'invalid lifecycle scope/result' }
                [void][UInt64]::Parse($f.world); [void][UInt32]::Parse($f.load)
                $f.Line=$lineNumber; $lifecycle += $f
                continue
            }
            if ($kind -eq 'USE' -and $f.ok -eq '0') { continue } # attempted/not yet in bed is not proof
            if ($kind -ne 'USE' -and $f.source -ne 'native') { continue }
            # A new native begin invalidates ALL older membership and occupancy proof,
            # even if this census never reaches END.
            if ($kind -eq 'CENSUSBEGIN') {
                $regions=@{}; $uses=@(); $errors=@(); $openRegion=$null
                $census=@{ Fields=$f; Ended=$false; Valid=$true; Regions=$regions; Uses=@(); Start=$lineNumber; End=0 }
                $history += $census
            }
            foreach ($key in @('role','world','load','census')) { if (-not $f.ContainsKey($key)) { throw "missing $key" } }
            if ($f.role -cne $Role -or $f.world -notmatch '^[1-9]\d*$' -or $f.load -notmatch '^[1-9]\d*$' -or $f.census -notmatch '^[1-9]\d*$') { throw 'invalid census scope' }
            [void][UInt64]::Parse($f.world); [void][UInt32]::Parse($f.load); [void][UInt64]::Parse($f.census)
            if ($null -eq $census) { throw 'native evidence without census begin' }
            if ($f.census -cne $census.Fields.census -or $f.world -cne $census.Fields.world -or $f.load -cne $census.Fields.load) { throw 'wrong census identity' }
            if ($kind -eq 'CENSUSBEGIN') {
                if ($seenCensuses.ContainsKey($f.census)) { throw 'duplicate census ID' }
                $seenCensuses[$f.census]=$true
                if ($f.count -notmatch '^\d+$') { throw 'invalid census membership count' }
                [void][UInt32]::Parse($f.count)
                continue
            }
            if ($kind -eq 'CENSUSEND') {
                if ($census.Ended -or $null -ne $openRegion) { throw 'invalid census end boundary' }
                if ($f.complete -ne '1' -or $f.count -cne $census.Fields.count -or $regions.Count -ne [UInt32]$f.count -or $f.beds -notmatch '^\d+$' -or $f.preserved -notmatch '^\d+$') { throw 'incomplete census membership/counts' }
                $total=0; $preservedTotal=0; $seenIDs=@{}; $seenHands=@{}
                foreach ($r in $regions.Values) {
                    if (-not $r.Ended -or $r.Fields.complete -ne '1' -or $r.Errors.Count -ne 0 -or $r.Rows.Count -ne [UInt32]$r.Fields.count -or $r.Preserved.Count -ne [UInt32]$r.Fields.preserved) { throw 'incomplete census region' }
                    $total += $r.Rows.Count; $preservedTotal += $r.Preserved.Count
                    foreach ($id in $r.Rows.Keys) {
                        if ($seenIDs.ContainsKey($id)) { throw 'duplicate logical ID across census regions' }
                        $seenIDs[$id]=$true
                    }
                    foreach ($hand in $r.Preserved.Keys) {
                        if ($seenHands.ContainsKey($hand)) { throw 'duplicate preserved hand across census regions' }
                        $seenHands[$hand]=$true
                    }
                }
                if ($total -ne [UInt32]::Parse($f.beds) -or $preservedTotal -ne [UInt32]::Parse($f.preserved)) { throw 'census bed/preserved count mismatch' }
                $census.Ended=$true
                $census.End=$lineNumber
                continue
            }
            if ($kind -ne 'USE' -and $census.Ended) { throw 'region evidence after census end' }
            if ($kind -eq 'USE' -and -not $census.Ended) { throw 'use before complete census' }
            foreach ($key in @('role','world','load','region','epoch')) { if (-not $f.ContainsKey($key)) { throw "missing $key" } }
            if ($f.role -cne $Role -or $f.world -notmatch '^[1-9]\d*$' -or $f.load -notmatch '^[1-9]\d*$' -or $f.epoch -notmatch '^[1-9]\d*$' -or $f.region -notmatch '^-?\d+,-?\d+$') { throw 'invalid native scope' }
            [void][UInt64]::Parse($f.world); [void][UInt32]::Parse($f.load); [void][UInt32]::Parse($f.epoch)
            $scope = "$($f.world)/$($f.load)/$($f.region)/$($f.epoch)"
            if ($kind -eq 'CATALOG') {
                if ($f.complete -notmatch '^[01]$' -or $f.count -notmatch '^\d+$' -or [UInt32]::Parse($f.count) -gt 512 -or $f.preserved -notmatch '^\d+$' -or $f.digest -notmatch '^[0-9a-fA-F]{8}$') { throw 'invalid catalog' }
                [void][UInt32]::Parse($f.preserved)
                if ($null -ne $openRegion -or $regions.ContainsKey($f.region)) { throw 'duplicate or overlapping region boundary' }
                $regions[$f.region] = @{ Scope=$scope; Fields=$f; Rows=@{}; Preserved=@{}; Errors=@(); Ended=$false }
                $openRegion=$f.region
            } elseif ($kind -eq 'REGIONEND') {
                if ($openRegion -cne $f.region -or -not $regions.ContainsKey($f.region)) { throw 'region end without unique begin' }
                $r=$regions[$f.region]
                if ($r.Ended -or $r.Scope -cne $scope -or $f.count -cne $r.Fields.count -or $r.Rows.Count -ne [UInt32]$f.count -or $f.preserved -cne $r.Fields.preserved -or $r.Preserved.Count -ne [UInt32]::Parse($f.preserved)) { throw 'region end scope/count/preserved mismatch' }
                $r.Ended=$true; $openRegion=$null
            } elseif ($kind -in @('OBJECT','PRESERVED')) {
                if (-not $regions.ContainsKey($f.region)) { throw 'object without catalog' }
                $r = $regions[$f.region]
                if ($openRegion -cne $f.region -or $r.Ended) { throw 'object outside region boundary' }
                if ($scope -cne $r.Scope) { $r.Errors += 'wrong object scope'; continue }
                foreach ($key in @('category','sid','pos','yaw','rotationX','rotationY','rotationZ','rotationW','floor','flags','function','state','progress','owner','town','townPos','inside','insidePos')) { if (-not $f.ContainsKey($key)) { throw "missing object $key" } }
                if ($f.category -ne '1' -or -not $f.sid -or $f.floor -notmatch '^-?\d+$') { throw 'invalid object type/floor' }
                $f.floor=[string][Int32]::Parse($f.floor)
                if ($kind -eq 'OBJECT') {
                    if ($f.id -notmatch '^[1-9]\d*$') { throw 'invalid logical object ID' }
                    [void][UInt64]::Parse($f.id)
                } else {
                    if ($f.ContainsKey('id') -or $f.hand -notmatch '^\d+\.\d+\.\d+\.\d+\.\d+$') { throw 'invalid preserved native hand' }
                    $f.hand=(@($f.hand -split '\.' | ForEach-Object { [string][UInt32]::Parse($_) }) -join '.')
                    if ($f.hand -eq '0.0.0.0.0') { throw 'null preserved native hand' }
                }
                foreach ($key in @('flags','function','state')) { $f[$key]=[string][UInt32]::Parse($f[$key]) }
                if (([UInt32]$f.flags -band 0xfffffff0L) -ne 0 -or ([UInt32]$f.state -band 0xfffffff0L) -ne 0) { throw 'unknown native flags' }
                foreach ($pair in @(@('owner',1),@('town',2),@('inside',4))) { if (([bool]([int]$f.flags -band $pair[1])) -ne ([bool]$f[$pair[0]])) { throw 'inconsistent parent flags' } }
                foreach ($key in @('pos','townPos','insidePos','yaw','progress')) {
                    $parts = @($f[$key] -split ','); $want = if ($key -in @('yaw','progress')) { 1 } else { 3 }
                    if ($parts.Count -ne $want) { throw "invalid $key" }
                    $values = @()
                    foreach ($part in $parts) {
                        $v = [double]::Parse($part,[Globalization.CultureInfo]::InvariantCulture)
                        if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { throw 'nonfinite native value' }
                        if ($key -eq 'progress' -and ($v -lt 0 -or ($kind -eq 'OBJECT' -and $v -gt 1))) { throw 'invalid construction progress' }
                        if ($key -eq 'yaw') { $v = $v % (2*[Math]::PI); if ($v -lt 0) { $v += 2*[Math]::PI } }
                        $scale = if ($key -in @('yaw','progress')) { 10000 } else { 100 }
                        $q = [Math]::Round($v*$scale,0,[MidpointRounding]::AwayFromZero)
                        if ($key -eq 'yaw' -and $q -ge 62832) { $q=0 }
                        $values += $q
                    }
                    $f[$key] = $values -join ','
                }
                $rotationKeys = @('rotationW','rotationX','rotationY','rotationZ')
                $rotation = @(); $lengthSquared = 0.0
                foreach ($key in $rotationKeys) {
                    $v = [double]::Parse($f[$key],[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture)
                    if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { throw 'nonfinite native quaternion' }
                    $rotation += $v
                    $lengthSquared += $v*$v
                }
                if ($lengthSquared -le 0 -or [double]::IsInfinity($lengthSquared)) { throw 'invalid native quaternion norm' }
                $length = [Math]::Sqrt($lengthSquared)
                for ($i=0; $i -lt 4; $i++) {
                    $rotation[$i] = [Math]::Round(($rotation[$i]/$length)*10000,0,[MidpointRounding]::AwayFromZero)
                }
                # q and -q agree, including near pi where W quantizes to zero.
                $sign = 1
                foreach ($v in $rotation) { if ($v -ne 0) { if ($v -lt 0) { $sign=-1 }; break } }
                for ($i=0; $i -lt 4; $i++) { $f[$rotationKeys[$i]] = [string][int]($rotation[$i]*$sign) }
                if ($kind -eq 'OBJECT') {
                    if ($r.Rows.ContainsKey($f.id)) { $r.Errors += "duplicate id $($f.id)" } else { $r.Rows[$f.id] = $f }
                } else {
                    if ($r.Preserved.ContainsKey($f.hand)) { $r.Errors += "duplicate preserved hand $($f.hand)" } else { $r.Preserved[$f.hand] = $f }
                }
            } else {
                foreach ($key in @('id','actor','actorPos','bedPos','inBed','pose','occupant','ok','t')) { if (-not $f.ContainsKey($key)) { throw "missing use $key" } }
                if ($f.id -notmatch '^[1-9]\d*$' -or $f.actor -notmatch '^\d+\.\d+\.\d+\.\d+\.\d+$' -or $f.actor -eq '0.0.0.0.0' -or $f.occupant -notmatch '^\d+\.\d+\.\d+\.\d+\.\d+$' -or $f.pose -notmatch '^-?\d+$' -or $f.t -notmatch '^\d+$') { throw 'invalid occupancy evidence' }
                $f.actorPosition=ConvertTo-ProceduralPosition $f.actorPos
                $f.bedPosition=ConvertTo-ProceduralPosition $f.bedPos
                $f.Scope=$scope; $f.Line=$lineNumber; $uses += $f
                $census.Uses += $f
            }
        } catch {
            if ($kind -eq 'LIFE') { $lifeErrors += "$Role lifecycle : $($_.Exception.Message)"; continue }
            if ($null -ne $census -and $kind -ne 'USE') { $census.Valid=$false }
            if ($kind -in @('OBJECT','PRESERVED') -and $null -ne $f -and $f.ContainsKey('region') -and $regions.ContainsKey($f.region)) {
                $regions[$f.region].Errors += "$Role $kind : $($_.Exception.Message)"
            } else { $errors += "$Role $kind : $($_.Exception.Message)" }
        }
    }
    if ($null -eq $census -or -not $census.Ended) { $errors += "$Role latest native census is incomplete" }
    return @{ Regions=$regions; Uses=$uses; Errors=$errors; Census=$census; History=$history; Lifecycle=$lifecycle; LifeErrors=$lifeErrors; Required=$required }
}
function Get-ProceduralDescriptorDifferences($Left,$Right) {
    foreach ($key in @('category','sid','pos','yaw','rotationW','rotationX','rotationY','rotationZ','floor','flags','function','state','progress','owner','town','townPos','inside','insidePos')) {
        if ($Left[$key] -cne $Right[$key]) { $key }
    }
}
function Get-ProceduralBedSetDifferences($Left,$Right,[string]$Label,[string[]]$Tables=@('Rows','Preserved')) {
    foreach ($table in $Tables) {
        $kind=if ($table -eq 'Rows') { 'bed' } else { 'preserved bed' }
        foreach ($id in $Left[$table].Keys) {
            if (-not $Right[$table].ContainsKey($id)) { "$Label missing $kind $id"; continue }
            foreach ($key in @(Get-ProceduralDescriptorDifferences $Left[$table][$id] $Right[$table][$id])) { "$Label $kind $id differs in $key" }
        }
        foreach ($id in $Right[$table].Keys) { if (-not $Left[$table].ContainsKey($id)) { "$Label extra $kind $id" } }
    }
}
function Get-ProceduralLifecycleEvidence($Evidence,[string]$Role) {
    $why=@($Evidence.LifeErrors); $phases=@{}; $bound=@{}; $hostUse=$null
    foreach ($p in $Evidence.Lifecycle) { if ($p.ok -cne '1') { $why += "$Role lifecycle $($p.phase) failed" } }
    $order=@('required','unloaded','reloaded')
    if ($Role -eq 'JOIN') { $order += 'reconnect_requested' }
    $order += @('reconnected','host_use','preserved','post_reconnect_use')
    foreach ($name in $order) {
        $rows=@($Evidence.Lifecycle | Where-Object { $_.phase -ceq $name })
        if ($rows.Count -ne 1) { $why += "$Role lifecycle needs one $name phase"; continue }
        $phases[$name]=$rows[0]
        if ($rows[0].ok -cne '1') { $why += "$Role lifecycle $name failed" }
    }
    if ($why.Count) { return @{Errors=$why;Phases=$phases;HostUse=$null} }
    try {
        $previous=0
        foreach ($name in $order) {
            $p=$phases[$name]
            if ($p.Line -le $previous) { throw "out-of-order $name phase" }
            $previous=$p.Line
            if ($name -eq 'required') { continue } # onStart can precede native world initialization
            if ($p.world -notmatch '^[1-9]\d*$' -or $p.load -notmatch '^[1-9]\d*$' -or $p.census -notmatch '^[1-9]\d*$' -or $p.region -notmatch '^-?\d+,-?\d+$') { throw "invalid $name native scope" }
            [void][UInt64]::Parse($p.census)
            $matchingCensuses=@($Evidence.History | Where-Object { $_.Fields.world -ceq $p.world -and $_.Fields.load -ceq $p.load -and $_.Fields.census -ceq $p.census })
            if ($matchingCensuses.Count -ne 1 -or -not $matchingCensuses[0].Ended -or -not $matchingCensuses[0].Valid -or $matchingCensuses[0].End -ge $p.Line) { throw "$name has no preceding complete native census" }
            $bound[$name]=$matchingCensuses[0]
        }
        $old=$phases.reloaded; $current=$phases.reconnected; $region=$old.region
        if ($phases.required.outside -ceq '1' -and $region -ceq '20,32') { throw 'outside lifecycle does not exercise an outside-Hub native region' }
        foreach ($name in @('unloaded','reloaded','reconnected','host_use','preserved','post_reconnect_use')) {
            if ($phases[$name].world -cne $old.world -or $phases[$name].region -cne $region) { throw "$name changed host world or target region" }
        }
        if ($phases.required.world -ne '0' -and $phases.required.world -cne $old.world) { throw 'required marker changed host world' }
        if ($phases.unloaded.load -cne $old.load -or $current.load -ceq $old.load -or $current.oldLoad -cne $old.load) { throw 'reconnect lacks distinct nonzero old/new native loads' }
        if ($Role -eq 'JOIN') {
            $request=$phases.reconnect_requested
            if ($request.world -cne $old.world -or $request.region -cne $region -or $request.load -cne $old.load -or $request.oldLoad -cne $old.load) { throw 'reconnect request does not fence actual old scope' }
        }
        if ($bound.unloaded.Regions.ContainsKey($region) -or $bound.unloaded.Start -le $phases.required.Line) { throw 'unloaded phase did not enumerate an actually absent target region' }
        if (-not $bound.reloaded.Regions.ContainsKey($region) -or $bound.reloaded.Start -le $phases.unloaded.Line) { throw 'reloaded phase lacks a newer actually loaded target census' }
        foreach ($name in @('reconnected','host_use','preserved','post_reconnect_use')) {
            if ($phases[$name].load -cne $current.load -or -not $bound[$name].Regions.ContainsKey($region) -or $bound[$name].Start -le $phases.reloaded.Line) { throw "$name lacks post-reconnect loaded native census" }
        }
        $p=$phases.host_use
        if ($p.id -notmatch '^[1-9]\d*$' -or $p.actor -notmatch '^\d+\.\d+\.\d+\.\d+\.\d+$' -or -not $bound.host_use.Regions[$region].Rows.ContainsKey($p.id)) { throw 'host_use does not identify a current native bed and actor' }
        $hostProof=@($bound.host_use.Uses | Where-Object {
            $_.Line -lt $p.Line -and $_.ok -ceq '1' -and $_.inBed -ceq '1' -and
            $_.Scope -ceq $bound.host_use.Regions[$region].Scope -and
            $_.id -ceq $p.id -and $_.actor -ceq $p.actor -and $_.occupant -ceq $_.actor
        })
        if ($hostProof.Count -eq 0) { throw 'host_use lacks actual preceding native occupancy' }
        $hostUse=$hostProof[-1]
        $last=$Evidence.Census
        if ($null -eq $last -or -not $last.Ended -or -not $last.Valid -or $phases.post_reconnect_use.census -cne $last.Fields.census -or $phases.post_reconnect_use.load -cne $last.Fields.load -or $phases.post_reconnect_use.world -cne $last.Fields.world) { throw 'post-reconnect phase is not the latest complete native census' }
        $baseline=@($Evidence.History | Where-Object { $_.Ended -and $_.Valid -and $_.Start -gt $phases.required.Line -and $_.End -lt $phases.unloaded.Line -and $_.Fields.world -ceq $old.world -and $_.Fields.load -ceq $old.load -and $_.Regions.ContainsKey($region) })
        if ($baseline.Count -eq 0) { throw 'no initial complete native bed set before unload' }
        $before=$baseline[0].Regions[$region]; $knownRows=@()
        foreach ($id in $before.Rows.Keys) { $knownRows += @{Id=$id;Fields=$before.Rows[$id]} }
        foreach ($name in @('reloaded','reconnected','host_use','preserved','post_reconnect_use')) {
            $after=$bound[$name].Regions[$region]
            $why += @(Get-ProceduralBedSetDifferences $before $after "$Role $name" @('Preserved'))
            $generatedDifferences=@(Get-ProceduralBedSetDifferences $before $after "$Role $name" @('Rows'))
            if ([UInt32]$after.Fields.epoch -lt [UInt32]$before.Fields.epoch) { $why += "$Role $name reset the authoritative native epoch" }
            if ($generatedDifferences.Count) {
                if ([UInt32]$after.Fields.epoch -le [UInt32]$before.Fields.epoch) { $why += "$Role $name changed generated layout without a strictly newer authoritative epoch" }
            } elseif ($before.Fields.digest -ine $after.Fields.digest) { $why += "$Role $name changed digest for unchanged generated descriptors" }
            foreach ($id in $after.Rows.Keys) {
                $known=$false
                foreach ($row in $knownRows) {
                    if (@(Get-ProceduralDescriptorDifferences $row.Fields $after.Rows[$id]).Count -eq 0) {
                        $known=$true
                        if ($row.Id -cne $id) { $why += "$Role $name reassigned surviving canonical descriptor from logical ID $($row.Id) to $id" }
                    }
                }
                if (-not $known) { $knownRows += @{Id=$id;Fields=$after.Rows[$id]} }
            }
            $before=$after
        }
    } catch { $why += "$Role lifecycle : $($_.Exception.Message)" }
    return @{Errors=$why;Phases=$phases;HostUse=$hostUse}
}
function Test-ProceduralBeds {
    param([string[]]$HostLines,[string[]]$JoinLines,[string]$HostFile,[string]$JoinFile)
    if ($HostFile) { $HostLines = @(Get-Content -LiteralPath $HostFile -Encoding UTF8) }
    if ($JoinFile) { $JoinLines = @(Get-Content -LiteralPath $JoinFile -Encoding UTF8) }
    $h = Get-ProceduralNativeEvidence $HostLines HOST; $j = Get-ProceduralNativeEvidence $JoinLines JOIN
    $why = @($h.Errors) + @($j.Errors); $hb=0; $jb=0; $hp=0; $jp=0
    $lifecycleRequired=$h.Required -or $j.Required; $hl=$null; $jl=$null
    if ($lifecycleRequired) {
        $hl=Get-ProceduralLifecycleEvidence $h HOST; $jl=Get-ProceduralLifecycleEvidence $j JOIN
        $why += @($hl.Errors) + @($jl.Errors)
        if (-not $hl.Errors.Count -and -not $jl.Errors.Count) {
            $hu=$hl.HostUse; $ju=$jl.HostUse
            if ($hu.Scope -cne $ju.Scope -or $hu.id -cne $ju.id -or $hu.actor -cne $ju.actor -or $hu.pose -cne $ju.pose -or
                -not (Test-ProceduralPositionParity $hu.actorPosition $ju.actorPosition) -or
                -not (Test-ProceduralPositionParity $hu.bedPosition $ju.bedPosition)) { $why += 'first participant native occupancy differs across peers' }
        }
    }
    if ($h.Regions.Count -eq 0 -or $j.Regions.Count -eq 0) { $why += 'no complete native region census on both peers' }
    foreach ($side in @($h,$j)) {
        $seen=@{}; $seenPreserved=@{}
        foreach ($r in $side.Regions.Values) {
            $why += $r.Errors
            if (-not $r.Ended -or $r.Fields.complete -ne '1' -or $r.Rows.Count -ne [int]$r.Fields.count -or $r.Preserved.Count -ne [UInt32]$r.Fields.preserved) { $why += "partial native region $($r.Scope)" }
            foreach ($id in $r.Rows.Keys) {
                if ($seen.ContainsKey($id)) { $why += "logical ID $id occurs in multiple native regions" }
                $seen[$id]=$true
            }
            foreach ($hand in $r.Preserved.Keys) {
                if ($seenPreserved.ContainsKey($hand)) { $why += "preserved hand $hand occurs in multiple native regions" }
                $seenPreserved[$hand]=$true
            }
        }
    }
    foreach ($r in $h.Regions.Values) { $hb += $r.Rows.Count; $hp += $r.Preserved.Count }
    foreach ($r in $j.Regions.Values) { $jb += $r.Rows.Count; $jp += $r.Preserved.Count }
    foreach ($rx in $h.Regions.Keys) {
        $hr=$h.Regions[$rx]
        if (-not $j.Regions.ContainsKey($rx)) { $why += "missing JOIN region $rx"; continue }
        $jr=$j.Regions[$rx]
        if ($hr.Scope -cne $jr.Scope -or $hr.Rows.Count -ne $jr.Rows.Count -or $hr.Preserved.Count -ne $jr.Preserved.Count -or $hr.Fields.digest -ine $jr.Fields.digest) { $why += "region scope/count/digest mismatch $rx" }
        $why += @(Get-ProceduralBedSetDifferences $hr $jr "JOIN region $rx")
    }
    foreach ($rx in $j.Regions.Keys) { if (-not $h.Regions.ContainsKey($rx)) { $why += "extra JOIN region $rx" } }
    $sameUse=$false
    foreach ($hu in $h.Uses) { foreach ($ju in $j.Uses) {
        if ($hu.ok -ne '1' -or $ju.ok -ne '1' -or $hu.inBed -ne '1' -or $ju.inBed -ne '1' -or $hu.actor -cne $hu.occupant -or $ju.actor -cne $ju.occupant) { continue }
        if ($hu.Scope -cne $ju.Scope -or $hu.id -cne $ju.id -or $hu.actor -cne $ju.actor -or $hu.pose -cne $ju.pose) { continue }
        if (-not (Test-ProceduralPositionParity $hu.actorPosition $ju.actorPosition) -or -not (Test-ProceduralPositionParity $hu.bedPosition $ju.bedPosition)) { continue }
        if ($lifecycleRequired) {
            if ($hl.Errors.Count -or $jl.Errors.Count) { continue }
            if ($hu.Line -le $hl.Phases.reconnected.Line -or $ju.Line -le $jl.Phases.reconnected.Line -or $hu.Line -ge $hl.Phases.post_reconnect_use.Line -or $ju.Line -ge $jl.Phases.post_reconnect_use.Line) { continue }
            # A mirror of one character is not two-player interaction. The
            # second native actor must use the very same current logical bed.
            if ($hu.actor -ceq $hl.HostUse.actor -or $ju.actor -ceq $jl.HostUse.actor -or
                $hu.id -cne $hl.HostUse.id -or $ju.id -cne $jl.HostUse.id -or
                $hu.world -cne $hl.HostUse.world -or $ju.world -cne $jl.HostUse.world -or
                $hu.region -cne $hl.HostUse.region -or $ju.region -cne $jl.HostUse.region -or
                $hu.epoch -cne $hl.HostUse.epoch -or $ju.epoch -cne $jl.HostUse.epoch -or
                $hu.pose -cne $hl.HostUse.pose -or $ju.pose -cne $jl.HostUse.pose) { continue }
        }
        if ($h.Regions.ContainsKey($hu.region) -and $j.Regions.ContainsKey($ju.region) -and $h.Regions[$hu.region].Scope -ceq $hu.Scope -and $j.Regions[$ju.region].Scope -ceq $ju.Scope -and $h.Regions[$hu.region].Rows.ContainsKey($hu.id) -and $j.Regions[$ju.region].Rows.ContainsKey($ju.id)) { $sameUse=$true }
    } }
    if (-not $sameUse) { $why += 'no actual same logical bed / native pose / occupant / physical XYZ observed on both peers in the current census' }
    $lifeStatus=if (-not $lifecycleRequired) { 'NOT_PERFORMED' } elseif ($why.Count) { 'FAIL' } else { 'PASS' }
    return (Add-GateResult -Name procedural_beds -Status $(if ($why.Count) { 'FAIL' } else { 'PASS' }) -Metrics @{regions=$h.Regions.Count;hostBeds=$hb;joinBeds=$jb;fullHostBeds=($hb+$hp);fullJoinBeds=($jb+$jp);preservedBeds=$hp;preservedJoinBeds=$jp;sameBedUse=$sameUse;reconnect=$lifeStatus;unload=$lifeStatus;lifecycle=$lifeStatus} -Detail ($why -join '; '))
}
