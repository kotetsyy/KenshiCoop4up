@echo off
setlocal
cd /d "%~dp0"
title KenshiCoop - HOST
rem Prompt inside PowerShell, never interpolate user input into a shell command.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '.\kit_preflight.ps1'; $kit=Get-Content '.\kit.json' -Raw|ConvertFrom-Json; if($kit.transport -eq 'steam'){ Show-MySteamId; & '.\friend_host.ps1' }elseif($kit.transport -eq 'udp'){ Write-Host 'UDP uses only the reachable host IP:port, without keys or invitations. Anyone who can reach the port may join a free seat.'; & '.\friend_host.ps1' }else{ throw 'Unknown kit transport' }"
echo.
echo Session ended. Read any errors above.
pause
