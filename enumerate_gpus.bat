@echo off
setlocal EnableDelayedExpansion

REM --- enumerate_gpus.bat ---
REM Standalone diagnostic tool compile script.
REM Does NOT touch viewTemp.exe / version.txt / build.bat.
REM Outputs: build\enumerate_gpus.obj + enumerate_gpus.exe + enumerate_gpus.log

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    exit /b 1
)

pushd "%~dp0"

REM --- make sure build\ exists (坑 12c) ---
if not exist build mkdir build

REM --- Compile (mirrored flags from build.bat but for console subsystem) ---
cl /nologo /c /EHsc /W3 /O1 /Os /Ob2 /GS- /Gy /GF /MT /utf-8 ^
   /D_WIN32_WINNT=0x0A00 /D_WINVER=0x0A00 /D_WIN32_IE=0x0A00 ^
   /Fo:build\enumerate_gpus.obj ^
   enumerate_gpus.cpp

if errorlevel 1 (
    echo [ERROR] Compile failed.
    popd
    exit /b 1
)

REM --- Link as CONSOLE subsystem so stdout appears in cmd window ---
link /nologo /SUBSYSTEM:CONSOLE,5.02 /OUT:enumerate_gpus.exe /MACHINE:X64 ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /MERGE:.rdata=.text ^
     /OSVERSION:10.0 /SUBSYSTEMVERSION:10.0 ^
     build\enumerate_gpus.obj ^
     user32.lib

if errorlevel 1 (
    echo [ERROR] Link failed.
    popd
    exit /b 1
)

REM --- Run immediately so 老林 sees the output without manual invocation ---
echo.
echo === Running enumerate_gpus.exe ===
echo.
enumerate_gpus.exe
set RC=%errorlevel%

echo.
echo [OK] Build + run complete. exit=%RC%
echo       Log: %~dp0enumerate_gpus.log
echo.
for %%A in (enumerate_gpus.exe) do echo     size = %%~zA bytes

popd
exit /b %RC%