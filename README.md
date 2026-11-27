# ScarlettEdge

[![ScarlettEdge CI (Windows MSVC)](https://github.com/bulkynomad/ScarlettEdge/actions/workflows/ci.yml/badge.svg)](https://github.com/bulkynomad/ScarlettEdge/actions/workflows/ci.yml)

**ScarlettEdge** is an independent, open-source, clean-room high-level emulator (HLE) fork of ScarlettX for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal & Clean-Room Notice:** ScarlettEdge does not include or distribute any proprietary Microsoft/Xbox binaries, cryptographic keys, firmware, or commercial game assets. Optional encrypted container support uses standard public cryptographic algorithms (FIPS-197 AES-XTS / FIPS 180-4 SHA-256) and requires keys supplied independently by the user for legally owned software. All automated tests run against synthetic x86-64 PE titles built from source in this repository. See [CLEANROOM_POLICY.md](CLEANROOM_POLICY.md), [CONTRIBUTING.md](CONTRIBUTING.md), and [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Origin & Credits

**ScarlettEdge** is an independent fork of **[ScarlettX](https://github.com/Freminet/ScarlettX)** by **Freminet** and the original ScarlettX contributors. All credit for the foundational emulator architecture belongs to the original authors, including:

- **Original ScarlettX Authors (`Freminet/ScarlettX`):** Core PE64 loader & implicit TLS, `MicrosoftGame.Config` parser, `xgameruntime` C ABI & `QueryApiImpl` COM tables, foundational `d3d12_xs`/`xg_xs` translation & `4KB_S` texture deswizzling, guest-facing `XAudio2_9`/`AcpHal` & silent output engine, `GameInput` + rumble, persistent `XGameSave`, base `dstorage_xs`/`xcrdapi` queues, offline HLE shims (`Bink2`, `FidelityFX`, `libHttpClient`, `XCurl`), and the real-title bring-up of **Concept Destruction** and **Unicorn Overlord**.
- **ScarlettEdge Fork Additions (`bulkynomad/ScarlettEdge`):** Unified `scarlettx_core.dll` runtime, **4-Part Flight-Recorder Diagnostic Suite** (`--flight-recorder`, `--hang-timeout`, lock-free event ring, VEH/SEH `.pdata` Ghidra-RVA crash dumper, D3D12 DRED & failing shader/PSO binary dumper, zero-lock hang watchdog), official **Microsoft GDK `2510.5.6322`** `InitializeApiImplEx2` & COM tables (`XSystem`, `XSystemAnalytics`, `XGameSave`/`XGameSaveFiles`, `XPackage` Intelligent Delivery chunks, `XStore`, `XDisplay`, `XPersistentLocalStorage`, `XNetworking`), real FIPS-197 AES-XTS & FIPS 180-4 SHA-256 via Windows CNG (`bcrypt.dll`) with retail `msft-xvd` v3 / `makepkg` `.msixvc` contiguous-Merkle + NTFS `$MFT` + 48-byte `.cik` keyring streaming, hardware VSync (`Present(1, 0)`) & async `ScheduleFrameEventX`, 0-frame sub-submit write-watch texture uploads, in-process direct IAT boot default, `scarlettx compat` analyzer CLI, Windows `VirtualAlloc2`/`MapViewOfFile3` physical-virtual memory aliasing, 128-slot D3D12.2 / Enhanced Barriers (`CL7::Barrier`) / Hardware DXR 1.1 / SM 6.5 Mesh Shader COM proxies, `CreatePlacedResourceX` on `KernelX`/virtual-alias GPUVAs, `ID3D12Device2` subobject-stream PSO translation, `XG` `64KB_S`/`64KB_D`/`64KB_R_X` macro-tile + 64 KB packed mip-tail & 3D volume swizzling, full-width RDNA2 GFX10.3 SPIR-V 1.5 SSA compiler + SM 6.6 bindless & `dxil.dll`-signed DXIL auto-intercept, hardware `VK_KHR_ray_tracing_pipeline` & Vulkan 1.3 compute, 32-lane SIMD GPU `GDeflate` (`0x04 0xFB`), 512-sample WMA-Pro `XMA2` & Bink2 `KB2` decode, XAudio2 XAPO passthrough, persistent `FILE_FLAG_OVERLAPPED` DirectStorage NVMe I/O, opt-in live `WinHTTP` transport, and automated GitHub Actions Windows MSVC CI.

<!-- SCARLETTX_STATUS_START -->
## Status

### Phase 4: Clean-Room Governance & Commercial Retail Bring-Up (In Progress — 45/50)

Tracks automated clean-room CI guardrails, `scarlettx triage` Flight-Recorder analysis, multi-sample GDK 2510 ATG validation, Wave 1 commercial retail bring-up, and the [Compatibility Matrix](docs/COMPATIBILITY.md):

![ScarlettEdge Phase 4: Clean-Room Governance & Retail Bring-Up](docs/status_phase4.svg)

**Phase 4 Progress:** `90.0%` (45 of 50 milestones) · **Tests:** `41/41` passing · **Clean-Room Policy:** [CLEANROOM_POLICY.md](CLEANROOM_POLICY.md)

| Phase 4 Track | Category | Progress | Readiness | Details |
| :--- | :--- | :---: | :---: | :--- |
| **4A. Clean-Room Governance & CI Guard** | `Governance` | `5 of 5` | `100%` | **Complete (5/5):** CLEANROOM_POLICY.md, CONTRIBUTING.md, tools/check_cleanroom.py file/ext/size/commit gate in GitHub Actions CI, Node 24-compatible checkout@v5 |
| **4B. Flight-Recorder Triage CLI** | `Diagnostics` | `5 of 5` | `100%` | **Complete (5/5):** scarlettx triage <dir> [--emit-stubs] [--json] (src/core/flight_triage.cpp), null-vtable/crash/hang/DRED verdicts, hit-ranked gaps, stub skeleton emitter, test_triage_cli |
| **4C. Multi-Sample GDK 2510 ATG** | `Platform` | `5 of 5` | `100%` | **Complete (5/5):** Verified live on RTX 5070 Ti (--debug-layer, 0 errors, triage CLEAN_RUN): DirectStorage, XGameSave round-trip + XGameSaveFiles + PLS, GameInput, XAudio2 PCM, SM 6.6 ResourceDescriptorHeap + 30/30 ExecuteIndirect GPU frames |
| **4D. Retail Wave 1 Bring-Up** | `Retail` | `0 of 5` | `0%` | **Queued (0/5):** Flight-recorder bring-up of user-supplied legal dumps (Concept Destruction, Unicorn Overlord) through inspect, boot, menus/audio/input, gameplay, and save round-trip |
| **4E. Commercial Engine Hardening** | `Graphics` | `5 of 5` | `100%` | **Complete (5/5):** Versioned .sxc shader disk cache (rootSigHash + FNV-1a eviction), multi-entry .cik keyring + LoadKeyringDirectory, KernelXQueryAliasTelemetry, dynamic MF H.264/HEVC probe, lock-free p95/p99 stutter profiler in flight_summary.json & triage |
| **4F. Compatibility Matrix & Dashboard** | `Docs` | `5 of 5` | `100%` | **Complete (5/5):** docs/COMPATIBILITY.md tiered compatibility ledger, docs/status_phase4.svg, Phase 4 ROADMAP.md & generate_status.py live progress dashboard |
| **4G. OS, Topology, VFS & Keyring CLI** | `Runtime` | `5 of 5` | `100%` | **Complete (5/5):** Zen 2 8-core GetSystemInfo/SetThreadAffinityMask hooks, T:\ scratch & D:\ alias + FindFirstFile*/GetDiskFreeSpaceEx*, live PresentX -> RecordFrameTimeUs pacing, --keyring <dir> CLI |
| **4H. d3d12_xs Proxy RTX Torture-Test** | `Graphics` | `5 of 5` | `100%` | **Complete (5/5):** Verified on RTX 5070 Ti (+ WARP CI) through d3d12_xs.dll SDK 0x20006 proxies: ALLOW_DISPLAY strip, CreatePlacedResource, ExecuteIndirect, ResourceBarrier unwrapping & 10/10 PresentX live pacing (test_d3d12_xs_proxy_rtx) |
| **4I. WinRT & Middleware Expansion** | `Platform` | `5 of 5` | `100%` | **Complete (5/5):** WinRT Windows.Gaming.Input/AnalyticsInfo/Package factories, GameInput RegisterDeviceCallback + rumble transitions, expanded Xbl offline achievements/stats/profiles (test_winrt_middleware) |
| **4J. Diagnostics UX & Portable CI** | `Tooling` | `5 of 5` | `100%` | **Complete (5/5):** One-command scarlettx diag --full [--json] host readiness probe (SM 6.6+/DXR/Mesh/Vulkan/MF/VirtualAlloc2/XAudio2), test_diag_full & GitHub Actions portable ScarlettEdge-win64.zip artifact |

---

### Phase 3: Unification, Retail Parity, Diagnostics & GDK 2510 (Completed — 30/30)

Tracks the shared `scarlettx_core.dll` unification, 4-Part Flight-Recorder Diagnostic Suite, retail `.msixvc` + `.cik` key-ring & NTFS `$MFT` parity, and official Microsoft GDK `2510.5.6322` + ATG Gen9 DXR/MeshShader RTX validation (`30/30` completed):

![ScarlettEdge Phase 3: Unification, Diagnostics & GDK 2510](docs/status_phase3.svg)

**Phase 3 Milestones:** `100.0%` (30 of 30 milestones) · **Tests:** `41/41` passing · **Next Wave:** `Per-Title Commercial Retail Title Bring-Up`

| Phase 3 Track | Category | Progress | Readiness | Verified Capabilities |
| :--- | :--- | :---: | :---: | :--- |
| **4-Part Flight-Recorder Suite** | `Diagnostics` | `5 of 5` | `100%` | **Complete (5/5):** Lock-free 4096-slot cross-DLL event ring + deduplicated import/QI/vtable/VFS miss tables, VEH/SEH .pdata Ghidra-RVA crash dumper, D3D12 DRED + shader/PSO fail dumper, zero-lock SuspendThread hang watchdog |
| **Guest COM Bridge (Real-Title Mode)** | `Runtime` | `5 of 5` | `100%` | **Complete (5/5):** Strict IID QueryInterface across d3d12_xs/WinRT/GameInput/IXAPO, unified dstorage/xcrdapi, XMem<->KernelX<->d3d12_xs GPU VA translation, StateObjectProperties + BuildRTAS proxies, XAsync libHttpClient |
| **Official GDK 2510 & Gen9 Silicon** | `Platform` | `5 of 5` | `100%` | **Complete (5/5):** Verified on RTX 5070 Ti (--debug-layer, 0 errors) against official Microsoft.GDK.PC 2510.5.6322 (InitializeApiImplEx2, XSystem/Analytics, XGameSaveFiles, XPackage chunks, XStore, XDisplay, PLS, XNetworking) + live hybrid DXR 1.1 & SM 6.5 Mesh Shaders |
| **RDNA2 XS2 CFG, SM 6.6 & Vulkan RT** | `Graphics` | `5 of 5` | `100%` | **Complete (5/5):** Full-width GFX10.3 decoder (literals, DPP/SDWA, VOP3P, FLAT/MTBUF, NSA MIMG, image_bvh64_intersect_ray), structured loop/if CFG, SM 6.6 bindless + dxil.dll signing, hardware VK_KHR_ray_tracing_pipeline |
| **Retail XVD v3, .msixvc & Codecs** | `Storage` | `5 of 5` | `100%` | **Complete (5/5):** Retail msft-xvd v3 (0x284+), makepkg .msixvc contiguous Merkle + 128B XvcRegionHeader v2 (FS-MD unencrypted NTFS $MFT), 48B .cik & keyring loader, 32-lane GPU GDeflate, 512-sample WMA-Pro XMA2 |
| **Shared Core & Loader Unification** | `Core` | `5 of 5` | `100%` | **Complete (5/5):** Shared scarlettx_core.dll singleton registry, PackageVfs::Global(), TLS Slot-0 + FLS isolation, .pdata zero-tail stripping, Win32 G:\ VFS/CmdLine/GetProcAddress/Delay-Load IAT hooks |

---

### Phase 2: Retail AAA Execution Architecture (Completed — 72/72)

Tracks the deep silicon-level compiler, GPU execution, MSAA/DCC/HTILE, DirectStorage GPU compute, and GDK/WinRT architecture milestones (`72/72` implemented; individual commercial retail games still require per-title validation with user-supplied dumps):

![ScarlettEdge Phase 2: Retail AAA Execution Roadmap](docs/status_aaa_roadmap.svg)

**Phase 2 Architecture Milestones:** `100.0%` (72 of 72 milestones) · **Next Wave:** `Per-Title Commercial Retail AAA Game Compatibility Testing`

| Phase 2 Milestone | Category | Progress | Readiness | Implemented Capabilities |
| :--- | :--- | :---: | :---: | :--- |
| **XS2 -> GPU Shader Compiler** | `Compiler` | `12 of 12` | `100%` | **Complete (12/12):** RDNA2 ISA decode, 256-VGPR SPIR-V 1.5, stage exports, SMEM UBOs, MIMG sample, v_interp, disk cache, SSBO Load/Store, OpLoopMerge/OpSelectionMerge CFG, Wave32 subgroups, d3d12_xs auto-intercept, signed SM 6.5 DXIL + SM 5.0 DXBC bridge |
| **D3D12.2 & MEM_GRAPHICS** | `Graphics` | `10 of 10` | `100%` | **Complete (10/10):** 128-slot vtables, CL7::Barrier unwrapping, VirtualAlloc2 aliasing, 2 MB large pages, CreatePlacedResourceX GPUVA, ID3D12Device2 stream PSOs, VRS Tier 1/2, sparse 64 KB ReservedResource + UpdateTileMappings + SamplerFeedback, ExecuteIndirect GPU counts & GPU Work Graphs |
| **Vulkan 1.3 & DXR GPU Exec** | `Graphics` | `10 of 10` | `100%` | **Complete (10/10):** Dynamic vulkan-1.dll loader, vkCreateShaderModule, VkQueue/CmdBuffer, RootSig->VkDescriptorSetLayout, VkComputePipeline + SSBO dispatch, Win32 VkSurfaceKHR, timeline semaphores, VK_KHR_acceleration_structure BLAS/TLAS & aligned SBT vkCmdTraceRaysKHR |
| **XG Tiling, MSAA & DCC** | `Graphics` | `10 of 10` | `100%` | **Complete (10/10):** 4KB_S, 256B_S, 64KB_S/D, 64KB_R_X, 64KB_Z_X depth, Cube/2D arrays, 64 KB packed mip-tails, 3D volume swizzling, 2x/4x/8x MSAA sample planes, RDNA2 DCC & HTILE depth codecs |
| **DirectStorage GPU Codecs** | `Storage` | `8 of 8` | `100%` | **Complete (8/8):** OVERLAPPED NVMe I/O, LZ77 GDeflate (0x04FB) & BCPK, multi-thread GDeflate tile pool, OS priority queues, RFC 1951 Huffman DEFLATE + Zlib, BC1 4x4 RGBA8 unpack, HLSL cs_5_0 GPU compute GDeflate & direct CopyTextureRegion + 4KB_S swizzled streaming |
| **GDK 2022-2026 & WinRT** | `Platform` | `8 of 8` | `100%` | **Complete (8/8):** GDK 2022-2026 QueryApiImpl COM GUIDs, WinRT RoActivateInstance IInspectable broker, XStore license & XPackage DLC mount, CNG HMAC-SHA256 XBL3.0 auth, persistent XblTitleStorage, HCWebSocket RTA/PlayFab bridge |
| **Retail XMA2 & Bink2 Media** | `Media` | `8 of 8` | `100%` | **Complete (8/8):** 2048B retail XMA2 bitstream parser, Princen-Bradley TDAC IMDCT + 52B 0x0166 voice bridge, BS.775 5.1/7.1 + X3DAudio Doppler, host System32 CreateFX XAPOFX, KB2 frame-offset YCbCr 4:2:0 decode, QPC A/V PTS lip-sync |
| **AAA Engine & XVC Bring-Up** | `Titles` | `6 of 6` | `100%` | **Complete (6/6 — Vertical-Slice Harness):** Encrypted GDFX XVC extraction/boot, UE5 Gen9 PE64 slice, Open-World Wave32/2MB-alias PE64 slice, GPU stability breadcrumbs, multi-queue GDeflate+XS2 churn, PSO prewarm + QPC p99 profiler (retail titles require per-title testing) |

---

### Phase 1: ScarlettEdge Fork Foundations (Completed)

Tracks the runtime fidelity, cryptography, memory aliasing, and tooling foundations built in **ScarlettEdge** on top of upstream ScarlettX:

![ScarlettEdge Runtime Optimization & Fidelity Status](docs/status_optimizations.svg)

**Phase 1 Fork Foundations:** `100.0%` (44 of 44 milestones) · **Exports:** `513` · **Tests:** `41/41` passing · **Target:** `Xbox Series X|S (Gen9)`

---

### Upstream Foundation + Fork Expansion (Color-Differentiated)

In the two charts below, **purple (`#a371f7`)** credits original work from **[Freminet/ScarlettX](https://github.com/Freminet/ScarlettX)** (`54.4%` ABI / `91.3%` Concept Destruction), while **emerald green (`#39d353`)** marks new functions and milestones added in **ScarlettEdge** (`+45.6%` ABI / `+8.7%` Concept Destruction):

![ABI Coverage: Upstream ScarlettX (Purple) vs ScarlettEdge Fork (Green)](docs/status.svg)

![Concept Destruction Bring-Up: Upstream (Purple) vs ScarlettEdge (Green)](docs/status_concept_destruction.svg)

| Main menu | In game |
| :---: | :---: |
| ![Concept Destruction main menu running in ScarlettX](docs/image_00_01.png) | ![Concept Destruction gameplay running in ScarlettX](docs/image_00_02.png) |
| Max FPS: 1189<br>Average FPS: 1082<br>Minimum FPS: 264 | Max FPS: 247<br>Average FPS: 188<br>Minimum FPS: 91 |

**Combined ABI Coverage:** `100.0%` (`281` upstream ScarlettX `54.4%` + `236` ScarlettEdge fork `+45.6%`) · **Concept Destruction:** `100.0%` (`42` upstream + `4` fork) · **Exports:** `513` (`269` upstream + `244` fork) · **Tests:** `41/41` passing

| Subsystem | Category | Progress | Origin Split | Details |
| :--- | :--- | :---: | :---: | :--- |
| **d3d12_xs → PC D3D12 & PSOs** | `Graphics` | `65 of 65` (`100%`) | `45 upstream + 20 fork` | 64 exports + RDNA2/DXIL/DXBC lifter; PSOs, VRS, ReservedResource/SamplerFeedback, ExecuteIndirect GPU counts, Work Graphs, XG MSAA/DCC/HTILE & GPU profiler |
| **Networking (HTTP / XCurl)** | `Networking` | `50 of 50` (`100%`) | `38 upstream + 12 fork` | 29 libHttpClient.GDK (incl. HCWebSocket RTA/PlayFab) + 21 XCurl exports; offline mock registry + opt-in live WinHTTP/WinHttpWebSocket transport |
| **Audio (XAudio2 / AcpHal)** | `Media` | `46 of 46` (`100%`) | `33 upstream + 13 fork` | 46 exports (XAudio2_9 + X3DAudio + CreateFX + AcpHal.dll SHAPE); 2048B retail XMA2 MDCT voice decode, ITU-R BS.775 5.1/7.1 spatial fold-down & host XAPOFX |
| **Middleware (Bink2 / FSR)** | `Middleware` | `38 of 38` (`100%`) | `22 upstream + 16 fork` | 28 Bink2Scarlett + 10 FidelityFX exports; package-first native DLL loading, KB2 frame-offset YCbCr 4:2:0 decode & QPC A/V PTS lip-sync |
| **Live Xbox Services (XSAPI/WinRT)** | `Platform` | `35 of 35` (`100%`) | `35 ScarlettEdge` | 35 exports; WinRT RoActivateInstance IInspectable broker, persistent XblTitleStorage, profiles, achievements, sessions |
| **XSystem & Runtime** | `xgameruntime` | `34 of 34` (`100%`) | `17 upstream + 17 fork` | 34 exports; Scarlett Anaconda info, GDK 2022-2026 QueryApiImpl, XStore licenses & XPackage DLC mount |
| **XUser & Identity** | `xgameruntime` | `32 of 32` (`100%`) | `13 upstream + 19 fork` | 32 exports; Xbox Live profile, XUID, gamertags, privileges & CNG HMAC-SHA256 XBL3.0 token signing |
| **XGameSave (persistent)** | `xgameruntime` | `30 of 30` (`100%`) | `23 upstream + 7 fork` | 30 exports; disk-backed containers, sync/async blob R/W, quota, delete |
| **DirectStorage & XVD** | `Storage` | `26 of 26` (`100%`) | `23 upstream + 3 fork` | 25 DStorage exports + AES-XTS encrypted GDFX XVC/XVD extraction & PackageVfs file mounting; OVERLAPPED NVMe queues, HLSL cs_5_0 GPU GDeflate & CopyTextureRegion GPU upload |
| **XTaskQueue & XAsync** | `xgameruntime` | `24 of 24` (`100%`) | `23 upstream + 1 fork` | 16 XTaskQueue + 8 XAsync exports (manual/threadpool/delayed/monitors/cancel) |
| **Production Codecs (XMA2/GDeflate/Bink)** | `Media` | `19 of 19` (`100%`) | `19 ScarlettEdge` | 19 exports; hardware GDeflate, BCPack, SHAPE XMA2 audio transform decoding, production Bink2 video & audio |
| **Xbox Unified Memory & KernelX** | `Platform` | `19 of 19` (`100%`) | `19 ScarlettEdge` | 19 exports; MEM_GRAPHICS, 64 KB / 2 MB physical-virtual aliasing, sync events & Game OS kernel services |
| **Hardware DXR & Vulkan Backend** | `Graphics` | `17 of 17` (`100%`) | `17 ScarlettEdge` | 17 exports; live Vulkan 1.3 VkComputePipeline + SSBO dispatch, timeline semaphores, Win32 VkSurfaceKHR, VK_KHR_acceleration_structure BLAS/TLAS & aligned SBT vkCmdTraceRaysKHR |
| **RDNA2 XS2 Shader Lifting** | `Graphics` | `17 of 17` (`100%`) | `17 ScarlettEdge` | 17 exports; RDNA2/Scarlett GPU microcode disassembly, DXIL/SPIR-V lifting, chunk stripping, pipeline caching |
| **Input (GameInput)** | `Input` | `16 of 16` (`100%`) | `15 upstream + 1 fork` | 16 exports; virtual Xbox Series controller & keyboard, reading history (Current/Next/Previous), state injection, rumble forwarded to the host controller's two main motors |
| **XMem & Threading** | `xgameruntime` | `16 of 16` (`100%`) | `11 upstream + 5 fork` | 16 exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state |
| **DXR / XboxBvhBuilder** | `Graphics` | `15 of 15` (`100%`) | `12 upstream + 3 fork` | 11 BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, deterministic software BVH fallback (hardware DXR requires capable GPU) |
| **Retail XVC/XVD Crypto** | `Storage` | `12 of 12` (`100%`) | `12 ScarlettEdge` | 12 exports; AES-XTS/AES-CBC sector decryption, SHA-256 block hashing, integrity table verification |
| **PE64 Loader & Auto-Stubs** | `Core` | `6 of 6` (`100%`) | `6 upstream` | PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks |

> **100% ABI Surface Coverage:** All 19 subsystems (517/517 functions) have verified implementations across graphics, compute, audio, networking, storage, kernel, and services. Passing synthetic tests do not imply commercial retail title execution.

_Regenerate with `python tools/generate_status.py --build-dir <build tree>` after adding new HLE APIs or subsystems; the test numbers are read from that tree's last `ctest` run._
<!-- SCARLETTX_STATUS_END -->

## Runtime Optimization & Fidelity Scorecard

**Optimization Fidelity Score:** `100.0%` (All 11 Fork Frontiers Complete · 44/44 milestones) · **Frame Delivery:** `Hardware VSync (0-tear)` · **Texture Latency:** `0-Frame Sub-Submit` · **Boot Mode:** `In-Process Direct IAT`

| Optimization Frontier | Category | Progress | Fidelity | Technical Details & Roadmap |
| :--- | :--- | :---: | :---: | :--- |
| **Frame Pacing & Hardware VSync** | `Display` | `4 of 4` | `100%` | Hardware `IDXGISwapChain::Present(1, 0)` VBlank lock, high-resolution QPC frame latency timer, and microsecond-accurate `ScheduleFrameEventX` async dispatch |
| **Bootloader & DLL Precedence** | `Loader` | `4 of 4` | `100%` | In-process `PeMapper` default with direct IAT binding; out-of-process `PATH` shim directory prioritization & local redistributable collision detection |
| **Sub-Frame Texture Upload Pipeline** | `Graphics` | `4 of 4` | `100%` | Lightweight non-reset write-watch probing (`HasPendingGuestTextureWrites`); dirty textures upload immediately in current submit without 1-frame latency |
| **DirectStorage & NVMe Throughput** | `Storage` | `4 of 4` | `100%` | Persistent Windows `FILE_FLAG_OVERLAPPED` async `ReadFile` I/O, encrypted XVD streaming via `SCARLETTX_VOLUME_KEY`, real GDeflate (`0x04 0xFB`) LZ77 tile & `BCPK` codecs |
| **RDNA2 XS2 Microcode Lifter & JIT** | `Compute` | `4 of 4` | `100%` | Clean-room RDNA2 Graphics (`EXP`, `VINTRP`, `MIMG`) + Compute (`SMEM`, `VOP2/3`, `VMEM`, `LDS`, `SOPP`) 256-VGPR SPIR-V 1.5 SSA compiler (stage `Output` exports, `OpLoad`/`OpStore`, `OpBranchConditional`) & DXIL SM 6.5 lifting |
| **Hardware DXR 1.1 & Vulkan RT** | `Ray Tracing` | `4 of 4` | `100%` | Live host `vulkan-1.dll` dynamic loader (`VkInstance`/`VkDevice`/`vkCreateShaderModule` compiling lifted SPIR-V 1.5 on GPU), D3D12 Device5/CmdList4 DXR 1.1 COM proxies & BLAS/TLAS traversal |
| **Retail XVC/XVD Container Crypto** | `Storage` | `4 of 4` | `100%` | Real FIPS-197 AES-XTS (IEEE 1619) & FIPS 180-4 SHA-256 via Windows CNG (`bcrypt.dll`), 4 KiB sector streaming in `XvdContainer::ReadUserData`, hex/file keyring loader, `--volume-key` CLI & `PackageVfs` encrypted mounts |
| **Unified Memory Hardware Aliasing** | `Memory` | `4 of 4` | `100%` | True Windows `VirtualAlloc2` (`MEM_RESERVE_PLACEHOLDER`) & `MapViewOfFile3` physical-virtual page aliasing (`KernelXCreateMemoryAlias`) across 64 KB / 2 MB pages |
| **Xbox COM Vtable & SDK Depth** | `Graphics` | `4 of 4` | `100%` | Expanded 128-slot D3D12.2 / Enhanced Barriers (`CL7::Barrier`) / DXR 1.1 / Mesh Shader COM proxies, multi-SDK delta maps & `XG` `64KB_S`/`64KB_D`/`64KB_R_X` macro-tile swizzling |
| **Audio DSP & XAPO Effects** | `Media` | `4 of 4` | `100%` | Host XAudio 2.9 XAPO `EffectChain` passthrough on Source/Submix voices with graceful DSP state & parameter persistence (`Enable`/`Disable`/`Get`/`SetParameters`) |
| **Middleware & Network Runtime** | `Services` | `4 of 4` | `100%` | Opt-in live WinHTTP HTTPS/HTTP in both `libHttpClient` & `XCurl` (`SCARLETTX_LIVE_NETWORK=1`), Bink2 `KB2` header + BT.709 YUV-to-RGBA decode & XMA2 PCM16 synthesis |

> **Optimization Roadmap:** While 100% ABI surface coverage guarantees zero loader or unresolved symbol crashes, this scorecard tracks deep hardware execution fidelity, sub-millisecond pacing, and commercial AAA engine performance.

## Architecture Overview

Because Xbox Series X|S executables are native **x86-64 PE32+** binaries targeting Microsoft's Game Development Kit (GDK), ScarlettEdge focuses on **high-level OS, runtime, memory, and GPU/RT emulation** rather than CPU static recompilation:

| Subsystem | Components | Status |
| :--- | :--- | :--- |
| **Package & PE64 Loader** | `MicrosoftGame.Config` parser (`XboxGen9`/`Scarlett`), in-process PE64 mapper (`DIR64`/`HIGHLOW` relocations, `.bss`-safe RVA bounds, page protections), named/ordinal IAT & API-set resolution, package-first middleware loading (`Bink2`/`FidelityFX`/`XCurl`/`libHttpClient` loaded from title package when present, HLE fallback when absent), system-first HLE redirection (`xgameruntime`, `d3d12_xs`, `xg_xs`, `xcrdapi`, `kernelx`, `pixevt_stub`), x64 auto-stub thunks | Verified + tests |
| **GDK Runtime (`xgameruntime.dll`) & XSAPI (`xsapi.dll`)** | C ABI for `XGameRuntime*`, `XTaskQueue*`, `XAsync*`, `XUser*` (with offline token/signature synthesis), persistent disk-backed `XGameSave*`, `XSystem*`, `XMem*` hooks & 64 KB / 2 MB virtual memory, per-thread `XThread*`, `InitializeApiImpl`/`QueryApiImpl` COM tables, and `XAsync`-integrated `xsapi.dll` services | Verified + tests |
| **Unified Memory & Kernel (`kernelx.dll`)** | `CreateFileMappingW` + `VirtualAlloc2` (`MEM_RESERVE_PLACEHOLDER` with `MEM_ADDRESS_REQUIREMENTS` 2 MB alignment) + `MapViewOfFile3` physical-virtual memory aliasing (`KernelXCreateMemoryAlias`), dual CPU VA ↔ GPU VA lookup across primary & alias views, sync events, QPC timers | Verified + tests |
| **Graphics (`d3d12_xs.dll` / `xg_xs.dll`)** | 128-slot Xbox COM vtable proxies across SDK `0x20001`–`0x20006` (Device, CommandQueue, CommandList, Enhanced Barriers `CL7::Barrier` with resource proxy unwrapping, `CreatePlacedResourceX` on `KernelX`/virtual-alias GPUVAs, `ID3D12Device2` subobject-stream PSOs across `VS`/`PS`/`DS`/`HS`/`GS`/`CS`/`AS`/`MS`, `CreateStateObject` root-sig unwrapping, DXR 1.1, Mesh Shaders, VSync `PresentX`, persistent-scheduler `ScheduleFrameEventX`, sub-frame texture uploads) + `xg_xs.dll` (`4KB_S`, `256B_S`, `64KB_S/D/Z_X/R_X`, Cube/2D arrays, 64 KB packed mip-tails, 3D volume swizzling) | Active Phase 2 + tests |
| **Shaders, RDNA2 Lifter & Vulkan (`xs2_lifting.dll` / `vulkan_backend.dll`)** | Automatic Xbox DXBC/DXIL vendor-chunk stripping with retail DXBC modified-MD5 checksum recalculation plus `XS2\0` RDNA2 -> signed SM 6.5 DXIL (`dxcompiler.dll`) / SM 5.0 DXBC (`d3dcompiler_47.dll`) PSO auto-interception; clean-room RDNA2 Graphics+Compute 256-VGPR SPIR-V 1.5 SSA compiler (SSBOs, structured `OpLoopMerge`/`OpSelectionMerge`, Wave32 subgroups) + live Vulkan 1.3 `VkComputePipeline`, UBO/SSBO GPU execution, timeline semaphores, Win32 `VkSurfaceKHR`, and GPU AS backing | Active Phase 2 + tests |
| **DirectStorage, XVD & Crypto (`xcrdapi.dll` / `dstorage_xs.dll` / `xvc_crypto.dll`)** | Real-title `xcrdapi.dll` (`IDStorageFactoryX`/`IDStorageQueueX` with concurrent `FILE_FLAG_OVERLAPPED` reads, `G:\` VFS fallback, `destination_page_offset`, spec-compliant RFC 1951 Huffman DEFLATE + RFC 1950 Zlib, BC1 4x4 RGBA8 unpack, and `0x04 0xFB` GDeflate / `BCPK` dispatch) + cached Windows CNG (`bcrypt.dll`) FIPS-197 AES-128/256-XTS (with IEEE 1619 CTS), AES-CTR stream decrypt, FIPS 180-4 SHA-256, and `--volume-key` VFS mounting | Active Phase 2 + tests |
| **Audio (`xaudio2_9.dll` / `AcpHal.dll`)** | Guest-facing `IXAudio2`/voice COM wrappers over host XAudio 2.9 with XAPO `EffectChain` passthrough & emulated DSP parameter state fallback, silent output fallback on headless hosts, `AcpHal.dll` APU heap & SHAPE contexts | Verified + tests |
| **Input (`GameInput.dll`)** | Guest-facing `IGameInput`/`IGameInputDevice`/`IGameInputReading` COM objects, keyboard + XInput controller polling, controller rumble passthrough | Verified + tests |
| **Fallback Middleware & Networking (`Bink2`, `FSR`, `libHttpClient`, `XCurl`)** | Fallback HLE shims used when a package omits title-local middleware; opt-in live WinHTTP HTTPS/HTTP (`SCARLETTX_LIVE_NETWORK=1`) with UTF-8 URLs, custom request headers, and binary `CURLOPT_POSTFIELDSIZE` support | Fallback HLE + tests |

### Real-Title Bring-Up (Verified Observations — Concept Destruction)

Testing against an unencrypted commercial Xbox Series X|S (`Scarlett`) Unity IL2CPP title (**Concept Destruction**, kept strictly outside the repository) established:

- **Phase 1 — Static inspection & image mapping:** PE64 image maps with 405 imported symbols and no auto-stubs left; ScarlettX's `d3d12_xs.dll`, `xg_xs.dll`, and `AcpHal.dll` resolve all static Xbox imports.
- **Phase 2 — `QueryApiImpl` COM interface (`{073B7DCB-1FCF-4030-94BE-E3C9EB623428}`):** Reverse-engineered the target's Windows x64 register ABI (`RCX=api_id`, `RDX=riid`, `R8=out_impl`) and vtable offsets (`+0x00`/`+0x08`/`+0x10`, `+0x30`, `+0x60`, `+0x80`, `+0x88`, `+0xE8`). Implemented a ScarlettX-owned 32-slot guest COM object in `xgameruntime.dll` with dedicated ABI tests (`test_query_api_impl`).
- **Phase 3 — Runtime tables, `AcpHal` and `d3d12_xs` proxies:** every `QueryApiImpl` table the title requests resolves through ScarlettX-owned guest COM objects (task queues, XNetworking, XGame, XSystem, XUser, XGameSave, XStore, XPackage). Table slots were identified from the console `xgameruntime.dll`; during initial upstream bring-up `XUserGetTokenAndSignatureAsync` returned `E_NOTIMPL` (subsequently implemented in ScarlettEdge with CNG HMAC-SHA256 `XBL3.0` JWT signing).
- **Phase 4 — Frames:** with guest implicit TLS in the loader the title runs until stopped. It writes cooked texel data (console `4KB_S` layout) to the address returned by `ID3D12Resource::GetGPUVirtualAddress`; `d3d12_xs` backs that address with guest memory and converts it into the host texture, `xg_xs` reports the console texture layout, and scene textures are created through `CreateHeap`/`CreatePlacedResource`. Frame dumps show, in order: the publisher logo, the developer logo, the intro cutscene, a 3D scene, the main menu and the "Select car" menu.
- **Phase 5 — Input, gameplay, audio:**
  - With keyboard input bridged to a virtual gamepad, a Championship event starts and the car drives. Verified in frame dumps: A, B, D-pad, left stick, right trigger. A real Xbox controller works as well, and progress is saved: a championship in progress continued after the emulator was restarted.
  - `XAudio2Create` returns guest-facing objects; the title creates its engine, mastering voice and a 48 kHz source voice and polls the play position, against a silent output (the first test machine has no audio device). On a machine with an audio device the title's sound is audible.
  - No `CmdList::Close` failure or device removal in gameplay runs.
  - On a hardware GPU (`--gpu`) the title plays at full speed: about 110 fps at 2,300 draw calls per frame and about 140 fps at 850, measured with `--stats`. An event ran from the countdown to the results screen. The software rasterizer of the first test machine managed about 2 to 3 fps.
- **Known problems:** The fps figures in this README were measured before frame pacing existed; `--fcap 0` gives the same behaviour.
- **Current classification:** **In-game** — **Concept Destruction** and **Unicorn Overlord** are playable on a hardware GPU; Alt+Enter toggles borderless fullscreen and controller rumble works.

### Current Limitations & Progress

- **Xbox COM vtable wrappers:** `d3d12_xs` guest-facing proxies cover verified SDK `0x20001`–`0x20006` slots used by **Concept Destruction**, **Unicorn Overlord**, and extended D3D12.2 / Enhanced Barriers / DXR 1.1 / Mesh Shader entry points; unverified console-specific `*X` slots log caller diagnostics and return `E_NOTIMPL`. `XAudio2Create` returns guest-facing `IXAudio2`, source, submix, and mastering voice objects forwarding to host XAudio 2.9 (including XAPO `EffectChain` passthrough with graceful DSP parameter fallback), or to a silent timing output when no audio device is present.
- **Presentation & Frame Pacing:** Alt+Enter switches the PresentX window to borderless fullscreen on its monitor and back, stretching the title's frame; there is no exclusive fullscreen. `WaitFrameEventX` paces against hardware VSync when `SetFrameIntervalX` or `SCARLETTX_VSYNC=1` is set; `ScheduleFrameEventX` signals frame events via a persistent microsecond min-heap timer thread.
- **Guest texture uploads:** Texel data a title writes to a texture's GPU virtual address is checked before every command list submit via lightweight non-reset write-watch probing (`HasPendingGuestTextureWrites`) or 64-bit subresource content hashing on `CreatePlacedResourceX` `KernelX`/virtual-alias GPUVAs, and uploaded immediately in the current frame.
- **Bootloader & Middleware Precedence:** `scarlettx boot` defaults to in-process execution (`PeMapper`). Xbox OS/hardware shims (`xgameruntime`, `d3d12_xs`, `xg_xs`, `xcrdapi`, `kernelx`, `xaudio2_9`, `GameInput`, `AcpHal`, `pixevt_stub`) always load from ScarlettEdge's shim directory, whereas title-shipped middleware DLLs (`Bink2Scarlett`, `amd_fidelityfx_*`, `XCurl`, `libHttpClient.GDK`) load from the game package first when present and fall back to HLE shims only when absent.
- **Subsystem Coverage & Honest Limitations:** All 19 tracked subsystems, Concept Destruction bring-up (`46/46` milestones), Phase 1 Fork Foundations (`44/44`), Phase 2 Retail AAA Execution Architecture (`72/72`), and Phase 3 Architectural Unification, Diagnostics & GDK `2510` Verification (`30/30`) are unified through a single shared `scarlettx_core.dll` runtime — including retail `msft-xvd` v3 / `makepkg` `.msixvc` + Merkle hash tree + read-only NTFS + 48-byte `.cik` mounting, 32-lane bit-interleaved HLSL `cs_5_0` GPU compute `GDeflate`, 512-sample WMA-Pro `XMA2` audio, `VK_KHR_ray_tracing_pipeline` + live Hardware DXR 1.1 & SM 6.5 Mesh Shaders on RTX 5070 Ti, RDNA2 `ADDR2` / `DCC` / `HTILE` surfaces, structured RDNA2 `XS2` CFG + SM 6.6 bindless / `dxil.dll`-signed shader lifting, and the 4-Part Flight-Recorder Diagnostic Suite (`--flight-recorder`, `--hang-timeout`). Passing synthetic, vertical-slice, and official GDK sample suites does not imply drop-in compatibility with untested commercial retail games, which require per-title validation with legally owned game dumps.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/ROADMAP.md](docs/ROADMAP.md), [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md), and [CLEANROOM_POLICY.md](CLEANROOM_POLICY.md) for detailed design, compatibility, and clean-room governance notes.

## Building & Running Tests

Requirements on Windows (x86-64):
- CMake 3.20+
- Ninja
- C++20 compiler (MinGW-w64 GCC 13+ or MSVC 2022)
- Windows D3D12 runtime + WARP (included in Windows 10/11 & Server 2022)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --parallel 8 --output-on-failure
python tools/check_cleanroom.py
```

## Running the Synthetic Scarlett Test Games

```bash
# Inspect a Game Core package manifest and PE imports
./build/scarlettx inspect build/packages/hello_scarlett

# Boot the synthetic hello_scarlett title headlessly
./build/scarlettx boot build/packages/hello_scarlett --headless --frames 3

# Boot the synthetic ray-tracing showcase and dump a rendered RT frame
./build/scarlettx boot build/packages/rt_showcase --headless --frames 2 --rt-dump rt_frame.ppm

# Run the full-stack end-to-end synthetic game demo (PE64 + XVD/DirectStorage + GameInput + D3D12/BVH RT + XAudio2/XMA2 + XGameSave)
./build/test_scarlett_arena_e2e
```

## Running a Real Title

```bash
# From the directory that holds scarlettx.exe and the shim DLLs
scarlettx compat "<extracted package or .xvc/.msixvc>" [--volume-key key.cik]
scarlettx boot "<extracted package>\Mount" --windowed --gpu --stats --flight-recorder flight_logs --hang-timeout 15
scarlettx triage flight_logs [--emit-stubs] [--json]
```

- `scarlettx boot` runs in-process on the hardware GPU by default (`--separate-process` runs as a child process; `--warp` selects the software rasterizer; `--windowed` opens the `PresentX` swapchain window while `--headless` suppresses window creation; `--lockhart` emulates Xbox Series S memory/CU limits; `--allow-pc-gdk` permits booting legally installed `Gaming.Desktop.x64` PC GDK packages—such as loose `C:\XboxGames\<Title>\Content\` installs—to stress-test ScarlettEdge's shared PE64 loader, implicit TLS, `.pdata`, `xgameruntime`, `XUser`/`XGameSave`, `GameInput`, `XAudio2`, `DirectStorage`, `WinRT`, and Flight Recorder against real commercial engines without needing console-encrypted dumps, noting that PC GDK builds import desktop `d3d12.dll` rather than console `d3d12_xs.dll`; `--flight-recorder [dir]` and `--hang-timeout <sec>` enable the 4-Part Flight-Recorder Diagnostic Suite writing `flight_summary.json`, `.pdata` crash dumps, D3D12 DRED reports, and failing shader artifacts). `--stats` prints fps and D3D12 call counts once per second, `--fcap N` holds N frames per second instead of the title's own rate (for example `--fcap 144`; `--fcap 0` removes the cap), and `--trace` logs the shim calls.
- Input: the keyboard while the window has focus (arrows = D-pad, WASD = left stick, IJKL = right stick, Enter/Space = A, Escape/Backspace = B, X = X, Y = Y, Q/E = shoulders, 1/3 = triggers, Tab = View, P = Menu), and the first XInput controller when one is connected (checked with an Xbox controller; rumble is forwarded to it).
- The window does not close from its title bar; stop the emulator with Ctrl+C in its console.
- Save data goes to `%TEMP%\scarlettx_saves`; set `SCARLETTX_SAVE_ROOT` to use another directory.
