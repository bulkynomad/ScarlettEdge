#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct PeSectionInfo {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_data_offset = 0;
    uint32_t raw_data_size = 0;
    uint32_t characteristics = 0;
};

struct PeImportSymbol {
    bool by_ordinal = false;
    uint16_t ordinal = 0;
    std::string name;
};

struct PeImportModule {
    std::string dll_name;
    std::string redirected_target;
    bool is_scarlett_hle = false;
    std::vector<PeImportSymbol> symbols;
};

struct PeImageInfo {
    std::string file_path;
    bool is_pe64 = false;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    uint64_t image_base = 0;
    uint32_t entry_point_rva = 0;
    uint32_t size_of_image = 0;
    std::vector<PeSectionInfo> sections;
    std::vector<PeImportModule> imports;
};

// Shared two-tier DLL precedence classifiers kept synchronized between static PE
// inspection (pe_loader.cpp) and in-process mapping / dynamic loading (pe_mapper.cpp).
//  - System HLE shims: ScarlettEdge HLE DLLs always take precedence over package/System32.
//  - Fallback middleware: Title-shipped package DLLs take precedence when present, falling
//    back to ScarlettEdge HLE middleware shims (e.g. xsapi.dll, Bink2Scarlett.dll) if absent.
const char* LookupSystemShimTarget(const std::string& lower_dll_name) noexcept;
const char* LookupFallbackMiddlewareTarget(const std::string& lower_dll_name) noexcept;

class DllRedirectionTable {
public:
    DllRedirectionTable();

    void RegisterRedirect(const std::string& guest_dll, const std::string& host_dll, bool is_hle);
    bool Resolve(const std::string& guest_dll, std::string& out_host_dll, bool& out_is_hle) const;

private:
    struct Entry {
        std::string host_dll;
        bool is_hle = false;
    };
    std::unordered_map<std::string, Entry> entries_;
};

Status InspectPeBytes(const std::vector<uint8_t>& data,
                      const std::string& display_path,
                      const DllRedirectionTable& redirects,
                      PeImageInfo& out_info);

Status InspectPeFile(const std::string& pe_path,
                     const DllRedirectionTable& redirects,
                     PeImageInfo& out_info);

}  // namespace scarlettx
