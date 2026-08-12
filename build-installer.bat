@echo off
setlocal
cd /d "%~dp0"

echo Building the non-Qt AiCli installer...
powershell -NoProfile -ExecutionPolicy Bypass -File "desktop\tools\build-windows-installer.ps1"
if errorlevel 1 (
    echo.
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo Installer created in:
echo %CD%\release\AI-CLI-Setup-1.0.0.3-x64.exe
pause
