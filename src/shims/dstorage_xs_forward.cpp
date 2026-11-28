// Unified forwarding & COM/C-ABI dispatch shim for dstorage_xs.dll.
// Forwards real-title and COM DStorageGetFactory(REFIID, void**) calls to xcrdapi.dll
// without ever writing to stack-allocated GUIDs, while preserving 1-arg synthetic
// DStorageGetFactory(uint64_t*) compatibility solely for ScarlettGuestMain test slices.

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "scarlettx/io/dstorage_abi.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif
#endif

extern "C" {

#if defined(_WIN32)
#  define SX_DS_XS_EXPORT __declspec(dllexport)
#  define SX_DS_XS_CALL WINAPI
#else
#  define SX_DS_XS_EXPORT __attribute__((visibility("default")))
#  define SX_DS_XS_CALL
#endif

// Imported from xcrdapi.dll for real-title COM DirectStorage factory queries.
int32_t SX_DS_XS_CALL ScarlettXcrdGetFactory(const void* riid, void** out);

namespace {

bool IsRealTitleEnvSet() {
    if (const char* env = std::getenv("SCARLETTX_REAL_TITLE"); env && env[0] == '1') {
        return true;
    }
#if defined(_WIN32)
    char buf[8]{};
    const DWORD len = GetEnvironmentVariableA("SCARLETTX_REAL_TITLE", buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf) && buf[0] == '1') {
        return true;
    }
#endif
    return false;
}

#if defined(_WIN32)
bool IsReadableRange(const uint8_t* base, size_t offset, size_t len, size_t image_limit) {
    if (offset > image_limit || len > image_limit - offset) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base + offset, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const auto* reg_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return reg_end >= (base + offset + len);
}

// Only synthetic test-harness PE64 binaries exporting "ScarlettGuestMain" (mapped via PeMapper
// with dstorage.dll -> dstorage_xs.dll redirection) invoke the 1-arg uint64_t* C-ABI overload.
// Every real title, GDK sample, and COM caller invokes 2-arg DStorageGetFactory(REFIID, void**).
bool CallerExportsScarlettGuestMain(const void* caller_rip) {
    if (!caller_rip) return false;
    MEMORY_BASIC_INFORMATION caller_mbi{};
    if (VirtualQuery(caller_rip, &caller_mbi, sizeof(caller_mbi)) != sizeof(caller_mbi) ||
        caller_mbi.State != MEM_COMMIT || !caller_mbi.AllocationBase) {
        return false;
    }

    const auto* base = static_cast<const uint8_t*>(caller_mbi.AllocationBase);
    if (!IsReadableRange(base, 0, sizeof(IMAGE_DOS_HEADER), 0x1000u)) return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) {
        return false;
    }
    const size_t nt_off = static_cast<size_t>(dos->e_lfanew);
    if (!IsReadableRange(base, nt_off, sizeof(IMAGE_NT_HEADERS64), nt_off + sizeof(IMAGE_NT_HEADERS64))) {
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + nt_off);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }

    const size_t img_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY exp_dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dd.VirtualAddress == 0 || exp_dd.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return false;
    }
    if (!IsReadableRange(base, exp_dd.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), img_size)) {
        return false;
    }

    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_dd.VirtualAddress);
    const uint32_t num_names = exp->NumberOfNames;
    if (num_names == 0 || num_names > 4096u || exp->AddressOfNames == 0) {
        return false;
    }
    if (!IsReadableRange(base, exp->AddressOfNames, static_cast<size_t>(num_names) * sizeof(uint32_t), img_size)) {
        return false;
    }

    const auto* name_rvas = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
    static constexpr char kHarnessEntry[] = "ScarlettGuestMain";
    static constexpr size_t kHarnessEntryLen = sizeof(kHarnessEntry);
    for (uint32_t i = 0; i < num_names; ++i) {
        const uint32_t rva = name_rvas[i];
        if (rva == 0 || !IsReadableRange(base, rva, kHarnessEntryLen, img_size)) continue;
        if (std::memcmp(base + rva, kHarnessEntry, kHarnessEntryLen) == 0) {
            return true;
        }
    }
    return false;
}
#endif

}  // namespace

SX_DS_XS_EXPORT int32_t SX_DS_XS_CALL DStorageXsSetConfiguration(const DSTORAGE_CONFIGURATION* config) {
    return DStorageSetConfiguration(config);
}

}  // extern "C"

// Note: dstorage_xs.def exports the 25 DStorage* names forwarding to dstorage.dll,
// except DStorageGetFactory which forwards COM (REFIID, void**) to xcrdapi.dll without
// touching stack-allocated GUIDs.
extern "C" SX_DS_XS_EXPORT int32_t SX_DS_XS_CALL DStorageXsGetFactoryDispatch(void* arg0_rcx, void** arg1_rdx) {
    if (!IsRealTitleEnvSet()) {
#if defined(_WIN32)
#if defined(_MSC_VER)
        const void* caller_rip = _ReturnAddress();
#else
        const void* caller_rip = __builtin_return_address(0);
#endif
        if (CallerExportsScarlettGuestMain(caller_rip)) {
            return DStorageGetFactory(static_cast<uint64_t*>(arg0_rcx));
        }
#endif
    }
    return ScarlettXcrdGetFactory(arg0_rcx, arg1_rdx);
}
