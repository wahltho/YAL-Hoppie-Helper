# YAL Hoppie Helper (C++)

This helper provides the HTTP POST layer for Hoppie ACARS and feeds the
`hoppiebridge/*` datarefs used by the Zibo and LevelUp 737NG aircraft.
It installs X-Plane-wide under `Resources/plugins/YAL_HoppieHelper`.

YAL integration is optional. Autark mode works without YAL.

Docs:
- Install and deploy: `INSTALL.md`
- Complete Zibo ACARS/CPDLC setup, including the optional LevelUp FANS CDU
  patch: `ACARS_CPDLC_GUIDE.md`
- Build instructions: `BUILD.md`

GitHub releases are packaged from the committed deploy artifacts through `.github/workflows/github-release.yml` and publish a ZIP, text manifest, JSON manifest, and SHA-256 checksums file.

Local builds are configured to use `/Users/wahltho/dev/YAL Hoppiehelper/` as the build root so the source tree can stay in iCloud without putting CMake build directories there.

Current plugin version: 2.1
