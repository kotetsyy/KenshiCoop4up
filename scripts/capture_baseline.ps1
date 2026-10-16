<#
.SYNOPSIS
  Capture the Phase 0 refactor baseline: the frozen contract facts every later
  phase diffs against. Snapshots the game-INDEPENDENT half deterministically
  (protocol version, packet-size inventory, canonical DLL SHA-256, manifest hash,
  config source hash, commit/dirty), and records WHERE the game-DEPENDENT half
  (the two-client regression verdicts) is stored so the two together form the
  baseline artifact set the plan requires.

.DESCRIPTION
  Writes tools\baseline\baseline_<timestamp>.json and refreshes
  tools\baseline\latest.json. Run once at the start of the refactor (and re-run
  only when a delta is intentional and documented).

  The game-dependent verdicts are NOT launched here (they need Kenshi + Steam +
  the shared saves). Capture them separately and they land under
  tools\test-runs\ (regress.ps1 writes verdict.json per run + history.jsonl):

    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full
    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -Variants clean,wan
    # focused: save_sync, load_sync, latejoin_sync, rejoin_items, camp_approach
    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Only save_sync,load_sync,latejoin_sync

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\capture_baseline.ps1
#>
[CmdletBinding()]
param(
    # Path to the canonical Release DLL to hash (defaults to the build output).
    [string]$Dll = "",
    [ValidateSet('Harness','Release')][string]$Configuration = 'Harness',
    [string]$NativeBin = ""
)

function Get-CoopBaselineProtocol {
    param([string]$Source, [ValidateSet('Harness','Release')][string]$BuildConfiguration)
    $branch=[regex]::Match($Source,'(?s)#ifdef\s+KENSHICOOP_NET_DIAG\b(?<private>.*?)#else(?<public>.*?)#endif')
    if (-not $branch.Success) { throw 'Cannot identify Wire.h conditional protocol contract.' }
    $selected=if ($BuildConfiguration -eq 'Harness') { $branch.Groups['private'].Value } else { $branch.Groups['public'].Value }
    $value=[regex]::Matches($selected,'PROTOCOL_VERSION\s*=\s*(\d+)')
    if ($value.Count -ne 1) { throw 'Missing or ambiguous selected wire protocol.' }
    return [int]$value[0].Groups[1].Value
}
function Read-CoopBaselineProtoOutput {
    param([string[]]$Lines, [string[]]$RequiredTypes)
    $sizes=@{}; $protocol=$null; $hello=$null; $hash=$null; $terminal=$false
    $nonempty=@($Lines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if (-not $nonempty.Count -or $nonempty[-1] -notmatch '^prototest: ([1-9][0-9]*)/([1-9][0-9]*) checks passed - PASS$') { throw 'Missing terminal native success record.' }
    foreach ($line in $Lines) {
        if ($line -match '\bFAIL\b') { throw 'Native prototest reported failure.' }
        if ($line -match 'ok\s+sizeof\(([A-Za-z0-9_]+)\)\s+\(=\s*(\d+)\)') { $sizes[$Matches[1]]=[int]$Matches[2] }
        if ($line -match '^prototest: .* \(protocol v(\d+)\)$') { $protocol=[int]$Matches[1] }
        if ($line -match 'ok\s+HELLO version field offset\s+\(=\s*(\d+)\)') { $hello=[int]$Matches[1] }
        if ($line -match 'canonical invEntryHash.*=\s*(\d+)\s*$') { $hash=[long]$Matches[1] }
        if ($line -match '^prototest: ([1-9][0-9]*)/([1-9][0-9]*) checks passed - PASS$') { $terminal=$Matches[1] -eq $Matches[2] }
    }
    if (-not $terminal -or $null -eq $protocol -or $hello -ne $protocol -or $null -eq $hash -or -not $RequiredTypes.Count) { throw 'Incomplete native prototest output.' }
    foreach ($type in $RequiredTypes) { if (-not $sizes.ContainsKey($type)) { throw "Missing native wire-size evidence: $type" } }
    return [pscustomobject]@{Sizes=$sizes;Protocol=$protocol;Hash=$hash}
}
if ($MyInvocation.InvocationName -eq '.') { return }

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
. (Join-Path $scriptDir 'NativeBuild.ps1') -Configuration $Configuration
$native = Get-NativeBuild -Configuration $Configuration -NativeBin $NativeBin
$core = Get-CoreBuild -Configuration $Configuration

function Get-Sha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required baseline input missing: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# ---- DLL hashes ---------------------------------------------------------------
# Phase 1 build separation: the regression pipeline runs the Harness DLL (with
# the scenario runner); the shipped canonical is the Release DLL. Record whichever
# the caller points at as the primary $dllHash (default: the test/Harness DLL that
# the baseline verdicts actually ran against), plus the shipped Release hash when
# a Release build is present, so both are diffable later.
if ($Dll -eq "") { $Dll=$core.Files['KenshiCoop.dll'] }
$Dll=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Dll)
$dllHash=Get-Sha256 -Path $Dll
if ($dllHash.ToLowerInvariant() -cne $core.Receipt.files.'KenshiCoop.dll') { throw 'Selected DLL does not match the selected core configuration/source/hash receipt.' }
$releaseDllPath=Join-Path $repoRoot 'src\plugin\x64\Release\KenshiCoop.dll'
$shippedDllHash=''
# Optional Release bytes are explicitly unvalidated observations, not selected-build proof.
if (Test-Path -LiteralPath $releaseDllPath -PathType Leaf) { $shippedDllHash=Get-Sha256 $releaseDllPath }

# ---- protocol version (from Wire.h) -------------------------------------------
$wire = Join-Path $repoRoot "src\netproto\Wire.h"
$wireSource=Get-Content -LiteralPath $wire -Raw
$proto=Get-CoopBaselineProtocol $wireSource $Configuration

# ---- packet-size inventory + canonical hashes (from prototest) ----------------
$unitBuild=Get-CoopTestBuild -Kind Protocol -Configuration Release
$prototest=$unitBuild.Files['prototest.exe']
$unitSource=Join-Path $repoRoot 'src\prototest\main.cpp'
$requiredTypes=@([regex]::Matches((Get-Content -LiteralPath $unitSource -Raw),'CHECK_EQ\("sizeof\(([A-Za-z0-9_]+)\)"') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -ne 'DebugLogFileHeader' } | Sort-Object -Unique)
$out = & $prototest 2>&1
$unitExit=$LASTEXITCODE
if ($unitExit -ne 0) { throw "Native prototest failed (exit $unitExit); baseline not changed." }
$facts=Read-CoopBaselineProtoOutput -Lines @($out | ForEach-Object { "$_" }) -RequiredTypes $requiredTypes
$sizes=$facts.Sizes; $protoFromTest=$facts.Protocol; $invEntryHash=$facts.Hash
$standaloneProtocol=Get-CoopBaselineProtocol $wireSource Release
if ($protoFromTest -ne $standaloneProtocol) { throw 'Standalone public prototest protocol disagrees with Wire.h Release branch.' }

# ---- manifest + config source hashes ------------------------------------------
$manifestHash = Get-Sha256 -Path (Join-Path $repoRoot "scripts\scenarios.psd1")
$configHash = @{
    "Config.cpp" = Get-Sha256 -Path (Join-Path $repoRoot "src\plugin\core\Config.cpp")
    "Config.h"   = Get-Sha256 -Path (Join-Path $repoRoot "src\plugin\core\Config.h")
}

# ---- commit provenance --------------------------------------------------------
$commit = ""; $dirty = $true
try { $commit = (& git -C $repoRoot rev-parse HEAD).Trim() } catch {}
try { $dirty = ((& git -C $repoRoot status --porcelain) | Measure-Object).Count -gt 0 } catch {}

# ---- assemble + write ---------------------------------------------------------
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$baseline = [pscustomobject]@{
    capturedAt      = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    commit          = $commit
    dirty           = $dirty
    protocolVersion = $proto
    protocolFromTest = $protoFromTest
    prototest = @{ path=$prototest; sha256=(Get-Sha256 $prototest); configuration='Release'; protocol=$protoFromTest; exitCode=$unitExit; receipt=$unitBuild.Receipt }
    coreReceipt = $core.Receipt
    packetSizes     = $sizes
    invEntryHash    = $invEntryHash
    canonicalDll    = @{ path = $Dll; sha256 = $dllHash }
    shippedDll      = @{ path = $releaseDllPath; sha256 = $shippedDllHash; receiptValidated = $false }
    nativeConfiguration = $Configuration
    nativeProviderHashes = @{
        steam = Get-Sha256 $native.Files['KenshiCoopNetSteam.dll']
        udp = Get-Sha256 $native.Files['KenshiCoopNetUDP.dll']
    }
    manifestSha256  = $manifestHash
    configSha256    = $configHash
    gameVerdicts    = @{
        note   = "Two-client verdicts are NOT captured here (need the game). Run regress.ps1; they land under tools\test-runs\ (verdict.json per run + history.jsonl)."
        source = "tools\test-runs\history.jsonl"
    }
}

$outDir = Join-Path $repoRoot "tools\baseline"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir ("baseline_${stamp}_"+[guid]::NewGuid().ToString('N')+'.json')
$latest  = Join-Path $outDir "latest.json"
$json = $baseline | ConvertTo-Json -Depth 6
$temporary=Join-Path $outDir ('.baseline-'+[guid]::NewGuid().ToString('N')+'.json')
try {
    [IO.File]::WriteAllText($temporary,$json,[Text.UTF8Encoding]::new($false))
    [IO.File]::Copy($temporary,$outFile,$false)
    if (Test-Path -LiteralPath $latest) { [IO.File]::Replace($temporary,$latest,$null) }
    else { [IO.File]::Move($temporary,$latest) }
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }

Write-Host "=== Phase 0 baseline captured ==="
Write-Host ("  protocol version : {0}" -f $proto)
Write-Host ("  packet types     : {0}" -f $sizes.Keys.Count)
Write-Host ("  canonical DLL    : {0}" -f $(if ($dllHash) { $dllHash.Substring(0, 16) + "..." } else { "MISSING (build first)" }))
Write-Host ("  manifest sha256  : {0}" -f $(if ($manifestHash) { $manifestHash.Substring(0, 16) + "..." } else { "n/a" }))
Write-Host ("  commit / dirty   : {0} / {1}" -f $(if ($commit) { $commit.Substring(0, 10) } else { "?" }), $dirty)
Write-Host ("  written          : {0}" -f $outFile)
Write-Host ("  latest           : {0}" -f $latest)
Write-Host ""
Write-Host "Next: capture the two-client verdicts with scripts\regress.ps1 (see the script header)."
