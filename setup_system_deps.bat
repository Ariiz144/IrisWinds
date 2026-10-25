@echo off
setlocal enabledelayedexpansion

echo =====================================================================
echo           IrisWinds Automated System Dependency Installer
echo =====================================================================
echo This script installs and configures all system-wide dependencies
echo required to build and run IrisWinds on Windows:
echo   - Git for Windows
echo   - MSYS2 environment (C:\msys64)
echo   - Clang/LLVM C/C++ compiler (UCRT64)
echo   - GNU Make and build tools
echo   - SDL3, FFmpeg, and ShaderC libraries
echo   - Python 3 runtime
echo   - System PATH configuration for ucrt64 and usr/bin
echo =====================================================================
echo.

:: 1. Check Winget availability
where winget >nul 2>nul
if %errorlevel% neq 0 (
    echo [WARNING] 'winget' command line tool was not detected.
    echo Please make sure Windows App Installer is updated from the Microsoft Store.
) else (
    echo [OK] Windows Package Manager (winget) detected.
)

:: 2. Check / Install Git
where git >nul 2>nul
if %errorlevel% equ 0 (
    echo [OK] Git is already available in PATH.
) else (
    echo [INFO] Installing Git for Windows via winget...
    winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
)

:: 3. Check / Install MSYS2
if exist "C:\msys64\usr\bin\bash.exe" (
    echo [OK] MSYS2 detected at C:\msys64.
) else (
    echo [INFO] Installing MSYS2 via winget...
    winget install --id MSYS2.MSYS2 -e --source winget --accept-package-agreements --accept-source-agreements
)

if not exist "C:\msys64\usr\bin\bash.exe" (
    echo [ERROR] MSYS2 was not found at C:\msys64.
    echo Please download and install MSYS2 manually from https://www.msys2.org/
    pause
    exit /b 1
)

:: 4. Install toolchain and libraries in MSYS2 UCRT64
echo.
echo [INFO] Updating MSYS2 packages and installing UCRT64 toolchains...
set "MSYS2_BASH=C:\msys64\usr\bin\bash.exe"

"%MSYS2_BASH%" -lc "pacman -Sy --noconfirm"
"%MSYS2_BASH%" -lc "pacman -S --needed --noconfirm make git mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-python"

if %errorlevel% neq 0 (
    echo [WARNING] Package installation completed with non-zero exit code. Retrying with basic toolchain...
    "%MSYS2_BASH%" -lc "pacman -S --needed --noconfirm make git mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-shaderc"
)

:: 5. Add MSYS2 UCRT64 and usr/bin to User PATH
echo.
echo [INFO] Ensuring MSYS2 UCRT64 is present in User PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$userPath = [Environment]::GetEnvironmentVariable('Path', 'User'); " ^
    "$addPaths = @('C:\msys64\ucrt64\bin', 'C:\msys64\usr\bin'); " ^
    "$modified = $false; " ^
    "foreach ($p in $addPaths) { " ^
    "    if ($userPath -notlike ('*' + $p + '*')) { " ^
    "        $userPath = $p + ';' + $userPath; " ^
    "        $modified = $true; " ^
    "    } " ^
    "}; " ^
    "if ($modified) { " ^
    "    [Environment]::SetEnvironmentVariable('Path', $userPath, 'User'); " ^
    "    Write-Host '[OK] Added MSYS2 UCRT64 to User PATH.'; " ^
    "} else { " ^
    "    Write-Host '[OK] MSYS2 UCRT64 is already configured in PATH.'; " ^
    "}"

echo.
echo =====================================================================
echo            System Dependencies Configured Successfully!
echo =====================================================================
echo You can now build IrisWinds by running:
echo     IrisWinds.bat
echo or:
echo     IrisWinds.bat release
echo =====================================================================
echo.
pause
