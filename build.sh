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
    echo "==> Syncing runtime DLLs and assets into build/..."
    mkdir -p build

    # Standardize configuration tool name
    if [ -f "build/iriswinds-config.exe" ]; then
        cp -f "build/iriswinds-config.exe" "build/IrisWinds Configuration.exe"
        rm -f "build/iriswinds-config.exe"
    fi

    # Sync scripts and branding assets directly into build/
    [ -f "IrisWinds.bat" ] && cp -u "IrisWinds.bat" "build/IrisWinds.bat" 2>/dev/null || true
    [ -f "setup_system_deps.bat" ] && cp -u "setup_system_deps.bat" "build/setup_system_deps.bat" 2>/dev/null || true
    [ -f "docs/icon/icon_1024.png" ] && cp -u "docs/icon/icon_1024.png" "build/icon.png" 2>/dev/null || true
    [ -f "docs/icon/splash.jpg" ] && cp -u "docs/icon/splash.jpg" "build/splash.jpg" 2>/dev/null || true

    # Auto-bundle all DLL dependencies directly into build/
    if [ -f "tools/list_dlls.py" ]; then
        python tools/list_dlls.py 2>/dev/null || true
    fi

    # Purge any redundant duplicate dist folder or legacy files
    rm -rf "build/dist"
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
        sync_runtime
        python -c '
import os, zipfile, glob

build_dir = "build"
zip_path = os.path.join(build_dir, "IrisWinds-0.3.5i-alpha-windows.zip")

# Files to include in the distribution
include_files = [
    "iriswinds.exe",
    "IrisWinds Configuration.exe",
    "IrisWinds.bat",
    "setup_system_deps.bat",
    "iriswinds.dll.a",
    "icon.png",
    "splash.jpg"
]

print("==> Packaging standalone distribution zip...")
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    # Add primary binaries and assets
    for name in include_files:
        src = os.path.join(build_dir, name)
        if os.path.isfile(src):
            z.write(src, arcname=f"IrisWinds/{name}")
            print(f"  + IrisWinds/{name}")
        else:
            print(f"  ! Missing file: {name}")

    # Add all bundled DLLs
    for dll in sorted(glob.glob(os.path.join(build_dir, "*.dll"))):
        dll_name = os.path.basename(dll)
        z.write(dll, arcname=f"IrisWinds/{dll_name}")
        print(f"  + IrisWinds/{dll_name}")

    # Add any fonts or ini files if present
    for extra in sorted(glob.glob(os.path.join(build_dir, "*.ttf")) + glob.glob(os.path.join(build_dir, "*.ini"))):
        extra_name = os.path.basename(extra)
        z.write(extra, arcname=f"IrisWinds/{extra_name}")
        print(f"  + IrisWinds/{extra_name}")

zip_size_mb = os.path.getsize(zip_path) / (1024 * 1024)
print(f"==> Package created successfully: {zip_path} ({zip_size_mb:.2f} MB)")
'
        # Ensure no duplicate dist folder remains on disk
        rm -rf "build/dist"
        echo "==> Build consolidated! Only single output folder exists: build/"
        ;;
    clean)
        echo "==> Cleaning build artifacts..."
        make clean 2>/dev/null || rm -rf build/rt build/imgui build/inc build/link build/iriswinds* build/savagewind* build/dist
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
