@echo off
REM Create a fully independent second Kenshi install for the co-op JOIN client so
REM it can't share saves/config/logs with the HOST instance on the same machine.
REM
REM Usage:
REM   scripts\setup_join_install.cmd ["C:\path\to\source Kenshi"] ["C:\path\to\Kenshi-Join"]
REM
REM Defaults:
REM   source = Steam install
REM   dest   = %USERPROFILE%\Kenshi-Join   (user-writable; no elevation needed)
REM
REM Safe to re-run: it re-syncs from the source but PRESERVES the join copy's own
REM save/ and settings so your second client keeps its independent state.
setlocal EnableDelayedExpansion

set "SRC=%~1"
if "%SRC%"=="" set "SRC=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

set "DST=%~2"
if "%DST%"=="" set "DST=%USERPROFILE%\Kenshi-Join"

if not exist "%SRC%\kenshi_x64.exe" (
    echo ERROR: source Kenshi not found at "%SRC%".
    echo        Pass the real install path as the first argument.
    exit /b 1
)

REM Preflight matched complete runtime before copying anything into the destination.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '%~dp0NativeBuild.ps1'; . '%~dp0kit_preflight.ps1'; if(Get-Process kenshi_x64 -ErrorAction SilentlyContinue){throw 'Close user-owned Kenshi sessions first'}; $src=[IO.Path]::GetFullPath($env:SRC).TrimEnd('\'); $dst=[IO.Path]::GetFullPath($env:DST).TrimEnd('\'); if($src -ieq $dst -or $dst.StartsWith($src+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Join destination must be separate from source'}; [void](Get-CoopHarnessRuntime -RepoRoot ([IO.Path]::GetFullPath('%~dp0..')) -NativeBin $env:KENSHICOOP_NATIVE_BIN)"
if errorlevel 1 exit /b 1

echo === Creating JOIN install ===
echo   source: %SRC%
echo   dest:   %DST%
echo.
echo This copies the full game (several GB) the first time. Subsequent runs only
echo sync changed game files and leave the join client's save/settings alone.
echo.

REM /E copy subdirs incl. empty. /XO skip older (don't clobber newer join-side
REM files). Exclude the mutable, per-instance state so the join client keeps its
REM own world/config/logs independent of the host after the first copy.
robocopy "%SRC%" "%DST%" /E /XO /R:1 /W:1 /NFL /NDL /NP /NJH /NJS ^
    /XD "%SRC%\save" "%SRC%\private" "%SRC%\credentials" "%SRC%\mods\KenshiCoop" ^
    /XF settings.cfg controls.cfg kenshi.cfg coop_config.json *.credential *.invite *.private ^
        kenshi.log kenshi_info.log Havok.log FileIOLog.txt RE_Kenshi_log.txt

set "RC=%ERRORLEVEL%"
REM robocopy: exit codes 0-7 are success (>=8 is a real failure).
if %RC% GEQ 8 (
    echo ERROR: robocopy failed with code %RC%.
    exit /b %RC%
)

REM Do not copy every user save. Runners synchronize only their named fixture.
if not exist "%DST%\save" mkdir "%DST%\save"

REM Only canonical matched six files enter the co-op directory; never host config/root credentials.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '%~dp0NativeBuild.ps1'; . '%~dp0kit_preflight.ps1'; $files=Get-CoopHarnessRuntime -RepoRoot ([IO.Path]::GetFullPath('%~dp0..')) -NativeBin $env:KENSHICOOP_NATIVE_BIN; $dest=Join-Path $env:DST 'mods\KenshiCoop'; New-Item -ItemType Directory -Force -Path $dest|Out-Null; foreach($file in $files){Copy-Item -LiteralPath $file -Destination $dest -Force}; Assert-CoopRuntime $dest"
if errorlevel 1 exit /b 1

REM A fresh clone has no renderer config. Seed only the known-working Ogre
REM renderer settings, never controls, co-op settings or credentials.
if not exist "%DST%\kenshi.cfg" (
    if not exist "%SRC%\kenshi.cfg" (
        echo ERROR: source renderer configuration missing.
        exit /b 1
    )
    copy /Y "%SRC%\kenshi.cfg" "%DST%\kenshi.cfg" >nul
    if errorlevel 1 exit /b 1
)
REM User save location=1 is AppData (shared by two processes on this PC).
REM A cloned join uses its OWN install/save folder, including on a fresh clone.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $p=Join-Path $env:DST 'settings.cfg'; $lines=if(Test-Path -LiteralPath $p){@(Get-Content -LiteralPath $p)}else{@()}; $found=$false; $lines=@($lines|ForEach-Object{if($_ -match '^\s*User save location\s*='){$found=$true;'User save location=0'}else{$_}}); if(-not $found){$lines+='User save location=0'}; [IO.File]::WriteAllLines($p,[string[]]$lines,[Text.Encoding]::ASCII)"
if errorlevel 1 exit /b 1

echo.
echo JOIN install ready at: %DST%
echo Next:
echo   1) Build the complete runtime: scripts\build_plugin.cmd Harness
echo   2) Deploy explicitly: scripts\deploy.cmd "%SRC%" Harness nomirror
echo                        scripts\deploy.cmd "%DST%" Harness nomirror
echo   3) Launch HOST: scripts\launch_host.cmd
echo   4) Launch JOIN: scripts\launch_join.cmd
echo   UDP JOIN requires its own private invitation FILE outside both game trees.
echo   Set KENSHICOOP_UDP_INVITE_FILE to that file; never copy a host signing profile.
endlocal
