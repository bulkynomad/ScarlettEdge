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
| **1. XG Tiling, MSAA & DCC** | `4KB_S`, `256B_S`, `64KB_S/D`, `64KB_R_X`, `64KB_Z_X` depth, 2D arrays & 6-face CubeMaps | 64 KB packed mip-tails, 3D volume swizzling (`_D`/`_S`), 2x/4x/8x MSAA sample addressing, RDNA2 DCC color & HTILE depth metadata decompression |
| **2. XS2 → GPU Shader Compiler** | RDNA2 ISA decode, 256-VGPR SPIR-V 1.5 SSA emitter, stage `Output` exports, SMEM UBOs, MIMG `OpImageSampleImplicitLod`, `v_interp`, disk shader cache | SSBO `buffer_load`/`buffer_store` descriptors, structured CFG loop/branch reconvergence, Wave32 subgroup ops, automatic `d3d12_xs` `XS2\0` PSO interception, SPIR-V → DXIL bridge |
| **3. DirectStorage GPU Codecs** | Concurrent `FILE_FLAG_OVERLAPPED` I/O, `0x04 0xFB` tile framing & multi-thread pool, OS priority queues, `xcrdapi` codec hookup | Spec-compliant RFC 1951 / Microsoft 32-stream bit-interleaved GDeflate Huffman decoder, hardware BCPACK BC1–BC7 block unpacker, GPU compute decompression, direct texture region upload |
| **4. Vulkan 1.3 & DXR GPU Execution** | Dynamic `vulkan-1.dll` loader, `VkDevice`/`VkQueue`/`VkCommandBuffer`, `VkDescriptorSetLayout`, live `vkCreateShaderModule` | Graphics/Compute `VkPipeline` creation, Win32 `VK_KHR_win32_surface` swapchain, GPU `VK_KHR_acceleration_structure` BLAS/TLAS builds, `vkCmdTraceRaysKHR` SBT dispatch, timeline semaphores |
| **5. D3D12.2 & `MEM_GRAPHICS`** | 128-slot vtables, `CL7::Barrier` proxy unwrapping, `VirtualAlloc2` aliasing, 2 MB aligned large pages | Console-verified command-list vtable slots above slot 67, unified `XMem`/`KernelX`/`D3D12` GPUVA allocator for `CreatePlacedResourceX`, Mesh Shader PSOs, VRS, Sampler Feedback, `ExecuteIndirect` count buffers |
| **6. GDK 2022–2026 & WinRT** | Live WinHTTP (`libHttpClient` & `XCurl`), `XAsync`-integrated `xsapi.dll`, baseline `QueryApiImpl` tables | Additional 2022–2026 `QueryApiImpl` interface GUIDs, `RoActivateInstance` WinRT factory broker, `XStore` DLC/license mounting, `XblTitleStorage` local sync, WebSocket (`HCWebSocket*`) transport |
| **7. Retail XMA2 & Media** | Package-first native `Bink2Scarlett.dll` execution, `KB2` header + fallback YUV-RGBA, XMA2 packet framing | Real XMA2 bitstream parser + WMA-Pro MDCT decoder, 5.1/7.1 spatial downmix + `X3DAudio`, host `CLSID_FXReverb`/`FXEQ`, A/V presentation timestamp lip-sync |
| **8. AAA Retail Title Bring-Up** | — | Full NTFS/FATX/GDFX filesystem extraction inside decrypted XVC volumes, Unreal Engine 5 boot-to-logos, Open-World / Creation Engine boot-to-logos, 3D menu stability, open-world streaming, sustained 30–60 FPS |
