@echo off
cd /d "%~dp0"

if /I "%~1"=="dev" goto :dev
if /I "%~1"=="x86" goto :x86

cmake --preset x64-release || exit /b 1
cmake --build --preset x64-release || exit /b 1
call :portable build\x64-release x64 || exit /b 1
if /I "%~1"=="x64" exit /b 0

:x86
cmake --preset x86-release || exit /b 1
cmake --build --preset x86-release || exit /b 1
call :portable build\x86-release x86 || exit /b 1
exit /b 0

:dev
if /I not "%~2"=="x86" (
    cmake --preset x64-dev || exit /b 1
    cmake --build --preset x64-dev || exit /b 1
)
if /I "%~2"=="x64" exit /b 0
cmake --preset x86-dev || exit /b 1
cmake --build --preset x86-dev
exit /b %errorlevel%

:portable
for /f "delims=" %%v in ('powershell -NoProfile -Command "(Get-Item '%~1\Release\regkit.exe').VersionInfo.FileVersion"') do set "VERSION=%%v"
powershell -NoProfile -ExecutionPolicy Bypass -File sign.ps1 "%~1\Release\regkit.exe" || exit /b 1
if not exist build\dist mkdir build\dist
tar -a -cf "build\dist\RegKit-Portable-%VERSION%-%~2.zip" --exclude=*.pdb -C "%~1\Release" * || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File sign.ps1 "build\dist\RegKit-Portable-%VERSION%-%~2.zip"
exit /b %errorlevel%
