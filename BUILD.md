# Build Instructions

## Requirements
- X-Plane SDK (CHeaders)
- libcurl (development headers for macOS/Linux)
- CMake 3.15+

## Artifacts & Layout
- Output names (set by CMake): `mac.xpl`, `lin.xpl`, `win.xpl`.
- Typical output locations: `build-<plat>/mac.xpl`, `build-<plat>/lin.xpl`, `build-<plat>/win.xpl`.
- X-Plane loads the platform-specific file from `<X-Plane>/Resources/plugins/YAL_hoppiehelper/64/`.

## Common Requirements
- X-Plane SDK path: `-DXPLANE_SDK_PATH=../SDKs/XPlane_SDK` (absolute paths are fine; in containers use a container path).

## Speed Tips (optional)
- Reuse build directories; only rerun `cmake -S ...` when CMake/toolchain settings change.
- Quick rebuilds: `cmake --build build-<plat> --config Release`
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
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH="../SDKs/XPlane_SDK"
cmake --build build-mac --config Release

# Universal binary (recommended):
cmake -S . -B build-mac-universal -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH="../SDKs/XPlane_SDK" -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build-mac-universal --config Release
```
Result: `build-mac/mac.xpl` or `build-mac-universal/mac.xpl`.

## Linux (container recommended)
- Podman or Docker with an Ubuntu image.
- If the SDK lives next to the repo (`../SDKs/XPlane_SDK`), mount it into the container.
- On Apple Silicon, use `--platform=linux/amd64` (X-Plane Linux is x86_64).
- Keep the same container mount path (e.g. `/workspace`) when reusing `build-lin*` to avoid CMake cache path mismatches.
```bash
podman machine start   # once (if using Podman)
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace -v "$(pwd)/../SDKs":/SDKs -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y build-essential cmake ninja-build libcurl4-openssl-dev && \
  cmake -S . -B build-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build build-lin"
```
Result: `build-lin/lin.xpl`.

## Windows (Visual Studio 2022)
- VS 2022 Desktop C++ Workload.
- Uses WinHTTP on Windows (no libcurl required).
```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DXPLANE_SDK_PATH=../SDKs/XPlane_SDK
cmake --build build-win --config Release
```
Result: `build-win/Release/win.xpl`.

## Windows (Cross-Compile via Container, optional)
- For CI or macOS/Linux hosts: use `mingw-w64` in an Ubuntu container.
- Keep the same container mount path (e.g. `/workspace`) when reusing `build-win*` to avoid CMake cache path mismatches.
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace -v "$(pwd)/../SDKs":/SDKs -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y cmake ninja-build mingw-w64 && \
  cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
    -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build build-win"
```
Result: `build-win/win.xpl` (Ninja).

Quick rebuild (already configured in container; reuse the same build directory, e.g. `build-win2`):
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace -v "$(pwd)/../SDKs":/SDKs -w /workspace ubuntu:22.04 bash -lc "\
  cmake --build build-win"
```

## Staging / Packaging
```bash
mkdir -p deploy/YAL_hoppiehelper/64

# macOS (universal recommended):
cp -f build-mac-universal/mac.xpl deploy/YAL_hoppiehelper/64/mac.xpl

# Linux:
cp -f build-lin/lin.xpl deploy/YAL_hoppiehelper/64/lin.xpl

# Windows (Ninja/Container):
cp -f build-win/win.xpl deploy/YAL_hoppiehelper/64/win.xpl
# Windows (Visual Studio Generator):
# cp -f build-win/Release/win.xpl deploy/YAL_hoppiehelper/64/win.xpl
```
