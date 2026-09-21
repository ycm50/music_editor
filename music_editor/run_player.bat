@echo off
rem ============================================================
rem  RCP Score Editor - command line player launcher
rem ============================================================
rem  Usage:
rem      run_player.bat <file.rcp> [--instrument NAME] [--mono] [--no-reverb]
rem      run_player.bat --help
rem  With no arguments it plays the bundled demo score.
rem ============================================================

setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "QTBIN="

for %%I in (qmake6.exe) do set "QTBIN=%%~dp$PATH:I"
if defined QTBIN if not exist "!QTBIN!Qt6Multimedia.dll" set "QTBIN="

if not defined QTBIN if exist "A:\msys64\ucrt64\bin\Qt6Multimedia.dll" set "QTBIN=A:\msys64\ucrt64\bin"
if not defined QTBIN if exist "A:\msys2\ucrt64\bin\Qt6Multimedia.dll" set "QTBIN=A:\msys2\ucrt64\bin"
if not defined QTBIN if exist "A:\msys64\mingw64\bin\Qt6Multimedia.dll" set "QTBIN=A:\msys64\mingw64\bin"
if not defined QTBIN if exist "A:\msys2\mingw64\bin\Qt6Multimedia.dll" set "QTBIN=A:\msys2\mingw64\bin"
if not defined QTBIN if exist "C:\msys64\ucrt64\bin\Qt6Multimedia.dll" set "QTBIN=C:\msys64\ucrt64\bin"
if not defined QTBIN if exist "C:\msys64\mingw64\bin\Qt6Multimedia.dll" set "QTBIN=C:\msys64\mingw64\bin"
if not defined QTBIN if exist "A:\Qt\6.11.2\mingw_64\bin\Qt6Multimedia.dll" set "QTBIN=A:\Qt\6.11.2\mingw_64\bin"
if not defined QTBIN if exist "C:\Qt\6.11.2\mingw_64\bin\Qt6Multimedia.dll" set "QTBIN=C:\Qt\6.11.2\mingw_64\bin"
if not defined QTBIN for /d %%V in ("A:\Qt\6.*\mingw_64\bin" "C:\Qt\6.*\mingw_64\bin") do (
    if not defined QTBIN if exist "%%~V\Qt6Multimedia.dll" set "QTBIN=%%~V"
)

set "SEARCH=%HERE%build\player;%HERE%build\save;%HERE%build\ui;%HERE%;%HERE%..;"
if defined QTBIN (
    set "PATH=%SEARCH%%QTBIN%;%PATH%"
) else (
    echo [run_player] WARNING: Qt6 not found automatically; edit this script and set QTBIN.
    set "PATH=%SEARCH%%PATH%"
)

set "PLAYER=%HERE%build\player\player.exe"
if not exist "%PLAYER%" set "PLAYER=%HERE%player.exe"
if not exist "%PLAYER%" (
    echo [run_player] ERROR: player.exe not found. Build it first:
    echo [run_player]   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=A:/msys64/ucrt64
    echo [run_player]   cmake --build build
    pause
    exit /b 1
)

if "%~1"=="" (
    set "DEF=%HERE%..\demos\01_piano_nocturne.rcp"
    if exist "!DEF!" (
        echo [run_player] No score given, playing demo: !DEF!
        "%PLAYER%" "!DEF!"
        endlocal & exit /b %ERRORLEVEL%
    )
    "%PLAYER%" --help
    endlocal & exit /b 0
)

"%PLAYER%" %*
endlocal & exit /b %ERRORLEVEL%
