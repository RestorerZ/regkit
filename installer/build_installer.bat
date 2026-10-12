@echo off
cd /d "%~dp0"
set "ISCC=%userprofile%\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" echo Inno Setup 6 was not found. & exit /b 1
if /I "%~1"=="x86" goto :x86
call :setup build x64 || exit /b 1
if /I "%~1"=="x64" exit /b 0
:x86
call :setup build32 x86
exit /b %errorlevel%

:setup
"%ISCC%" /DArch=%~2 regkit.iss || exit /b 1
for /f "delims=" %%v in ('powershell -NoProfile -Command "(Get-Item '..\%~1\Release\regkit.exe').VersionInfo.FileVersion"') do set "VERSION=%%v"
powershell -NoProfile -ExecutionPolicy Bypass -File ..\sign.ps1 "dist\RegKit-Setup-%VERSION%-%~2.exe"
exit /b %errorlevel%
