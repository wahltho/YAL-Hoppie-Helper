# YAL Hoppie Helper (C++)

This helper provides the HTTP POST layer for Hoppie ACARS and feeds the
`hoppiebridge/*` datarefs required by the Zibo FMC. YAL stays functional
without this helper; CPDLC simply remains inactive.

Autark modus without YAL available.

Docs:
- Install and deploy: `INSTALL.md`
- Build instructions: `BUILD.md`

Local builds are configured to use `/Users/wahltho/dev/YAL Hoppiehelper/` as the build root so the source tree can stay in iCloud without putting CMake build directories there.

Current plugin version: 2.1
