# oracles/Panel.ps1 - F2 in-game panel configurability oracle (2026-07-14).
# Dot-sourced by CoopOracles.psm1 (module scope).
#
# The F2 panel is driven by hand (keyboard/mouse), so it is not exercised by the
# auto-start SCENARIO harness. Instead this oracle validates the [coop-ui] log
# contract emitted by any manual/real session, turning a hand-played panel
# session into a checkable artifact. It guards the ownership-rank role-switch
# fix: a panel connect that resolves ranks from the ROLE default must own {0}
# as HOST and {1} as JOIN - never the peer's rank (the "their character won't
# move" bug).
#
# Log contract (must match src/plugin/game/EngineEntity.cpp + Plugin.cpp):
#   [coop-ui] CONNECT role=HOST|JOIN transport=steam|udp          (panel intent)
#   [coop-ui] connect: role=HOST|JOIN transport=steam|udp peer=<n> ownRanks={csv} src=env|role  (resolved)
# Must NOT change those strings without updating this oracle.

# Parse the resolved connect lines from one log.
function Get-PanelConnects {
    param([string]$File)
    $out = @()
    if ($File -eq "" -or -not (Test-Path $File)) { return $out }
    $pat = '\[coop-ui\] connect: role=(HOST|JOIN) transport=(steam|udp) peer=(\d+) ownRanks=\{([\d,]*)\} src=(env|role)'
    foreach ($m in @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $out += @{
            line      = $m.LineNumber
            role      = $g[1].Value
            transport = $g[2].Value
            peer      = $g[3].Value
            ranks     = $g[4].Value      # e.g. "1" or "0,2"
            src       = $g[5].Value
        }
    }
    return $out
}

# Parse the panel-intent CONNECT lines (what the user selected in the panel).
function Get-PanelIntents {
    param([string]$File)
    $out = @()
    if ($File -eq "" -or -not (Test-Path $File)) { return $out }
    $pat = '\[coop-ui\] CONNECT role=(HOST|JOIN) transport=(steam|udp)'
    foreach ($m in @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $out += @{ role = $g[1].Value; transport = $g[2].Value; line = $m.LineNumber }
    }
    return $out
}

# Validate the F2 panel configurability + rank resolution on one log.
#   PASS : every role-sourced connect owns the correct default rank AND the
#          panel's (role,transport) selections all reached the connect path.
#   FAIL : a role-sourced connect owns the wrong rank (the freeze bug), or a
#          panel intent never produced a matching resolved connect.
#   SKIP : no [coop-ui] connect lines (panel not used, or old build) - can't judge.
function Test-PanelConfig {
    param([string]$File, [string]$GateName = "panel_config")
    $connects = @(Get-PanelConnects -File $File)
    $intents  = @(Get-PanelIntents  -File $File)

    if ($connects.Count -eq 0) {
        return (Add-GateResult -Name $GateName -Status $(if ($intents.Count) { 'FAIL' } else { 'SKIP' }) `
            -Metrics @{ connects = 0; intents = $intents.Count; roleSourced = 0; problems = $intents.Count } `
            -Detail $(if ($intents.Count) { 'panel connect attempted but never resolved' } else { 'panel not used' }))
    }

    $problems = @()
    $roleSourced = 0
    foreach ($c in $connects) {
        if ($c.src -eq "role") {
            $roleSourced++
            $want = if ($c.role -eq "HOST") { "0" } else { "1" }
            if ($c.ranks -ne $want) {
                $problems += "$($c.role) resolved ownRanks={$($c.ranks)} expected {$want}"
            }
        }
    }

    # A resolution must follow its own intent and precede the next attempt.
    # Older successes cannot satisfy newer failures.
    for ($n = 0; $n -lt $intents.Count; $n++) {
        $i = $intents[$n]
        $end = if ($n + 1 -lt $intents.Count) { $intents[$n + 1].line } else { [int]::MaxValue }
        $resolved = @($connects | Where-Object { $_.line -gt $i.line -and $_.line -lt $end })
        if ($resolved.Count -ne 1 -or $resolved[0].role -ne $i.role -or $resolved[0].transport -ne $i.transport) {
            $problems += "panel intent $($i.role)/$($i.transport) at line $($i.line) has no unique following matching resolution"
        }
    }

    $metrics = @{
        connects    = $connects.Count
        intents     = $intents.Count
        roleSourced = $roleSourced
        problems    = $problems.Count
    }
    if ($problems.Count -gt 0) {
        return (Add-GateResult -Name $GateName -Status FAIL -Metrics $metrics `
            -Detail ($problems -join "; "))
    }
    return (Add-GateResult -Name $GateName -Status PASS -Metrics $metrics `
        -Detail "$($connects.Count) connect(s), $roleSourced role-sourced rank(s) correct")
}
