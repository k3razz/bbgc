@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM   Auto-push script for csgo-gc
REM   Target: GitLab
REM ============================================================

REM ---- EDIT THESE ----
set "GIT_USER_NAME=sasha190409"
set "GIT_USER_EMAIL=ivanhihlov4@gmail.com"
set "REMOTE_URL=https://gitlab.com/sasha190409/csgo-gc.git"
set "BRANCH=main"
REM --------------------

REM ---- cd to script folder ----
cd /d "%~dp0"

REM ---- init repo if missing ----
if not exist ".git" (
    echo [*] Initializing git repository...
    git init
)

REM ---- ensure we are on the correct branch ----
echo [*] Ensuring current branch is '%BRANCH%'...
git branch -M %BRANCH%

REM ---- set LOCAL git identity ----
echo [*] Setting local git identity...
git config user.name  "%GIT_USER_NAME%"
git config user.email "%GIT_USER_EMAIL%"

REM ---- ensure origin remote ----
git remote get-url origin >nul 2>&1
if errorlevel 1 (
    echo [*] Adding remote origin...
    git remote add origin "%REMOTE_URL%"
) else (
    echo [*] Updating remote origin URL...
    git remote set-url origin "%REMOTE_URL%"
)

REM ---- commit message ----
set "MSG=%~1"
if not "%MSG%"=="" goto :have_msg

:menu
echo.
echo ============================================================
echo   Select commit reason:
echo ============================================================
echo   1. Feature        - add new feature
echo   2. Fix            - bug fix
echo   3. Refactor       - code refactor
echo   4. Docs           - documentation
echo   5. Style          - formatting/style only
echo   6. Config         - config/scripts changes
echo   7. Performance    - performance improvement
echo   8. Cleanup        - remove junk/dead code
echo   9. Auto-update    - generic auto-update
echo   0. Custom         - type your own message
echo   Q. Cancel
echo ============================================================
set /p "CHOICE=Enter choice [1-9,0,Q]: "

if /i "%CHOICE%"=="Q" (
    echo [*] Cancelled.
    pause
    exit /b 0
)
if "%CHOICE%"=="1" set "MSG=feat: add new feature"
if "%CHOICE%"=="2" set "MSG=fix: bug fix"
if "%CHOICE%"=="3" set "MSG=refactor: code cleanup"
if "%CHOICE%"=="4" set "MSG=docs: update documentation"
if "%CHOICE%"=="5" set "MSG=style: formatting"
if "%CHOICE%"=="6" set "MSG=chore: config/scripts update"
if "%CHOICE%"=="7" set "MSG=perf: performance improvement"
if "%CHOICE%"=="8" set "MSG=chore: remove junk/dead code"
if "%CHOICE%"=="9" set "MSG=Auto-update %DATE% %TIME%"
if "%CHOICE%"=="0" (
    set /p "MSG=Enter custom commit message: "
)

if "%MSG%"=="" (
    echo [ERROR] No message selected. Try again.
    goto :menu
)

REM ---- optional details ----
set /p "DETAILS=Add extra details? (leave empty to skip): "
if not "%DETAILS%"=="" set "MSG=%MSG% - %DETAILS%"

:have_msg
echo [*] Commit message: !MSG!

REM ---- stage everything ----
echo [*] Staging changes...
git add -A

REM ---- commit only if there is something staged ----
git diff --cached --quiet
if errorlevel 1 (
    echo [*] Committing: !MSG!
    git commit -m "!MSG!"
) else (
    echo [*] Nothing new to commit.
)

REM ---- pull latest (rebase) to avoid rejected pushes ----
echo [*] Pulling latest from origin/%BRANCH%...
git pull --rebase origin %BRANCH%
if %errorlevel% neq 0 (
    echo [WARN] Pull --rebase failed.
    echo [WARN] This is normal for a brand new repository with no commits yet.
    echo [WARN] Otherwise, resolve conflicts manually and run the script again.
)

REM ---- push ----
echo [*] Pushing to %REMOTE_URL% (%BRANCH%)...
git push -u origin %BRANCH%
if %errorlevel% neq 0 (
    echo [ERROR] Push failed.
    echo [ERROR] Make sure you are using a GitLab Personal Access Token as the password!
    pause
    exit /b 1
)

echo [*] Done.
pause
endlocal