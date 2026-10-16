# Build setup

## Scope: native gameplay transport

This checkout targets **0.2.1**, Release protocol **82**, private
Harness protocol **83**. The selected real native provider carries gameplay:
Steamworks P2P or encrypted direct UDP, never ENet or an automatic fallback.
Migration from v0.2.0 requires manual installation of all six runtime files;
its updater rejects ABI3. Publication does not certify two-account Steam:
**real two-account Steam P2P remains unverified**.
Provider tests/interface acquisition do not establish gameplay or Internet reachability.

## Prerequisites

- Windows x64; VC++2010 x64/v100, Windows SDK7.1 and KB2519277 for engine core/UI.
- VS2022 Build Tools, v143 x64 and Windows SDK10 for the isolated providers/tools.
- CMake >=3.24, Git, `tar.exe`, PowerShell, network access to pinned archives.
  If CMake is absent, `build_transports.ps1` can use installed `uv`/CMake4.4.4.
- Git LFS and the external KenshiLib/Boost dependencies below.
- `KENSHILIB_DIR` = `third_party\KenshiLib_deps\KenshiLib` and
  `BOOST_INCLUDE_PATH` = `third_party\KenshiLib_deps\boost_1_60_0`.

VS2022 orchestrates MSBuild, not the engine C++ ABI. Core/UI remain v100 with
the game-compatible release STL/CRT layout. No STL, exceptions, SDK objects or
cross-CRT frees cross `CoopUiApi.h` (API4) or `TransportApi.h` (ABI3).
The providers use v143 **static CRT**, with pinned static GNS/protobuf contained
in the UDP DLL. No modern runtime/dependency DLL accompanies the runtime kit.

## Restore external engine dependencies

Check out the exact source revision you intend to build; never dependency `latest`.

```bat
git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps.git third_party/KenshiLib_deps
git -C third_party/KenshiLib_deps checkout b566d74bf3d74629cc2fb632a97595b8202993f1
git -C third_party/KenshiLib_deps apply ../vc10_compat/kenshilib-v100.patch
git -C third_party/KenshiLib_deps lfs pull
tar -xf third_party/KenshiLib_deps/boost_1_60_0/boost.zip --directory third_party/KenshiLib_deps/boost_1_60_0
```

The tracked v100 compatibility patch guards the shared enum and supplies a
`CraftingItem` placeholder for a by-value container declaration; it is **not**
a recovered native crafting ABI. Do not construct/access native crafting objects
using that placeholder. ENet/tunnel build sources are no longer used.

## Exact native provider build and receipts

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_transports.ps1 -Configuration Release
# Optional clean source build outside the checkout:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_transports.ps1 -Configuration Harness -BuildRoot C:\CoopNativeClean
```

The build clears inherited v100/compiler SDK variables and selects the VS2022
v143 x64 generator explicitly. `build_plugin.cmd` runs this independent modern
build **before** entering `v100_env.cmd`. Optional `-CMake` selects an explicit
executable; `-Jobs` is 1..64. Default root is
`%LOCALAPPDATA%\KenshiCoop\native-build`. Outputs are exactly
`providers-v143-<checkout-path-id>-<Release|Harness>\bin`; CMake's physical Release
configuration is separate from the recorded **logical** configuration.
Debug core/UI uses native Harness, not a Debug provider CRT.

`NativeBuild.ps1` explicitly imports this engine's Utility module. A Windows
PowerShell 5.1 child of PowerShell 7 otherwise inherits a module search path that
can select incompatible PS7 hashing/JSON commands and prevent real native builds.

`src/transport/dependencies.json` pins:

| Input | Revision | Archive SHA-256 |
|---|---|---|
| GameNetworkingSockets | 1.6.1 / `d534c19aa760df3fb75fd20db13ba1932b8a5463` | `099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab` |
| protobuf | 21.12 / `f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c` | `d594b561fb41bf243233d8f411c7f2b7d913e5c9c1be4ca439baf7e48384c893` |

Archive hashes verify before extraction. GNS uses the original pinned archive,
without the former authenticated-IP patch. Original source trees use a separate
`-original` cache namespace; older patched caches are preserved, never silently
reused or repaired. Cached dependencies are private build inputs, not ZIP extras.

The output contains two providers, the sanctioned Steam probe and real
`transport_integration.exe`; no credential tool is built. A `native-build.json`
receipt records checkout, logical configuration, source digest, pins, ABI and
hashes of providers/controller.
`NativeBuild.ps1` resolves only that canonical output or an explicit `-NativeBin`;
it rejects missing/mismatched receipts, changed sources and changed DLLs.
It never picks the newest directory or an installed DLL. Receipts are not shipped.

## Core/UI build without touching the game

```bat
scripts\build_plugin.cmd Release
scripts\build_plugin.cmd Harness
scripts\build_plugin.cmd Debug
```

The core and UI outputs are canonical `src\plugin\x64\<Config>\KenshiCoop.dll`
and `src\ui\x64\<Config>\KenshiCoopUI.dll`. A source snapshot taken before
compilation must still match afterward; `dist\build-receipts\core-<Config>.json`
records source/configuration and both hashes. Kits/deploy/publication reject stale
or independently replaced outputs. Release excludes the scenario runner;
Harness/Debug keep it and forcibly disable the updater in `start`, regardless
of config or caller-provided settings. `build_ui.cmd` remains an iteration tool,
not a way to create an installable matching runtime. No UI-only deploy exists.

RE_Kenshi loads **only** KenshiCoop.dll. Core loads/pins UI and dynamically loads
only the selected transport beside itself. Steam borrows the game's already
initialized `steam_api64.dll`; it neither loads/replaces/initializes/shuts down
another Steam context nor substitutes UDP if interface/authentication fails.
Steam callback dispatch runs on the main game thread: the game's
`SteamAPI_RunCallbacks` serves lobby callbacks, followed by an explicit sockets
`RunCallbacks` drain. The legacy Steam API did not deliver callbacks configured on
the borrowed `SteamNetworkingSockets012` interface in the actual game probe;
native timeout state alone must not be mistaken for a delivered disconnect.
Provider startup and teardown are fenced against the mod's pump.
Direct UDP never loads Steam, uses no Steam IDs/relay/NAT service, and requires a
reachable numeric IPv4/IPv6 host endpoint, firewall allowance and any necessary
router port forwarding.

## Connections and F2

Steam HOST shares its own SteamID64; no guest ID list or Allow command is required.
Up to three guests may connect using authenticated, encrypted Steam identities.
JOIN still verifies the expected host identity. Native connection is separate
from the gameplay handshake and load-specific READY barrier.

UDP HOST selects its listening port; JOIN enters a numeric IPv4/IPv6 endpoint.
No private profile, invitation, credential file, password or allow-list is used.
Removed controls and request/state APIs are not retained as compatibility shims.
GNS requires encryption, but UDP identities are unauthenticated: encryption does
not establish a trusted host identity or protect against an active intermediary.
Anyone who can reach the listening port can join an available seat. Internet
UDP still needs firewall/router reachability; it does not bypass CGNAT.

Install the complete matching six-file kit on every peer with Kenshi closed.
Native ABI3/UI API4 and protocol82/83 cannot be mixed with v0.2.0 DLLs.
The ABI2 updater in v0.2.0 rejects an ABI3 manifest, so this transition requires
manual full-kit installation rather than an automatic in-place ABI migration.
Old private files are neither required nor deleted; config and saves are preserved.

## Unified game-independent verification

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Configuration Release -NativeBin C:\exact\bin
# Reuse artifacts only after their integrated build:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -SkipBuild -Configuration Harness -NativeBin C:\exact\Harness\bin -CoreRunner C:\exact\core_native_tests.exe
```

The gate retains wire/save/interpolation units, scenario/contract drift fixtures
and oracle fixtures. It additionally runs actual C++ updater parser/transaction
code in disposable installs, production six-file ZIP/publisher/preflight tests,
the real separate-process native controller, and the real separate-process
NetLink consumer (`core_native_tests.exe --controller --udp ... --steam ...`).
Missing native/core runners fail; no mock replaces them. `build_prototest.cmd`
builds `prototest.exe`, `updater_tests.exe` and actual `native_frame_tests.exe`; the parent-owned
`build_core_native_tests.cmd` builds the core consumer. Verification does not
launch Kenshi. No historical test count is proof of this checkout's result.

Steam inspect/host/join remains a separate installed-game probe:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_steam_probe.ps1 -ProbeDll C:\exact\bin\KenshiCoopSteamProbe.dll -GameRoot D:\Kenshi -Role inspect
# Different real users, concurrently on their own machines:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_steam_probe.ps1 -ProbeDll C:\exact\bin\KenshiCoopSteamProbe.dll -GameRoot D:\Kenshi -Role host -Seconds 145
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_steam_probe.ps1 -ProbeDll C:\exact\bin\KenshiCoopSteamProbe.dll -GameRoot D:\Kenshi -Role join -PeerSteamId '<real-host-SteamID64>' -Seconds 145
```

Exercise actual Steam allocated-message ownership without another account:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_steam_probe.ps1 -ProbeDll C:\exact\bin\KenshiCoopSteamProbe.dll -GameRoot D:\Kenshi -Role loopback -Seconds 45
```

This uses the borrowed native Steam library's network-loopback socket pair,
not the standalone UDP provider. It calls the production sender with delayed
delivery in both directions on all three lanes, compares exact payload bytes
and lane IDs, rejects duplicates, and checks recovery after an invalid send.
The runner requires the current PID's fresh `LOOPBACK PASS` record; a stale
appended log or merely surviving the timer cannot pass. It still refuses an
existing game and restores the original loader/probe/display configuration.
This does not exercise Steam P2P admission, remote routing or reconnect and is
not certification of a distinct-account HOST/JOIN session.

Only actual distinct-account payload/identity/route evidence certifies Steam
P2P. Interface acquisition and relay availability alone do not. Visible game
checks must fail closed without the non-primary monitor; no primary fallback.
Preserve user config/save/display, restore loader changes and terminate only
owned process trees. Reported trade/merchant/time/bed/combat issues are not
claimed fixed by transport or build checks.

### Procedural Hub beds: native reconciliation

Only the verified Hub region `(20,32)` and Camp Bed template are enabled.
Native generation is not suppressed. Birth scopes and deferred native work
record positive provenance; unknown provenance blocks reconciliation instead
of authorizing deletion. Other categories and regions remain native-only.

The reliable gameplay lane carries complete region/epoch snapshots with an
explicit end marker. JOIN acknowledges only after a fresh native census equals
HOST, including full quaternion, floor, parents, ownership and native state.
`WORLD_READY` also requires current **enabled** region membership and these
acknowledgements, followed by a successfully queued reliable scope commit.
A complete native registry with no enabled region uses an explicit zero-count
commit, after any pending retirements. Unknown or incomplete native registries
cannot certify an empty scope. JOIN never waits for snapshots of native-only
regions which HOST has not loaded.
Unreconciled procedural beds reject native interaction. Removal requires fresh
positive procedural provenance and native occupancy/AI/reference safety checks.
No raw network handle or approximate nearest-object lookup authorizes removal.

`procedural_beds` exercises the basic native vertical slice; its first successful
local run is `tools/test-runs/procedural-beds-native-20261015-3`. It matched
20 procedural beds per peer and actual native sleep occupancy. That result does
not certify reconnect, unload/reload, player construction or arbitrary AI/nav
behavior. `procedural_beds_lifecycle` is the separate extended native check;
game-independent oracles cannot substitute for its actual engine evidence.

The full native census also records protected saved/player-built beds by their
complete native handles, separately from procedural IDs. Compare their raw
native states: existing saved beds can have construction progress `5`; it is
not a normalized fraction and must not be clipped to `1`. The first extended
run matched 18 procedural plus 6 protected beds per peer, but correctly failed
because its movement scaffold never proved native region unload. A stale bed
task immediately after native release is not occupied-bed position evidence.

The extended native run
`tools/test-runs/procedural-beds-lifecycle-native-20261015-3` passed on two owned
Steam 1.0.65 installations on the secondary monitor. Before reconciliation JOIN
had 24 procedural beds versus HOST's 23; native removal and creation completed
before ACK, scope commit and `WORLD_READY`. Both complete current sets matched.
Actual native unload/reload changed HOST's layout to 19 procedural beds in epoch
2; JOIN adopted that complete set, rather than retaining a fixed expected count.
After a real disconnect and forced streamed-save reconnect, both peers retained
the same 19 logical IDs and 6 protected native handles/descriptors. Distinct HOST
and JOIN characters then occupied logical bed `24` in turn, as observed in both
native processes. Native path orders, occupancy, player-built ownership and
engine-integrity gates passed; no global AI/navigation guarantee is implied.

Full native XYZ, type, floor, parent and gameplay-state fields matched; quaternion
components differed by at most `1.19e-7` after native normalization, below the
canonical comparison precision. Protected saved/player-built states were
unchanged. Screenshot timing reported `captureOk=false`; PNGs exist, but HOST's
camera was occluded, so they are not full visual layout proof. Loopback evidence
does not certify WAN, separate-account Steam P2P, GOG or other procedural categories.

Run owned, separate game installations on the non-primary monitor with the same
exact Harness build and independent save roots. Force save streaming when
testing bootstrap; a copied fixture is not proof of network transfer. Do not
publish these local builds or replace the stable release feed.


## Local audit integration checks

Run `scripts/tests/AuditIntegration.Tests.ps1` with both Windows PowerShell 5.1
and PowerShell 7. It exercises environment cleanup, scenario precedence,
byte-preserving configuration changes, save transactions and failed restoration
without launching Kenshi or changing system settings. Free play removes
diagnostic overrides; only scripted entries pin `CELL_AUTH=0`. An absent
environment variable must be passed as `[NullString]::Value`: PowerShell `$null`
leaves an empty variable in the .NET setter under PowerShell 7.

`scripts/verify.ps1 -SkipBuild` also runs the crawl and audit integration fixtures.
Build current native/protocol/core consumer artifacts first. Run the two shells
sequentially because they share build outputs. Crawl coordinates use invariant
formatting, independently of the Windows locale. These checks do not certify
floor-item identity, NPC poses, engine pause or disconnect recovery in Kenshi.

Source receipts use ordinal path ordering: Windows PowerShell's NLS collation
and PowerShell 7's ICU collation otherwise hash the same SDK files in different
orders. `NativeBuild.ps1` also puts the running engine's built-in modules first
for child processes while retaining external module paths. This prevents a
Windows PS5.1 child of `pwsh` from binding incompatible PS7 Utility modules.
`BuildDistributionRegression.Tests.ps1` exercises both real engines against the
same consumed-input tree and rejects stale receipts after a header change.
`kit_preflight.ps1` applies the same engine-local builtin resolution before
standalone harness consumers run, not only after native-build helpers load.
This also covers `HarnessSafetyRegression.Tests.ps1` launched directly by a
PS7 parent: Get-FileHash and native input boundary compilation run in PS5.1.
The native item witness is compiled in `verify.ps1` through `v100_env.cmd`; it
does not require a caller-preconfigured `cl.exe` in PATH.

`Clear-CoopRunEnvironment` treats an absent or empty preservation list as
preserve-nothing. Explicit scenario keys restore only their captured values;
stale credentials do not survive implicitly. Both engines exercise this against
the real process environment before any game launch.

Physical W1/W2 oracles consume the emitted full before/after item handles,
recipient container, item type, occurrence ID and simultaneous bag/ground
quantities. Host seat `0` is valid; a missing occurrence ID is not. A successful
parser fixture does not certify the native item transition in Kenshi.
Floor screenshot anchors use `WPMIRROR`/`WGRP` startup records instead of
unrelated movement markers, so captures happen while both owned games are live.
For a run without a named scenario, `run_test.ps1` starts its screenshot clock
at the first observed HOST gameplay entry, before launching JOIN. Time consumed
by the staggered JOIN startup is deducted rather than slept a second time.

`world_pickup_mirror` seeds its material through the registered world-object
factory, then moves that exact object into the source bag before measuring the
baseline. Detached inventory fabrication produces a `4,0,0,0,0` handle and cannot
prove a physical transition; do not bypass handle resolution to make it pass.
The real owned HOST/JOIN run covers the full spawn/pickup/claim transaction,
not initial-world parity or simultaneous pickup arbitration.
`speed_pause_ui` defaults to the tracked `squad1` fixture, not an installation's
untracked `autosave0`. Its observer requires four actual host/client UI or
keyboard transitions and eight-second engine-clock holds; direct speed setters
cannot satisfy the scenario.
The observer first requires one uninterrupted second in the requested state:
local UI prediction may briefly revert before the host's reply. Only this settle
period restarts; a relapse during the subsequent strict eight-second hold is
still a failure, and the 45-second phase deadline is unchanged. MainBar's
`setGameSpeed(0, true)` is captured as a pause command while preserving the
previous speed tier; automatic/guarded zero writes do not create user votes.
The actual owned HOST/JOIN run passed all four transitions (eight peer holds),
with zero clock advance during both pause phases. This does not certify NPC
animation or all time-speed combinations.


During the STOP audit, the primary checkout at
`C:\Users\kotetsy\source\KenshiCoop` rebuilt v100 core/UI and v143 providers;
current checkout/configuration/source/output receipts validated in PS5.1 and PS7.
Both sequential `verify.ps1 -SkipBuild -Configuration Harness` runs passed:
543 protocol checks, production updater transactions, native frame checks,
filesystem/harness/oracle regressions, the real UDP controller and the real
NetLink host-plus-three consumer. The latter exercised READY load tokens, pause
heartbeat precedence, exact reliable/bulk delivery, world/session replay fences
and reconnect. These primary-build/consumer results alone do **not** prove
that floor items, NPC poses, engine pause or four Kenshi worlds agree. The
separate real-UI pause proof is described above.

The current protocol must be checked on both participants; old proto73 archives
are not current artifacts. To exercise the bounded menu/provider smoke with the
user's game closed, use the freshly built exact six-file Harness kit:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_native_smoke.ps1 -Kit C:\Downloads\KenshiCoop-dev-kit.zip -GameRoot D:\Kenshi -NativeBin C:\exact\Harness\bin -AutoConnect -Seconds 120
```

This uses the installed sanctioned loader, a private host profile and the shared
owned-process/non-primary-monitor guard. It does not select a user's save or
certify Internet reachability. In-world HOST/JOIN and two-account Steam P2P
require separate observed verification.


## Exact six-file distribution and safe deploy

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/make_mod_kit.ps1 -SkipBuild -Configuration Release -NativeBin C:\exact\bin
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/make_mod_kit.ps1 -SkipBuild -Configuration Harness -NativeBin C:\exact\Harness\bin -OutputZip C:\Downloads\KenshiCoop-local.zip
```

The ZIP has exactly `KenshiCoop/` entries: **KenshiCoop.dll, KenshiCoopUI.dll,
KenshiCoopNetSteam.dll, KenshiCoopNetUDP.dll, KenshiCoop.mod, RE_Kenshi.json**.
JSON `Plugins` lists only the core. Canonical core/UI and native receipts must
match current source/configuration; all ZIP hashes match those inputs. No
config, secret, launcher, provenance, source, probe, test or document is included.

```bat
scripts\deploy.cmd "D:\explicit\Kenshi" Harness nomirror "C:\exact\Harness\bin"
```

Deploy requires an explicit installation and defaults to **no mirror**. `mirror`
in the third argument explicitly adds `%USERPROFILE%\Kenshi-Join`; the fourth
argument supplies the exact native bin. The helper preflights all six inputs
and every destination before copying, propagates copy/hash failures, and never
copies/removes user config, saves or credentials. Keep the games closed.
The initial migration must be installed as the full kit, not through an old updater.

## Four-component updater and non-mutating publisher

Schema2 requires `transportAbi=3`, `version`, and all four
`coreSha256/coreUrl`, `uiSha256/uiUrl`, `steamSha256/steamUrl`,
`udpSha256/udpUrl` pairs. **No legacy sha256/url fields**: the published old
parser rejects before fetching/replacing the core. Hashes are lowercase SHA-256;
URLs are exactly the fixed repository/release/version/component asset paths.
Duplicate/unknown fields, malformed versions, hashes and URLs reject before
payload fetch or disk mutation. Same-version comparison checks all four images.

All four payloads validate before mutation; each stage write, flush and on-disk
hash must succeed before the durable recovery journal. Backup and promotion
failures check reverse rollback. Incomplete rollback preserves journal/backups
and cannot report success. A verified committed marker allows next-launch
cleanup only after all four live hashes match; incomplete transactions recover
before companions load. Recovery changing the currently mapped core blocks
startup until restart. Private Harness/Debug never sweeps public recovery files;
it fails closed without mutation when any are pending. Orphaned/corrupt recovery
data is preserved, never blindly deleted. Config/save/profile files are outside
the transaction.

Runtime application also owns the shared `RuntimeImages.h` lifecycle barrier:
the first UI attempt must settle before replacement, UI/provider loaders cannot
overlap renames, and new companion loads/session starts are refused after apply
until Kenshi restarts. Intentionally omitted/failed UI attempts settle too.
Complete verified rollback reopens loading; pending recovery never does. This
is separate from disk crash recovery and keeps a mapped old core from loading
new companions during the current process.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/publish_release.ps1 -NativeBin C:\exact\bin -Local
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/publish_release.ps1 -NativeBin C:\exact\bin -WhatIf
```

Both modes and the default are read-only. The helper validates source receipts
and exact six-file kit hashes, prints schema2 and **unexecuted** five-asset upload
commands (kit plus four DLLs), and never uploads/tags/pushes. Explicit
`-WriteManifest -Out <new-non-stable-file>` can create a new local candidate;
`-Local`/`-WhatIf` still suppress it. Stable `dist/UPDATE.txt` and notes cannot be
written by the publisher. Publication requires explicit maintainer authorization.
After authorization, publish the matching source/tag and complete five-asset
release, verify downloaded hashes, then advance public `main` and its schema2
feed with a normal fast-forward. Never point the stable feed at missing assets
or merge the divergent private debug history over public history.

## Source and license obligations

Preserve nhoral's original authorship and AGPL-3.0; KenshiLib/RE_Kenshi GPL,
Boost, Valve GameNetworkingSockets BSD-3-Clause, protobuf BSD and all applicable
linked dependency notices. The release source revision and pinned fetch/patch
instructions are part of corresponding-source availability; do not assume a
precompiled bundle or automatic project source ZIP alone satisfies obligations.
Do not attach a separate player-facing source ZIP or put notices/source extras
inside the exact six-file runtime kit. Make applicable notices and sources
available with the source/release documentation instead.
