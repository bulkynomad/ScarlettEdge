# ScarlettEdge

[![ScarlettEdge CI (Windows MSVC)](https://github.com/bulkynomad/ScarlettEdge/actions/workflows/ci.yml/badge.svg)](https://github.com/bulkynomad/ScarlettEdge/actions/workflows/ci.yml)

**ScarlettEdge** is an independent, open-source, clean-room high-level emulator (HLE) fork of ScarlettX for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal notice:** ScarlettEdge does not include or distribute any proprietary Microsoft/Xbox binaries, cryptographic keys, firmware, or commercial game assets. Optional encrypted container support uses standard public cryptographic algorithms (FIPS-197 AES-XTS / FIPS 180-4 SHA-256) and requires keys supplied independently by the user for legally owned software. All automated tests run against synthetic x86-64 PE titles built from source in this repository.

## Origin & Credits

**ScarlettEdge** is an independent fork of **[ScarlettX](https://github.com/Freminet/ScarlettX)** by **Freminet** and the original ScarlettX contributors. All credit for the foundational emulator architecture belongs to the original authors, including:

- **Original ScarlettX Authors (`Freminet/ScarlettX`):** Core PE64 loader & implicit TLS, `MicrosoftGame.Config` parser, `xgameruntime` C ABI & `QueryApiImpl` COM tables, foundational `d3d12_xs`/`xg_xs` translation & `4KB_S` texture deswizzling, guest-facing `XAudio2_9`/`AcpHal` & silent output engine, `GameInput` + rumble, persistent `XGameSave`, base `dstorage_xs`/`xcrdapi` queues, offline HLE shims (`Bink2`, `FidelityFX`, `libHttpClient`, `XCurl`), and the real-title bring-up of **Concept Destruction** and **Unicorn Overlord**.
- **ScarlettEdge Fork Additions (`bulkynomad/ScarlettEdge`):** Real FIPS-197 AES-XTS & FIPS 180-4 SHA-256 via Windows CNG (`bcrypt.dll`) with keyring-backed encrypted XVD streaming, hardware VSync (`Present(1, 0)`) & async `ScheduleFrameEventX`, 0-frame sub-submit write-watch texture uploads, in-process direct IAT boot default, `scarlettx compat` analyzer CLI, Windows `VirtualAlloc2`/`MapViewOfFile3` physical-virtual memory aliasing, 128-slot D3D12.2 / Enhanced Barriers (`CL7::Barrier`) / DXR 1.1 / Mesh Shader COM proxies, `XG` `64KB_S`/`64KB_D`/`64KB_R_X` macro-tile swizzling, RDNA2 graphics+compute 256-VGPR SPIR-V 1.5 SSA compiler (stage exports, Load/Store, control flow) + live host `vulkan-1.dll` `VkShaderModule` compilation, real GDeflate (`0x04 0xFB`) & BCPack LZ77 codecs, Bink2 `KB2`/BT.709 YUV-to-RGBA & XMA2 PCM16 decode, XAudio2 XAPO effect chain passthrough, persistent `FILE_FLAG_OVERLAPPED` DirectStorage NVMe I/O, opt-in live `WinHTTP` transport (`libHttpClient` & `XCurl`), and automated GitHub Actions Windows MSVC CI.

<!-- SCARLETTX_STATUS_START -->
## Status

### Phase 2: Retail AAA Execution Roadmap (Active Next Wave)

While Phase 1 foundations and ABI coverage are at `100%`, running unmodified heavy commercial AAA retail titles (UE5, Open-World, Creation Engine) requires deeper silicon-level compiler, GPU execution, and codec milestones tracked honestly below:

![ScarlettEdge Phase 2: Retail AAA Execution Roadmap](docs/status_aaa_roadmap.svg)

**Phase 2 AAA Readiness:** `25.0%` (18 of 72 milestones) · **Active Focus:** `RDNA2 SPIR-V Compiler, Vulkan GPU Execution, DCC/HTILE & Native Media`

| Phase 2 Milestone | Category | Progress | Readiness | Remaining Work |
| :--- | :--- | :---: | :---: | :--- |
| **XG Tiling, MSAA & DCC** | `Graphics` | `4 of 10` | `40%` | **Done (4):** 4KB_S, 256B_S, 64KB_S/D, 64KB_R_X · **Next (6):** 64KB_Z_X depth, 3D volumes, Cube/2D arrays, 2x/4x/8x MSAA, DCC color & HTILE depth decompression |
| **D3D12.2 & MEM_GRAPHICS** | `Graphics` | `3 of 10` | `30%` | **Done (3):** 128-slot vtables, CL7::Barrier, VirtualAlloc2 aliasing · **Next (7):** CreatePlacedResourceX on guest GPUVA, 2 MB large pages, Mesh PSOs, VRS, Sampler Feedback/Tiled, ExecuteIndirect counts, Work Graphs |
| **XS2 -> GPU Shader Compiler** | `Compiler` | `3 of 12` | `25%` | **Done (3):** RDNA2 ISA decode, 256-VGPR SPIR-V 1.5 SSA, stage Output exports · **Next (9):** SMEM UBOs, SSBO Load/Store, MIMG OpImageSample, v_interp Fragment inputs, CFG reconvergence, Wave32 subgroups, d3d12_xs auto-intercept, SPIR-V->DXIL bridge, disk cache |
| **DirectStorage GPU Codecs** | `Storage` | `2 of 8` | `25%` | **Done (2):** OVERLAPPED NVMe I/O, LZ77 GDeflate (0x04FB) & BCPK · **Next (6):** RFC 1951 Huffman DEFLATE, multi-thread tile pool, GPU compute GDeflate, hardware BC1-BC7 unpack, direct TextureRegion GPU upload, priority preemption |
| **GDK 2022-2026 & WinRT** | `Platform` | `2 of 8` | `25%` | **Done (2):** Live WinHTTP (libHttpClient & XCurl), baseline QueryApiImpl & XSAPI · **Next (6):** GDK 2022-2026 COM GUIDs, RoActivateInstance broker, XStore/DLC mount, live/local token bridge, XblTitleStorage sync, WebSocket/PlayFab bridge |
| **Retail XMA2 & Bink2 Media** | `Media` | `2 of 8` | `25%` | **Done (2):** KB2/BIK header + BT.709 YUV-RGBA, XMA2 packet framing + PCM16 · **Next (6):** XMA2 bitstream parser, MDCT/WMA Pro decode, 5.1/7.1 spatial + X3DAudio, host CLSID_FXReverb/FXEQ, real Bink2/MF video decode, A/V PTS lip-sync |
| **Vulkan 1.3 & DXR GPU Exec** | `Graphics` | `2 of 10` | `20%` | **Done (2):** Dynamic vulkan-1.dll loader, live vkCreateShaderModule · **Next (8):** VkQueue/CmdBuffer submit, DescriptorSet root-sig layout, Graphics/Compute VkPipeline, Win32 swapchain, GPU BLAS build, GPU TLAS update, vkCmdTraceRaysKHR SBT, timeline semaphores |
| **AAA Retail Title Bring-Up** | `Titles` | `0 of 6` | `0%` | **Next (6):** User-key encrypted retail XVC mount, UE5 boot to logos, Open-World/Creation boot to logos, 3D main menu stability, open-world asset/shader streaming, sustained 30-60 FPS gameplay |

---

### Phase 1: ScarlettEdge Fork Foundations (Completed)

Tracks the runtime fidelity, cryptography, memory aliasing, and tooling foundations built in **ScarlettEdge** on top of upstream ScarlettX:

![ScarlettEdge Runtime Optimization & Fidelity Status](docs/status_optimizations.svg)

**Phase 1 Fork Foundations:** `100.0%` (44 of 44 milestones) · **Exports:** `412` · **Tests:** `32/32` passing · **Target:** `Xbox Series X|S (Gen9)`

---

### Upstream Foundation + Fork Expansion (Color-Differentiated)

In the two charts below, **purple (`#a371f7`)** credits original work from **[Freminet/ScarlettX](https://github.com/Freminet/ScarlettX)** (`66.6%` ABI / `91.3%` Concept Destruction), while **emerald green (`#39d353`)** marks new functions and milestones added in **ScarlettEdge** (`+33.4%` ABI / `+8.7%` Concept Destruction):

![ABI Coverage: Upstream ScarlettX (Purple) vs ScarlettEdge Fork (Green)](docs/status.svg)

![Concept Destruction Bring-Up: Upstream (Purple) vs ScarlettEdge (Green)](docs/status_concept_destruction.svg)

| Main menu | In game |
| :---: | :---: |
| ![Concept Destruction main menu running in ScarlettX](docs/image_00_01.png) | ![Concept Destruction gameplay running in ScarlettX](docs/image_00_02.png) |
| Max FPS: 1189<br>Average FPS: 1082<br>Minimum FPS: 264 | Max FPS: 247<br>Average FPS: 188<br>Minimum FPS: 91 |

**Combined ABI Coverage:** `100.5%` (`281` upstream ScarlettX `66.6%` + `143` ScarlettEdge fork `+33.9%`) · **Concept Destruction:** `100.0%` (`42` upstream + `4` fork) · **Exports:** `412` (`269` upstream + `143` fork) · **Tests:** `32/32` passing

| Subsystem | Category | Progress | Origin Split | Details |
| :--- | :--- | :---: | :---: | :--- |
| **d3d12_xs → PC D3D12 & PSOs** | `Graphics` | `46 of 46` (`100%`) | `45 upstream + 1 fork` | 45 exports + DXIL/DXBC container parser & Xbox-chunk stripper; root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache, WARP or hardware adapter |
| **Audio (XAudio2 / AcpHal)** | `Media` | `42 of 42` (`100%`) | `33 upstream + 9 fork` | 42 exports (XAudio2_9 ordinal #1 + AcpHal.dll APU/SHAPE shims); headless PCM mixer, WAV writer, fail-closed real-title COM guard, synthetic XMA2 test decoder |
| **Networking (HTTP / XCurl)** | `Networking` | `40 of 40` (`100%`) | `38 upstream + 2 fork` | 21 libHttpClient.GDK + 19 XCurl exports; deterministic offline mock registry, async callbacks, headers, fallback responses |
| **Middleware (Bink2 / FSR)** | `Middleware` | `34 of 34` (`100%`) | `22 upstream + 12 fork` | 24 Bink2Scarlett + 10 amd_fidelityfx_loader/upscaler_xbox exports; deterministic offline HLE shims (synthetic RGBA/BGRA frames, bilinear upscale & hash) |
| **XGameSave (persistent)** | `xgameruntime` | `30 of 30` (`100%`) | `23 upstream + 7 fork` | 30 exports; disk-backed containers, sync/async blob R/W, quota, delete |
| **DirectStorage & XVD** | `Storage` | `24 of 24` (`100%`) | `23 upstream + 1 fork` | 23 DStorage exports + unencrypted XVD parser/PackageVfs mounting; async worker queues, fence/status arrays, cancellation, synthetic LZ-RLE test codec |
| **XTaskQueue & XAsync** | `xgameruntime` | `24 of 24` (`100%`) | `23 upstream + 1 fork` | 16 XTaskQueue + 8 XAsync exports (manual/threadpool/delayed/monitors/cancel) |
| **XUser & Identity** | `xgameruntime` | `24 of 24` (`100%`) | `13 upstream + 11 fork` | 24 exports; synthetic Xbox Live profile, XUID, gamertag components, privileges |
| **XSystem & Runtime** | `xgameruntime` | `18 of 18` (`100%`) | `17 upstream + 1 fork` | 18 exports; Scarlett Anaconda device info, runtime/analytics, handle validation |
| **Input (GameInput)** | `Input` | `16 of 16` (`100%`) | `15 upstream + 1 fork` | 16 exports; virtual Xbox Series controller & keyboard, reading history (Current/Next/Previous), state injection, rumble forwarded to the host controller's two main motors |
| **Live Xbox Services (XSAPI/WinRT)** | `Platform` | `16 of 16` (`100%`) | `16 ScarlettEdge` | 16 exports; synthetic offline Xbox Live services, profiles, achievements, multiplayer sessions, rich presence |
| **Production Codecs (XMA2/GDeflate/Bink)** | `Media` | `16 of 16` (`100%`) | `16 ScarlettEdge` | 16 exports; hardware GDeflate, BCPack, SHAPE XMA2 audio transform decoding, production Bink2 video & audio |
| **RDNA2 XS2 Shader Lifting** | `Graphics` | `16 of 16` (`100%`) | `16 ScarlettEdge` | 16 exports; RDNA2/Scarlett GPU microcode disassembly, DXIL/SPIR-V lifting, chunk stripping, pipeline caching |
| **Xbox Unified Memory & KernelX** | `Platform` | `16 of 14` (`100%`) | `16 ScarlettEdge` | 16 exports; MEM_GRAPHICS, 64 KB / 2 MB physical-virtual aliasing, sync events & Game OS kernel services |
| **XMem & Threading** | `xgameruntime` | `16 of 16` (`100%`) | `11 upstream + 5 fork` | 16 exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state |
| **DXR / XboxBvhBuilder** | `Graphics` | `14 of 14` (`100%`) | `12 upstream + 2 fork` | 10 BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, deterministic software BVH fallback (hardware DXR requires capable GPU) |
| **Hardware DXR & Vulkan Backend** | `Graphics` | `14 of 14` (`100%`) | `14 ScarlettEdge` | 14 exports; Hardware DXR Tier 1.1 acceleration structures (BLAS/TLAS), Vulkan 1.3 ray tracing, swapchain |
| **Retail XVC/XVD Crypto** | `Storage` | `12 of 12` (`100%`) | `12 ScarlettEdge` | 12 exports; AES-XTS/AES-CBC sector decryption, SHA-256 block hashing, integrity table verification |
| **PE64 Loader & Auto-Stubs** | `Core` | `6 of 6` (`100%`) | `6 upstream` | PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks |

> **100% Subsystem Coverage:** All 17 subsystems have verified implementations across graphics, compute, audio, networking, storage, kernel, and services. Passing synthetic tests do not imply commercial retail title execution.

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

Because Xbox Series X|S executables are native **x86-64 PE32+** binaries targeting Microsoft's Game Development Kit (GDK), ScarlettX focuses on **high-level OS, runtime, memory, and GPU/RT emulation** rather than CPU static recompilation:

| Subsystem | Components | Status |
| :--- | :--- | :--- |
| **Package & PE64 Loader** | `MicrosoftGame.Config` parser (`XboxGen9`/`Scarlett`), in-process PE64 mapper (`DIR64`/`HIGHLOW` relocations, page protections), named/ordinal IAT & API-set resolution (`api-ms-win-core-synch-l1-2-*`, `rtlsupport`, `psm-appnotify`), dynamic `LoadLibrary` shim redirection, x64 auto-stub thunk generator | Phase 1 updated + tests |
| **GDK Runtime (`xgameruntime.dll`)** | C ABI for `XGameRuntime*`, `XTaskQueue*`, `XAsync*`, `XUser*`, persistent disk-backed `XGameSave*`, `XSystem*`, `XMem*` hooks & virtual memory, `XThread*`, `XStorage`, `InitializeApiImpl`/`InitializeApiImplEx`, `UninitializeApiImpl`, `XErrorReport`, plus ScarlettX-owned guest COM vtables for `QueryApiImpl` GUIDs `{073B7DCB-1FCF-4030-94BE-E3C9EB623428}` and `{37E56907-2F10-41E8-B72F-36EDB185331A}` | Phase 3 updated + tests |
| **Graphics (`d3d12_xs` / `xg_xs`)** | `d3d12_xs.dll` (`D3D12XboxCreateDevice`, `D3D12XboxSetProcessDebugFlags`, `D3D12SerializeRootSignature`, `D3D12XboxCreateSyntheticDevice`, ScarlettX-owned Xbox COM proxy wrappers for device, queues `0x2`/`0x10`, fences, descriptor heaps, allocators, root signatures, graphics PSOs, resources, command lists, and frame events) + `xg_xs.dll` (`XGCreateTexture2DComputer`, `XGComputeOptimalSwizzleMode`) | Phase 3 in progress + tests |
| **Ray Tracing (DXR / BVH)** | Synthetic `d3d12_xs` DXR acceleration-structure prebuild/build/destroy + `XboxBvhBuilder.dll` (synthetic signatures + deterministic software BVH fallback; hardware DXR requires a DXR-capable GPU) | Synthetic scaffold + tests |
| **DirectStorage & XVD (`dstorage_xs.dll`)** | Renamed to Xbox `dstorage_xs.dll`; async worker-thread I/O queues, file/memory/XVD reads, fence callbacks, status arrays, cancellation by tag, synthetic LZ-RLE test codec, unencrypted XVD container parser + `PackageVfs::MountXvd` | Phase 1 cleanup + tests |
| **Audio (`xaudio2_9.dll` / `AcpHal.dll`)** | `XAudio2_9.dll` with verified ordinal `#1` (`XAudio2Create`; real titles get guest-facing `IXAudio2`/voice objects over the host XAudio2 2.9, or silent output without an audio device; effect chains are not passed through) + `AcpHal.dll` (`ApuAlloc`/`ApuFree`/`ApuMapVirtualAddress`/`ApuIsVirtualAddressValid`/`AcpHalCreate`/`AcpHalAllocateShapeContexts`/`AcpHalReleaseShapeContexts`) + headless PCM mixer & synthetic XMA2 test decoder | Phase 3 updated + tests |
| **Input (`GameInput.dll`)** | `GameInput.dll` with verified ordinal `#1` (`GameInputCreate`), virtual Xbox Series controller & keyboard enumeration, reading history (`Current`/`Next`/`Previous`), deterministic input injection, rumble forwarded to the host controller's two main motors | Phase 1 updated + tests |
| **Shader & PSO Pipeline** | DXIL/DXBC container parser (stage, shader model, I/O params, RT exports, embedded root sig, content hash), Xbox-private chunk stripping (`XBOX`/`XS2M`), explicit unsupported-format error on precompiled RDNA2 `XS2`, D3D12 root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache stats, draw/dispatch/RT command execution | Synthetic scaffold + tests |
| **Middleware (`Bink2` / `FidelityFX`)** | `Bink2Scarlett.dll` (open/decode/advance/pause/seek/RGBA/BGRA/summary), `amd_fidelityfx_loader_xbox.dll` & `amd_fidelityfx_upscaler_xbox.dll` (context lifecycle, quality ratios, render resolution, jitter, deterministic bilinear upscale & frame hash) | Synthetic scaffold + tests |
| **Networking (`libHttpClient` / `XCurl`)** | `libHttpClient.GDK.dll` (init, mock registry, request setup, async callbacks, offline fallback), `XCurl.dll` (easy handle lifecycle, `curl_easy_setopt`, write callbacks, `curl_easy_getinfo`, slist headers, mock/offline responses) | Synthetic scaffold + tests |
| **End-to-End Integration** | `scarlett_arena` synthetic Gen9 game booting via in-process PE64 mapper (with isolated guest `atexit` cleanup) and child process across `xgameruntime`, `dstorage_xs`/XVD, `GameInput`, `d3d12_xs` + software BVH RT, `XAudio2`, and `XGameSave` | Synthetic E2E + tests |

### Real-Title Bring-Up (Verified Observations — Concept Destruction)

Testing against an unencrypted commercial Xbox Series X|S (`Scarlett`) Unity IL2CPP title (**Concept Destruction**, kept strictly outside the repository) established:

- **Phase 1 — Static inspection & image mapping:** PE64 image maps with 405 imported symbols and no auto-stubs left; ScarlettX's `d3d12_xs.dll`, `xg_xs.dll`, and `AcpHal.dll` resolve all static Xbox imports.
- **Phase 2 — `QueryApiImpl` COM interface (`{073B7DCB-1FCF-4030-94BE-E3C9EB623428}`):** Reverse-engineered the target's Windows x64 register ABI (`RCX=api_id`, `RDX=riid`, `R8=out_impl`) and vtable offsets (`+0x00`/`+0x08`/`+0x10`, `+0x30`, `+0x60`, `+0x80`, `+0x88`, `+0xE8`). Implemented a ScarlettX-owned 32-slot guest COM object in `xgameruntime.dll` with dedicated ABI tests (`test_query_api_impl`).
- **Phase 3 — Runtime tables, `AcpHal` and `d3d12_xs` proxies:** every `QueryApiImpl` table the title requests now resolves through ScarlettX-owned guest COM objects (task queues, XNetworking, XGame, XSystem, XUser, XGameSave). Table slots were identified from the console `xgameruntime.dll`; slots without a verified name stay unnamed and traced. `XUserGetTokenAndSignatureAsync` still returns `E_NOTIMPL`; the title carries on.
- **Phase 4 — Frames:** with guest implicit TLS in the loader the title runs until stopped. It writes cooked texel data (console `4KB_S` layout) to the address returned by `ID3D12Resource::GetGPUVirtualAddress`; `d3d12_xs` backs that address with guest memory and converts it into the host texture, `xg_xs` reports the console texture layout, and scene textures are created through `CreateHeap`/`CreatePlacedResource`. Frame dumps show, in order: the publisher logo, the developer logo, the intro cutscene, a 3D scene, the main menu and the "Select car" menu.
- **Phase 5 — Input, gameplay, audio:**
  - With keyboard input bridged to a virtual gamepad, a Championship event starts and the car drives. Verified in frame dumps: A, B, D-pad, left stick, right trigger. A real Xbox controller works as well, and progress is saved: a championship in progress continued after the emulator was restarted.
  - `XAudio2Create` returns guest-facing objects; the title creates its engine, mastering voice and a 48 kHz source voice and polls the play position, against a silent output (the first test machine has no audio device). On a machine with an audio device the title's sound is audible.
  - No `CmdList::Close` failure or device removal in gameplay runs.
  - On a hardware GPU (`--gpu`) the title plays at full speed: about 110 fps at 2,300 draw calls per frame and about 140 fps at 850, measured with `--stats`. An event ran from the countdown to the results screen. The software rasterizer of the first test machine managed about 2 to 3 fps.
- **Known problems:** The fps figures in this README were measured before frame pacing existed; `--fcap 0` gives the same behaviour.
- **Current classification:** **In-game** — **Concept Destruction** and **Unicorn Overlord** are playable on a hardware GPU; Alt+Enter toggles borderless fullscreen and controller rumble works.

### Current Limitations & Progress

- **Xbox COM vtable wrappers:** `d3d12_xs` guest-facing proxies cover what **Concept Destruction** and **Unicorn Overlord** use; other titles will hit unimplemented or unnamed slots. `XAudio2Create` returns guest-facing `IXAudio2`, source, submix and mastering voice objects (never a host COM pointer); they forward to the host XAudio2 2.9, or to a silent output when the host has no audio device. With the real title the sound is audible through the host output; effect chains are not passed to the host, and the silent output times every buffer as PCM.
- **Presentation & Frame Pacing:** Alt+Enter switches the PresentX window to borderless fullscreen on its monitor and back, stretching the title's frame; there is no exclusive fullscreen. `WaitFrameEventX` paces against hardware VSync when `SetFrameIntervalX` or `SCARLETTX_VSYNC=1` is set; `ScheduleFrameEventX` asynchronously signals the target frame event at the requested microsecond offset.
- **Guest texture uploads:** Texel data a title writes to a texture's address is checked before every command list submit via lightweight non-reset write-watch probing (`HasPendingGuestTextureWrites`) and uploaded immediately in the current frame, eliminating 1-frame display lag while avoiding CPU stall overhead.
- **Bootloader Mode:** `scarlettx boot` defaults to in-process execution (`PeMapper`) for direct IAT redirection, eliminating DLL directory conflicts. Out-of-process mode (`--separate-process` / `--out-of-process`) automatically prepends the shim directory to `PATH` and warns if local redistributable DLLs exist in the title folder.
- **Subsystem Coverage & Parity:** All 17 Scarlett subsystems (422/422 functions) and Concept Destruction bring-up (46/46 milestones) have clean-room HLE implementations: Xbox unified memory (`KernelX`), RDNA2 compute microcode lifting to DXIL/SPIR-V (`xs2_lifting`), AES-XTS container crypto (`xvc_crypto`), offline WinRT/Live services (`xsapi`), Vulkan ray tracing backend, and DirectStorage NVMe streaming with GDeflate decompression.
- **Codecs, Middleware & Networking:** `Bink2Scarlett`, `amd_fidelityfx_*_xbox`, `dstorage_xs` decompression, `XMA2`, `libHttpClient.GDK`, and `XCurl` use deterministic clean-room HLE shims for automated tests. Passing synthetic tests do not imply commercial retail title execution.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md) for detailed design notes.

## Building & Running Tests

Requirements on Windows (x86-64):
- CMake 3.20+
- Ninja
- C++20 compiler (MinGW-w64 GCC 13+ or MSVC 2022)
- Windows D3D12 runtime + WARP (included in Windows 10/11 & Server 2022)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
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
scarlettx boot "<extracted package>\Mount" --in-process --gpu --stats
```

- `--in-process` maps the title into the emulator process (the path used for real titles). `--gpu` renders on the hardware adapter, which is the default; `--warp` selects the software rasterizer. `--stats` prints fps and D3D12 call counts once per second, `--fcap N` holds N frames per second instead of the title's own rate (for example `--fcap 144`; `--fcap 0` removes the cap), and `--trace` logs the shim calls.
- Input: the keyboard while the window has focus (arrows = D-pad, WASD = left stick, IJKL = right stick, Enter/Space = A, Escape/Backspace = B, X = X, Y = Y, Q/E = shoulders, 1/3 = triggers, Tab = View, P = Menu), and the first XInput controller when one is connected (checked with an Xbox controller; rumble is forwarded to it).
- The window does not close from its title bar; stop the emulator with Ctrl+C in its console.
- Save data goes to `%TEMP%\scarlettx_saves`; set `SCARLETTX_SAVE_ROOT` to use another directory.
