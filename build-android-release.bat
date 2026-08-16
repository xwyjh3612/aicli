@echo off
setlocal

cd /d "%~dp0"
title AiCli Android Release Builder

echo.
echo ========================================
echo   Building signed Android Release APK
echo ========================================
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass ^
  -File "%~dp0android-app\tools\build-release.ps1"

set "AICLI_BUILD_EXIT=%ERRORLEVEL%"
echo.

if not "%AICLI_BUILD_EXIT%"=="0" (
  echo [FAILED] Android Release APK build failed.
  echo Check the error output above.
) else (
  echo [SUCCESS] Android Release APK is ready.
  echo Output: %~dp0release\AI-CLI-Android-1.1.0-release.apk
  echo SHA256: %~dp0release\AI-CLI-Android-1.1.0-release.apk.sha256
)

echo.
if /I not "%~1"=="--no-pause" pause
exit /b %AICLI_BUILD_EXIT%
