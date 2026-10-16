@echo off
setlocal
cd /d "%~dp0"
title KenshiCoop - HOST
rem Prompt inside PowerShell, never interpolate user input into a shell command.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '.\kit_preflight.ps1'; $kit=Get-Content '.\kit.json' -Raw|ConvertFrom-Json; if($kit.transport -eq 'steam'){ Show-MySteamId; & '.\friend_host.ps1' }elseif($kit.transport -eq 'udp'){ Write-Host 'UDP has no Steam dependency. In F2 create a distinct invitation for each guest and share it privately; never share the host profile.'; & '.\friend_host.ps1' }else{ throw 'Unknown kit transport' }"
echo.
echo Session ended. Read any errors above.
pause
