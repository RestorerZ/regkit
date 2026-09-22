@echo off
cd /d "%~dp0"

if /I "%~1"=="x86" goto :x86

cmake --preset x64-release || exit /b 1
cmake --build --preset x64-release || exit /b 1
call :portable build x64 || exit /b 1
if /I "%~1"=="x64" exit /b 0

:x86
cmake --preset x86-release || exit /b 1
cmake --build --preset x86-release || exit /b 1
call :portable build32 x86 || exit /b 1
exit /b 0

:portable
for /f "delims=" %%v in ('powershell -NoProfile -Command "(Get-Item '%~1\Release\regkit.exe').VersionInfo.FileVersion"') do set "VERSION=%%v"
powershell -NoProfile -ExecutionPolicy Bypass -File sign.ps1 "%~1\Release\regkit.exe" || exit /b 1
if not exist installer\dist mkdir installer\dist
tar -a -cf "installer\dist\RegKit-Portable-%VERSION%-%~2.zip" -C "%~1\Release" *
exit /b %errorlevel%
