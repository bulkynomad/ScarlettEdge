#pragma once

// Loader services a manually mapped guest image needs from the host process:
// Windows-managed implicit TLS, a registry of guest modules for the module
// queries the OS loader cannot answer, and import overrides built on it.

#include <cstdint>
#include <string>

#include "scarlettx/core/status.h"

namespace scarlettx {
namespace guest {

struct TlsBinding {
    void* helper_module = nullptr;  // generated carrier DLL that owns the TLS index
    std::wstring helper_path;
    uint32_t index = 0;
    int slot = -1;
};

// Gives the image at `image_base` real implicit TLS. `tls_directory` points at its
// IMAGE_TLS_DIRECTORY64 inside the mapped (and relocated) image. The image's index
// variable is patched, so the page holding it must still be writable.
// is_exe: also point TLS slot 0 of every thread at the image block, because
// executables are compiled to read slot 0 without consulting _tls_index.
Status AttachTls(uint8_t* image_base, const void* tls_directory, bool is_exe, TlsBinding* out);
// Lets new and exiting threads run the image's TLS callbacks. Call once the
// image is executable and its process-attach callbacks have run.
void EnableTlsCallbacks(TlsBinding* binding);
void DetachTls(TlsBinding* binding);

void RegisterModule(uint8_t* base, uint32_t size, const std::string& path, bool is_exe);
void UnregisterModule(uint8_t* base);

// Base of the guest module containing `address`, or null.
void* FindModuleBase(const void* address);

// Replacement for a host import the guest (or a host CRT acting for it) must not
// call directly. `target_dll_lower` is the resolved host DLL name in lower case.
void* LookupImportOverride(const std::string& target_dll_lower, const std::string& symbol);

// Redirects `RtlPcToFileHeader` in the import table of a host module, so MSVC C++
// exception throw info inside guest images resolves to the guest's base.
void PatchHostModuleForGuests(void* host_module);

}  // namespace guest
}  // namespace scarlettx
