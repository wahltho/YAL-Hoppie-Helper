# ACARS and CPDLC Setup for Zibo Mod

This guide covers the complete setup from a Hoppie's ACARS logon code to
weather requests and CPDLC in the Zibo Mod CDU. YAL is optional: the helper can
use either YAL's settings or its own preference file.

## Before You Start

You need:

- X-Plane 12 with a current Zibo Mod installation
- YAL HoppieHelper installed X-Plane-wide
- Internet access from the computer running X-Plane
- A personal Hoppie's ACARS logon code
- YAL only if you want to configure the code through YAL or use voice output

Three different identifiers are involved:

- **Hoppie's ACARS logon code:** the private access key sent to you by e-mail.
  Store it in YAL or `YAL_HoppieHelper.prf`; never use it as an aircraft
  callsign or enter it on the CDU ATC logon page.
- **Aircraft callsign / flight ID:** your flight identifier, for example
  `DLH123`. It should match the callsign used on your online network.
- **CPDLC facility:** the ATC station to which you log on, normally a
  four-letter identifier supplied by the controller. On the CDU this is the
  `LOGON TO` value.

## 1. Get a Hoppie's ACARS Logon Code

Register at the official
[Hoppie's ACARS registration page](https://www.hoppie.nl/acars/system/register.html).
The code is independent of your VATSIM or IVAO account. Keep it private.

Hoppie's ACARS separates VATSIM, IVAO and `None` into different network
namespaces. The network affiliation of your ACARS account must match the ATC
station you intend to use. You can review or change it on the official
[ACARS account page](https://www.hoppie.nl/acars/system/account.html).

If an old code has not been used for an extended period, it may have expired.
Request a new one through the same registration page if the server reports an
invalid logon.

## 2. Install YAL HoppieHelper

Extract the release so the plugin has this exact layout:

```text
X-Plane 12/
  Resources/
    plugins/
      YAL_HoppieHelper/
        64/
          mac.xpl
          lin.xpl
          win.xpl
```

Do not leave an additional `YAL_HoppieHelper` directory between the plugin
folder and `64`. X-Plane loads the binary for the current operating system and
ignores the other two.

After starting X-Plane, `Log.txt` should contain lines beginning with
`[YAL HoppieHelper]`, including `Starting plugin` and `Enabled v...`.

## 3. Configure the Logon Code

Choose exactly one of the following methods.

### Option A: Use YAL

1. Open `Plugins -> Yet Another Linda -> Settings`.
2. Enter the personal Hoppie's ACARS logon code in `Hoppie ID`.
3. Optionally enable `ATIS/CPDLC to Voice`.

Despite the setting's short label, `Hoppie ID` is the private ACARS logon code,
not the aircraft callsign.

Do not shorten the code. If the complete code cannot be entered in the YAL
field, use the standalone preference file below.

### Option B: Run Without YAL

Create this file in the active X-Plane preferences directory:

```text
X-Plane 12/Output/preferences/YAL_HoppieHelper.prf
```

Use this minimal content:

```ini
logon=YOUR_PRIVATE_HOPPIE_LOGON_CODE
debug_level=1
poll_fast=0
```

The file enables standalone mode. Normally, do not add `callsign=` because the
CDU supplies the flight ID. A `callsign=` entry is available only as a fallback
when the aircraft does not publish one.

If this preference file exists, its values take precedence over YAL's Hoppie
settings. Remove the file to return to YAL-based configuration. The helper
checks the file while X-Plane is running, but an aircraft reload is a useful
first step if the mode does not change as expected.

## 4. Enable Datalink in Zibo Mod

The recommended configuration uses the FANS CDU pages:

1. Open the Zibo EFB and go to Home page 2.
2. Open `SETTINGS -> OPTIONAL ACCESS -> FMS`.
3. Set `CMU` to `MK II+`.
4. Set `CDU` to `FANS MCDU`.
5. Confirm that `CPDLC` is set to `FANS`.
6. Select the online network you use under `ONLINE NETWORK`.
7. Set `METAR SOURCE` to `NETWORK` for the Hoppie's ACARS weather test below.

The classic `MCDU` with `CMU-900` and `ATN B1` also exposes datalink pages, but
the page layout and labels differ. The FANS configuration is used for all CDU
paths in the remainder of this guide.

Press the CDU `MENU` key. A `DLK` entry must be visible. If it is missing, check
the EFB `CMU` and `CPDLC` settings before troubleshooting the helper.

## 5. Set the Aircraft Callsign

Enter the flight number in the normal route setup or in the `FLT ID` field on
the ATC logon page. Use the same callsign as your network flight, without
spaces; for example, `DLH123`.

Power the aircraft avionics. Once the helper has the private logon code and the
CDU has supplied a flight ID, it performs an initial server poll. `Log.txt`
should then show entries such as:

```text
[YAL HoppieHelper] Logon available (...)
[YAL HoppieHelper] Callsign available: DLH123
[YAL HoppieHelper] Poll ok. Communication ready.
[YAL HoppieHelper] Comm ready.
```

The private logon code is not printed to the log.

## 6. Test ACARS with a METAR Request

A METAR request is the best initial connection test because it does not require
a live controller.

1. On the CDU, select `MENU -> DLK -> ACARS`.
2. Select `REQUESTS -> WEATHER REQ`.
3. Enter an ICAO airport code such as `EHAM` in `STA 1`.
4. Select `SEND`.
5. Open the new ACARS message when the CDU annunciates it.

Stored reports can also be found under
`ACARS -> MISC MENU -> RCVD MSGS`. Delivery normally takes a few seconds, but
Hoppie's ACARS is a polled store-and-forward service, so allow up to roughly one
minute before declaring the request failed.

For an ATIS request, use `ACARS -> REQUESTS -> ATIS`, enter the airport, select
arrival or departure service, and select `SEND`. Online ATIS availability
depends on the selected network and the stations currently publishing data. A
successful METAR with no ATIS response therefore does not by itself indicate a
helper fault.

## 7. Log On to CPDLC

CPDLC requires a compatible ATC station to be online. Do not use a random
facility merely to test the connection; use the identifier published or given
to you by the controller. Hoppie's official
[Stations Online](https://www.hoppie.nl/acars/system/online.html) page can help
verify that the facility is visible in the same network namespace as your
account.

1. Select `MENU -> DLK -> ATC`.
2. On `ATC INDEX`, select `LOGON/STATUS`.
3. Verify the `FLT ID`, origin and destination.
4. Enter the controller's facility identifier in `LOGON TO`.
5. Select `SEND LOGON`.
6. Wait for the state to progress from `LOGON IN PROGRESS` to `LOGGED ON`.

An accepted CPDLC logon establishes communication with that ATC unit. Incoming
messages produce an `ATC MSG` indication. Open the message, choose the
appropriate CDU response, verify it and send it. Use the CDU logoff function
when CPDLC service ends unless the controller transfers you to the next unit.

The personal Hoppie's ACARS logon code is never entered in `LOGON TO`; that
field contains the ATC facility identifier.

## 8. Optional Voice Output

YAL provides the voice function; YAL HoppieHelper only transports and publishes
the received message. To hear supported METAR, ATIS and CPDLC content:

1. Install and enable YAL.
2. Open `Plugins -> Yet Another Linda -> Settings`.
3. Enable `ATIS/CPDLC to Voice`.
4. Confirm that normal YAL speech output is audible.

Standalone helper mode works for CDU ACARS/CPDLC but does not speak messages by
itself.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No `[YAL HoppieHelper]` lines in `Log.txt` | Verify the plugin folder layout and that the correct platform binary is present in `64`. |
| `WAIT_YAL` | YAL datarefs are unavailable. Install/start YAL or create `Output/preferences/YAL_HoppieHelper.prf` for standalone mode. |
| `WAIT_LOGON` | The private Hoppie's ACARS logon code is empty. Check the selected configuration method. |
| `WAIT_CALLSIGN` | Enter a valid flight number / `FLT ID` in the CDU. |
| `WAIT_POLL` | The prerequisites exist, but no server exchange has succeeded yet. Wait for the initial poll, then check Internet access and the logon code. |
| `DLK` is absent or `KEY/FUNCTION INOP` appears | Recheck the Zibo EFB `CMU`, `CDU` and `CPDLC` selections. |
| METAR works but CPDLC does not | Verify the ATC facility identifier, network affiliation and that the controller's CPDLC station is online. |
| METAR works but ATIS returns no data | Try another staffed airport or network; ATIS data is not always available. |
| Messages appear on the CDU but are not spoken | Voice requires YAL and the `ATIS/CPDLC to Voice` setting. |
| Messages arrive under the wrong flight | Ensure no other ACARS client is using the same callsign and that the CDU flight ID matches your current network callsign. |

For deeper diagnosis, raise `debug_level` temporarily and inspect X-Plane's
`Log.txt` for `[YAL HoppieHelper]` entries. Return to level `1` afterward to
avoid unnecessary logging.

The official [Hoppie's ACARS FAQ](https://www.hoppie.nl/acars/system/faq.html)
explains account, callsign, network and online-station behavior in more detail.
