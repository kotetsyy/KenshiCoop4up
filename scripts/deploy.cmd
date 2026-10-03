@echo off
REM Deploy KenshiCoop into a Kenshi install's mods folder.
REM Usage:  scripts\deploy.cmd ["C:\path\to\Kenshi"] [Harness|Release|Debug] [ui]
REM Defaults to the Steam install path if no argument is given.
REM
REM KenshiCoop.dll and KenshiCoopUI.dll always land in the same folder: the core
REM loads the UI companion from its own directory (RE_Kenshi.json lists only the
REM core). Pass "ui" as the 3rd argument to copy ONLY KenshiCoopUI.dll after
REM scripts\build_ui.cmd - Kenshi must be closed either way.
setlocal EnableDelayedExpansion

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "KENSHI=%~1"
if "%KENSHI%"=="" set "KENSHI=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

REM Build config to deploy (Phase 1 build separation). Default = Harness (the
REM test build with the scenario runner). Pass "Release" as the 2nd argument to
REM deploy the shipped player DLLs instead.
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Harness"
set "UIONLY="
if /I "%~3"=="ui" set "UIONLY=1"

set "DLL=%REPO%\src\plugin\x64\%CONFIG%\KenshiCoop.dll"
set "UIDLL=%REPO%\src\ui\x64\%CONFIG%\KenshiCoopUI.dll"
set "JSON=%REPO%\dist\mods\KenshiCoop\RE_Kenshi.json"
set "MOD=%REPO%\dist\mods\KenshiCoop\KenshiCoop.mod"
set "DST=%KENSHI%\mods\KenshiCoop"

if not defined UIONLY if not exist "%DLL%" (
    echo ERROR: %DLL% not found. Build first: scripts\build_plugin.cmd %CONFIG%
    exit /b 1
)
if not exist "%UIDLL%" (
    echo ERROR: %UIDLL% not found. Build first: scripts\build_plugin.cmd %CONFIG%
    exit /b 1
)
if not exist "%KENSHI%\kenshi_x64.exe" (
    echo ERROR: Kenshi not found at "%KENSHI%". Pass the path as the first argument.
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

if defined UIONLY goto :ui

copy /Y "%DLL%"  "%DST%\KenshiCoop.dll"   >nul
if errorlevel 1 (
    echo ERROR: could not copy KenshiCoop.dll to "%DST%".
    echo        The file is locked - a Kenshi instance is probably still running.
    echo        Close all Kenshi processes and retry.
    exit /b 1
)
echo Copied KenshiCoop.dll

:ui
copy /Y "%UIDLL%" "%DST%\KenshiCoopUI.dll" >nul
if errorlevel 1 (
    echo ERROR: could not copy KenshiCoopUI.dll to "%DST%".
    echo        The file is locked - a Kenshi instance is probably still running.
    echo        Close all Kenshi processes and retry.
    exit /b 1
)
echo Copied KenshiCoopUI.dll
if defined UIONLY goto :join

copy /Y "%JSON%" "%DST%\RE_Kenshi.json"   >nul
if errorlevel 1 (
    echo ERROR: could not copy RE_Kenshi.json to "%DST%" ^(locked?^).
    exit /b 1
)
echo Copied RE_Kenshi.json

REM KenshiCoop.mod is a real FCS data mod now: it carries the "Multiplayer
REM (Wanderer x2)" and "Multiplayer+ (Wanderer x2)" co-op starts (regenerate with
REM tools\MultiplayerStartGen). The repo owns it, so always overwrite the
REM install's copy with the repo's (a stale placeholder would hide the starts).
if not exist "%MOD%" (
    echo ERROR: %MOD% not found in the repo.
    exit /b 1
)
copy /Y "%MOD%" "%DST%\KenshiCoop.mod"   >nul
if errorlevel 1 (
    echo ERROR: could not copy KenshiCoop.mod to "%DST%" ^(locked?^).
    exit /b 1
)
echo Copied KenshiCoop.mod

:join
echo.
echo Deployed to: %DST%
dir /b "%DST%"

REM Also deploy into the separate JOIN install if it exists, so both clients run
REM the same freshly-built plugin. (Created by scripts\setup_join_install.cmd.)
set "JOINDIR=%USERPROFILE%\Kenshi-Join"
if not "%KENSHI%"=="%JOINDIR%" if exist "%JOINDIR%\kenshi_x64.exe" (
    set "JDST=%JOINDIR%\mods\KenshiCoop"
    if not exist "!JDST!" mkdir "!JDST!"
    if not defined UIONLY (
        copy /Y "%DLL%"  "!JDST!\KenshiCoop.dll" >nul
        if errorlevel 1 (
            echo ERROR: could not copy KenshiCoop.dll to join install "!JDST!".
            echo        The file is locked - a Kenshi-Join instance is probably still running.
            exit /b 1
        )
        echo Copied KenshiCoop.dll  -^> join install
    )
    copy /Y "%UIDLL%" "!JDST!\KenshiCoopUI.dll" >nul
    if errorlevel 1 (
        echo ERROR: could not copy KenshiCoopUI.dll to join install "!JDST!".
        echo        The file is locked - a Kenshi-Join instance is probably still running.
        exit /b 1
    )
    echo Copied KenshiCoopUI.dll  -^> join install
    if not defined UIONLY (
        copy /Y "%JSON%" "!JDST!\RE_Kenshi.json" >nul && echo Copied RE_Kenshi.json  -^> join install
        copy /Y "%MOD%"  "!JDST!\KenshiCoop.mod" >nul && echo Copied KenshiCoop.mod   -^> join install
    )
)

echo.
echo Next: launch Kenshi, enable "KenshiCoop" in the Mods tab, then check
echo RE_Kenshi_log.txt for "KenshiCoop loaded!".
endlocal
