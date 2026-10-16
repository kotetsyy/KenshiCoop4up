<#
.SYNOPSIS
  Zero-game verification gate: the fast, game-independent safety net every
  commit and every refactor phase must keep green. Wraps the C++ unit layer
  (prototest - wire contract, content hash, interpolation buffer, and the other
  pure units) and the PowerShell harness contract fixtures (manifest schema,
  scenario drift, oracle registry, verdict rule) into a single PASS/FAIL.

  Requires no Kenshi launch. Includes genuine separate-process provider/core
  consumers, actual updater disposable transactions and exact kit contracts.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1

.EXAMPLE
  # Reuse an already-built prototest (skip the compile step).
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    # Reuse the existing dist\prototest.exe instead of rebuilding it.
    [switch]$SkipBuild,
    [ValidateSet('Release','Harness')][string]$Configuration='Release',
    [string]$NativeBin='',
    [string]$CoreRunner=''
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$currentShell = (Get-Process -Id $PID).Path
if(-not $CoreRunner){$CoreRunner=Join-Path $repoRoot 'dist\core_native_tests.exe'}
. (Join-Path $PSScriptRoot 'NativeBuild.ps1') -Configuration $Configuration

$overall = $true

# ---- 1. C++ unit layer (prototest: wire/hash/interp + the pure units) ----------
Write-Host "############################################################"
Write-Host "# verify: C++ unit layer (prototest)"
Write-Host "############################################################"
$prototest = Join-Path $repoRoot "dist\prototest.exe"
if (-not $SkipBuild) {
    Write-Host "=== build prototest ==="
    & cmd.exe /c "`"$scriptDir\build_prototest.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "verify: FAIL (prototest build failed, exit $LASTEXITCODE)"; exit 1 }
}
$protocolBuild = Get-CoopTestBuild -Kind Protocol -Configuration Release
$prototest = $protocolBuild.Files['prototest.exe']
if (Test-Path $prototest) {
    & $prototest
    $unitOk = ($LASTEXITCODE -eq 0)
    Write-Host ("UNIT LAYER: " + $(if ($unitOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $unitOk) { $overall = $false }
} else {
    Write-Host "UNIT LAYER: FAIL - dist\prototest.exe not found (run without -SkipBuild)"
    $overall = $false
}

# ---- 2. PowerShell contract / drift fixtures (zero game) -----------------------
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: harness contract fixtures"
Write-Host "############################################################"
$fixtures = Join-Path $scriptDir "tests\Contract.Tests.ps1"
if (Test-Path $fixtures) {
    & $currentShell -NoProfile -ExecutionPolicy Bypass -File $fixtures
    $fixOk = ($LASTEXITCODE -eq 0)
    Write-Host ("CONTRACT FIXTURES: " + $(if ($fixOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $fixOk) { $overall = $false }
} else {
    Write-Host "CONTRACT FIXTURES: FAIL - scripts\tests\Contract.Tests.ps1 not found"
    $overall = $false
}

# ---- 3. Per-oracle fixtures on synthetic logs (zero game) ----------------------
# An oracle judged only by its own green runs is unfalsifiable: these feed each
# gate a log pair shaped like the BUG it exists for and require a FAIL, so a
# passing regression run means the gate looked rather than that it cannot look.
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: oracle fixtures (synthetic logs)"
Write-Host "############################################################"
$oracleOk = $true
foreach ($f in @("tests\CrawlMove.Fixture.ps1")) {
    $p = Join-Path $scriptDir $f
    if (-not (Test-Path $p)) {
        Write-Host "ORACLE FIXTURE: FAIL - scripts\$f not found"
        $oracleOk = $false
        continue
    }
    & $currentShell -NoProfile -ExecutionPolicy Bypass -File $p
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("ORACLE FIXTURE ${f}: FAIL (exit $LASTEXITCODE)")
        $oracleOk = $false
    }
}
Write-Host ("ORACLE FIXTURES: " + $(if ($oracleOk) { "PASS" } else { "FAIL" }))
if (-not $oracleOk) { $overall = $false }

# ---- 4. Primary-checkout behavioral safety boundaries (zero game) ---------------
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: audit integration boundaries"
Write-Host "############################################################"
$auditFixtures = Join-Path $scriptDir "tests\AuditIntegration.Tests.ps1"
$auditOk = $false
if (Test-Path -LiteralPath $auditFixtures -PathType Leaf) {
    & $currentShell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $auditFixtures
    $auditOk = ($LASTEXITCODE -eq 0)
} else {
    Write-Host "AUDIT FIXTURES: FAIL - scripts\tests\AuditIntegration.Tests.ps1 not found"
}
if (-not $auditOk) { $overall = $false }
# Actual updater implementation in disposable installs (not a PS reimplementation).
$additionalFailures = New-Object System.Collections.ArrayList
$updaterRunner=$protocolBuild.Files['updater_tests.exe']
if(-not(Test-Path -LiteralPath $updaterRunner)){throw 'Missing real-code updater_tests.exe; build_prototest.cmd required.'}
& $updaterRunner
if($LASTEXITCODE -ne 0){$overall=$false; [void]$additionalFailures.Add("updater_tests.exe (exit $LASTEXITCODE)")}
$frameRunner=$protocolBuild.Files['native_frame_tests.exe']
if(-not(Test-Path -LiteralPath $frameRunner)){throw 'Missing real native frame/value tests.'}
& $frameRunner
if($LASTEXITCODE -ne 0){$overall=$false; [void]$additionalFailures.Add("native_frame_tests.exe (exit $LASTEXITCODE)")}
foreach($name in @('npc_census_tests.exe','environment_items_state_tests.exe')){
    $runner=$protocolBuild.Files[$name]
    if(-not $runner -or -not(Test-Path -LiteralPath $runner)){throw "Missing receipted native test: $name"}
    & $runner
    if($LASTEXITCODE -ne 0){$overall=$false; [void]$additionalFailures.Add("$name (exit $LASTEXITCODE)")}
}
foreach($name in @('Distribution.Tests.ps1','Publisher.Tests.ps1','PrivateHarness.Tests.ps1','DependencyCache.Tests.ps1',
    'HarnessSafetyRegression.Tests.ps1','RuntimeRunnersRegression.Tests.ps1','SessionNetworkRegression.Tests.ps1',
    'NativeItemsRegression.Tests.ps1','BuildDistributionRegression.Tests.ps1','SaveTransactionsRegression.Tests.ps1',
    'DiagnosticToolsRegression.Tests.ps1','OracleRegression.Tests.ps1','ProceduralBedsRegression.Tests.ps1','SevenSyncNativeRegression.Tests.ps1')) {
    $fixture = Join-Path $scriptDir "tests\$name"
    if ($name -eq 'NativeItemsRegression.Tests.ps1') {
        # The real header witness needs the same compiler environment as the core,
        # not a cl.exe accidentally inherited from the caller's shell.
        & cmd.exe /d /c "call `"$scriptDir\v100_env.cmd`" && `"$currentShell`" -NoProfile -ExecutionPolicy Bypass -File `"$fixture`""
    } else {
        & $currentShell -NoProfile -ExecutionPolicy Bypass -File $fixture
    }
    if($LASTEXITCODE -ne 0){
        $overall=$false
        [void]$additionalFailures.Add("$name (exit $LASTEXITCODE)")
        Write-Host "BEHAVIORAL FIXTURE ${name}: FAIL (exit $LASTEXITCODE)"
    }
}

# Exact logical configuration/source/hash receipt, no installed/newest fallback.
$native=Get-NativeBuild -Configuration $Configuration -NativeBin $NativeBin
$nativeRunner=$native.Files['transport_integration.exe']
& $nativeRunner --controller --udp $native.Files['KenshiCoopNetUDP.dll'] --steam $native.Files['KenshiCoopNetSteam.dll']
if($LASTEXITCODE -ne 0){$overall=$false; [void]$additionalFailures.Add("transport_integration.exe (exit $LASTEXITCODE)")}
if(-not $SkipBuild) {
    & cmd.exe /c "`"$scriptDir\build_core_native_tests.cmd`" $Configuration"
    if($LASTEXITCODE -ne 0){throw 'Real NetLink core consumer build failed.'}
}
if(-not(Test-Path -LiteralPath $CoreRunner -PathType Leaf)){throw 'Genuine core native consumer runner missing.'}
$coreBuild = Get-CoopTestBuild -Kind CoreNative -Configuration $Configuration -Runner $CoreRunner
$CoreRunner = $coreBuild.Files['core_native_tests.exe']
& $CoreRunner --controller --udp $native.Files['KenshiCoopNetUDP.dll'] --steam $native.Files['KenshiCoopNetSteam.dll']
if($LASTEXITCODE -ne 0){$overall=$false; [void]$additionalFailures.Add("core_native_tests.exe (exit $LASTEXITCODE)")}

# ---- summary -------------------------------------------------------------------
Write-Host ""
Write-Host "================= VERIFY SUMMARY ================="
Write-Host ("  unit layer (prototest):   " + $(if ($unitOk) { "PASS" } else { "FAIL" }))
Write-Host ("  contract fixtures:        " + $(if ($fixOk)  { "PASS" } else { "FAIL" }))
Write-Host ("  oracle fixtures:          " + $(if ($oracleOk) { "PASS" } else { "FAIL" }))
Write-Host ("  audit integration:        " + $(if ($auditOk) { "PASS" } else { "FAIL" }))
Write-Host ("  additional regressions:   " + $(if ($additionalFailures.Count -eq 0) { "PASS" } else { "FAIL" }))
foreach($failure in $additionalFailures){Write-Host "    FAIL $failure"}
Write-Host ("OVERALL: " + $(if ($overall) { "PASS" } else { "FAIL" }))
if ($overall) { exit 0 } else { exit 1 }
