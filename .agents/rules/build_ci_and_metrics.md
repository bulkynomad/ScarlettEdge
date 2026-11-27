---
description: Enforce Ninja zero-dirty incremental builds, PCH and link hygiene, deterministic Sleep-free CTests, clean-room checks, and honest retail-vs-synthetic status metrics when editing CMake, CI, scripts, or tests.
trigger: glob
globs: "**/CMakeLists.txt, **/*.cmake, **/CMakePresets.json, **/tools/*.py, **/.github/workflows/*.yml, **/tests/**/*.cpp, **/tests/**/*.h, **/docs/COMPATIBILITY.md"
---

# Build, CI, CTest & Metrics Truth Rules

1. **Incremental Build, PCH & Link Graph Hygiene (`CMakeLists.txt`):**
   - Preserve zero-work incremental builds: after `cmake --build build`, `ninja -C build -n` must report `ninja: no work to do` (never delete a target's primary Ninja output in `POST_BUILD` without matching `BYPRODUCTS`/import-lib rules).
   - Maintain `target_precompile_headers` for `<windows.h>`, `<d3d12.h>`, and common STL headers where appropriate, prefer forward declarations in `include/scarlettx/**` over heavy transitive `#include`s, and keep `d3d12`, `dxgi`, and `d3dcompiler` `PRIVATE` on `scarlettx_core`.
2. **Deterministic, Non-Intrusive CTests (`tests/**`):**
   - Never write temporary `.dll`, `.xvd`, `.ppm`, or save files directly into `SCARLETTX_BINARY_DIR` (`build/`) where CI `.zip` packaging can sweep them up; use isolated per-test temp directories and remove them on exit.
   - Any test calling `vfs::MapPackageDrive` (`DefineDosDeviceA`) must set `RESOURCE_LOCK "virtual_drive_g"`.
   - Never use `Sleep()` / `sleep_for()` spin loops or trigger monitor-hijacking fullscreen/modal UI loops in CTests; synchronize deterministically via Win32 events, `ID3D12Fence::SetEventOnCompletion`, or condition variables so the full suite completes in `< 5.0s`.
   - New HLE tests must exercise the real guest COM / `SCARLETTX_REAL_TITLE=1` path rather than adding synthetic `uint64_t`-handle test exports.
3. **Honest Metrics & Clean-Room Gate (`tools/generate_status.py`, `tools/check_cleanroom.py`):**
   - Never clamp `target = implemented` to manufacture `100%` badges, and always report **Retail Xbox ABI Exports & Verified COM Slots** separately from **Synthetic Test-Harness C Exports**.
   - Keep all tracked files and commit messages 100% compliant with `python tools/check_cleanroom.py` (including UTF-16LE decoding, case-insensitive trademark checks, zero prohibited extensions, zero host user paths, and full PR commit ranges).
