# oracles/Parsing.ps1 - clock-offset + log-parsing helpers (monolith split of
# CoopOracles.psm1, 2026-07-12): Get-LogClockOffsetMs, Get-ClockSyncStats,
# Convert-StampToMs, Get-ScenarioLines, Get-ScenarioSeries, Get-MarkerTimeMs.
# Dot-sourced by CoopOracles.psm1 into the MODULE scope, so $script: state
# ($script:ClockOffsetCache) is shared with every other fragment.
# Must NOT: contain oracles (Test-*) - parsers only. Regex phrasing is the
# C++-to-oracle log contract (resources/CODE_MAP.md, log-tag index).
# ---- Clock-offset helpers -----------------------------------------------------

# Per-log wall-clock offset (ms) relative to the HOST clock, estimated by the
# plugin's wire time-sync and logged as "CLOCKSYNC offset=<ms> rtt=<ms>". We take
# the minimum-RTT sample (the classic NTP filter: least queueing noise). A log
# with no CLOCKSYNC lines (host, or a pre-time-sync DLL) gets offset 0.
function Get-LogClockOffsetMs {
    param([string]$File)
    if (-not (Test-Path $File)) { return 0 }
    if ($script:ClockOffsetCache.ContainsKey($File)) { return $script:ClockOffsetCache[$File] }
    $best = $null; $bestRtt = [int]::MaxValue
    foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
        $off = [int]$m.Matches[0].Groups[1].Value
        $rtt = [int]$m.Matches[0].Groups[2].Value
        if ($rtt -le $bestRtt) { $bestRtt = $rtt; $best = $off }
    }
    $result = if ($null -ne $best) { $best } else { 0 }
    $script:ClockOffsetCache[$File] = $result
    return $result
}

# Count of CLOCKSYNC samples + the min-RTT sample, for the clock_sync gate.
function Get-ClockSyncStats {
    param([string]$File)
    $n = 0; $bestOff = $null; $bestRtt = [int]::MaxValue
    if (Test-Path $File) {
        foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
            $n++
            $off = [int]$m.Matches[0].Groups[1].Value
            $rtt = [int]$m.Matches[0].Groups[2].Value
            if ($rtt -le $bestRtt) { $bestRtt = $rtt; $bestOff = $off }
        }
    }
    return [pscustomobject]@{ samples = $n; offsetMs = $bestOff; rttMs = $(if ($n -gt 0) { $bestRtt } else { $null }) }
}

# Parse a "[HH:MM:SS.mmm]" stamp match-group set into ms-since-midnight, applying
# the log's clock offset so all returned times are in the HOST clock frame.
function Convert-StampToMs {
    param($Groups, [int]$OffsetMs)
    if ([int]$Groups[1].Value -gt 23 -or [int]$Groups[2].Value -gt 59 -or
        [int]$Groups[3].Value -gt 59 -or [int]$Groups[4].Value -gt 999) { throw 'Invalid wall-clock timestamp' }
    $t = ([int]$Groups[1].Value * 3600 + [int]$Groups[2].Value * 60 + [int]$Groups[3].Value) * 1000 + [int]$Groups[4].Value
    return $t + $OffsetMs
}

# Index timestamps once in file order, then apply CLOCKSYNC. The first log
# anchors the run's day; subsequent logs use the nearest aligned day. An extra
# day keeps marker thresholds with a zero default before every real sample.
function Get-LogTimeMs {
    param([Parameter(Mandatory = $true)]$Match)
    $file = $Match.Path
    if (-not $script:LogTimelineCache.ContainsKey($file)) {
        $times = @{}
        $offset = Get-LogClockOffsetMs -File $file
        [long]$day = 0; $previous = $null; $lineNumber = 0
        foreach ($line in (Get-Content -LiteralPath $file -Encoding UTF8)) {
            $lineNumber++
            $stamp = [regex]::Match($line, '^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
            if (-not $stamp.Success) { continue }
            $raw = [long](Convert-StampToMs -Groups $stamp.Groups -OffsetMs 0)
            if ($null -eq $previous) {
                $day = 86400000 - [long]([Math]::Floor(($raw + $offset) / 86400000.0)) * 86400000
                $first = $raw + $day + $offset
                if ($null -eq $script:LogTimelineAnchor) { $script:LogTimelineAnchor = $first }
                else { $day += [long]([Math]::Floor(($script:LogTimelineAnchor - $first + 43200000.0) / 86400000.0)) * 86400000 }
            } elseif ($raw -lt ($previous - 43200000)) {
                $day += 86400000
            }
            $times[[long]$lineNumber] = $raw + $day + $offset
            $previous = $raw
        }
        $script:LogTimelineCache[$file] = $times
    }
    return $script:LogTimelineCache[$file][[long]$Match.LineNumber]
}

# ---- Log parsing helpers ------------------------------------------------------

# Parse SCENARIO <Kind> hand=.. pos=.. lines from a log into hand -> "x,y,z"
# (latest logged position per hand wins). $Kind is MEMBER or RECV.
function Get-ScenarioLines {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $number = '[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?'
    $pat = "SCENARIO $Kind hand=(\d+(?:,\d+){4}) pos=($number,$number,$number)(?: |$)"
    foreach ($m in (Select-String -Path $File -Pattern "SCENARIO $Kind hand=" -Encoding UTF8)) {
        $record = [regex]::Match($m.Line, $pat)
        if (-not $record.Success) { throw "Malformed full-identity $Kind row at ${File}:$($m.LineNumber)" }
        $g = $record.Groups
        $map[$g[1].Value] = $g[2].Value
    }
    return $map
}

# Parse timestamped SCENARIO <Kind> lines into hand -> list of @{t=ms; p=@(x,y,z)}.
# Timestamps are converted into the HOST clock frame via the log's CLOCKSYNC
# offset (0 when absent), so host and join series are directly time-comparable
# even across machines with disagreeing wall clocks.
function Get-ScenarioSeries {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $number = '[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?'
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO $Kind hand=(\d+(?:,\d+){4}) pos=($number,$number,$number)(?= |$)(?: task=(\d+))?(?: pelvis=($number))?(?: crouch=(-?\d+))?(?: idle=(-?\d+))?(?: bs=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern "SCENARIO $Kind hand=" -Encoding UTF8)) {
        $record = [regex]::Match($m.Line, $pat)
        if (-not $record.Success) { throw "Malformed timestamped full-identity $Kind row at ${File}:$($m.LineNumber)" }
        $g = $record.Groups
        $t = Get-LogTimeMs -Match $m
        $hand = $g[5].Value
        $pos  = @($g[6].Value.Split(',') | ForEach-Object { [double]::Parse($_, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture) })
        if (@($pos | Where-Object { [double]::IsNaN($_) -or [double]::IsInfinity($_) }).Count) {
            throw "Non-finite position at ${File}:$($m.LineNumber)"
        }
        $task = if ($g[7].Success) { [int]$g[7].Value } else { 65535 }
        $pelvis = if ($g[8].Success) { [double]$g[8].Value } else { -1.0 }
        $crouch = if ($g[9].Success) { [int]$g[9].Value } else { -1 }
        $bs   = if ($g[11].Success) { [int]$g[11].Value } else { 0 }
        if (-not $map.ContainsKey($hand)) { $map[$hand] = New-Object System.Collections.ArrayList }
        [void]$map[$hand].Add(@{ t = $t; p = $pos; task = $task; pelvis = $pelvis; crouch = $crouch; bs = $bs })
    }
    return $map
}

# Every "index,serial" a body has answered to in $File, given the identity the
# WIRE uses for it. A client that drives someone else's town NPC gets it
# re-keyed the moment a fight starts - the engine separates the body into a
# fresh local platoon and renumbers it - so from then on every LOCAL row (the
# scenario's captureNpcs sweep, its VITALS reads) is filed under a hand the peer
# has never heard of. To an oracle matching on the wire hand that is
# indistinguishable from the body despawning. The replicator logs the
# translation as [rekey]; this follows it.
function Get-HandAliases {
    param([string]$File, [string]$WireIndexSerial)
    $ids = @($WireIndexSerial)
    foreach ($m in (Select-String -Path $File -Pattern ("\[rekey\] wire=" + [regex]::Escape($WireIndexSerial) + " local=(\d+),(\d+)") -ErrorAction SilentlyContinue)) {
        $alias = $m.Matches[0].Groups[1].Value + ',' + $m.Matches[0].Groups[2].Value
        if ($ids -notcontains $alias) { $ids += $alias }
    }
    return ,$ids
}

# Find the first timestamped marker line matching $Pattern in $File and return its
# ms-since-midnight in the HOST clock frame, or $null.
function Get-MarkerTimeMs {
    param([string]$File, [string]$Pattern)
    $om = Select-String -Path $File -Pattern ("\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*" + $Pattern) -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { return $null }
    $off = Get-LogClockOffsetMs -File $File
    return (Get-LogTimeMs -Match $om)
}


# Reliable delivery is keyed by occurrence and the full wire identity. Local
# rekeys do not alter the packet hand, so SEND/RECV never need fuzzy matching.
function Get-ReliableEventRows {
    param([string]$File, [ValidateSet('SEND','RECV')][string]$Direction)
    if ($File -eq '' -or -not (Test-Path -LiteralPath $File)) { return }
    $pattern = "\[event\] $Direction id=(?<id>\d+) ev=(?<event>\d+).*?hand=(?<hand>\d+(?:,\d+){4}) actor=(?<actor>\d+,\d+)(?: |$)"
    foreach ($line in (Select-String -LiteralPath $File -Pattern "\[event\] $Direction " -Encoding UTF8)) {
        $record = [regex]::Match($line.Line, $pattern)
        if (-not $record.Success) { throw "Malformed reliable $Direction record at ${File}:$($line.LineNumber)" }
        $g = $record.Groups
        $owner = [regex]::Match($line.Line, '\bowner=(\d+)(?: |$)')
        $session = [regex]::Match($line.Line, '\bsession=(\d+)(?: |$)')
        $actorFull = [regex]::Match($line.Line, '\bactor5=(\d+(?:,\d+){4})(?: |$)')
        [pscustomobject]@{
            Id=$g['id'].Value; Event=[int]$g['event'].Value; Hand=$g['hand'].Value
            Actor=$g['actor'].Value; Owner=$(if ($owner.Success) { $owner.Groups[1].Value } else { '' })
            Session=$(if ($session.Success) { $session.Groups[1].Value } else { '' })
            ActorFull=$(if ($actorFull.Success) { $actorFull.Groups[1].Value } else { '' })
            Time=(Get-LogTimeMs -Match $line); Line=$line.LineNumber
        }
    }
}

function Find-ReliableReceipt {
    param($Send, $Receipts)
    if ($null -eq $Send -or $Send.Owner -eq '' -or $null -eq $Send.Time) { return }
    if ($Send.Actor -ne '0,0' -and ($Send.ActorFull -eq '' -or -not $Send.ActorFull.EndsWith(','+$Send.Actor))) { return }
    $Receipts | Where-Object {
        $_.Id -eq $Send.Id -and $_.Event -eq $Send.Event -and $_.Hand -eq $Send.Hand -and
        $_.Actor -eq $Send.Actor -and $_.ActorFull -eq $Send.ActorFull -and $_.Owner -eq $Send.Owner -and $_.Session -eq $Send.Session -and
        $null -ne $_.Time -and $_.Time -ge $Send.Time
    } | Sort-Object -Property @('Time','Line') | Select-Object -First 1
}

# Scenario rows use i,s,type,container,containerSerial; events use the wire
# tuple in type,container,containerSerial,i,s order. Only a logged rekey may
# translate it, and the translated body's samples must follow that rekey.
function Get-MappedScenarioSamples {
    param($Map, [string]$WireHand, [string]$File)
    if ($Map.ContainsKey($WireHand)) { $Map[$WireHand] }
    $parts = $WireHand.Split(',')
    if ($parts.Count -ne 5) { return }
    $wireShort = [regex]::Escape("$($parts[0]),$($parts[1])")
    $pattern = "\[rekey\] wire=$wireShort local=(\d+),(\d+) localc=(\d+),(\d+)(?: |$)"
    $seen = @{}
    foreach ($line in (Select-String -LiteralPath $File -Pattern $pattern -Encoding UTF8)) {
        $g = $line.Matches[0].Groups
        $key = "$($g[1].Value),$($g[2].Value),$($parts[2]),$($g[3].Value),$($g[4].Value)"
        if ($key -eq $WireHand -or $seen.ContainsKey($key) -or -not $Map.ContainsKey($key)) { continue }
        $since = Get-LogTimeMs -Match $line
        if ($null -eq $since) { continue }
        $seen[$key] = $true
        $Map[$key] | Where-Object { $_.t -ge $since }
    }
}
