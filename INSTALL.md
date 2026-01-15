# Install Guide

## Requirements
- X-Plane 12
- Zibo Mod (for FMC + datarefs)
- YAL plugin (optional if using autark mode)
- Hoppie logon code

## Install
1) Create the plugin folder:

```
X-Plane 12/
  Resources/
    plugins/
      YAL_hoppiehelper/
        64/
```

2) Copy the platform binary into `64/`:
- macOS: `mac.xpl`
- Linux: `lin.xpl`
- Windows: `win.xpl`

(You can take the files from `deploy/YAL_hoppiehelper/64` after a build.)

## Usage (YAL mode)
- Logon is read from `YAL/hoppie/logon`.
- Callsign is supplied by the FMC via `hoppiebridge/send_callsign`.
- The helper sets `hoppiebridge/comm_ready` and `laminar/B738/HBDR_ready`.

## Autark Mode (optional)
Autark mode is enabled when `Output/preferences/YAL_HoppieHelper.prf` exists.
When enabled, the helper ignores YAL logon/debug datarefs and uses values from
this file instead.

Supported keys:
- `logon=` (required for autark)
- `debug_level=` (0-3, optional)
- `poll_fast=` (optional; forces fast polling when truthy)
- `callsign=` (optional; applied if empty or previously set by prefs)

Example:
```
logon=YOURHOPPIELOGON
debug_level=2
poll_fast=1
callsign=DLH123
```

Remove the file to disable autark mode. Zibo tailnum gating still applies.

## Troubleshooting
- Check `Log.txt` for lines starting with `[YAL HoppieHelper]`.
- Ensure avionics are on and the callsign/logon are set.
- Increase debug level (YAL dataref or pref file) if needed.

## Uninstall
Remove `X-Plane 12/Resources/plugins/YAL_hoppiehelper`.
