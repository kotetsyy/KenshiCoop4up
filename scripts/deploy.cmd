@echo off
REM Complete six-file deployment with full source/destination preflight.
REM Usage: deploy.cmd "C:\explicit\Kenshi" [Harness^|Release^|Debug] [nomirror^|mirror] [exact-native-bin]
REM Single explicit installation by default. No user config/save/credential copies.
setlocal
if "%~1"=="" (
    echo ERROR: supply an explicit Kenshi installation path.
    exit /b 2
)
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Harness"
set "MIRROR="
if /I "%~3"=="mirror" set "MIRROR=-Mirror"
if not "%~3"=="" if /I not "%~3"=="nomirror" if /I not "%~3"=="mirror" (
    echo ERROR: third argument must be nomirror or mirror; UI-only deployment removed.
    exit /b 2
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0RuntimeDistribution.ps1" -Deploy -Kenshi "%~1" -Configuration "%CONFIG%" -NativeBin "%~4" %MIRROR%
exit /b %ERRORLEVEL%
