@echo off
rem ============================================================
rem  RCP Score Editor - GUI launcher
rem ============================================================
rem  Auto-detects the Qt6 bin directory and prepends it to PATH,
rem  so ui.exe can find Qt6Widgets.dll / Qt6Core.dll.
rem  If detection fails, set QTBIN manually below, e.g.
rem      set "QTBIN=A:\msys64\ucrt64\bin"
rem ============================================================

setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "QTBIN="

rem --- 1) Qt already on PATH (qmake6.exe)? ---
for %%I in (qmake6.exe) do set "QTBIN=%%~dp$PATH:I"
if defined QTBIN if not exist "!QTBIN!Qt6Widgets.dll" set "QTBIN="

rem --- 2) well-known install locations ---
if not defined QTBIN if exist "A:\msys64\ucrt64\bin\Qt6Widgets.dll" set "QTBIN=A:\msys64\ucrt64\bin"
if not defined QTBIN if exist "A:\msys2\ucrt64\bin\Qt6Widgets.dll" set "QTBIN=A:\msys2\ucrt64\bin"
if not defined QTBIN if exist "A:\msys64\mingw64\bin\Qt6Widgets.dll" set "QTBIN=A:\msys64\mingw64\bin"
if not defined QTBIN if exist "A:\msys2\mingw64\bin\Qt6Widgets.dll" set "QTBIN=A:\msys2\mingw64\bin"
if not defined QTBIN if exist "C:\msys64\ucrt64\bin\Qt6Widgets.dll" set "QTBIN=C:\msys64\ucrt64\bin"
if not defined QTBIN if exist "C:\msys64\mingw64\bin\Qt6Widgets.dll" set "QTBIN=C:\msys64\mingw64\bin"
if not defined QTBIN if exist "A:\Qt\6.11.2\mingw_64\bin\Qt6Widgets.dll" set "QTBIN=A:\Qt\6.11.2\mingw_64\bin"
if not defined QTBIN if exist "C:\Qt\6.11.2\mingw_64\bin\Qt6Widgets.dll" set "QTBIN=C:\Qt\6.11.2\mingw_64\bin"
if not defined QTBIN for /d %%V in ("A:\Qt\6.*\mingw_64\bin" "C:\Qt\6.*\mingw_64\bin") do (
    if not defined QTBIN if exist "%%~V\Qt6Widgets.dll" set "QTBIN=%%~V"
)

rem --- 3) PATH: dev layout (build\ui, build\player, build\save) and flat release both work ---
set "SEARCH=%HERE%build\ui;%HERE%build\player;%HERE%build\save;%HERE%;%HERE%..;"
if defined QTBIN (
    set "PATH=%SEARCH%%QTBIN%;%PATH%"
    echo [run_ui] Qt6 found at: %QTBIN%
) else (
    echo [run_ui] WARNING: Qt6 not found automatically.
    echo [run_ui] Edit this script and set QTBIN, e.g.:
    echo [run_ui]     set "QTBIN=A:\msys64\ucrt64\bin"
    set "PATH=%SEARCH%%PATH%"
)

rem --- 4) locate ui.exe ---
set "UI=%HERE%build\ui\ui.exe"
if not exist "%UI%" set "UI=%HERE%ui.exe"
if not exist "%UI%" (
    echo [run_ui] ERROR: ui.exe not found. Build it first:
    echo [run_ui]   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=A:/msys64/ucrt64
    echo [run_ui]   cmake --build build
    pause
    exit /b 1
)

start "" "%UI%" %*
endlocal
exit /b 0
