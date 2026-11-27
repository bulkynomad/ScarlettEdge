# ScarlettEdge Compatibility Matrix

> **Clean-Room Disclaimer:** ScarlettEdge ships **zero** copyrighted games, SDK binaries, or encryption keys. Every commercial retail entry below must be verified using a legally obtained, user-supplied dump kept strictly outside this repository. See [CLEANROOM_POLICY.md](../CLEANROOM_POLICY.md).

## Status Tiers

| Tier | Meaning |
| :--- | :--- |
| **`Inspectable`** | `scarlettx inspect` / `compat` parses `MicrosoftGame.Config`, PE64 imports, and `.msixvc`/`.xvc` container metadata. |
| **`Loads`** | PE64 maps into memory, IAT/HLE/TLS/VFS initialize, and guest `main()` begins executing. |
| **`Intro/Menus`** | Reaches title screen or menus with D3D12 `PresentX` output, audio, and `GameInput` navigation. |
| **`In-Game`** | Enters active 3D/2D gameplay with rendering and input (may have minor visual/audio/perf issues). |
| **`Playable`** | Full-speed gameplay, audio, input, and persistent `XGameSave` round-trip with 0 D3D12 debug errors or hangs. |

---

## Verified Official GDK `2510.5.6322` Hardware Suites (Out-of-Repo Local Bring-Up)

| Suite / Sample | GDK / Silicon Features Exercised | Hardware | Status | Notes |
| :--- | :--- | :--- | :---: | :--- |
| **GDK 2510 Core Bring-Up** (`AtgScarlettBringup.exe`) | `InitializeApiImplEx2`, `XSystem`, `XSystemAnalytics`, `XGameSave`, `XPackage`, `XStore`, `PresentX` | NVIDIA GeForce RTX 5070 Ti | **`Playable`** | 0 D3D12 debug errors; also verified inside authentic `makepkg` `.msixvc` + `.cik` |
| **GDK 2510 Gen9 Silicon Suite** (`AtgGen9SiliconSuite.exe`) | Hardware DXR 1.1 (`DispatchRays`), SM 6.5 Mesh Shaders (`DispatchMesh`), DirectStorage, `XDisplay`, `XPersistentLocalStorage`, `XNetworking`, `XPackage` Intelligent Delivery | NVIDIA GeForce RTX 5070 Ti | **`Playable`** | 90/90 frames, `--debug-layer` clean (`0` errors), `flight_summary.json` `CLEAN_RUN` |
| **GDK 2510 Phase 4 Multi-Subsystem Suite** (`AtgPhase4MultiSuite.exe`) | DirectStorage (`dstorage_xs`), `XGameSave` write/read round-trip, `XGameSaveFiles`, `XPersistentLocalStorage`, `GameInput`, `XAudio2` 48 kHz PCM voice, D3D12.2 SM 6.6 `ResourceDescriptorHeap[]` + `ExecuteIndirect` | NVIDIA GeForce RTX 5070 Ti | **`Playable`** | 30/30 SM 6.6 `ExecuteIndirect` GPU frames, `--debug-layer` clean (`0` errors), `scarlettx triage` `CLEAN_RUN` |
| **GDK 2510 Phase 4 Deep Stress Suite** (`AtgPhase4DeepSuite.exe`) | 4x MSAA RT + hardware `ResolveSubresource` + readback, Track 4G `T:\` scratch + `D:\` alias + `FindFirstFileW(G:\*)` + 8-core `GetSystemInfo` + `SetThreadAffinityMask` clamping, 4-thread `XTaskQueue` (64/64) + `XGameSave` (16/16) + `XPackage` slots 37/38 mount (16/16) | NVIDIA GeForce RTX 5070 Ti | **`Playable`** | 30/30 4x MSAA resolve frames (`--debug-layer` clean, `0` errors), `scarlettx triage` `CLEAN_RUN` (`0` misses/crashes/hangs/DRED) |

---

## Commercial Retail Titles (User-Supplied Legal Dumps)

| Title | Engine / GDK | Hardware | Status | Notes |
| :--- | :--- | :--- | :---: | :--- |
| **Concept Destruction** | Unity (Gen9) | Upstream ScarlettX baseline + ScarlettEdge | **`Playable`** *(upstream baseline; Phase 4D re-verification pending user dump)* | Upstream ScarlettX achieved full-speed gameplay; ScarlettEdge Wave 1 Flight-Recorder validation queued in Track 4D |
| **Unicorn Overlord** | Custom 2D/3D (Gen9) | Upstream ScarlettX baseline + ScarlettEdge | **`Playable`** *(upstream baseline; Phase 4D re-verification pending user dump)* | Upstream ScarlettX verified; ScarlettEdge Wave 1 Flight-Recorder validation queued in Track 4D |

---

## Synthetic Clean-Room CI Verification Suites (Built In-Repo)

| Target | Purpose | CI Status |
| :--- | :--- | :---: |
| `hello_scarlett.exe` | End-to-end PE64 boot, TLS, `xgameruntime`, D3D12 `PresentX`, `flight_recorder` | **Passing (CI)** |
| `rt_showcase.exe` | Hardware/WARP DXR 1.1 BLAS/TLAS + `DispatchRays` pipeline | **Passing (CI)** |
| `ue5_slice.exe` | Unreal-style Gen9 DXR + Mesh Shader + DirectStorage + `XGameSave` vertical slice | **Passing (CI)** |
| `openworld_slice.exe` | Open-world Wave32 XS2 compute + 2 MB `VirtualAlloc2` alias + GDeflate streaming vertical slice | **Passing (CI)** |
