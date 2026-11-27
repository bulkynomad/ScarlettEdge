---
name: check-abi-layout
description: Audit Xbox Series X|S COM vtable slots, Win64 calling conventions, struct sizes/offsets, sret return pointers, and two-tier DLL precedence when adding or fixing HLE shims.
---

# Win64 & Xbox Gen9 ABI Layout Verification Procedure

1. **Calling Convention & Register Mapping:** Confirm `__stdcall` / `WINAPI` / `SCARLETTX_GDK_CALLBACK` and exact parameter types (`RCX`, `RDX`, `R8`, `R9`, `XMM0..XMM3` float args, and `[rsp+0x28]+` stack slots). Never cast >4-arg or float methods through a 4-integer `ComVtableFn`.
2. **MSVC x64 `sret` Struct Return Rule:** For COM methods returning structs by value (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`, `GetAdapterLuid`), verify `RCX = this` and `RDX = out_struct*` (`sret`).
3. **Compile-Time Layout Locks:** Add `static_assert(sizeof(...) == ...)` and `static_assert(offsetof(..., field) == ...)` for every guest-visible ABI struct (`XPackageDetails`, `FFX` descriptors, `D3D12` Xbox structs).
4. **SDK Version Vtable Shifts & `QueryInterface`:** Check Xbox SDK version shifts (`0x20001..0x20006`) and ensure `QueryInterface` checks explicit IIDs and returns `E_NOINTERFACE` (+ `FlightRecorder` miss) for unknown IIDs.
5. **Out-Pointers & Precedence:** Initialize all out-handles/tokens before returning `S_OK`, and verify package-shipped middleware DLLs route through `ResolveFallbackMiddlewareTarget` rather than `ResolveSystemShimTarget`.
