#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/stub_thunk.h"

namespace scarlettx {

enum class ImportResolutionKind {
    HleRegistry,
    HostDll,
    StubThunk,
};

struct PeImportResolution {
    std::string dll_name;
    std::string target_dll;
    std::string symbol_name;
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    ImportResolutionKind kind = ImportResolutionKind::HostDll;
    uint64_t resolved_address = 0;
    uint32_t stub_id = 0;
};

// In-process HLE symbol override table (checked before host DLLs or stub thunks).
class HleSymbolRegistry {
public:
    void RegisterSymbol(const std::string& dll_name, const std::string& symbol_name, void* address);
    void RegisterOrdinal(const std::string& dll_name, uint16_t ordinal, void* address);

    void* LookupSymbol(const std::string& dll_name, const std::string& symbol_name) const;
    void* LookupOrdinal(const std::string& dll_name, uint16_t ordinal) const;

private:
    std::unordered_map<std::string, void*> named_symbols_;
    std::unordered_map<std::string, void*> ordinal_symbols_;
};

struct PeMapOptions {
    bool force_relocate = false;
    bool auto_stub_missing = true;
    uint64_t default_stub_return = 0;
    bool run_tls_callbacks = true;
    bool verbose_stub_logging = false;
    std::string shim_directory;
};

// Maps a PE32+ (x86-64) image directly into the current process, applies base
// relocations, resolves and patches IAT entries (supporting name, ordinal,
// API-set redirection, HLE registry overrides, and x86-64 auto-stub thunks),
// and applies per-section memory protections.
class MappedPeImage {
public:
    MappedPeImage() = default;
    ~MappedPeImage();

    MappedPeImage(const MappedPeImage&) = delete;
    MappedPeImage& operator=(const MappedPeImage&) = delete;

    Status MapFile(const std::string& pe_path,
                   const DllRedirectionTable& redirects,
                   const HleSymbolRegistry& hle_registry,
                   const PeMapOptions& options = {});

    void Unmap();

    bool IsMapped() const noexcept { return mapped_base_ != nullptr; }
    uint64_t MappedBase() const noexcept { return reinterpret_cast<uint64_t>(mapped_base_); }
    uint64_t PreferredImageBase() const noexcept { return preferred_image_base_; }
    bool WasRelocated() const noexcept { return MappedBase() != preferred_image_base_; }
    size_t RelocationsApplied() const noexcept { return relocations_applied_; }
    uint32_t SizeOfImage() const noexcept { return size_of_image_; }

    void* EntryPointAddress() const noexcept;
    void* GetExportAddress(const std::string& export_name) const;
    void* GetExportAddress(uint16_t ordinal) const;

    // Invokes an exported `int __cdecl Fn(void)` guest entry point in-process.
    Status CallExportedEntry(const std::string& export_name, int& out_exit_code) const;

    // Invokes the PE image's native AddressOfEntryPoint in-process.
    Status CallEntryPoint(int& out_exit_code) const;

    const std::vector<PeImportResolution>& Resolutions() const noexcept { return resolutions_; }
    const StubThunkTable& Stubs() const noexcept { return stubs_; }

private:
    uint8_t* mapped_base_ = nullptr;
    uint64_t preferred_image_base_ = 0;
    uint32_t size_of_image_ = 0;
    uint32_t entry_point_rva_ = 0;
    uint32_t export_dir_rva_ = 0;
    uint32_t export_dir_size_ = 0;
    size_t relocations_applied_ = 0;
    bool is_dll_ = false;
    bool dll_attached_ = false;

    std::vector<void*> loaded_modules_;
    void* loader_state_ = nullptr;  // TLS binding, unwind table and module registration
    std::vector<PeImportResolution> resolutions_;
    StubThunkTable stubs_;
};

// Resolves common Windows / Xbox Game Core API-set virtual DLL names
// (api-ms-win-core-*, api-ms-win-crt-*, ext-ms-win-*) to concrete host DLLs.
std::string ResolveApiSetDll(const std::string& dll_name);

// Aborts an active in-process guest call via longjmp if executing on the guest entry thread.
void TryAbortActiveInProcessGuest(uint32_t exit_code) noexcept;

}  // namespace scarlettx
