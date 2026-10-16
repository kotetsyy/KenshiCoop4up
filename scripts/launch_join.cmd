@echo off
setlocal
rem Native free-play join. UDP requires its own private invitation FILE env path.
rem Pass -HostIp numericIPv4OrIPv6 or -HostSteamId ID; no transport fallback.
rem For two clients on one machine use manual_session.ps1 -TitleScreen instead.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0join_session.ps1" -Scenario free -SkipBuild %*
exit /b %ERRORLEVEL%
