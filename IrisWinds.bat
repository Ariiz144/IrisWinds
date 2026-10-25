@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

:: Check for MSYS2 bash installation
set "BASH_PATH="
if exist "C:\msys64\usr\bin\bash.exe" (
    set "BASH_PATH=C:\msys64\usr\bin\bash.exe"
) else (
    where bash >nul 2>nul
    if %errorlevel% equ 0 (
        for /f "tokens=*" %%i in ('where bash') do (
            if not defined BASH_PATH set "BASH_PATH=%%i"
        )
    )
)

if not defined BASH_PATH (
    echo [ERROR] MSYS2 bash was not found in C:\msys64\usr\bin\bash.exe or PATH.
    echo Please install MSYS2 from https://www.msys2.org/ to compile IrisWinds.
    pause
    exit /b 1
)

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

echo =====================================================================
echo                     IrisWinds Build Tool
echo =====================================================================
echo Using bash: %BASH_PATH%
echo Action:     %ACTION%
echo.

if not exist "build.sh" (
    if exist "iriswinds.exe" (
        echo Starting IrisWinds...
        start "" "iriswinds.exe"
        exit /b 0
    )
)

if /i "%ACTION%"=="run" (
    if exist "iriswinds.exe" (
        echo Starting IrisWinds...
        start "" "iriswinds.exe"
        exit /b 0
    )
    if not exist "build\iriswinds.exe" (
        echo [INFO] iriswinds.exe not found. Compiling first...
        "%BASH_PATH%" -c "export PATH=/ucrt64/bin:/usr/bin:$PATH; ./build.sh build"
        if %errorlevel% neq 0 (
            echo [ERROR] Build failed!
            pause
            exit /b %errorlevel%
        )
    )
    echo Starting IrisWinds...
    start "" "build\iriswinds.exe"
    exit /b 0
)

"%BASH_PATH%" -c "export PATH=/ucrt64/bin:/usr/bin:$PATH; ./build.sh %ACTION%"
set "BUILD_STATUS=%errorlevel%"

if %BUILD_STATUS% equ 0 (
    echo.
    echo =====================================================================
    echo                 Build finished successfully!
    echo =====================================================================
    echo Binaries are available in the build\ directory.
    if "%~1"=="" (
        echo Press any key to exit...
        pause >nul
    )
) else (
    echo.
    echo [ERROR] Build failed with exit code %BUILD_STATUS%.
    pause
)

exit /b %BUILD_STATUS%
