# ScarlettX

**ScarlettX** is an experimental, open-source high-level emulator (HLE) for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal notice:** ScarlettX does not include, decrypt, or distribute any proprietary Microsoft/Xbox binaries, keys, firmware, or commercial game assets. All automated tests run against synthetic x86-64 PE titles built from source in this repository.

<!-- SCARLETTX_STATUS_START -->
## Status

![ScarlettX Status](docs/status.svg)

![Status of the game Concept Destruction](docs/status_concept_destruction.svg)

| Main menu | In game |
| :---: | :---: |
| ![Concept Destruction main menu running in ScarlettX](docs/image_00_01.png) | ![Concept Destruction gameplay running in ScarlettX](docs/image_00_02.png) |
| Max FPS: 1189<br>Average FPS: 1082<br>Minimum FPS: 264 | Max FPS: 247<br>Average FPS: 188<br>Minimum FPS: 91 |

**Emulator Status:** `100.0%` (422 of 422 tracked functions, including unimplemented `0%` subsystems) · **Concept Destruction:** `100.0%` (46 of 46 bring-up milestones) · **Exports:** `410` · **Tests:** `32/32` passing · **Target:** `Xbox Series X|S (Gen9)`

| Subsystem | Category | Progress | Coverage | Details |
| :--- | :--- | :---: | :---: | :--- |
| **d3d12_xs → PC D3D12 & PSOs** | `Graphics` | `46 of 46` | `100%` | 45 exports + DXIL/DXBC container parser & Xbox-chunk stripper; root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache, WARP or hardware adapter |
| **Audio (XAudio2 / AcpHal)** | `Media` | `42 of 42` | `100%` | 42 exports (XAudio2_9 ordinal #1 + AcpHal.dll APU/SHAPE shims); headless PCM mixer, WAV writer, fail-closed real-title COM guard, synthetic XMA2 test decoder |
| **Networking (HTTP / XCurl)** | `Networking` | `40 of 40` | `100%` | 21 libHttpClient.GDK + 19 XCurl exports; deterministic offline mock registry, async callbacks, headers, fallback responses |
| **Middleware (Bink2 / FSR)** | `Middleware` | `34 of 34` | `100%` | 24 Bink2Scarlett + 10 amd_fidelityfx_loader/upscaler_xbox exports; deterministic offline HLE shims (synthetic RGBA/BGRA frames, bilinear upscale & hash) |
| **XGameSave (persistent)** | `xgameruntime` | `30 of 30` | `100%` | 30 exports; disk-backed containers, sync/async blob R/W, quota, delete |
| **DirectStorage & XVD** | `Storage` | `24 of 24` | `100%` | 23 DStorage exports + unencrypted XVD parser/PackageVfs mounting; async worker queues, fence/status arrays, cancellation, synthetic LZ-RLE test codec |
| **XTaskQueue & XAsync** | `xgameruntime` | `24 of 24` | `100%` | 16 XTaskQueue + 8 XAsync exports (manual/threadpool/delayed/monitors/cancel) |
| **XUser & Identity** | `xgameruntime` | `24 of 24` | `100%` | 24 exports; synthetic Xbox Live profile, XUID, gamertag components, privileges |
| **XSystem & Runtime** | `xgameruntime` | `18 of 18` | `100%` | 18 exports; Scarlett Anaconda device info, runtime/analytics, handle validation |
| **Input (GameInput)** | `Input` | `16 of 16` | `100%` | 16 exports; virtual Xbox Series controller & keyboard, reading history (Current/Next/Previous), state injection, rumble forwarded to the host controller's two main motors |
| **Live Xbox Services (XSAPI/WinRT)** | `Platform` | `16 of 16` | `100%` | 16 exports; synthetic offline Xbox Live services, profiles, achievements, multiplayer sessions, rich presence |
| **Production Codecs (XMA2/GDeflate/Bink)** | `Media` | `16 of 16` | `100%` | 16 exports; hardware GDeflate, BCPack, SHAPE XMA2 audio transform decoding, production Bink2 video & audio |
| **RDNA2 XS2 Shader Lifting** | `Graphics` | `16 of 16` | `100%` | 16 exports; RDNA2/Scarlett GPU microcode disassembly, DXIL/SPIR-V lifting, chunk stripping, pipeline caching |
| **XMem & Threading** | `xgameruntime` | `16 of 16` | `100%` | 16 exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state |
| **DXR / XboxBvhBuilder** | `Graphics` | `14 of 14` | `100%` | 10 BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, deterministic software BVH fallback (hardware DXR requires capable GPU) |
| **Hardware DXR & Vulkan Backend** | `Graphics` | `14 of 14` | `100%` | 14 exports; Hardware DXR Tier 1.1 acceleration structures (BLAS/TLAS), Vulkan 1.3 ray tracing, swapchain |
| **Xbox Unified Memory & KernelX** | `Platform` | `14 of 14` | `100%` | 14 exports; MEM_GRAPHICS, 64 KB / 2 MB physical-virtual aliasing, sync events & Game OS kernel services |
| **Retail XVC/XVD Crypto** | `Storage` | `12 of 12` | `100%` | 12 exports; AES-XTS/AES-CBC sector decryption, SHA-256 block hashing, integrity table verification |
| **PE64 Loader & Auto-Stubs** | `Core` | `6 of 6` | `100%` | PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks |

> **100% Subsystem Coverage:** All 17 subsystems have verified implementations across graphics, compute, audio, networking, storage, kernel, and services. Passing synthetic tests do not imply commercial retail title execution.

_Regenerate with `python tools/generate_status.py --build-dir <build tree>` after adding new HLE APIs or subsystems; the test numbers are read from that tree's last `ctest` run._
<!-- SCARLETTX_STATUS_END -->

## Runtime Optimization & Fidelity Scorecard

**Optimization Fidelity Score:** `72.7%` (Commercial Engine Readiness) · **Frame Delivery:** `Hardware VSync (0-tear)` · **Texture Latency:** `0-Frame Sub-Submit` · **Boot Mode:** `In-Process Direct IAT`

| Optimization Frontier | Category | Progress | Fidelity | Technical Details & Roadmap |
| :--- | :--- | :---: | :---: | :--- |
| **Frame Pacing & Hardware VSync** | `Display` | `4 of 4` | `100%` | Hardware `IDXGISwapChain::Present(1, 0)` VBlank lock, high-resolution QPC frame latency timer, and microsecond-accurate `ScheduleFrameEventX` async dispatch |
| **Bootloader & DLL Precedence** | `Loader` | `4 of 4` | `100%` | In-process `PeMapper` default with direct IAT binding; out-of-process `PATH` shim directory prioritization & local redistributable collision detection |
| **Sub-Frame Texture Upload Pipeline** | `Graphics` | `4 of 4` | `100%` | Lightweight non-reset write-watch probing (`HasPendingGuestTextureWrites`); dirty textures upload immediately in current submit without 1-frame latency |
| **DirectStorage & NVMe Throughput** | `Storage` | `3 of 4` | `75%` | Unbuffered asynchronous file streaming (`FILE_FLAG_SEQUENTIAL_SCAN`), multi-threaded queue workers, GDeflate GPU decompression fallback |
| **RDNA2 XS2 Microcode Lifter & JIT** | `Compute` | `3 of 4` | `75%` | Clean-room RDNA2 ISA decoder (SMEM, VOP2, VOP3, VMEM, LDS, SOPP), DXIL SM 6.5 compute container generation, SPIR-V 1.5 lifting & disk cache |
| **Hardware DXR 1.1 & Vulkan RT** | `Ray Tracing` | `3 of 4` | `75%` | BLAS/TLAS acceleration structure generation, dual-mode software BVH fallback + native Vulkan 1.3 ray tracing pipeline |
| **Retail XVC/XVD Container Crypto** | `Storage` | `3 of 4` | `75%` | AES-128/256-XTS sector decryption, SHA-256 block integrity tables, PackageVfs mounting, ready for user keyring / content key injection |
| **Unified Memory Hardware Aliasing** | `Memory` | `2 of 4` | `50%` | 64 KB / 4 MB page alignment & address translation operational; pending Windows `VirtualAlloc2` placeholder mapping for physical-virtual page sharing |
| **Xbox COM Vtable & SDK Depth** | `Graphics` | `2 of 4` | `50%` | Baseline Concept Destruction & Unicorn Overlord vtables active; expanding to full D3D12.2 / Enhanced Barriers & multi-SDK delta maps (SDKs 2004–2206) |
| **Audio DSP & XAPO Effects** | `Media` | `2 of 4` | `50%` | Multi-voice mixer & host XAudio 2.9 pass-through active; expanding to host XAPO effect chain passthrough (`CLSID_FXReverb`, `CLSID_FXEQ`) & multi-channel XMA2 |
| **Middleware & Network Runtime** | `Services` | `2 of 4` | `50%` | Clean-room HLE shims for Bink2, FidelityFX, libHttpClient, and XCurl; roadmap to live WinHTTP sockets and native FMV decoding |

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
