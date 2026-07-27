# Build Instructions

## Requirements
- X-Plane SDK (CHeaders)
- libcurl (development headers for macOS/Linux)
- CMake 3.15+ for manual `cmake -S/-B` builds
- CMake 3.23+ to use the included `CMakePresets.json`

## Artifacts & Layout
- Output names (set by CMake): `mac.xpl`, `lin.xpl`, `win.xpl`.
- Build directories now live outside the iCloud-synced source tree under `/Users/wahltho/dev/YAL Hoppiehelper/`.
- Typical output locations: `/Users/wahltho/dev/YAL Hoppiehelper/build-<plat>/mac.xpl`, `/Users/wahltho/dev/YAL Hoppiehelper/build-<plat>/lin.xpl`, `/Users/wahltho/dev/YAL Hoppiehelper/build-<plat>/win.xpl`.
- X-Plane loads the platform-specific file from `<X-Plane>/Resources/plugins/YAL_hoppiehelper/64/`.

## Common Requirements
- Export the SDK path once before configuring:
```bash
export XPLANE_SDK_PATH="/absolute/path/to/XPlane_SDK"
```
- The bundled presets read `XPLANE_SDK_PATH` from the environment.
- Manual `cmake -S/-B` invocations still work; point `-B` at `/Users/wahltho/dev/YAL Hoppiehelper/...`.

## Speed Tips (optional)
- Reuse build directories; only rerun `cmake -S ...` when CMake/toolchain settings change.
- Quick rebuilds with presets: `cmake --build --preset build-<plat>`
- If Ninja is available, add `-G Ninja` on macOS/Linux for faster incremental builds.
- For container cross-compiles, keep the same mount path (e.g. `/workspace`) and reuse the same `build-win*` directory to avoid CMake cache path mismatches.
- For container builds, create a local image once to avoid `apt-get` on every run:
```bash
podman build -t yal-xplane-build - <<'EOF2'
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build libcurl4-openssl-dev mingw-w64
EOF2
```
Then replace `ubuntu:22.04` with `yal-xplane-build` and drop the `apt-get ...` part in the container commands below.

## macOS (universal recommended)
```bash
cmake --preset build-mac
cmake --build --preset build-mac

# Universal binary (recommended):
cmake --preset build-mac-universal
cmake --build --preset build-mac-universal
```
Result: `/Users/wahltho/dev/YAL Hoppiehelper/build-mac/mac.xpl` or `/Users/wahltho/dev/YAL Hoppiehelper/build-mac-universal/mac.xpl`.

Manual equivalent:
```bash
cmake -S . -B "/Users/wahltho/dev/YAL Hoppiehelper/build-mac" -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH="$XPLANE_SDK_PATH"
cmake --build "/Users/wahltho/dev/YAL Hoppiehelper/build-mac" --config Release
```

## Linux (container recommended)
- Podman or Docker with an Ubuntu image.
- If the SDK lives next to the repo (`../SDKs/XPlane_SDK`), mount it into the container.
- On Apple Silicon, use `--platform=linux/amd64` (X-Plane Linux is x86_64).
- Keep the same container mount path (e.g. `/workspace`) when reusing `build-lin*` to avoid CMake cache path mismatches.
```bash
podman machine start   # once (if using Podman)
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace \
  -v "$(pwd)/../SDKs":/SDKs \
  -v "/Users/wahltho/dev/YAL Hoppiehelper":/build-root \
  -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y build-essential cmake ninja-build libcurl4-openssl-dev && \
  cmake -S . -B /build-root/build-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build /build-root/build-lin"
```
Result: `/Users/wahltho/dev/YAL Hoppiehelper/build-lin/lin.xpl`.

## Windows (Visual Studio 2022)
- VS 2022 Desktop C++ Workload.
- Uses WinHTTP on Windows (no libcurl required).
```powershell
set XPLANE_SDK_PATH=C:\path\to\XPlane_SDK
cmake --preset build-win-host
cmake --build --preset build-win-host
```
Result: `/Users/wahltho/dev/YAL Hoppiehelper/build-win-host/Release/win.xpl`.

## Windows (Cross-Compile via Container, optional)
- For CI or macOS/Linux hosts: use `mingw-w64` in an Ubuntu container.
- Keep the same container mount path (e.g. `/workspace`) when reusing `build-win*` to avoid CMake cache path mismatches.
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace \
  -v "$(pwd)/../SDKs":/SDKs \
  -v "/Users/wahltho/dev/YAL Hoppiehelper":/build-root \
  -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y cmake ninja-build mingw-w64 && \
  cmake -S . -B /build-root/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
    -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build /build-root/build-win"
```
Result: `/Users/wahltho/dev/YAL Hoppiehelper/build-win/win.xpl` (Ninja).

Quick rebuild (already configured in container; reuse the same build directory, e.g. `build-win2`):
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace \
  -v "/Users/wahltho/dev/YAL Hoppiehelper":/build-root \
  -w /workspace ubuntu:22.04 bash -lc "\
  cmake --build /build-root/build-win"
```

## Staging / Packaging
```bash
mkdir -p deploy/YAL_hoppiehelper/64

# macOS (universal recommended):
cp -f "/Users/wahltho/dev/YAL Hoppiehelper/build-mac-universal/mac.xpl" deploy/YAL_hoppiehelper/64/mac.xpl

# Linux:
cp -f "/Users/wahltho/dev/YAL Hoppiehelper/build-lin/lin.xpl" deploy/YAL_hoppiehelper/64/lin.xpl

# Windows (Ninja/Container):
cp -f "/Users/wahltho/dev/YAL Hoppiehelper/build-win/win.xpl" deploy/YAL_hoppiehelper/64/win.xpl
# Windows (Visual Studio Generator):
# cp -f "/Users/wahltho/dev/YAL Hoppiehelper/build-win-host/Release/win.xpl" deploy/YAL_hoppiehelper/64/win.xpl
```

## GitHub Release Workflow
- `.github/workflows/github-release.yml` packages the committed deploy artifacts into GitHub release assets.
- Release assets:
  - `YAL-HoppieHelper-<version>.zip`
  - `YAL-HoppieHelper-<version>-manifest.txt`
  - `YAL-HoppieHelper-<version>-manifest.json`
  - `YAL-HoppieHelper-<version>-checksums.txt`
- Rebuild and commit `deploy/YAL_HoppieHelper/64/*.xpl` before running the release workflow, because the workflow packages the binaries already stored in the repo.
