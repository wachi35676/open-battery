@echo off
rem Builds WirelessBattery.exe with Visual Studio (2022 or newer, "Desktop development with C++").
rem Alternative to build.bat / make, which need MinGW-w64.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio not found & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build-msvc mkdir build-msvc
rem build\version.h: a "dev" build, so the updater (official releases only) stays off and never replaces this build.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\generate-version.ps1 -Version 0.0.0 -Channel dev || exit /b 1
echo.>> build\version.h
rem (rc needs a newline after the last #define)
(for %%f in (lang\*.txt) do (echo @file %%~nf& type "%%f"& echo.)) > build-msvc\languages.txt
rc /nologo /i res /i build-msvc /fo build-msvc\app.res res\app.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /EHsc /O1 /MT /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /FI"%~dp0msvc_compat.h" ^
   /Fobuild-msvc\ src\*.cpp src\drivers\*.cpp build-msvc\app.res ^
   /Fe:WirelessBattery.exe /link /SUBSYSTEM:WINDOWS ^
   hid.lib setupapi.lib cfgmgr32.lib ole32.lib oleaut32.lib comctl32.lib gdi32.lib runtimeobject.lib user32.lib shell32.lib advapi32.lib ws2_32.lib bluetoothapis.lib || exit /b 1
echo Built WirelessBattery.exe
