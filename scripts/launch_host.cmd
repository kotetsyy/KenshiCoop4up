@echo off
setlocal
rem Native free-play host. Uses owned launcher hooks/non-primary display/exact restore.
rem For two clients on one machine use manual_session.ps1 -TitleScreen instead.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0host_session.ps1" -Scenario free -SkipBuild %*
exit /b %ERRORLEVEL%
