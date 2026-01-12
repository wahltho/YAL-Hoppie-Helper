# YAL_hoppiehelper (C++)

This helper provides the HTTP POST layer for Hoppie ACARS and feeds the
`hoppiebridge/*` datarefs required by the Zibo FMC. YAL stays functional
without this helper; CPDLC simply remains inactive.

## Build

Requirements:
- X-Plane SDK (CHeaders)
- libcurl
- CMake 3.15+

Example:

```
cmake -S . -B build -DXPLANE_SDK_PATH=/path/to/XPlaneSDK
cmake --build build
```

Output:
- Windows: `build/win.xpl`
- macOS: `build/mac.xpl`
- Linux: `build/lin.xpl`

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

## Debug Datarefs

The helper exposes status via YAL datarefs:

- `YAL/hoppie/debug_level` (0=off, 1=errors, 2=info, 3=debug)
- `YAL/hoppie/status`
- `YAL/hoppie/last_error`
- `YAL/hoppie/last_http`
- `YAL/hoppie/send_count`
- `YAL/hoppie/poll_count`
