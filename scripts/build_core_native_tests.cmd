@echo off
REM Compile actual NetLink+Inbound consumers; actors use real native providers.
setlocal
set "REPO=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Harness"
if /I "%CONFIG%"=="Harness" set "CONFIG=Harness"
if /I "%CONFIG%"=="Release" set "CONFIG=Release"
set "DEFINES=/D KENSHICOOP_PROTOTEST /D WIN32_LEAN_AND_MEAN /D NOMINMAX"
if /I "%CONFIG%"=="Harness" set "DEFINES=%DEFINES% /D KENSHICOOP_NET_DIAG"
if /I not "%CONFIG%"=="Harness" if /I not "%CONFIG%"=="Release" exit /b 2
call "%~dp0v100_env.cmd"
if errorlevel 1 exit /b 1
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"
set "SOURCE_DIGEST="
for /f "delims=" %%H in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -PrintTestDigest') do set "SOURCE_DIGEST=%%H"
if not defined SOURCE_DIGEST exit /b 1
set "BUILD_STARTED="
for /f "delims=" %%H in ('powershell -NoProfile -Command "[DateTime]::UtcNow.Ticks"') do set "BUILD_STARTED=%%H"
if not defined BUILD_STARTED exit /b 1
if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\core-native-%CONFIG%" mkdir "%REPO%\build\core-native-%CONFIG%"
cl.exe /nologo /O2 /EHsc /W3 %DEFINES% ^
 /Fo"%REPO%\build\core-native-%CONFIG%\\" ^
 /Fe"%REPO%\dist\core_native_tests.exe" ^
 "%REPO%\src\plugin\net\tests\CoreNativeTests.cpp" ^
 "%REPO%\src\plugin\net\NetLink.cpp" ^
 "%REPO%\src\plugin\CoopLog.cpp"
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0NativeBuild.ps1" -RecordTests -TestKind CoreNative -Configuration %CONFIG% -ExpectedDigest "%SOURCE_DIGEST%" -BuildStartedUtcTicks "%BUILD_STARTED%"
if errorlevel 1 exit /b 1
echo Actual v100 NetLink consumer built: %REPO%\dist\core_native_tests.exe [%CONFIG%]
exit /b 0
