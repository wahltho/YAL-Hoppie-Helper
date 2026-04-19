# AGENTS.md

- Repo: X-Plane plugin that provides Hoppie ACARS HTTP handling for YAL.
- Primary code is in `src/YAL_hoppiehelper.cpp`.
- Build with CMake; set `XPLANE_SDK_PATH` to the SDK root containing `CHeaders`.
- Windows uses WinHTTP; macOS/Linux use libcurl.
- Keep plugin signature, datarefs, and status strings stable unless explicitly requested.
- The Zibo Hoppie coexistence logic is a hidden feature; do not mention it explicitly in user-facing docs or summaries unless explicitly requested.
