# ScarlettEdge Engineering Roadmap

This document tracks the technical progression of **ScarlettEdge** from clean-room GDK/HLE foundations (Phase 1, completed) and retail AAA silicon architecture (Phase 2, completed) to unified COM/guest execution and retail format parity (Phase 3, completed), ahead of per-title commercial game validation.

---

## Completed Foundations

### Upstream ScarlettX Baseline ([Freminet/ScarlettX](https://github.com/Freminet/ScarlettX))
- PE64 in-process loader, implicit TLS, `.pdata` unwinding, and `G:\` virtual drive mapping.
- Core `xgameruntime.dll` C ABI + `QueryApiImpl` COM tables, `XTaskQueue` / `XAsync`, persistent `XGameSave`.
- Guest-facing `d3d12_xs.dll` & `xg_xs.dll` (`4KB_S`), `xaudio2_9.dll` + `AcpHal.dll`, `GameInput.dll` + rumble, and `xcrdapi.dll` DirectStorage queues.
- Playable real-title bring-up of **Concept Destruction** and **Unicorn Overlord**.

### Phase 1: ScarlettEdge Fork Foundations (`44 / 44` Complete)
- **Cryptography & Containers:** Cached Windows CNG (`bcrypt.dll`) FIPS-197 AES-128/256-XTS (with IEEE 1619 CTS), AES-CTR stream transform, FIPS 180-4 SHA-256, and `--volume-key` `PackageVfs` XVD/XVC mounting.
- **Loader & Boot Hygiene:** Package-first game middleware precedence (`Bink2`, `FidelityFX`, `XCurl`, `libHttpClient`) with HLE fallback, complete system HLE redirections (`kernelx`, `xcrdapi`, `xsapi`, `pixevt_stub`), `.bss`-safe RVA bounds checking, and `scarlettx compat` CLI analyzer.
- **Memory & KernelX:** Guaranteed 2 MB large-page `VirtualAlloc2` (`MEM_ADDRESS_REQUIREMENTS`) alignment and `MapViewOfFile3` physical-virtual memory aliasing (`KernelXCreateMemoryAlias`) with alias-aware GPU VA lookup.
- **Graphics, Shaders, & Audio:** 128-slot SDK `0x20001`–`0x20006` COM proxies, Enhanced Barriers (`CL7::Barrier`) & `CreateStateObject` proxy unwrapping, automatic Xbox DXBC chunk stripping with retail DXBC MD5 recalculation, `XG` `64KB_S/D/Z_X/R_X` + CubeMap swizzling, RDNA2 Graphics+Compute SPIR-V 1.5 SSA compiler + dynamic `vulkan-1.dll` `VkShaderModule` backend, and XAudio2 XAPO `EffectChain` passthrough.
- **Networking & CI:** Opt-in live WinHTTP transport (`SCARLETTX_LIVE_NETWORK=1`) with UTF-8 URLs, request headers, and binary `CURLOPT_POSTFIELDSIZE`, backed by automated GitHub Actions Windows MSVC `/W4` parallel CI (`ctest --parallel 4`).

---

## Phase 2: Retail AAA Execution Roadmap (`72 / 72` Architecture Sub-Goals Complete)

> **Scope Note:** All `72 / 72` (`100.0%`) clean-room Phase 2 silicon, GPU translation, codec, and GDK/WinRT architecture sub-goals below are implemented and verified in automated CI. External commercial retail AAA titles still require per-title validation with user-supplied game dumps and volume keys.

| Track | Completed Sub-Goals | Ongoing Title-Driven Hardening |
| :--- | :--- | :--- |
| **1. XS2 → GPU Shader Compiler (`12/12` — Complete)** | RDNA2 ISA decode, 256-VGPR SPIR-V 1.5 emitter, stage `Output` exports, SMEM UBOs, MIMG `OpImageSampleImplicitLod`, `v_interp`, disk cache, SSBO `StorageBuffer` Load/Store, structured `OpLoopMerge`/`OpSelectionMerge` CFG, Wave32 `GroupNonUniform` subgroups, automatic `d3d12_xs` `XS2\0` & `XS2M` PSO interception, signed SM 6.5 `dxcompiler.dll` DXIL + SM 5.0 `D3DCompile` DXBC bridge | Extend RDNA2 opcode coverage on new retail AAA shader dumps (tessellation, ray-query inline BVH ops, 16-bit packed math) |
| **2. XG Tiling, MSAA & DCC (`10/10` — Complete)** | `4KB_S`, `256B_S`, `64KB_S/D`, `64KB_R_X`, `64KB_Z_X` depth, 2D arrays & 6-face CubeMaps, 64 KB packed mip-tails, 3D volume swizzling (`XGCreateTexture3DComputer`), `2x/4x/8x MSAA` sample-plane swizzled layout & addressing, and RDNA2 `DCC` (`0x00`/`0x20`/`0x40`/`0x80`/`0xC0`/`0xFF`) & `HTILE` (`ZMin`/`ZMax` 14-bit + expanded depth) codecs | GPU compute shader DCC/HTILE fast-clear resolve on titles streaming raw metadata planes |
| **3. Vulkan 1.3 & DXR GPU Execution (`10/10` — Complete)** | Dynamic `vulkan-1.dll` loader, `VkDevice`/`VkQueue`/`VkCommandBuffer`, RootSig → `VkDescriptorSetLayout`, live `vkCreateShaderModule`, live `VkComputePipeline` + UBO/SSBO GPU execution (`VulkanXsDispatchComputeSpirv`), Win32 `VK_KHR_win32_surface` binding, Vulkan 1.2 timeline semaphores, hardware `VK_KHR_acceleration_structure` BLAS/TLAS device-address builds, and aligned SBT (`VulkanXsBuildShaderBindingTable`) + `vkCmdTraceRaysKHR` | Multi-geometry TLAS instance masking & callable shader SBT specialization |
| **4. DirectStorage GPU Codecs (`8/8` — Complete)** | Concurrent `FILE_FLAG_OVERLAPPED` I/O, `0x04 0xFB` tile framing & multi-thread pool, OS priority queues, `xcrdapi` codec hookup, spec-compliant RFC 1951 canonical Huffman DEFLATE + RFC 1950 Zlib (Adler-32), hardware BC1 4x4 RGBA8 unpack, HLSL `cs_5_0` GPU compute `GDeflate` buffer unpacker (`DStorageDecompressGpuCompute`), and direct `CopyTextureRegion` + `4KB_S` swizzled texture streaming | `ID3D12MetaCommand` vendor-specific GDeflate fast path when Agility SDK metacommand is present |
| **5. D3D12.2 & `MEM_GRAPHICS` (`10/10` — Complete)** | 128-slot vtables, `CL7::Barrier` proxy unwrapping, `VirtualAlloc2` aliasing, 2 MB aligned large pages, `CreatePlacedResourceX` on `KernelX`/alias GPUVAs, `ID3D12Device2::CreatePipelineState` stream PSOs (`VS`/`PS`/`CS`/`AS`/`MS`), Variable Rate Shading (`VRS` Tier 1/2), sparse 64 KB `ReservedResource` + `UpdateTileMappings` + `SamplerFeedback`, `ExecuteIndirect` GPU count buffers, and autonomous GPU Producer→Consumer Work Graphs | Native SM 6.8 `D3D12_STATE_OBJECT_TYPE_EXECUTABLE` Work Graph passthrough as titles adopt GDK 2025+ |
| **6. GDK 2022–2026 & WinRT (`8/8` — Complete)** | GDK 2022–2026 `QueryApiImpl` COM GUIDs (`XStore`, `XPackage` 2025, `XUser` 2025, `XNetworking` slots 8/9/10/14), `RoActivateInstance` / `RoGetActivationFactory` WinRT `IInspectable` COM broker, `XStore` license + `XPackage` DLC mount, CNG HMAC-SHA256 `XBL3.0` JWT token signing, persistent disk-backed `XblTitleStorage`, and `HCWebSocket` RTA/PlayFab offline + live `WinHttpWebSocket` bridge | Expand WinRT namespace surface as demanded by individual commercial titles |
| **7. Retail XMA2 & Bink2 Media (`8/8` — Complete)** | 2048-byte retail `XMA2` multi-stream bitstream parser, Princen-Bradley sine-windowed `MDCT`/`IMDCT` ($N=128$, 50% TDAC overlap-add) + 52-byte `0x0166` `XMA2WAVEFORMATEX` guest COM voice bridge, ITU-R BS.775 `5.1`/`7.1` downmix + `X3DAudio` spherical panning/Doppler, host `System32\xaudio2_9.dll` `CreateFX` (`CLSID_FXReverb`, `FXEQ`, `FXMasteringLimiter`), `KB2` frame-offset YCbCr 4:2:0 decode, and QPC A/V PTS lip-sync | Optional Media Foundation H.264/HEVC `IMFSourceReader` hardware decode for non-Bink cutscenes |
| **8. AAA Engine & XVC Bring-Up (`6/6` — Complete)** | Cycle-safe `GDFX` (`MICROSOFT*XBOX*MEDIA`) + read-only `NTFS` filesystem parsing & `PackageVfs` file mounting inside AES-XTS encrypted `.xvc` volumes, direct `scarlettx boot`/`compat` `.xvc` support, `ue5_slice.exe` & `openworld_slice.exe` Gen9 PE64 vertical-slice titles, 64-slot GPU stability breadcrumbs + TDR check, concurrent multi-queue `GDeflate`+`XS2`+alias churn, and PSO prewarm + QPC `p99` frame-time profiler | Title-by-title compatibility bring-up across user-supplied commercial retail Gen9 games |

---

## Phase 3: Architectural Unification, Retail Parity, Diagnostics & GDK 2510 (`30 / 30` Complete)

| Track | Completed Sub-Goals | Ongoing Title-Driven Hardening |
| :--- | :--- | :--- |
| **1. Shared Core & Loader (`5/5` — Complete)** | Unified `scarlettx_core.dll` singleton module registry, shared `PackageVfs::Global()`, TLS Slot-0 reservation + FLS isolation, `.pdata` zero-tail stripping, native PEB `ApiSetSchema` deferral, and Win32 `G:\` VFS/command-line/`ExitProcess`/`GetProcAddress`/Delay-Load IAT hooks | Edge-case DLL redirections encountered on new commercial titles |
| **2. Guest COM Bridge (`5/5` — Complete)** | Strict IID `QueryInterface` validation across `d3d12_xs`, WinRT `IInspectable`, `IGameInput`, and `IXAPO`, unified `dstorage` / `dstorage_xs` / `xcrdapi` state, `XMemVirtualAlloc` (`MEM_GRAPHICS`) <-> `KernelX` <-> `d3d12_xs` GPU VA translation, `ID3D12StateObjectProperties` + `BuildRaytracingAccelerationStructure` COM proxies, and `XAsync`-integrated `libHttpClient` | Expand extended `ID3D12Device*` / `ID3D12GraphicsCommandList*` slots on demand |
| **3. Retail XVD v3, `.msixvc` & Codecs (`5/5` — Complete)** | Retail `msft-xvd` v3 (`0x284+`), authentic `makepkg` `.msixvc` contiguous-Merkle + 128-byte `XvcRegionHeader` v2 (`FS-MD` unencrypted NTFS `$MFT`), 48-byte `.cik` & keyring loader, 32-lane SIMD bit-interleaved `GDeflate` (`0x04 0xFB`) in C++ and HLSL `cs_5_0` GPU compute, and 512-sample WMA-Pro `XMA2` packet decoding | Multi-region CIK slot matching for multi-key retail DLC packs |
| **4. RDNA2 `XS2` CFG, SM 6.6 & Vulkan RT (`5/5` — Complete)** | Full-width RDNA2 GFX10.3 instruction length decoding (32-bit literals, DPP/SDWA, `VOP3P`, `FLAT`/`MTBUF`, NSA `MIMG`, `image_bvh64_intersect_ray`, Xbox `SPI_SHADER_PGM_RSRC*` headers), structured loop/if CFG lifting, SM 6.6 `ResourceDescriptorHeap[]` + `dxil.dll` cryptographic container re-signing, and hardware-backed `VK_KHR_ray_tracing_pipeline` execution (`hardwareBacked = 1`) | Retail shader corpus coverage as commercial dumps are tested |
| **5. 4-Part Flight-Recorder Suite (`5/5` — Complete)** | Lock-free 4096-slot cross-DLL event ring + deduplicated import/QI/vtable/VFS miss tables, VEH/SEH `.pdata` stack-walk crash dumper with Ghidra `0x140000000+RVA` guest address resolution, D3D12 DRED auto-breadcrumb/page-fault + failing Xbox shader/PSO binary dumper, and zero-lock `SuspendThread` hang/deadlock watchdog + `flight_summary.json`/`.txt` telemetry | Live telemetry aggregation across commercial title bring-up |
| **6. Official GDK `2510.5.6322` & Gen9 Silicon (`5/5` — Complete)** | Hardware-verified on **NVIDIA GeForce RTX 5070 Ti** (`--debug-layer`, 0 errors) against official `Microsoft.GDK.PC` `2510.5.6322` (`InitializeApiImplEx2`, `XSystem` `{1861CF2E}`, `XSystemAnalytics` `{B884675D}` `sret`, `XGameSave` `{1BFFF3AF}` + `XGameSaveFiles`, `XPackage` `{E2A4734B}` Intelligent Delivery slots `3..19` & `39`, `XStore` `{0DD112AC}`/`{B09D803C}` 68-slot table, `XDisplay` `{03F0FE74}`/`{1B339674}`, `XPersistentLocalStorage` `{F4FAF4D4}`/`{41A4E10C}`, `XNetworking` `{37E56907}`, `D3D12_HEAP_FLAG_ALLOW_DISPLAY` stripping) + live hybrid Hardware DXR 1.1 & SM 6.5 Mesh Shaders via `PresentX` | Commercial Gen9 title validation with user-supplied dumps |

---

## Phase 4: Clean-Room Governance & Commercial Retail Bring-Up (`In Progress`)

| Track | Scope | Status |
| :--- | :--- | :--- |
| **4A. Clean-Room Governance & CI Guardrails (`5/5`)** | [CLEANROOM_POLICY.md](../CLEANROOM_POLICY.md), [CONTRIBUTING.md](../CONTRIBUTING.md), automated `tools/check_cleanroom.py` file/extension/size/commit-message gate in GitHub Actions CI, and Node 24-compatible `actions/checkout@v5` | **Complete** |
| **4B. Flight-Recorder Triage CLI (`5/5`)** | `scarlettx triage <flight_dir> [--emit-stubs] [--json]` (`src/core/flight_triage.cpp`), crash/hang/DRED/null-vtable verdict classifier, hit-ranked gap tables, clean-room C++ stub skeleton emitter, and `test_triage_cli` CTest | **Complete** |
| **4C. Multi-Sample GDK 2510 ATG Validation (`5/5`)** | Verified live on **NVIDIA GeForce RTX 5070 Ti** (`--debug-layer`, `0` errors, `scarlettx triage` `CLEAN_RUN`): DirectStorage (`dstorage_xs`), `XGameSave` write/read round-trip + `XGameSaveFiles` + `XPersistentLocalStorage`, `GameInput`, `XAudio2` 48 kHz PCM voice, and D3D12.2 SM 6.6 `ResourceDescriptorHeap[]` + 30/30 `ExecuteIndirect` GPU frames | **Complete** |
| **4D. Commercial Retail Bring-Up — Wave 1 (`0/5`)** | Flight-recorder-driven validation of user-supplied legal dumps (**Concept Destruction**, **Unicorn Overlord**) through inspect, boot, menus/audio/input, gameplay, and persistent save round-trip | **Queued** |
| **4E. Commercial Engine Hardening (`0/5`)** | Versioned shader disk cache, multi-region `.msixvc`/`.cik` streaming, `D3D12_HEAP_FLAG_*` & alias churn telemetry, optional Media Foundation H.264/HEVC path, and `p95`/`p99` stutter profiler in `flight_summary.json` | **Queued** |
| **4F. Compatibility Matrix & Phase 4 Dashboard (`5/5`)** | [docs/COMPATIBILITY.md](COMPATIBILITY.md) tiered compatibility ledger, `docs/status_phase4.svg`, and Phase 4 tracking in `tools/generate_status.py` and `README.md` | **Complete** |

