---
description: Enforce PE64 multi-DLL loading, zero-staging VFS/XVD streaming, GDK QueryApiImpl slot safety, and audio/storage/middleware ABI rules when editing loader, crypto, kernel, GDK, audio, storage, or middleware files.
trigger: glob
globs: "**/src/loader/**/*.cpp, **/src/loader/**/*.h, **/include/scarlettx/loader/**/*.h, **/include/scarlettx/crypto/**/*.h, **/include/scarlettx/io/**/*.h, **/src/runtime/**/*.cpp, **/include/scarlettx/runtime/**/*.h, **/src/kernel/**/*.cpp, **/include/scarlettx/kernel/**/*.h, **/src/shims/*.h, **/src/shims/xgameruntime_shim.cpp, **/src/shims/xgr_*.cpp, **/src/shims/xsapi_shim.cpp, **/src/shims/dstorage*.cpp, **/src/shims/xaudio2*.cpp, **/src/shims/acphal_shim.cpp, **/src/shims/gameinput_shim.cpp, **/src/shims/codecs_shim.cpp, **/src/shims/ffx_*.cpp, **/src/shims/bink2_*.cpp, **/src/shims/libhttpclient_shim.cpp, **/src/shims/xcurl_shim.cpp, **/include/scarlettx/gdk/*.h, **/include/scarlettx/middleware/*.h"
---

# Loader, VFS/XVD, GDK `QueryApiImpl` & Guest Middleware ABI Rules

1. **Two-Tier DLL Precedence & Multi-DLL Guest Hooking (`pe_mapper.cpp`, `pe_loader.cpp`, `guest_modules.cpp`, `stub_thunk.cpp`):**
   - Keep `pe_loader.cpp` and `pe_mapper.cpp` redirection tables 100% synchronized.
   - Never hijack title-shipped C++/C thunk middleware (`Microsoft.Xbox.Services.*.GDK.C.Thunks.dll`, unversioned `Microsoft.Xbox.Services.C.Thunks.dll`, `Bink2Scarlett.dll`, `amd_fidelityfx_*`, `libHttpClient.GDK.dll`, `XCurl.dll`) in `ResolveSystemShimTarget`; route them through `ResolveFallbackMiddlewareTarget` so package-local binaries load natively first.
   - Any dynamic code thunk (`StubThunkTable`) that modifies `RSP` must register `RUNTIME_FUNCTION` `.pdata` via `RtlAddFunctionTable` and transition executable pages from `PAGE_EXECUTE_READWRITE` to `PAGE_EXECUTE_READ`.
   - Apply full `LookupGuestRuntimeHook` IAT hooks (`CreateFileW/2`, `FindFirstFile*`, `VirtualAlloc` `MEM_GRAPHICS` strip, `LoadLibrary*`, `GetProcAddress`, `RoGetActivationFactory`, `FlsAlloc`) to all package-loaded guest DLLs (`GameAssembly.dll`, engine/middleware modules), not solely the main `.exe`.
2. **Zero-Staging XVD/XVC Streaming & Crypto (`xvd_container.cpp`, `package_vfs.cpp`, `aes_xts_cng.h`, `emulator.cpp`):**
   - Never extract whole `.xvc`/`.msixvc` containers to `%TEMP%` on boot; serve `G:\` Win32 and DirectStorage reads directly from mounted `XvdContainer`s with concurrent `std::shared_mutex` + LRU sector caching, single-call 4 KiB `BCRYPT_XTS_AES_ALGORITHM` decryption, and 16-byte `.cik` KeyId GUID matching against `XvcInfo`.
3. **GDK `QueryApiImpl`, DirectStorage (`xcrdapi`), Audio & Middleware Safety:**
   - Verify exact GDK vtable slot order and struct sizes (`XPackageDetails` = `0x50` bytes including `titleId`; `XDisplayAcquireTimeoutDeferral` at slot 3 vs `Close` at slot 4; `FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT` = `0x10005`). Never leave output handles/tokens uninitialized on `S_OK`.
   - Real titles use `dstorage_guest.cpp` (`xcrdapi.dll`, supporting `BTYPE=0/1/2` Dynamic-Huffman 32-lane GDeflate via a persistent worker pool) and `xaudio2_guest.cpp` (`XAudio2Create`). Never block `XTaskQueue` worker threads with synchronous `WinHTTP` or uncached directory scans, and never free the active front DMA buffer in `IXAudio2SourceVoice::FlushSourceBuffers`.
