#!/usr/bin/env bash
# IrisWinds build script for Windows (MSYS2 UCRT64) and Linux
set -euo pipefail

# Add MSYS2 UCRT64 toolchain to PATH if present
if [ -d "/ucrt64/bin" ]; then
    export PATH="/ucrt64/bin:/usr/bin:$PATH"
fi

cd "$(dirname "$0")"

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ACTION="${1:-build}"

sync_runtime() {
    echo "==> Syncing runtime DLLs and assets to build/..."
    if [ -d "build/dist/IrisWinds" ]; then
        cp -u build/dist/IrisWinds/*.dll build/ 2>/dev/null || true
        cp -u build/dist/IrisWinds/*.ttf build/ 2>/dev/null || true
        cp -u build/dist/IrisWinds/*.png build/ 2>/dev/null || true
        cp -u build/dist/IrisWinds/*.jpg build/ 2>/dev/null || true
        cp -u build/dist/IrisWinds/*.ini build/ 2>/dev/null || true
        [ -d "build/dist/IrisWinds/gamelib-src" ] && cp -ru build/dist/IrisWinds/gamelib-src build/ 2>/dev/null || true
        [ -d "build/dist/IrisWinds/swgen" ] && cp -ru build/dist/IrisWinds/swgen build/ 2>/dev/null || true
        [ -d "build/dist/IrisWinds/zig" ] && cp -ru build/dist/IrisWinds/zig build/ 2>/dev/null || true
    fi
    # Clean up redundant legacy / duplicate files in build directory
    rm -f "build/Savage Wind Configuration.exe" build/savagewind* build/build.bat
}

case "$ACTION" in
    build)
        echo "==> Building IrisWinds ($JOBS parallel jobs)..."
        make -j"$JOBS"
        sync_runtime
        echo "==> Build complete! Output in build/"
        ;;
    release)
        echo "==> Building IrisWinds Release (GUI mode, $JOBS parallel jobs)..."
        make RELEASE=1 -j"$JOBS"
        sync_runtime
        echo "==> Release build complete: build/iriswinds.exe"
        ;;
    dist)
        echo "==> Building and packaging IrisWinds distribution ($JOBS parallel jobs)..."
        make RELEASE=1 -j"$JOBS"
        python3 -c '
import os, shutil
b = "build"
d = "build/dist/IrisWinds"
os.makedirs(d, exist_ok=True)
shutil.copy2(f"{b}/iriswinds.exe", f"{d}/iriswinds.exe")
shutil.copy2(f"{b}/iriswinds-config.exe", f"{d}/IrisWinds Configuration.exe")
shutil.copy2(f"{b}/iriswinds.dll.a", f"{d}/iriswinds.dll.a")
if os.path.exists("IrisWinds.bat"): shutil.copy2("IrisWinds.bat", f"{d}/IrisWinds.bat")
if os.path.exists("docs/icon/icon_1024.png"): shutil.copy2("docs/icon/icon_1024.png", f"{d}/icon.png")
if os.path.exists("docs/icon/splash.jpg"): shutil.copy2("docs/icon/splash.jpg", f"{d}/splash.jpg")
# Remove any accidental duplicates or legacy names:
for f in ["iriswinds-config.exe", "savagewind.exe", "savagewind-config.exe", "Savage Wind Configuration.exe", "savagewind.dll.a", "build.bat"]:
    p = os.path.join(d, f)
    if os.path.exists(p):
        os.remove(p)
'
        sync_runtime
        echo "==> IrisWinds portable folder updated at build/dist/IrisWinds/"
        ;;
    clean)
        echo "==> Cleaning build artifacts..."
        make clean 2>/dev/null || rm -rf build/rt build/imgui build/link build/iriswinds* build/savagewind* build/dist
        echo "==> Clean complete."
        ;;
    *)
        echo "Usage: $0 [build|release|dist|clean]"
        echo "  build   : Build standard binaries (default)"
        echo "  release : Build release binaries (no console window)"
        echo "  dist    : Build and package release zip"
        echo "  clean   : Remove built objects"
        exit 1
        ;;
esac
