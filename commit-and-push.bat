@echo off
setlocal
cd /d "%~dp0"

git status --short --branch
echo.
set "message=%*"
if "%message%"=="" set "message=Update AiCli"

echo Staging tracked file changes only (untracked files are excluded)...
git add -u
git diff --cached --quiet
if not errorlevel 1 (
    git commit -m "%message%"
    if errorlevel 1 (
        echo.
        echo Commit failed.
        pause
        exit /b 1
    )
) else (
    echo No tracked changes to commit.
)

git -c http.version=HTTP/1.1 -c http.postBuffer=524288000 push origin main
if errorlevel 1 (
    echo.
    echo Push failed. Local commit is preserved.
    pause
    exit /b 1
)

echo.
echo Commit and push completed.
pause
