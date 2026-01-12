# YAL_hoppiehelper (C++)

This helper provides the HTTP POST layer for Hoppie ACARS and feeds the
`hoppiebridge/*` datarefs required by the Zibo FMC. YAL stays functional
without this helper; CPDLC simply remains inactive.

## Build

Requirements:
- X-Plane SDK (CHeaders)
- libcurl (development headers)
- CMake 3.15+

### Artifacts & Layout
- Output names (set by CMake): `mac.xpl`, `lin.xpl`, `win.xpl`.
- Typical output locations: `build-<plat>/mac.xpl`, `build-<plat>/lin.xpl`, `build-<plat>/win.xpl`.
- X-Plane loads the platform-specific file from `<X-Plane>/Resources/plugins/YAL_hoppiehelper/64/`.

### Common Requirements
- X-Plane SDK path: `-DXPLANE_SDK_PATH=../SDKs/XPlane_SDK` (absolute paths are fine; in containers use a container path).

### Speed Tips (optional)
- Reuse build directories; only rerun `cmake -S ...` when CMake/toolchain settings change.
- Quick rebuilds: `cmake --build build-<plat> --config Release`
- If Ninja is available, add `-G Ninja` on macOS/Linux for faster incremental builds.
- For container builds, create a local image once to avoid `apt-get` on every run:
```bash
podman build -t yal-xplane-build - <<'EOF'
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build libcurl4-openssl-dev mingw-w64
EOF
```
Then replace `ubuntu:22.04` with `yal-xplane-build` and drop the `apt-get ...` part in the container commands below.

### macOS (universal recommended)
```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH="../SDKs/XPlane_SDK"
cmake --build build-mac --config Release

# Universal binary (recommended):
cmake -S . -B build-mac-universal -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH="../SDKs/XPlane_SDK" -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build-mac-universal --config Release
```
Result: `build-mac/mac.xpl` or `build-mac-universal/mac.xpl`.

### Linux (container recommended)
- Podman or Docker with an Ubuntu image.
- If the SDK lives next to the repo (`../SDKs/XPlane_SDK`), mount it into the container.
- On Apple Silicon, use `--platform=linux/amd64` (X-Plane Linux is x86_64).
```bash
podman machine start   # once (if using Podman)
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/work -v "$(pwd)/../SDKs":/SDKs -w /work ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y build-essential cmake ninja-build libcurl4-openssl-dev && \
  cmake -S . -B build-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build build-lin"
```
Result: `build-lin/lin.xpl`.

### Windows (Visual Studio 2022)
- VS 2022 Desktop C++ Workload.
- Uses WinHTTP on Windows (no libcurl required).
```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DXPLANE_SDK_PATH=../SDKs/XPlane_SDK
cmake --build build-win --config Release
```
Result: `build-win/Release/win.xpl`.

### Windows (Cross-Compile via Container, optional)
- For CI or macOS/Linux hosts: use `mingw-w64` in an Ubuntu container.
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/work -v "$(pwd)/../SDKs":/SDKs -w /work ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y cmake ninja-build mingw-w64 && \
  cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
    -DXPLANE_SDK_PATH=/SDKs/XPlane_SDK && \
  cmake --build build-win"
```
Result: `build-win/win.xpl` (Ninja).

### Staging / Packaging
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

## Install

Create a plugin folder in X-Plane and copy the platform binary:

```
X-Plane 12/
  Resources/
    plugins/
      YAL_hoppiehelper/
        64/
          win.xpl  (Windows)
          mac.xpl  (macOS)
          lin.xpl  (Linux)
```

## Notes

- The helper reads the Hoppie logon from `YAL/hoppie/logon`.
- The FMC provides the callsign via `hoppiebridge/send_callsign`.
- The helper sets `hoppiebridge/comm_ready` and `laminar/B738/HBDR_ready`.
- Plugin signature (ID): `yal.hoppiehelper`
- Current plugin version: 1.0.0

## Debug Datarefs

The helper exposes status via YAL datarefs:

- `YAL/hoppie/debug_level` (0=off, 1=errors, 2=info, 3=debug)
- `YAL/hoppie/status`
- `YAL/hoppie/last_error`
- `YAL/hoppie/last_http`
- `YAL/hoppie/send_count`
- `YAL/hoppie/poll_count`
