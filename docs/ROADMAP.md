# ScarlettEdge Engineering Roadmap

This document tracks the technical progression of **ScarlettEdge** from clean-room GDK/HLE foundations (Phase 1, completed) to unmodified commercial AAA Xbox Series X|S execution (Phase 2, active).

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
- **Networking & CI:** Opt-in live WinHTTP transport (`SCARLETTX_LIVE_NETWORK=1`) with UTF-8 URLs, request headers, and binary `CURLOPT_POSTFIELDSIZE`, backed by automated GitHub Actions Windows MSVC `/W4` CI (`32/32` CTest suites).

---

## Phase 2: Retail AAA Execution Roadmap (Active Next Wave)

| Track | Completed Sub-Goals | Next Engineering Milestones |
| :--- | :--- | :--- |
| **1. XS2 → GPU Shader Compiler (`12/12` — Complete)** | RDNA2 ISA decode, 256-VGPR SPIR-V 1.5 emitter, stage `Output` exports, SMEM UBOs, MIMG `OpImageSampleImplicitLod`, `v_interp`, disk cache, SSBO `StorageBuffer` Load/Store, structured `OpLoopMerge`/`OpSelectionMerge` CFG, Wave32 `GroupNonUniform` subgroups, automatic `d3d12_xs` `XS2\0` & `XS2M` PSO interception, signed SM 6.5 `dxcompiler.dll` DXIL + SM 5.0 `D3DCompile` DXBC bridge | Extend full RDNA2 ISA coverage on retail AAA shader dumps (tessellation, ray-query inline BVH ops, 16-bit packed math) |
| **2. XG Tiling, MSAA & DCC (`8/10`)** | `4KB_S`, `256B_S`, `64KB_S/D`, `64KB_R_X`, `64KB_Z_X` depth, 2D arrays & 6-face CubeMaps, 64 KB packed mip-tails, 3D volume swizzling (`XGCreateTexture3DComputer`) | 2x/4x/8x MSAA sample addressing, RDNA2 DCC color & HTILE depth metadata decompression |
| **3. Vulkan 1.3 & DXR GPU Execution (`8/10`)** | Dynamic `vulkan-1.dll` loader, `VkDevice`/`VkQueue`/`VkCommandBuffer`, RootSig → `VkDescriptorSetLayout`, live `vkCreateShaderModule`, live `VkComputePipeline` + UBO/SSBO GPU execution (`VulkanXsDispatchComputeSpirv`), Win32 `VK_KHR_win32_surface` binding, GPU AS backing buffers, Vulkan 1.2 timeline semaphores | Hardware `VK_KHR_acceleration_structure` BLAS/TLAS traversal builds, `vkCmdTraceRaysKHR` SBT dispatch |
| **4. DirectStorage GPU Codecs (`6/8`)** | Concurrent `FILE_FLAG_OVERLAPPED` I/O, `0x04 0xFB` tile framing & multi-thread pool, OS priority queues, `xcrdapi` codec hookup, spec-compliant RFC 1951 canonical Huffman DEFLATE + RFC 1950 Zlib (Adler-32), hardware BC1 4x4 RGBA8 unpack | GPU compute GDeflate decompression, direct `TextureRegion` GPU upload |
| **5. D3D12.2 & `MEM_GRAPHICS` (`6/10`)** | 128-slot vtables, `CL7::Barrier` proxy unwrapping, `VirtualAlloc2` aliasing, 2 MB aligned large pages, `CreatePlacedResourceX` on `KernelX`/alias GPUVAs, `ID3D12Device2::CreatePipelineState` stream PSOs (`VS`/`PS`/`CS`/`AS`/`MS`) | Variable Rate Shading (VRS), Sampler Feedback/Tiled resources, `ExecuteIndirect` count buffers, Work Graphs |
| **6. GDK 2022–2026 & WinRT (`8/8` — Complete)** | GDK 2022–2026 `QueryApiImpl` COM GUIDs (`XStore`, `XPackage` 2025, `XUser` 2025, `XNetworking` slots 8/9/10/14), `RoActivateInstance` / `RoGetActivationFactory` WinRT `IInspectable` COM broker, `XStore` license + `XPackage` DLC mount, CNG HMAC-SHA256 `XBL3.0` JWT token signing, persistent disk-backed `XblTitleStorage`, and `HCWebSocket` RTA/PlayFab offline + live `WinHttpWebSocket` bridge | Expand WinRT namespace surface as demanded by individual commercial titles |
| **7. Retail XMA2 & Bink2 Media (`8/8` — Complete)** | 2048-byte retail `XMA2` multi-stream bitstream parser, Princen-Bradley sine-windowed `MDCT`/`IMDCT` ($N=128$, 50% TDAC overlap-add) + 52-byte `0x0166` `XMA2WAVEFORMATEX` guest COM voice bridge, ITU-R BS.775 `5.1`/`7.1` downmix + `X3DAudio` spherical panning/Doppler, host `System32\xaudio2_9.dll` `CreateFX` (`CLSID_FXReverb`, `FXEQ`, `FXMasteringLimiter`), `KB2` frame-offset YCbCr 4:2:0 decode, and QPC A/V PTS lip-sync | Optional Media Foundation H.264/HEVC `IMFSourceReader` hardware decode for non-Bink cutscenes |
| **8. AAA Engine & XVC Bring-Up (`6/6` — Vertical-Slice Complete)** | Cycle-safe `GDFX` (`MICROSOFT*XBOX*MEDIA`) filesystem extraction & `PackageVfs` file mounting inside AES-XTS encrypted `.xvc` volumes, direct `scarlettx boot`/`compat` `.xvc` support, `ue5_slice.exe` & `openworld_slice.exe` Gen9 PE64 vertical-slice titles, 64-slot GPU stability breadcrumbs + TDR check, concurrent multi-queue `GDeflate`+`XS2`+alias churn, and PSO prewarm + QPC `p99` frame-time profiler | Complete Option A hardware silicon items (`MSAA`/`DCC`/`HTILE`, `VK_KHR_ray_tracing_pipeline`, GPU `GDeflate`, `VRS`/Tiled/Work Graphs) for unmodified external retail games |
