<#
.SYNOPSIS
  Game-independent behavioral checks of oracle dispatch, configured thresholds
  and fail-closed scenario verdicts. Does not inspect source text or duplicate
  the manifest/schema implementation. Diagnostic capture is covered by the
  complete-participant cases in OracleRegression.Tests.ps1.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$scriptsRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptsRoot 'CoopOracles.psm1') -Force
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Condition)
    if ($Condition) { $script:Pass++; Write-Host "  ok   $Name" }
    else { $script:Fail++; Write-Host "  FAIL $Name" }
}
$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ('kc_contract_' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $tmpDir)
try {
    $hostLog = Join-Path $tmpDir 'host.log'
    $joinLog = Join-Path $tmpDir 'join.log'
    [IO.File]::WriteAllText($hostLog, '')
    [IO.File]::WriteAllText($joinLog, '')
    Reset-GateResults
    $unknown = Invoke-OneOracle -Id 'definitely_not_an_oracle' -HostLog $hostLog -JoinLog $joinLog -Tolerance 3.0 -ExpectedSkewMs $null
    Check 'unknown oracle cannot certify a run' ($unknown -eq 'FAIL')

    $smooth = Join-Path $tmpDir 'smooth.log'
    Set-Content -LiteralPath $smooth -Encoding UTF8 -Value '[00:00:01.000] [JOIN] INFO: SCENARIO SMOOTH active=5000 zeroFrac=0.500 stallFrac=0.000 maxStep=10.0 slewSkip=0'
    Reset-GateResults
    $loose = Invoke-OneOracle -Id 'smoothness' -HostLog $hostLog -JoinLog $smooth -Tolerance 0 -ExpectedSkewMs $null -Bounds @{ MaxZeroFrac = 0.60 }
    Reset-GateResults
    $tight = Invoke-OneOracle -Id 'smoothness' -HostLog $hostLog -JoinLog $smooth -Tolerance 0 -ExpectedSkewMs $null -Bounds @{ MaxZeroFrac = 0.40 }
    Check 'configured threshold changes the same movement evidence verdict' ($loose -eq 'PASS' -and $tight -eq 'FAIL')

    foreach ($scenario in @('leader_move', 'no_such_scenario_xyz')) {
        @(
            '[10:00:00.000] [HOST] INFO: gameplay started',
            "[10:00:00.500] [HOST] INFO: SCENARIO $scenario start",
            '[10:00:02.000] [HOST] INFO: SCENARIO RESULT PASS'
        ) | Set-Content -LiteralPath $hostLog -Encoding UTF8
        @(
            '[10:00:00.000] [JOIN] INFO: gameplay started',
            "[10:00:00.500] [JOIN] INFO: SCENARIO $scenario start",
            '[10:00:00.600] [JOIN] INFO: CLOCKSYNC offset=4 rtt=8 n=1',
            '[10:00:01.100] [JOIN] INFO: CLOCKSYNC offset=4 rtt=8 n=2',
            '[10:00:01.600] [JOIN] INFO: CLOCKSYNC offset=4 rtt=8 n=3',
            '[10:00:02.000] [JOIN] INFO: SCENARIO RESULT PASS'
        ) | Set-Content -LiteralPath $joinLog -Encoding UTF8
        $jsonPath = Join-Path $tmpDir ($scenario + '.json')
        Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $scenario -OutJson $jsonPath | Out-Null
        $verdict = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
        $prerequisites = @($verdict.gates | Where-Object { $_.gate -in @('health_host','health_join','result_host','result_join') })
        Check "${scenario}: participant completion is valid before testing missing movement" ($prerequisites.Count -eq 4 -and @($prerequisites | Where-Object { $_.status -ne 'PASS' }).Count -eq 0)
        $primary = @($verdict.gates | Where-Object { $_.gate -eq 'crosscheck' })
        Check "${scenario}: missing required movement evidence cannot certify gameplay" (-not $verdict.pass -and $verdict.primary -eq 'crosscheck' -and $primary.Count -eq 1 -and $primary[0].status -eq 'FAIL')
    }
} finally {
    Remove-Item -LiteralPath $tmpDir -Recurse -Force
}
Write-Host ('contract fixtures: {0}/{1} checks passed - {2}' -f $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) {'FAIL'} else {'PASS'}))
exit $script:Fail
