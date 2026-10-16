@echo off
REM Build the real v143 native providers independently BEFORE entering the v100
REM environment, then build the canonical v100 core/UI with the same logical
REM configuration. Debug uses the native Harness configuration/static CRT.
setlocal

REM Build configuration (Phase 1 build separation). Default = Harness, the
REM optimized TEST build that INCLUDES the scenario runner - this is what the
REM regression/manual pipeline needs. Pass "Release" to produce the shipped
REM player DLLs (no scenario code); "Debug" for a dev build.
REM   Usage:  scripts\build_plugin.cmd [Harness|Release|Debug]
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"
if /I not "%CONFIG%"=="Release" if /I not "%CONFIG%"=="Harness" if /I not "%CONFIG%"=="Debug" exit /b 2
if /I "%CONFIG%"=="Release" set "CONFIG=Release"
if /I "%CONFIG%"=="Harness" set "CONFIG=Harness"
if /I "%CONFIG%"=="Debug" set "CONFIG=Debug"
set "NATIVE_CONFIG=%CONFIG%"
if /I "%CONFIG%"=="Debug" set "NATIVE_CONFIG=Harness"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_transports.ps1" -Configuration %NATIVE_CONFIG%
if errorlevel 1 exit /b 1
set "SOURCE_DIGEST="
for /f "delims=" %%H in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -PrintCoreDigest') do set "SOURCE_DIGEST=%%H"
if not defined SOURCE_DIGEST exit /b 1

call "%~dp0v100_env.cmd"
if errorlevel 1 exit /b 1
set "BUILD_STARTED="
for /f "delims=" %%H in ('powershell -NoProfile -Command "[DateTime]::UtcNow.Ticks"') do set "BUILD_STARTED=%%H"
if not defined BUILD_STARTED exit /b 1

echo === Building KenshiCoop.dll (%CONFIG%^|x64, v100) ===
where cl.exe

REM UseEnv=true: use the INCLUDE/LIB/PATH above instead of registry-derived paths.
REM TrackFileAccess=false: avoid Tracker.exe TRK0002 under redirected shells.
"%MSBUILD%" "%REPO%\src\plugin\KenshiCoop.vcxproj" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1

echo === Building KenshiCoopUI.dll (%CONFIG%^|x64, v100) ===
"%MSBUILD%" "%REPO%\src\ui\KenshiCoopUI.vcxproj" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -RecordCore -Configuration %CONFIG% -ExpectedDigest "%SOURCE_DIGEST%" -BuildStartedUtcTicks "%BUILD_STARTED%"
if errorlevel 1 exit /b 1

endlocal
exit /b 0
