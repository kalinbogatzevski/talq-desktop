@echo off
rem Build the WGC capture component with MSVC (cl.exe) + the Windows 11 SDK.
rem Produces talq_wgc.dll (+ import lib) and the talq_wgc_spike.exe validator.
rem Run from anywhere: it cd's to its own dir and sets up the MSVC environment.
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at:
    echo   "%VCVARS%"
    echo Install VS2022 Build Tools "Desktop development with C++" + a Windows 11 SDK.
    exit /b 1
)
call "%VCVARS%" >nul

rem /MT (static CRT) makes talq_wgc.dll fully self-contained: it carries its own
rem MSVC runtime statically, so it needs NO vcruntime140/msvcp140 redist on the
rem target machine. Safe here because the MSVC<->MinGW boundary is pure C — no
rem CRT objects or heap ownership ever cross it (frame bytes are copied in the
rem callback). One DLL ships beside talq.exe; nothing else.
echo [1/2] talq_wgc.dll ...
cl /nologo /std:c++17 /EHsc /MT /O2 /W3 /DTALQ_WGC_BUILDING /D_CRT_SECURE_NO_WARNINGS ^
   /LD talq_wgc.cpp /Fe:talq_wgc.dll ^
   /link windowsapp.lib d3d11.lib dxgi.lib user32.lib
if errorlevel 1 ( echo DLL build FAILED & exit /b 1 )

echo [2/2] talq_wgc_spike.exe ...
cl /nologo /std:c++17 /EHsc /MT /O2 /W3 /D_CRT_SECURE_NO_WARNINGS talq_wgc_spike.cpp ^
   /Fe:talq_wgc_spike.exe /link talq_wgc.lib user32.lib
if errorlevel 1 ( echo spike build FAILED & exit /b 1 )

echo.
echo OK -- built talq_wgc.dll + talq_wgc_spike.exe
