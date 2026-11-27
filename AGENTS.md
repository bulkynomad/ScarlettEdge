# ScarlettEdge Workspace Invariants

ScarlettEdge is a clean-room Windows x64 high-level emulator (HLE) for Xbox Series X|S (`XboxGen9` / `Scarlett` Game Core) PE32+ titles. Read `@docs/AUDIT_BASELINE.md` before planning architectural changes, modifying COM vtables, or updating status docs.

## Build, Test & Hygiene Commands

Run from repository root in PowerShell or MSVC x64 Developer Prompt:

- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Check incremental graph cleanliness: `ninja -C build -n` (must output `no work to do`)
- Run test suite: `ctest --test-dir build --parallel 8 --output-on-failure`
- Run clean-room & IP gate: `python tools/check_cleanroom.py --commit-range origin/main..HEAD`
- CLI diagnostics & triage: `./build/scarlettx diag --full` and `./build/scarlettx triage <flight_dir> [--json]`

## C++20, Compiler & Win64 ABI Rules

- Target C++20 (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`) on x86-64 Windows (`MSVC` `/W4` primary, MinGW-w64 secondary). Define `WIN32_LEAN_AND_MEAN` and `NOMINMAX` before `<windows.h>`.
- Never introduce new warnings under `/W4`. Validate pointers, flags, and out-parameters explicitly rather than relying on `/wd4100`.
- Guest-facing exports and COM vtable methods must use explicit Win64 calling conventions (`__stdcall` / `WINAPI` / `SCARLETTX_GDK_CALLBACK`) and exact parameter types—never erase >4-arg or `float`/`XMM` methods behind generic 4-integer function pointers (`uintptr_t a1..a3`).
- On MSVC x64 COM interfaces, methods returning non-trivial or aggregate structs by value (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`, `GetAdapterLuid`) pass `this` in `RCX` and hidden return pointer (`sret`) in `RDX`—never guess `RCX` vs `RDX` via `VirtualQuery`.
- Never perform heap allocations, `std::mutex` locks, `std::filesystem` calls, or `GetModuleFileName*` (PEB Loader Lock) inside `AddVectoredExceptionHandler`, `SetConsoleCtrlHandler`, or `SuspendThread` scopes.

## Real-Title vs. Synthetic Harness Boundary & Locked Architecture

Real Xbox titles execute ONLY through retail guest entrypoints (`SCARLETTX_REAL_TITLE=1` / `mainCRTStartup`):
- Graphics & Shaders: `src/shims/d3d12_xs_shim.cpp` COM proxies (`D3D12XboxCreateDevice`) -> Host D3D12 Agility SDK + `src/gpu/shader_ir.cpp` + `src/gpu/xg_layout.cpp`.
- Storage & Audio: `src/shims/dstorage_guest.cpp` (`xcrdapi.dll`) and `src/shims/xaudio2_guest.cpp` (`xaudio2_9.dll` `XAudio2Create`).
- GDK Runtime: `src/shims/xgr_api_tables.cpp` (`QueryApiImpl` COM tables) + `src/shims/xgameruntime_shim.cpp` + `src/shims/xgr_task_queue.cpp`.

Do NOT implement real-title fixes inside unreachable synthetic test APIs (`d3d12_xs_shim.cpp` `D3D12XboxCreateSyntheticDevice` tail, `dstorage_shim.cpp`, `xaudio2_shim.cpp` handle mixer, `xs2_lifting_shim.cpp`, `vulkan_backend_shim.cpp`, `xvc_crypto.cpp`, or orphaned `src/kernel/x*.cpp`). Commit exclusively to Host D3D12 (not `vulkan_backend.dll`), fail closed on unknown RDNA2 opcodes in `shader_ir.cpp`, and keep third-party codecs clean-room safe (package-first guest DLLs or optional external LGPL FFmpeg `LoadLibraryW` outside the repo).

## Anti-Stub & Verification Invariant

- **Never mark an API or subsystem complete if it returns a no-op success stub without functional semantics and a verifying test.**
- Returning `S_OK` / `0` while leaving out-pointers/tokens uninitialized, dropping GPU barriers (`FlushPipelineX`), aliasing unknown `QueryInterface` IIDs to `self`, or matching hardcoded test-input bytes is strictly prohibited. Unimplemented slots must log a `FlightRecorder` miss and return an honest failure code (`E_NOTIMPL` / `E_NOINTERFACE`).
- Every real-title HLE fix must include a CTest running through the guest COM / `SCARLETTX_REAL_TITLE=1` boundary using deterministic Win32 event/fence synchronization (never arbitrary `Sleep()` delays or temp writes directly inside `build/`).

## Clean-Room, Legal & Git Guardrails

- Strictly obey `CLEANROOM_POLICY.md`: zero proprietary Microsoft GDK headers/libs/DLLs/`.nupkg`, commercial binaries/assets/shaders, containers (`.xvc`, `.msixvc`, `.xvd`), keys (`.cik`, `.ekb`, `.lekb`), or host user paths inside this repository.
- Use only generic technical names (`openworld_slice`, `ue5_slice`, `real_title_com`)—never third-party publisher trademarks or codenames in identifiers, docs, or commit messages.
- Never push to `upstream` (`Freminet/ScarlettX`). Only push to `origin` when explicitly requested by the user.
