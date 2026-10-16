@echo off
REM Build dist\prototest.exe - the asserting unit layer for the wire protocol,
REM content hash and interpolation buffer (src\prototest). Uses the same v100
REM (VC++ 2010) x64 compiler as the plugin so the packed-struct layout under
REM test is EXACTLY the layout the shipped DLL compiles (that is the point:
REM prototest locks the wire contract of this toolchain).
REM
REM No game or KenshiLib dependencies. Updater tests use Win32/CryptoAPI.
setlocal

call "%~dp0v100_env.cmd"
if errorlevel 1 exit /b 1
set "SOURCE_DIGEST="
for /f "delims=" %%H in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -PrintTestDigest') do set "SOURCE_DIGEST=%%H"
if not defined SOURCE_DIGEST exit /b 1
set "BUILD_STARTED="
for /f "delims=" %%H in ('powershell -NoProfile -Command "[DateTime]::UtcNow.Ticks"') do set "BUILD_STARTED=%%H"
if not defined BUILD_STARTED exit /b 1
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\prototest" mkdir "%REPO%\build\prototest"

echo === Building prototest.exe (Release^|x64, v100) ===
REM KENSHICOOP_PROTOTEST keeps SaveXfer.cpp CRT-only (its NetLink/engine-coupled
REM sender + quiescence watch are #ifdef'd out) so the real save-transfer RECEIVER
REM (onSaveBegin/onSaveFile/onSaveDone -> stage/verify/commit) can be exercised
REM end-to-end here without pulling in the game.
cl.exe /nologo /O2 /EHsc /W3 /D KENSHICOOP_PROTOTEST ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\prototest.exe" ^
    "%REPO%\src\prototest\main.cpp" ^
    "%REPO%\src\plugin\sync\Interp.cpp" ^
    "%REPO%\src\plugin\sync\SaveXfer.cpp"
if errorlevel 1 (
    echo prototest build FAILED
    exit /b 1
)
echo === Building real-code updater_tests.exe (v100) ===
cl.exe /nologo /O2 /EHsc /W3 /D KENSHICOOP_UPDATER_TEST ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\updater_tests.exe" ^
    "%REPO%\src\prototest\UpdaterTests.cpp" ^
    "%REPO%\src\plugin\net\UpdatePackage.cpp" advapi32.lib
if errorlevel 1 exit /b 1
echo === Building actual native_frame_tests.exe (v100) ===
cl.exe /nologo /O2 /EHsc /W3 ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\native_frame_tests.exe" ^
    "%REPO%\src\plugin\net\tests\NativeFrameTests.cpp"
if errorlevel 1 exit /b 1
echo === Building native NPC census and environment state tests (v100) ===
cl.exe /nologo /O2 /EHsc /W3 ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\npc_census_tests.exe" ^
    "%REPO%\src\plugin\test\NpcCensusLedgerTests.cpp"
if errorlevel 1 exit /b 1
cl.exe /nologo /O2 /EHsc /W3 ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\environment_items_state_tests.exe" ^
    "%REPO%\src\plugin\test\EnvironmentItemsStateTests.cpp"
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -RecordTests -TestKind Protocol -Configuration Release -ExpectedDigest "%SOURCE_DIGEST%" -BuildStartedUtcTicks "%BUILD_STARTED%"
if errorlevel 1 exit /b 1
echo prototest built: %REPO%\dist\prototest.exe
exit /b 0
