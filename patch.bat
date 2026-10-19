@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Apply Patches

rem ---- Parse arguments -------------------------------------------------
set "NOPAUSE="
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="nopause" (
    set "NOPAUSE=1"
) else (
    echo [WARN] Unknown argument: %~1
)
shift
goto :parse_args
:args_done

pushd "%~dp0"

if not exist "patch\" (
    echo [ERROR] "patch" folder not found in "%CD%".
    set "RC=2"
    goto :error
)

where robocopy >nul 2>&1 || (set "RC=3" & goto :error)

echo [*] Applying patches from "%CD%\patch"...
echo [^!] For NEW compilers only.
echo.

robocopy "patch" "." /E /XO /MT:16 /J /R:1 /W:1 /NP ^
         /NFL /NDL /NJH /NJS /NC /NS /LOG+:"patch.log"
set "RC=%ERRORLEVEL%"

rem ---- Decode the bitmap ------------------------------------------------
call :describe %RC% "%RC%"
echo.

rem Fatal bits set? -> error path
if %RC% GEQ 8 goto :error

rem Success path
if %RC% EQU 0 (
    echo [OK] Nothing to do - destination already up to date.
) else (
    echo [OK] Patches applied.
    echo      See patch.log for the file list.
)

popd
if not defined NOPAUSE (
    echo %CMDCMDLINE% | find /i "/c" >nul && pause
)
exit /b 0

:error
echo [ERROR] Patching failed.
echo         Full log: "%CD%\patch.log"
popd
if not defined NOPAUSE pause
exit /b %RC%

rem =====================================================================
rem :describe <code> <original-code>
rem   Prints a human-readable breakdown of a robocopy exit code.
rem =====================================================================
:describe
setlocal EnableDelayedExpansion
set "code=%~1"
set "raw=%~2"

echo Robocopy exit code: !raw!

if !code! EQU 0 (
    echo   - No errors, no files copied ^(everything already in sync^).
    endlocal & exit /b 0
)

if !code! GEQ 16 (
    echo   [FATAL] Serious error: nothing was copied.
    echo           Usually a bad command line or insufficient privileges.
    set /a "code-=16"
    set "fatal=1"
)
if !code! GEQ 8 (
    echo   [FAIL]  Some files/folders could NOT be copied ^(retries exhausted^).
    echo           Check patch.log for "ERROR" lines.
    set /a "code-=8"
    set "fail=1"
)
if !code! GEQ 4 (
    echo   [WARN]  Mismatched files/folders detected.
    set /a "code-=4"
)
if !code! GEQ 2 (
    echo   [INFO]  Extra files/folders exist in destination ^(not in source^).
    set /a "code-=2"
)
if !code! GEQ 1 (
    echo   [INFO]  One or more files were copied successfully.
)

endlocal & exit /b 0