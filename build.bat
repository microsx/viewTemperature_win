@echo off

setlocal EnableDelayedExpansion



REM --- Locate vcvars64.bat (VS 2022 Community, x64) ---

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto :no_vcvars



call "%VCVARS%" >nul

if errorlevel 1 goto :vcvars_failed



pushd "%~dp0"



REM --- Auto-bump version: version.txt holds the human version in hundredths ---

REM     "1"   -> 0.01

REM     "100" -> 1.00

REM     "123" -> 1.23

REM     Allowed to exceed 100 (i.e. 1.00). No upper cap.

set "VERSION_FILE=%~dp0version.txt"

if not exist "%VERSION_FILE%" (

    REM Fresh start per 老林: 0.01

    > "%VERSION_FILE%" echo 1

)

set /a VERSION_HUNDREDTHS=0

for /f "usebackq delims=" %%V in ("%VERSION_FILE%") do set /a VERSION_HUNDREDTHS=%%V

set /a VERSION_HUNDREDTHS=VERSION_HUNDREDTHS+1

> "%VERSION_FILE%" echo !VERSION_HUNDREDTHS!

set /a VERSION_MAJOR=VERSION_HUNDREDTHS/100

set /a VERSION_MINOR_RAW=VERSION_HUNDREDTHS%%100

REM Always render minor with two digits (00..99) — required by 老林 ("0.01", "1.00").

if !VERSION_MINOR_RAW! LSS 10 (set "VERSION_MINOR=0!VERSION_MINOR_RAW!") else (set "VERSION_MINOR=!VERSION_MINOR_RAW!")



REM --- Compile ---

cl /nologo /c /EHsc /W3 /O1 /Os /Ob2 /GS- /Gy /GF /MT /utf-8 ^

   /D_WIN32_WINNT=0x0A00 /D_WINVER=0x0A00 /D_WIN32_IE=0x0A00 ^
   /D VERSION_MAJOR=!VERSION_MAJOR! /D VERSION_MINOR=!VERSION_MINOR! ^

   /Fp:build\vc.pch /Fo:build\ /Fd:build\vc.pdb ^

   viewTemp.cpp

if errorlevel 1 goto :compile_failed



REM --- Compile resource (version info only) ---

rc /nologo /d VERSION_MAJOR=!VERSION_MAJOR! /d VERSION_MINOR=!VERSION_MINOR! /fo build\viewTemp.res viewTemp.rc

if errorlevel 1 goto :rc_failed



REM --- Link (MinSize, no incremental, merge sections) ---

link /nologo /SUBSYSTEM:WINDOWS,5.02 /OUT:viewTemp.exe /MACHINE:X64 ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /MERGE:.rdata=.text ^
     /OSVERSION:10.0 /SUBSYSTEMVERSION:10.0 ^
     /MANIFEST:EMBED /MANIFESTINPUT:viewTemp.exe.manifest ^
     build\viewTemp.obj build\viewTemp.res ^
     user32.lib gdi32.lib shell32.lib ole32.lib gdiplus.lib advapi32.lib dxgi.lib dxguid.lib

if errorlevel 1 goto :link_failed



REM --- Manifest is embedded by the linker via /MANIFEST:EMBED above ---



del /q build\vc.pdb 2>nul



echo.

echo [OK] Built viewTemp.exe  v!VERSION_MAJOR!.!VERSION_MINOR!

for %%A in (viewTemp.exe) do echo     size = %%~zA bytes

popd

exit /b 0



:no_vcvars

echo [ERROR] vcvars64.bat not found at "%VCVARS%"

exit /b 1



:vcvars_failed

echo [ERROR] vcvars64.bat failed.

exit /b 1



:compile_failed

echo [ERROR] Compile failed.

popd

exit /b 1



:rc_failed

echo [ERROR] Resource compile failed.

popd

exit /b 1



:link_failed

echo [ERROR] Link failed.

popd

exit /b 1

