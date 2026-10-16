@echo off
setlocal
cd /d "%~dp0"
title KenshiCoop - JOIN
rem Direct UDP needs only the host numeric address and kit port.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; . '.\kit_preflight.ps1'; $kit=Get-Content '.\kit.json' -Raw|ConvertFrom-Json; if($kit.transport -eq 'steam'){ Show-MySteamId; $hostId=Read-Host 'Host Steam friend code (Enter uses kit default)'; if($hostId){ & '.\friend_join.ps1' -HostSteamId $hostId }else{ & '.\friend_join.ps1' } }elseif($kit.transport -eq 'udp'){ $ip=Read-Host 'Host numeric IPv4 or IPv6 address'; & '.\friend_join.ps1' -HostIp $ip }else{ throw 'Unknown kit transport' }"
echo.
echo Session ended. Read any errors above.
pause
