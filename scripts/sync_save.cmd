@echo off
rem Explicit named-save synchronization, never a mirror/delete of every user save.
rem Usage: sync_save.cmd "host game" "join game" "save name"
setlocal
set "KC_SYNC_HOST=%~1"
set "KC_SYNC_JOIN=%~2"
set "KC_SYNC_SAVE=%~3"
if not defined KC_SYNC_HOST set "KC_SYNC_HOST=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if not defined KC_SYNC_JOIN set "KC_SYNC_JOIN=%USERPROFILE%\Kenshi-Join"
if not defined KC_SYNC_SAVE (
    echo ERROR: specify the single save name as the third argument.
    exit /b 2
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '%~dp0kit_preflight.ps1'; if(Get-Process kenshi_x64 -ErrorAction SilentlyContinue){throw 'Close user-owned Kenshi sessions before syncing'}; Sync-CoopNamedSave -Save $env:KC_SYNC_SAVE -HostDir $env:KC_SYNC_HOST -JoinDir $env:KC_SYNC_JOIN"
exit /b %ERRORLEVEL%
