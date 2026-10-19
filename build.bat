@echo off
chcp 1251 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

REM =============================================================
REM  Local Windows port of .github/workflows/build.yml
REM
REM  Requires (on PATH or discoverable via vswhere):
REM    - Visual Studio 2019/2022 with the "Desktop development
REM      with C++" workload and the MSVC x86 toolset
REM    - CMake >= 3.15 (multi-target --target), Ninja
REM    - Optional: sccache on PATH  -> enables compiler caching
REM    - Optional: System32\tar.exe -> fast zip; falls back to
REM      PowerShell Compress-Archive
REM
REM  Usage:
REM    build.bat                 full build + package
REM    build.bat dev             build only, skip packaging
REM    build.bat gc              build only the csgo_gc target
REM    build.bat srv             build only the srcds target
REM
REM  Flags may be combined, e.g. "build.bat gc dev".
REM =============================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"
set "RELEASE_DIR=%ROOT%\release"
set "OUTNAME=csgo_gc-windows-latest.zip"
set "OUTZIP=%ROOT%\%OUTNAME%"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=11"
set "JOBS=%NUMBER_OF_PROCESSORS%"

set "DO_PACKAGE=1"
set "TARGETS=csgo srcds csgo_gc"

REM ---------- Parse arguments ------------------------------------
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="dev"          (set "DO_PACKAGE=0"        & shift & goto :parse_args)
if /I "%~1"=="--no-package" (set "DO_PACKAGE=0"        & shift & goto :parse_args)
if /I "%~1"=="gc"           (set "TARGETS=csgo_gc"     & shift & goto :parse_args)
if /I "%~1"=="srv"          (set "TARGETS=srcds"       & shift & goto :parse_args)
echo [WARN] Unknown argument ignored: %~1
shift
goto :parse_args
:args_done

echo [*] Workspace:  %ROOT%
echo [*] Targets:    %TARGETS%
echo [*] Jobs:       %JOBS%
echo [*] Packaging:  %DO_PACKAGE%
echo Start time: %date% %time%

REM ---------- Locate Visual Studio --------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install VS 2019/2022 with C++ tools.
  goto :fail
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSINSTALL=%%i"

if "%VSINSTALL%"=="" (
  echo [ERROR] No Visual Studio with VC.Tools.x86.x64 found.
  goto :fail
)
echo [*] Visual Studio: %VSINSTALL%

REM ---------- Prefer VS-bundled CMake / Ninja ---------------------
set "VSCMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
if exist "%VSNINJA%\ninja.exe" set "PATH=%VSNINJA%;%PATH%"

REM ---------- Initialize MSVC x86 (skip if already active) --------
if /I "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo [*] vcvars x86 already active - skipping
) else (
  call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat"
)
where cl    >nul 2>&1 || (echo [ERROR] cl.exe not on PATH after vcvars. & goto :fail)
where cmake >nul 2>&1 || (echo [ERROR] cmake not found in PATH. & goto :fail)
where ninja >nul 2>&1 || (echo [ERROR] ninja not found in PATH. & goto :fail)

REM ---------- Detect optional sccache -----------------------------
set "LAUNCHER_OPTS="
where sccache >nul 2>&1
if not errorlevel 1 (
  echo [*] sccache found - enabling compiler cache
  set "LAUNCHER_OPTS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
) else (
  echo [*] sccache not found - building without a compiler cache
)

REM ---------- Configure (skip if already done) ------------------
if exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [*] Build dir already configured, skipping cmake configure
  echo     ninja will rerun cmake automatically if CMakeLists changes
) else (
  echo [*] Configuring...
  if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
  cmake -G Ninja -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release ^
        %LAUNCHER_OPTS% ^
        -S "%ROOT%" -DOUTDIR="%RELEASE_DIR%" ^
        -DCMAKE_LINKER=lld-link ^
        -DCMAKE_UNITY_BUILD=OFF ^
        -DCMAKE_UNITY_BUILD_BATCH_SIZE=8 ^
        -DFETCHCONTENT_UPDATES_DISCONNECTED=OFF ^
        -DFETCHCONTENT_FULLY_DISCONNECTED=OFF
  if errorlevel 1 (echo [ERROR] cmake configure failed. & goto :fail)
)

REM ---------- Patch ----------------------------------------------
call patch.bat nopause

REM ---------- Build ----------------------------------------------
echo [*] Building targets: %TARGETS%  (%JOBS% parallel jobs)
taskkill /f /im csgo.exe  >nul 2>&1
taskkill /f /im srcds.exe >nul 2>&1

set "BUILD_OK=0"
set "WANT_GC=0"
echo %TARGETS% | findstr /C:"csgo_gc" >nul 2>&1 && set "WANT_GC=1"

REM [FIX] Remove any stale DLLs first so a link failure can't be masked
REM       by an old artifact lying around from a previous build.
if "%WANT_GC%"=="1" (
  if exist "%RELEASE_DIR%\csgo_gc\csgo_gc.dll" del /Q "%RELEASE_DIR%\csgo_gc\csgo_gc.dll"
)

REM [FIX] Use || so the failure is caught no matter what exit code
REM       "cmake --build" decides to return on this CMake version.
cmake --build "%BUILD_DIR%" --target %TARGETS% --parallel %JOBS% || (
  echo [ERROR] cmake build failed.
  goto :fail
)

REM [FIX] Belt-and-braces: also verify via ERRORLEVEL in case a future
REM       CMake version returns 0 on ninja failure again.
if not "%ERRORLEVEL%"=="0" (
  echo [ERROR] cmake build failed ^(ERRORLEVEL=%ERRORLEVEL%^).
  goto :fail
)
set "BUILD_OK=1"

REM ---------- Copy csgo_gc.dll + launch, only on a real success -----
if "%WANT_GC%"=="0" (
  echo [*] csgo_gc was not part of the targets - skipping DLL copy and game launch.
  goto :skip_launch
)

set "DLL_SRC=%RELEASE_DIR%\csgo_gc\csgo_gc.dll"
set "DLL_DEST=%~dp0..\csgo_gc.dll"

if not exist "%DLL_SRC%" (
  echo [ERROR] "%DLL_SRC%" was not produced by the build.
  goto :fail
)

echo [*] Copying csgo_gc.dll to "%DLL_DEST%"
copy /Y "%DLL_SRC%" "%DLL_DEST%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy csgo_gc.dll.
  goto :fail
)

REM [FIX] Only launch once. The old script had a second launch block
REM       later on which fired whenever BUILD_OK==1, producing the
REM       "launches twice" behaviour.
echo [*] Launching steam://rungameid/4465480
start "" "steam://rungameid/4465480"

:skip_launch

if "%DO_PACKAGE%"=="0" (
  echo [*] Skipping packaging ^(dev mode^)
  goto :done
)

REM ---------- Package --------------------------------------------
echo [*] Packaging...
copy /Y "%ROOT%\README.md" "%RELEASE_DIR%\" >nul

if not exist "%RELEASE_DIR%\licenses" mkdir "%RELEASE_DIR%\licenses"
if not exist "%RELEASE_DIR%\csgo_gc"  mkdir "%RELEASE_DIR%\csgo_gc"

copy /Y "%ROOT%\LICENSE" "%RELEASE_DIR%\licenses\LICENSE-csgo_gc.txt" >nul
copy /Y "%BUILD_DIR%\_deps\cryptopp-cmake-build\cryptopp\License.txt" ^
     "%RELEASE_DIR%\licenses\LICENSE-crypto++.txt" >nul
copy /Y "%BUILD_DIR%\_deps\distorm-src\COPYING" ^
     "%RELEASE_DIR%\licenses\LICENSE-distorm.txt" >nul
copy /Y "%BUILD_DIR%\_deps\funchook-src\LICENSE" ^
     "%RELEASE_DIR%\licenses\LICENSE-funchook.txt" >nul
copy /Y "%BUILD_DIR%\_deps\protobuf-src\LICENSE" ^
     "%RELEASE_DIR%\licenses\LICENSE-protobuf.txt" >nul

copy /Y "%ROOT%\examples\config.txt"             "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\inventory.txt"          "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\price_sheet.txt"        "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\unusual_loot_lists.txt" "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\passes.txt"             "%RELEASE_DIR%\csgo_gc\" >nul

REM ---------- Zip ------------------------------------------------
REM  Prefer System32\tar.exe (bsdtar) over Compress-Archive: ~5-10x
REM  faster on the same tree.  Note that entries will carry a "./"
REM  prefix; harmless for almost every consumer.
echo [*] Creating archive: %OUTZIP%
if exist "%OUTZIP%" del /Q "%OUTZIP%"

set "ZIP_DONE=0"
set "SYSTAR=%SystemRoot%\System32\tar.exe"
if exist "%SYSTAR%" (
  "%SYSTAR%" -a -c -f "%OUTZIP%" -C "%RELEASE_DIR%" .
  if not errorlevel 1 set "ZIP_DONE=1"
)

if "%ZIP_DONE%"=="1" goto :zip_done

echo [*] tar unavailable or failed - falling back to Compress-Archive
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%RELEASE_DIR%\*' -DestinationPath '%OUTZIP%' -Force"
if errorlevel 1 (echo [ERROR] Failed to create zip. & goto :fail)

:zip_done
echo [OK] Done: %OUTZIP%

:done
echo End time: %date% %time%
endlocal
exit /b 0

:fail
echo.
echo [ERROR] Build failed. Press any key to close this window...
pause >nul
endlocal
exit /b 1