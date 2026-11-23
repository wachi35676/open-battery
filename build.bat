@echo off
rem Builds BatteryHub.exe with MinGW-w64 (g++ and windres on PATH).
rem Every .cpp file in src\ and src\drivers\ is compiled, so a new driver needs no change here.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build
set "APP_VERSION=%~1"
if not defined APP_VERSION set "APP_VERSION=0.0.0"
set "APP_CHANNEL=%~2"
if not defined APP_CHANNEL set "APP_CHANNEL=dev"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\generate-version.ps1 -Version "%APP_VERSION%" -Channel "%APP_CHANNEL%" || exit /b 1
rem Every lang\*.txt is a built-in UI language: joined into one resource, each after an "@file <name>" line.
(for %%f in (lang\*.txt) do (echo @file %%~nf& type "%%f"& echo.)) > build\languages.txt
windres -i res\app.rc -o build\app_res.o --include-dir res --include-dir build || exit /b 1
set SOURCES=
for %%f in (src\*.cpp src\drivers\*.cpp) do set SOURCES=!SOURCES! %%f
g++ -std=c++17 -Os -municode -mwindows -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wno-missing-field-initializers ^
    !SOURCES! ^
    build\app_res.o -o BatteryHub.exe ^
    -lhid -lsetupapi -lcfgmgr32 -lole32 -loleaut32 -lcomctl32 -lgdi32 -lruntimeobject -lws2_32 -lbluetoothapis -ld2d1 -ldwrite -ldwmapi || exit /b 1
echo Built BatteryHub.exe
