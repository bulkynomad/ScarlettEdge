// Unified forwarding & COM/C-ABI dispatch shim for dstorage_xs.dll.
// Shares 100% of DStorageState with dstorage.dll and forwards real-title
// COM DStorageGetFactory(REFIID, void**) calls to xcrdapi.dll.

#include "scarlettx/io/dstorage_abi.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {

#if defined(_WIN32)
#  define SX_DS_XS_EXPORT __declspec(dllexport)
#else
#  define SX_DS_XS_EXPORT __attribute__((visibility("default")))
#endif

// Imported from xcrdapi.dll for real-title COM DirectStorage factory queries.
int32_t
#if defined(_WIN32)
WINAPI
#endif
ScarlettXcrdGetFactory(const void* riid, void** out);

namespace {

bool IsRealTitleOrReadOnlyGuidPtr(const void* ptr) {
    if (const char* env = std::getenv("SCARLETTX_REAL_TITLE"); env && env[0] == '1') {
        return true;
    }
#if defined(_WIN32)
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (!writable) {
            return true;
        }
    }
#endif
    return false;
}

}  // namespace

SX_DS_XS_EXPORT int32_t DStorageXsSetConfiguration(const DSTORAGE_CONFIGURATION* config) {
    return DStorageSetConfiguration(config);
}

}  // extern "C"

// Note: dstorage_xs.def exports the exact 25 DStorage* names forwarding directly to dstorage.dll,
// except DStorageGetFactory which safely dispatches COM (REFIID, void**) vs synthetic (uint64_t*).
extern "C" SX_DS_XS_EXPORT int32_t DStorageXsGetFactoryDispatch(void* arg0_rcx, void** arg1_rdx) {
    if (IsRealTitleOrReadOnlyGuidPtr(arg0_rcx)) {
        return ScarlettXcrdGetFactory(arg0_rcx, arg1_rdx);
    }
    return DStorageGetFactory(static_cast<uint64_t*>(arg0_rcx));
}
