---
description: Enforce Xbox d3d12_xs COM vtable ABI, GPU upload/barrier synchronization, real ID3D12Heap aliasing, XG swizzle accuracy, and fail-closed shader translation rules when editing GPU or graphics shim files.
trigger: glob
globs: "**/src/shims/d3d12_xs*.cpp, **/src/shims/*d3d12*.h, **/src/shims/xg_xs*.cpp, **/src/shims/*xg_xs*.h, **/src/shims/xbox_bvh_builder_shim.cpp, **/src/gpu/**/*.cpp, **/src/gpu/**/*.h, **/include/scarlettx/gpu/**/*.h"
---

# GPU, `d3d12_xs`, `xg_xs` & Shader Translation Rules

1. **Retail COM Path vs. Synthetic Tail (`d3d12_xs_shim.cpp`):**
   - Real titles enter `D3D12XboxCreateDevice` and use `XboxD3D12*Proxy` COM vtables backed by Host D3D12 + Agility SDK. Lines `7050+` (`D3D12XboxCreateSyntheticDevice`, `D3D12XboxCreateWorkGraph`, etc.) and standalone `xs2_lifting.dll` / `vulkan_backend.dll` are synthetic test islands with zero callers in real-title execution. Wire all production GPU features into the COM proxies and `src/gpu/shader_ir.cpp`.
2. **Strict COM `QueryInterface` & Typed Vtable Signatures:**
   - Never return `*out = self; return S_OK` for unrecognized IIDs—check explicit IID tables, record unhandled IIDs via `FlightRecorder::RecordUnhandledQi`, and return `E_NOINTERFACE` (`0x80004002`).
   - Declare exact Win64 parameter lists (`float` `XMM0..XMM3` args, `>4` stack args, and `RCX=this, RDX=sret` struct-return methods) before casting to vtable slots. Account for SDK version vtable shifts (`0x20001`..`0x20006`, `+4` at CmdList slot 28, `+2` V4 shifts).
3. **GPU Synchronization, Upload Ring & Heap Aliasing:**
   - Never `memcpy` into a mapped `D3D12_HEAP_TYPE_UPLOAD` buffer referenced by an in-flight command list or an earlier draw in the same command list; suballocate from a fence-retired upload ring buffer and reclaim transient guest slabs.
   - `CmdListFlushPipelineX` (slot 67) must emit real D3D12 UAV/aliasing/transition barriers—never a no-op `return 0`.
   - Back `CreateHeap` and `CreatePlacedResource` / `CreatePlacedResourceX` with real host `ID3D12Heap` objects at exact byte offsets to preserve resource memory aliasing (never isolated `CreateCommittedResource` fallbacks unless host alignment strictly requires an explicit, logged fallback).
   - Cache `ID3D12GraphicsCommandList4/5/6/7` pointers on proxy creation and use stack small-buffers (`std::array`) instead of per-call `std::vector` heap allocations.
4. **Fail-Closed RDNA2 / DXIL Translation & Tiling:**
   - In `shader_ir.cpp`, never silently map unknown RDNA2 ISA opcodes to fallback arithmetic (`v_fma_f32`, `v_mov_b32`, `s_nop`) or emit hardcoded test geometry. Fail closed, dump the failing binary via `FlightRecorder`, and report the exact unimplemented encoding/opcode.
   - Do not assume `XG_SWIZZLE_MODE_4KB_S` for placed/64KB resources; distinguish linear, `4KB_S`, and `64KB_*` modes explicitly.
