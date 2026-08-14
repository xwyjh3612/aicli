@echo off
setlocal
cd /d "%~dp0"

if not exist "local-screenshots" mkdir "local-screenshots"

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo This directory is not a Git repository.
    pause
    exit /b 1
)

for /f "delims=" %%B in ('git branch --show-current') do set "branch=%%B"
if not defined branch (
    echo Cannot push while Git is in detached HEAD state.
    pause
    exit /b 1
)

set "message=%*"
if not defined message (
    for /f "delims=" %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HH-mm-ss"') do set "message=Update AiCli %%T"
)

echo Staging all changes except files matched by .gitignore...
git add -A
if errorlevel 1 (
    echo.
    echo Staging failed.
    pause
    exit /b 1
)

git diff --cached --quiet
if errorlevel 2 (
    echo.
    echo Unable to inspect staged changes.
    pause
    exit /b 1
) else if errorlevel 1 (
    echo Creating commit: %message%
    git commit -m "%message%"
    if errorlevel 1 (
        echo.
        echo Commit failed.
        pause
        exit /b 1
    )
) else (
    echo No new changes to commit. Any existing local commits will still be pushed.
)

echo.
echo Pushing branch %branch% to origin...
git -c http.version=HTTP/1.1 -c http.postBuffer=16m push origin "%branch%"
if errorlevel 1 (
    echo.
    echo Push failed. Local commit is preserved.
    pause
    exit /b 1
)

echo.
echo Commit and push completed successfully.
git status --short --branch
pause
