@echo off
cd /d "%~dp0"
set "ISCC=%userprofile%\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" echo Inno Setup 6 was not found. & exit /b 1
set "SIGN=/Sregkit=powershell -NoProfile -ExecutionPolicy Bypass -File $q%~dp0..\sign.ps1$q $f"
if /I "%~1"=="x86" goto :x86
"%ISCC%" "%SIGN%" /DArch=x64 regkit.iss || exit /b 1
if /I "%~1"=="x64" exit /b 0
:x86
"%ISCC%" "%SIGN%" /DArch=x86 regkit.iss || exit /b 1
