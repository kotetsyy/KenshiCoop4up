# Build setup

## Prerequisites

- Windows x64; VC++ 2010 x64 compiler (v100), Windows SDK 7.1 and KB2519277.
- VS2022 Build Tools for MSBuild; `vswhere` resolves its path.
- Vendored `third_party/KenshiLib_deps`, Boost and the C89-patched ENet.
- `KENSHILIB_DIR` pointing to `third_party\KenshiLib_deps\KenshiLib` and
  `BOOST_INCLUDE_PATH` pointing to `third_party\KenshiLib_deps\boost_1_60_0`.

`scripts\v100_env.cmd` supplies PATH, INCLUDE, LIB and an absolute `MtToolPath`
from an installed SDK. Build commands fail if either DLL fails.

## Restore external dependencies

From a fresh checkout of the release tag, restore the dependency revisions used
for v0.1.22 through v0.1.25. Git LFS is required for KenshiLib libraries and the Boost archive:

```bat
git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps.git third_party/KenshiLib_deps
git -C third_party/KenshiLib_deps checkout b566d74bf3d74629cc2fb632a97595b8202993f1
git -C third_party/KenshiLib_deps lfs pull
tar -xf third_party/KenshiLib_deps/boost_1_60_0/boost.zip --directory third_party/KenshiLib_deps/boost_1_60_0
git clone https://github.com/lsalzman/enet.git third_party/enet/enet
git -C third_party/enet/enet checkout 5a9c537fd464b3c6d3c55e1d3bd47588faf71b42
git -C third_party/enet/enet apply ../patches/0001-enet-c89-for-loops.patch
git -C third_party/enet/enet apply ../patches/0002-enet-socket-hooks.patch
```

Update the pinned revisions when a release changes its dependencies. Vendored
libraries remain outside the mod ZIP; the patches and v100 compatibility
headers are tracked in the project repository.

## Build and deploy

```bat
scripts\build_plugin.cmd Release
scripts\build_plugin.cmd Harness
scripts\build_ui.cmd Harness
scripts\deploy.cmd "D:\SteamLibrary\steamapps\common\Kenshi" Harness
scripts\deploy.cmd "D:\SteamLibrary\steamapps\common\Kenshi" Harness ui
```

`build_plugin.cmd` builds both `src\plugin\x64\<Config>\KenshiCoop.dll` and
`src\ui\x64\<Config>\KenshiCoopUI.dll`. Default configuration is Harness;
Release excludes the scenario harness. `build_ui.cmd` builds only the UI;
`deploy.cmd ... ui` replaces only the UI. Keep the game closed while deploying.

The UI uses v100 `/MD` and the game's release STL layout, including in its Debug
configuration: it modifies engine-owned strings. Cross-DLL communication uses
only the packed, versioned POD C ABI in `src\ui\CoopUiApi.h`. RE_Kenshi loads
only the core; the core loads and pins the UI beside itself. No hot reload.

## Distribution and updates

`scripts\make_mod_kit.ps1 -SkipBuild` packages the Release pair into
`dist\KenshiCoop-kit.zip`, containing only the four runtime files under
`KenshiCoop/`: core DLL, UI DLL, mod and RE_Kenshi JSON. Packaged hashes are
verified against the build outputs, but no `PROVENANCE.json` is shipped.
Use `-Configuration Harness -SkipBuild` for `dist\KenshiCoop-dev-kit.zip`.
F2 creates `coop_config.json` when saving settings; updates do not replace it.
Instructions live in the release body. Do not upload `KenshiCoop-source.zip`
or any other dedicated source archive as a release asset: link the exact
release tag in the public repository instead. GitHub's automatic source
downloads are sufficient for the tracked project files; restore the external
dependencies below to build them. Keep tags tied to the source used for their
DLLs. Source availability required by AGPL is retained without adding another
player-facing download.
On this workstation, use `pwsh -NoProfile -File scripts/make_mod_kit.ps1 -SkipBuild`;
the legacy `powershell` invocation did not resolve `Get-FileHash`.

Publish both DLL assets together. `scripts\publish_release.ps1` emits the core
`sha256`/`url` plus the companion `uiSha256`/`uiUrl` in the update manifest.
An incomplete manifest is rejected; both payloads are verified before replacing
the installed pair. Changes take effect after restarting the game.

An older core-only updater needs the new companion too: manually install both
DLLs, or let the new core repair the missing UI on its next update check and
restart again. UI absence/ABI rejection is logged; it does not stop networking.

The current public Release pair uses protocol 70; the current Harness pair uses
private protocol 71. Both share the load-specific READY barrier and a 1 ms
maximum idle ENet wait; only the private pair mirrors the client's local log.
The packed `TimePacket` is 21 bytes in both configurations (`readyLoadId` is
always present). Earlier public 59/64/66/68 and private 63/65/67/69 pairs are rejected at handshake.
`InvItemEntry` is 165 bytes: stock snapshots also carry the native loose section
and two grid coordinates. Complete building stock restores the host's physical
stacks at those positions instead of attempting a different greedy packing.
`SpawnInfoPacket` is 211 bytes: a merchant description includes its shop home
and the actual host platoon template SID. Native `isATrader` reads that
template's `is trader` flag; the character SID and squad role alone are insufficient.

v0.1.25 targets 100 Hz entity snapshots with a 10 ms send interval on the existing
monotonic QPC clock, not the coarse `GetTickCount` clock. The sender retains its
snapshot buffer capacity between sends and preserves capture-time wire stamps.
Fresh captures remain game-frame-paced; interpolation adapts to those stamps
and ignores duplicate captures. The 50 ms distant-NPC slice selection and other
channel budgets are unchanged. Public 64/private 65 packet formats do not change.

The private launcher reads `dist/debug-kit/private-update.json` and synchronizes
against the latest closed GitHub release before starting Kenshi. The current
manual Harness pair is not published to that feed: starting its old launcher
would restore the previous remote pair. Public publication is separate.

### Manual installation of v0.1.27

`dist/KenshiCoop-dev-kit.zip` is a four-file manual update for existing private
installs, identical on HOST and JOIN. With both games closed, copy
`KenshiCoop/` into `<Kenshi>/mods/`, replacing the two DLLs, mod and RE_Kenshi
JSON while leaving `coop_config.json` intact. Launch through Steam or
`kenshi_x64.exe`, not the private launcher: the manual connection hotfix has
not been published to its update feed, so it would restore the previous release.

Save filesystem operations use Unicode Windows APIs while engine/wire paths
remain UTF-8. An unsuccessful host save or refused bootstrap transfer ends
the session instead of issuing an unreadable `LOAD_GO` or holding the host
for a READY that cannot arrive. The load/READY pause is now shared by Release
and Harness: READY is sent after the new world becomes live and must match the
host's current `LOAD_GO` id for each connected join. The temporary hold preserves
the selected speed/pause vote and requires save, load, speed and time sync.
Release/Harness core/UI builds succeeded; protocol checks passed 565/565 and
contract fixtures 29/29. All 11 paired native game regressions passed in disposable
installations on nonprimary DISPLAY1, never in the user's primary installation.
The launcher may exit after starting a new native game process; the batch run
waits on the actual game PIDs, not the bootstrap process.
The named-shop oracle compares complete native trade quantities and content
fingerprints on both peers; a nonempty but different shop still fails.
