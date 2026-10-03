@echo off
REM Build KenshiCoop.dll and its UI companion KenshiCoopUI.dll with the legacy
REM v100 (VC++ 2010) x64 toolchain on a machine that has only "Windows SDK 7.1 +
REM VC2010 SP1 compiler update" (no full VS2010). The toolchain environment
REM lives in v100_env.cmd (shared with build_ui.cmd).
REM
REM Both DLLs ship as one pair: every deploy/kit/release path copies them
REM together, so this builds both with the SAME configuration. For UI-only
REM iteration without recompiling the core use scripts\build_ui.cmd.
setlocal

REM Build configuration (Phase 1 build separation). Default = Harness, the
REM optimized TEST build that INCLUDES the scenario runner - this is what the
REM regression/manual pipeline needs. Pass "Release" to produce the shipped
REM player DLLs (no scenario code); "Debug" for a dev build.
REM   Usage:  scripts\build_plugin.cmd [Harness|Release|Debug]
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"

call "%~dp0v100_env.cmd"

echo === Building KenshiCoop.dll (%CONFIG%^|x64, v100) ===
where cl.exe

REM UseEnv=true: use the INCLUDE/LIB/PATH above instead of registry-derived paths.
REM TrackFileAccess=false: avoid Tracker.exe TRK0002 under redirected shells.
"%MSBUILD%" "%REPO%\src\plugin\KenshiCoop.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1

echo === Building KenshiCoopUI.dll (%CONFIG%^|x64, v100) ===
"%MSBUILD%" "%REPO%\src\ui\KenshiCoopUI.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1

endlocal
exit /b 0
