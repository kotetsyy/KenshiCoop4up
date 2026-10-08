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
for v0.1.22 and v0.1.23. Git LFS is required for KenshiLib libraries and the Boost archive:

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

The private Harness kit uses protocol 63 and `dist/debug-kit/private-update.json`
with four SHA-256 entries, including both DLLs. The private launcher always
synchronizes against the latest closed GitHub release before starting Kenshi.
Replacing only the local kit without publishing its private manifest/assets
would reinstall the previous remote pair. Public publication is separate.

### Protocol 63 connection hotfix (2026-10-09)

`dist/KenshiCoop-dev-kit.zip` is a four-file manual update for existing private
installs, identical on HOST and JOIN. With both games closed, copy
`KenshiCoop/` into `<Kenshi>/mods/`, replacing the two DLLs, mod and RE_Kenshi
JSON while leaving `coop_config.json` intact. Launch through Steam or
`kenshi_x64.exe`, not the private launcher: the manual connection hotfix has
not been published to its update feed, so it would restore the previous release.

Save filesystem operations use Unicode Windows APIs while engine/wire paths
remain UTF-8. An unsuccessful host save or refused bootstrap transfer ends
the session instead of issuing an unreadable `LOAD_GO` or holding the host
for a READY that cannot arrive. Protocol 63 and the normal load/READY pause
are unchanged. Harness core/UI builds succeeded; the Unicode save regression
passed before the user's request to skip further tests. End-to-end connection
verification is left to the user; no two-account Steam check is claimed.
