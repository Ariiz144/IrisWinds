# IrisWinds 3.7i

IrisWinds is a Windows-ready fork of [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp), based on its `test` branch. It keeps the upstream Alpha 3 build pipeline and compatibility targets while carrying the upstream rendering, cache, and CPU/GPU scheduling updates.

## Start the game

1. Extract the IrisWinds package to a folder you can write to, such as Desktop or Documents. Avoid `Program Files`.
2. Start `iriswinds.exe`. For settings and game-file setup later, open `IrisWinds Configuration.exe`.
3. On first setup, select the folder containing your own legally obtained Breath of the Wild 1.9.0 files (`exefs` and `romfs`). IrisWinds copies the required files into its folder and builds the game code on your computer.

The package contains the application and its builder tools. It does not contain game files, saves, or personal data. Keep your game files and saves in your own IrisWinds folder. The log is `iriswinds.log` beside the executable.

The package includes the IrisWinds start-screen artwork as `splash.jpg`. You can replace that image with your own.

## Hardware and graphics

Windows 10 or later, a CPU with SSE4.2, a Vulkan 1.3 driver, and 16 GB RAM are required for this target setup. The game-code build uses the upstream x86-64-v3 target when supported and retains the x86-64-v2 compatibility target for older CPUs. The upstream `test` branch also includes its updated per-device Vulkan pipeline cache handling and CPU/GPU overlap work.

This package has not been visually tested with your game files on the target laptop. The branch contains renderer changes, but artifact-free gameplay and a particular frame rate should be confirmed on the actual device.

On Intel Windows graphics, this build defaults to the conservative Vulkan setup from issue #85's safe mode. This avoids maintenance5, graphics pipeline libraries, and attachment feedback loops; it can reduce performance. Other GPUs and platforms keep the upstream defaults.

## Build from source (Windows)

Use an MSYS2 UCRT64 shell after running the documented setup script:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
```

Then, from the MSYS2 UCRT64 shell with its UCRT64 and `usr/bin` directories on `PATH`:

```sh
make deps-windows
make gen-helper
tools/fetch_zig.sh
make dist
```

`make dist` creates `build/dist/IrisWinds-<version>-windows.zip`. The archive includes the runtime dependencies, launcher, configuration program, generator, slim Zig toolchain, required headers, and fonts. It excludes game files and saves. See [docs/BUILD-windows.md](docs/BUILD-windows.md) for details.

## Upstream and licensing

IrisWinds is derived from Savage Wind Recomp and retains its upstream notices and licenses. See [LICENSE](LICENSE) and the upstream project for source history and third-party component notices.
