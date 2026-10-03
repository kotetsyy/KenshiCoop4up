@echo off
REM Build ONLY KenshiCoopUI.dll (src\ui) for UI iteration; the core
REM KenshiCoop.dll is not recompiled. Copy the result next to the installed
REM KenshiCoop.dll while Kenshi is CLOSED (the core loads the UI once per
REM process and never unloads it):
REM   scripts\deploy.cmd ["C:\path\to\Kenshi"] [Config] ui
REM The UI talks to the core only through src\ui\CoopUiApi.h, so a rebuilt UI
REM pairs with any core built against the same COOP_UI_API_VERSION.
REM   Usage:  scripts\build_ui.cmd [Harness|Release|Debug]
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"

call "%~dp0v100_env.cmd"

echo === Building KenshiCoopUI.dll (%CONFIG%^|x64, v100) ===
"%MSBUILD%" "%REPO%\src\ui\KenshiCoopUI.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1

endlocal
exit /b 0
