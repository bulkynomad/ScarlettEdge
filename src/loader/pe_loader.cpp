#include "scarlettx/loader/pe_loader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace scarlettx {

namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

struct DllTargetMapping {
    const char* guest;
    const char* target;
};

// OS / hardware system DLLs: always prioritize ScarlettEdge HLE shims.
constexpr DllTargetMapping kSystemShimTable[] = {
    {"xgameruntime.dll", "xgameruntime.dll"},
    {"xmem.dll", "xgameruntime.dll"},
    {"d3d12_xs.dll", "d3d12_xs.dll"},
    {"xg_xs.dll", "xg_xs.dll"},
    {"acphal.dll", "AcpHal.dll"},
    {"gameinput.dll", "GameInput.dll"},
    {"xaudio2_9.dll", "xaudio2_9.dll"},
    {"xaudio2_9redist.dll", "xaudio2_9.dll"},
    {"x3daudio1_7.dll", "xaudio2_9.dll"},
    {"xapofx1_5.dll", "xaudio2_9.dll"},
    {"xcrdapi.dll", "xcrdapi.dll"},
    {"xboxbvhbuilder.dll", "XboxBvhBuilder.dll"},
    {"dstorage_xs.dll", "dstorage_xs.dll"},
    {"dstorage.dll", "dstorage_xs.dll"},
    {"dstoragecore.dll", "dstorage_xs.dll"},
    {"kernelx.dll", "kernelx.dll"},
    {"xvc_crypto.dll", "xvc_crypto.dll"},
    {"xbox_codecs.dll", "xbox_codecs.dll"},
    {"xsapi.dll", "xsapi.dll"},
    {"xs2_lifting.dll", "xs2_lifting.dll"},
    {"vulkan_backend.dll", "vulkan_backend.dll"},
    {"pixevt.dll", "pixevt_stub.dll"},
    {"pixevt_stub.dll", "pixevt_stub.dll"},
};

// Title-shipped middleware & GDK C thunk DLLs: prioritize the real DLL inside
// the game package/VFS when present, falling back to ScarlettEdge HLE shims only when absent.
constexpr DllTargetMapping kFallbackMiddlewareTable[] = {
    {"bink2scarlett.dll", "Bink2Scarlett.dll"},
    {"amd_fidelityfx_loader_xbox.dll", "amd_fidelityfx_loader_xbox.dll"},
    {"amd_fidelityfx_upscaler_xbox.dll", "amd_fidelityfx_upscaler_xbox.dll"},
    {"libhttpclient.gdk.dll", "libHttpClient.GDK.dll"},
    {"xcurl.dll", "XCurl.dll"},
    {"microsoft.xbox.services.c.thunks.dll", "xsapi.dll"},
    {"microsoft.xbox.services.140.gdk.c.thunks.dll", "xsapi.dll"},
    {"microsoft.xbox.services.141.gdk.c.thunks.dll", "xsapi.dll"},
    {"microsoft.xbox.services.142.gdk.c.thunks.dll", "xsapi.dll"},
};

template <typename T>
bool ReadAt(const std::vector<uint8_t>& buf, size_t offset, T& out) {
    if (offset + sizeof(T) > buf.size()) {
        return false;
    }
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    return true;
}

bool ReadCString(const std::vector<uint8_t>& buf, size_t offset, std::string& out) {
    if (offset >= buf.size()) {
        return false;
    }
    size_t end = offset;
    while (end < buf.size() && buf[end] != 0) {
        ++end;
    }
    if (end == buf.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(buf.data() + offset), end - offset);
    return true;
}

bool RvaToFileOffset(uint32_t rva, const std::vector<PeSectionInfo>& sections, size_t& out_offset) {
    for (const auto& sec : sections) {
        if (rva >= sec.virtual_address) {
            const uint32_t offset_in_sec = rva - sec.virtual_address;
            if (offset_in_sec < sec.raw_data_size) {
                out_offset = static_cast<size_t>(sec.raw_data_offset) + offset_in_sec;
                return true;
            }
        }
    }
    return false;
}

#pragma pack(push, 1)
struct PeCoffHeader {
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
};

struct PeDataDirectory {
    uint32_t virtual_address;
    uint32_t size;
};

struct PeSectionHeaderRaw {
    char name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_linenumbers;
    uint16_t number_of_relocations;
    uint16_t number_of_linenumbers;
    uint32_t characteristics;
};

struct PeImportDescriptorRaw {
    uint32_t original_first_thunk;
    uint32_t time_date_stamp;
    uint32_t forwarder_chain;
    uint32_t name_rva;
    uint32_t first_thunk;
};
#pragma pack(pop)

bool EqualsIgnoreCaseAscii(const std::string& a, const char* b_lower) noexcept {
    const size_t n = std::strlen(b_lower);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const char la = (ca >= 'A' && ca <= 'Z') ? static_cast<char>(ca + ('a' - 'A'))
                                                 : static_cast<char>(ca);
        if (la != b_lower[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* LookupSystemShimTarget(const std::string& lower_dll_name) noexcept {
    for (const auto& m : kSystemShimTable) {
        if (EqualsIgnoreCaseAscii(lower_dll_name, m.guest)) {
            return m.target;
        }
    }
    return nullptr;
}

const char* LookupFallbackMiddlewareTarget(const std::string& lower_dll_name) noexcept {
    for (const auto& m : kFallbackMiddlewareTable) {
        if (EqualsIgnoreCaseAscii(lower_dll_name, m.guest)) {
            return m.target;
        }
    }
    return nullptr;
}

DllRedirectionTable::DllRedirectionTable() {
    for (const auto& m : kSystemShimTable) {
        RegisterRedirect(m.guest, m.target, true);
    }
    for (const auto& m : kFallbackMiddlewareTable) {
        RegisterRedirect(m.guest, m.target, true);
    }
}

void DllRedirectionTable::RegisterRedirect(const std::string& guest_dll,
                                           const std::string& host_dll,
                                           bool is_hle) {
    entries_[ToLowerAscii(guest_dll)] = Entry{host_dll, is_hle};
}

bool DllRedirectionTable::Resolve(const std::string& guest_dll,
                                  std::string& out_host_dll,
                                  bool& out_is_hle) const {
    auto it = entries_.find(ToLowerAscii(guest_dll));
    if (it == entries_.end()) {
        return false;
    }
    out_host_dll = it->second.host_dll;
    out_is_hle = it->second.is_hle;
    return true;
}

Status InspectPeBytes(const std::vector<uint8_t>& data,
                      const std::string& display_path,
                      const DllRedirectionTable& redirects,
                      PeImageInfo& out_info) {
    out_info = PeImageInfo{};
    out_info.file_path = display_path;

    uint16_t mz = 0;
    if (!ReadAt(data, 0, mz) || mz != 0x5A4D) {
        return Status::Error(StatusCode::UnsupportedFormat, "Not a valid MZ executable");
    }

    uint32_t pe_offset = 0;
    if (!ReadAt(data, 0x3C, pe_offset)) {
        return Status::Error(StatusCode::ParseError, "Truncated DOS header");
    }

    uint32_t pe_sig = 0;
    if (!ReadAt(data, pe_offset, pe_sig) || pe_sig != 0x00004550) {
        return Status::Error(StatusCode::UnsupportedFormat, "Missing PE\\0\\0 signature");
    }

    PeCoffHeader coff{};
    if (!ReadAt(data, pe_offset + 4, coff)) {
        return Status::Error(StatusCode::ParseError, "Truncated COFF header");
    }

    out_info.machine = coff.machine;
    const size_t opt_offset = pe_offset + 4 + sizeof(PeCoffHeader);
    uint16_t opt_magic = 0;
    if (!ReadAt(data, opt_offset, opt_magic)) {
        return Status::Error(StatusCode::ParseError, "Truncated Optional Header");
    }

    if (opt_magic != 0x20B) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Expected PE32+ (64-bit x86-64) executable for Scarlett");
    }
    out_info.is_pe64 = true;

    ReadAt(data, opt_offset + 16, out_info.entry_point_rva);
    ReadAt(data, opt_offset + 24, out_info.image_base);
    ReadAt(data, opt_offset + 56, out_info.size_of_image);
    ReadAt(data, opt_offset + 68, out_info.subsystem);

    uint32_t num_rva_and_sizes = 0;
    ReadAt(data, opt_offset + 108, num_rva_and_sizes);
    const size_t dd_offset = opt_offset + 112;

    PeDataDirectory import_dir{};
    if (num_rva_and_sizes > 1) {
        ReadAt(data, dd_offset + sizeof(PeDataDirectory) * 1, import_dir);
    }

    const size_t sections_offset = opt_offset + coff.size_of_optional_header;
    out_info.sections.reserve(coff.number_of_sections);
    for (uint16_t i = 0; i < coff.number_of_sections; ++i) {
        PeSectionHeaderRaw raw{};
        if (!ReadAt(data, sections_offset + i * sizeof(PeSectionHeaderRaw), raw)) {
            return Status::Error(StatusCode::ParseError, "Truncated section table");
        }
        PeSectionInfo sec;
        size_t len = 0;
        while (len < 8 && raw.name[len] != '\0') {
            ++len;
        }
        sec.name.assign(raw.name, len);
        sec.virtual_address = raw.virtual_address;
        sec.virtual_size = raw.virtual_size;
        sec.raw_data_offset = raw.pointer_to_raw_data;
        sec.raw_data_size = raw.size_of_raw_data;
        sec.characteristics = raw.characteristics;
        out_info.sections.push_back(std::move(sec));
    }

    if (import_dir.virtual_address != 0) {
        size_t imp_file_off = 0;
        if (!RvaToFileOffset(import_dir.virtual_address, out_info.sections, imp_file_off)) {
            return Status::Error(StatusCode::ParseError, "Failed to map import directory RVA");
        }

        for (size_t idx = 0;; ++idx) {
            PeImportDescriptorRaw desc{};
            if (!ReadAt(data, imp_file_off + idx * sizeof(PeImportDescriptorRaw), desc)) {
                break;
            }
            if (desc.name_rva == 0 && desc.first_thunk == 0 && desc.original_first_thunk == 0) {
                break;
            }

            size_t dll_name_off = 0;
            if (!RvaToFileOffset(desc.name_rva, out_info.sections, dll_name_off)) {
                continue;
            }

            PeImportModule mod;
            if (!ReadCString(data, dll_name_off, mod.dll_name)) {
                continue;
            }

            if (!redirects.Resolve(mod.dll_name, mod.redirected_target, mod.is_scarlett_hle)) {
                mod.redirected_target = mod.dll_name;
                mod.is_scarlett_hle = false;
            }

            const uint32_t thunk_rva =
                desc.original_first_thunk != 0 ? desc.original_first_thunk : desc.first_thunk;
            size_t thunk_off = 0;
            if (thunk_rva != 0 && RvaToFileOffset(thunk_rva, out_info.sections, thunk_off)) {
                for (size_t t = 0;; ++t) {
                    uint64_t thunk_val = 0;
                    if (!ReadAt(data, thunk_off + t * sizeof(uint64_t), thunk_val) || thunk_val == 0) {
                        break;
                    }
                    PeImportSymbol sym;
                    constexpr uint64_t kOrdinalFlag64 = 1ull << 63;
                    if (thunk_val & kOrdinalFlag64) {
                        sym.by_ordinal = true;
                        sym.ordinal = static_cast<uint16_t>(thunk_val & 0xFFFFu);
                    } else {
                        size_t hint_name_off = 0;
                        if (RvaToFileOffset(static_cast<uint32_t>(thunk_val & 0xFFFFFFFFu),
                                            out_info.sections,
                                            hint_name_off)) {
                            ReadCString(data, hint_name_off + 2, sym.name);
                        }
                    }
                    mod.symbols.push_back(std::move(sym));
                }
            }

            out_info.imports.push_back(std::move(mod));
        }
    }

    return Status::Ok();
}

Status InspectPeFile(const std::string& pe_path,
                     const DllRedirectionTable& redirects,
                     PeImageInfo& out_info) {
    std::ifstream ifs(pe_path, std::ios::binary);
    if (!ifs) {
        out_info = PeImageInfo{};
        out_info.file_path = pe_path;
        return Status::Error(StatusCode::NotFound, "Cannot open PE image: " + pe_path);
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    return InspectPeBytes(data, pe_path, redirects, out_info);
}

}  // namespace scarlettx
