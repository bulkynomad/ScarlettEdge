---
description: Apply when debugging guest title crashes, hangs, D3D12 DRED device removals, VEH/SEH stack unwinding, or analyzing Flight-Recorder logs and scarlettx triage output.
trigger: model_decision
---

# Flight-Recorder, Crash Unwinding & Triage Debugging Protocol

1. **Async-Signal & Fault-Context Safety (`src/core/flight_recorder.cpp`, `src/main.cpp`):**
   - `AddVectoredExceptionHandler` (`VEH`) and `SetConsoleCtrlHandler` run in hostile contexts (threads holding heap locks, PEB Loader Lock, `STATUS_HEAP_CORRUPTION`, `EXCEPTION_STACK_OVERFLOW`, or OS-injected Ctrl threads).
   - Never call `malloc`/`new`, `std::filesystem`, `std::ofstream`, `std::mutex::lock`, or `GetModuleFileName*` directly inside a faulting `VEH` handler or `Ctrl+C` callback; signal an atomic flag or delegate serialization to a pre-spawned dumper thread with pre-allocated buffers, and always reset re-entrancy guards (`g_active_crash_tid`) on every return path.
   - Filter benign first-chance handled guest `EXCEPTION_ACCESS_VIOLATION` guard-page probes before consuming crash-report quotas.
2. **Accurate Win64 Stack Unwinding (`RtlLookupFunctionEntry` / `StubThunk`):**
   - Every dynamically generated thunk (`StubThunkTable` `sub rsp, 0x48`) and manually mapped guest PE64 image must have valid `.pdata` registered via `RtlAddFunctionTable` (with zero-tail padding stripped) so `RtlVirtualUnwind` resolves exact guest RVAs (`0x140000000 + RVA`).
3. **End-to-End Triage Loop (`scarlettx boot` -> `scarlettx triage`):**
   - Reproduce guest issues with `./build/scarlettx boot <pkg> --debug-layer --flight-recorder <dir> --hang-timeout 15`.
   - Inspect `<dir>/flight_summary.json`, `crash_report_*.json`, `hang_snapshot_*.json`, `gpu_dred_report_*.json`, and `shader_fail_*` via `./build/scarlettx triage <dir> --json`, then cross-check missing imports, COM IIDs, and vtable slots against `@docs/AUDIT_BASELINE.md`.
