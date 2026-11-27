# ScarlettEdge Architecture

**ScarlettEdge** (a clean-room high-level emulation fork of [Freminet/ScarlettX](https://github.com/Freminet/ScarlettX)) executes native **Xbox Series X|S (`XboxGen9` / `Scarlett` Game Core)** x86-64 PE32+ titles on Windows 10/11 x64 without CPU translation, intercepting the Xbox OS, GDK runtime, unified memory, DirectStorage, audio, input, and `d3d12_xs` / `xg_xs` graphics boundary.

---

## 1. Execution & Module Loading Pipeline

1. **Manifest & Container Discovery (`src/loader/game_config.cpp`, `src/runtime/emulator.cpp`, `src/loader/package_vfs.cpp`):**
   - Parses `MicrosoftGame.Config` (`TargetDeviceFamily` = `XboxGen9` / `Scarlett`), extracts `TitleId` and primary executable metadata, maps virtual `G:\` via `DefineDosDeviceA` (`src/runtime/virtual_drive.cpp`) with crash-safe cleanup markers, and mounts any `.xvd` / `.xvc` volumes (using `--volume-key` / `SCARLETTX_VOLUME_KEY` when encrypted).
2. **In-Process PE64 Mapper (`src/loader/pe_mapper.cpp`, `src/loader/guest_modules.cpp`):**
   - Maps the guest `.exe` into process memory, applies `IMAGE_REL_BASED_DIR64` / `HIGHLOW` relocations, registers `.pdata` exception unwind tables (`RtlAddFunctionTable`) so guest C++ exceptions unwind cleanly, binds implicit TLS (`TEB->ThreadLocalStoragePointer` slot 0 and dynamic `_tls_index` callbacks), and isolates guest `atexit` tables.
3. **Two-Tier DLL Precedence Policy:**
   - **System / Hardware HLE DLLs (Shim-First):** `xgameruntime.dll` (`xmem.dll`), `d3d12_xs.dll`, `xg_xs.dll`, `AcpHal.dll`, `xcrdapi.dll`, `dstorage_xs.dll`, `xaudio2_9.dll`, `GameInput.dll`, `XboxBvhBuilder.dll`, `kernelx.dll`, `xvc_crypto.dll`, `xbox_codecs.dll`, `xsapi.dll`, `xs2_lifting.dll`, `vulkan_backend.dll`, and `pixevt_stub.dll` always resolve from ScarlettEdge's shim directory.
   - **Title Middleware DLLs (Package-First, HLE Fallback):** `Bink2Scarlett.dll`, `amd_fidelityfx_loader_xbox.dll`, `amd_fidelityfx_upscaler_xbox.dll`, `libHttpClient.GDK.dll`, and `XCurl.dll` load from the game's package directory first when shipped with the title (and are patched via `PatchHostModuleForGuests`), falling back to ScarlettEdge's HLE shims only when absent.
   - **Unresolved Imports (`src/loader/stub_thunk.cpp`):** Any missing named or ordinal import receives an executable x86-64 thunk that captures register arguments, logs diagnostics, and returns a configurable status value.

---

## 2. Graphics, Shaders, & Texture Layout (`d3d12_xs`, `xg_xs`, `xs2_lifting`, `vulkan_backend`)

- **COM Vtable Isolation (`src/shims/d3d12_xs_shim.cpp`):**
  - Real Scarlett titles never receive raw Windows `ID3D12*` COM pointers; every interface (`Device`, `CommandQueue`, `GraphicsCommandList`, `Resource`, `Heap`, `DescriptorHeap`, `Fence`, `CommandAllocator`, `RootSignature`, `PipelineState`, `StateObject`) uses a ScarlettEdge-owned Xbox vtable layout supporting SDK versions `0x20001` through `0x20006` (including the `+4` command-list slot shift at slot `>= 28` and the `+2` SDK `0x20004+` shift).
  - Resource and root-signature proxies are unwrapped across legacy barriers (`ResourceBarrier`), D3D12 Enhanced Barriers (`ID3D12GraphicsCommandList7::Barrier`), and DXR `CreateStateObject` subobjects.
- **Zero-Lag Write-Watched Guest Textures (`src/shims/d3d12_xs_shim.cpp`, `src/gpu/xg_layout.cpp`):**
  - On Xbox's unified memory architecture, titles write cooked swizzled texels directly to `ID3D12Resource::GetGPUVirtualAddress()`. ScarlettEdge backs texture GPU VAs with `MEM_WRITE_WATCH` memory, probes dirty pages before `ExecuteCommandLists`, deswizzles modified subresources via `xg_layout` (`4KB_S`, `256B_S`, `64KB_S/D/Z_X/R_X`, 2D arrays, CubeMaps), and batches uploads in a single copy submission.
- **Shader Translation & RDNA2 Lifting (`src/gpu/shader_ir.cpp`, `src/gpu/xs2_lifting_shim.cpp`):**
  - Standard DXBC/DXIL shaders containing Xbox-private chunks (`XBOX`, `XS2M`, `XDBG`, `XBIN`) are stripped and have their retail 16-byte DXBC modified-MD5 checksum recomputed automatically during `CreateGraphicsPipelineState` / `CreateComputePipelineState`.
  - Precompiled RDNA2 (`XS2\0`) shaders are decoded by `xs2_lifting.dll` (`SOPP`, `SMEM`, `VOP1/2/3`, `VMEM`, `MIMG`, `LDS`, `EXP`, `VINTRP`) and lifted into SPIR-V 1.5 SSA modules validated against host `vulkan-1.dll` (`vkCreateShaderModule`).

---

## 3. Unified Memory, Storage, Crypto, & Runtime Services

- **Unified Memory & Physical Aliasing (`src/kernel/kernelx_shim.cpp`):**
  - Uses `CreateFileMappingW` + `VirtualAlloc2` (`MEM_RESERVE_PLACEHOLDER` with `MEM_ADDRESS_REQUIREMENTS` for guaranteed 64 KB and 2 MB large-page alignment) + `MapViewOfFile3` so multiple CPU virtual addresses (`KernelXCreateMemoryAlias`) map coherently onto the same physical pages while maintaining CPU VA ↔ GPU VA translation.
- **Containers, Crypto, & DirectStorage (`src/loader/xvd_container.cpp`, `src/loader/xvc_crypto.cpp`, `src/shims/dstorage_guest.cpp`):**
  - `aes_xts_cng.h` dynamically binds Windows `bcrypt.dll` for cached-key FIPS-197 AES-128/256-XTS (including IEEE 1619 ciphertext stealing), AES-CTR stream transforms, and FIPS 180-4 SHA-256 verification.
  - Real titles import `xcrdapi.dll!DStorageGetFactory` (`dstorage_guest.cpp`), which executes concurrent `FILE_FLAG_OVERLAPPED` reads with `G:\` VFS fallback, `destination_page_offset` support, status arrays, D3D12 fence signaling, and `0x04 0xFB` tile GDeflate / `BCPK` codec dispatch.
- **GDK Runtime, Audio, & Input (`src/shims/xgameruntime_shim.cpp`, `src/shims/xgr_task_queue.cpp`, `src/shims/xaudio2_guest.cpp`, `src/shims/gameinput_shim.cpp`):**
  - Full `XTaskQueue` / `XAsync` engine, `QueryApiImpl` COM tables, persistent `XGameSave` storage, `XAsync`-driven `xsapi.dll` services, XAudio 2.9 guest proxies with XAPO `EffectChain` passthrough (and headless null-output fallback), and `GameInput` keyboard + XInput controller/rumble bridging.
