#include <algorithm>
#include "sx_check.h"
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/loader/stub_thunk.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/emulator.h"

#if defined(_WIN32)
#include <unknwn.h>
#include <windows.h>
#endif

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint64_t FakeOrdinalHleHandler(uint64_t a, uint64_t b) {
    return (a * 1000ull) + b + 7ull;
}

#if defined(_WIN32)
struct SynthImportGroup {
    std::string dll_name;
    std::vector<std::pair<std::string, std::string>> symbol_and_export;
};

struct SynthConstExport {
    std::string export_name;
    uint32_t return_u32;
};

std::vector<uint8_t> BuildSyntheticPe64Dll(
    const std::string& internal_dll_name,
    uint64_t preferred_base,
    const std::vector<SynthImportGroup>& import_groups,
    const std::vector<SynthConstExport>& const_exports) {
    std::vector<uint8_t> img(0x4000, 0);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img.data() + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 3;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;

    auto& opt = nt->OptionalHeader;
    opt.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    opt.MajorLinkerVersion = 14;
    opt.SizeOfCode = 0x1000;
    opt.SizeOfInitializedData = 0x2000;
    opt.AddressOfEntryPoint = 0x1000;
    opt.BaseOfCode = 0x1000;
    opt.ImageBase = preferred_base;
    opt.SectionAlignment = 0x1000;
    opt.FileAlignment = 0x1000;
    opt.MajorOperatingSystemVersion = 6;
    opt.MajorSubsystemVersion = 6;
    opt.SizeOfImage = 0x4000;
    opt.SizeOfHeaders = 0x1000;
    opt.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.DllCharacteristics =
        IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA | IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
    opt.SizeOfStackReserve = 0x100000;
    opt.SizeOfStackCommit = 0x1000;
    opt.SizeOfHeapReserve = 0x100000;
    opt.SizeOfHeapCommit = 0x1000;
    opt.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    auto* secs = IMAGE_FIRST_SECTION(nt);
    std::memcpy(secs[0].Name, ".text\0\0\0", 8);
    secs[0].Misc.VirtualSize = 0x1000;
    secs[0].VirtualAddress = 0x1000;
    secs[0].SizeOfRawData = 0x1000;
    secs[0].PointerToRawData = 0x1000;
    secs[0].Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    std::memcpy(secs[1].Name, ".rdata\0\0", 8);
    secs[1].Misc.VirtualSize = 0x1000;
    secs[1].VirtualAddress = 0x2000;
    secs[1].SizeOfRawData = 0x1000;
    secs[1].PointerToRawData = 0x2000;
    secs[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    std::memcpy(secs[2].Name, ".reloc\0\0", 8);
    secs[2].Misc.VirtualSize = 12;
    secs[2].VirtualAddress = 0x3000;
    secs[2].SizeOfRawData = 0x1000;
    secs[2].PointerToRawData = 0x3000;
    secs[2].Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE;

    // DllMain at 0x1000: mov eax, 1; ret
    const uint8_t dll_main_bytes[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(img.data() + 0x1000, dll_main_bytes, sizeof(dll_main_bytes));

    struct ExportItem {
        std::string name;
        uint32_t func_rva;
    };
    std::vector<ExportItem> exports;

    // Layout .rdata starting at 0x2000:
    //   [IAT arrays] [ILT arrays] [IMAGE_IMPORT_DESCRIPTORs] [Hint/Name + DLL strings] [Exports]
    uint32_t rdata_cursor = 0x2000;
    std::vector<uint32_t> group_iat_rva(import_groups.size(), 0);
    std::vector<uint32_t> group_ilt_rva(import_groups.size(), 0);
    std::vector<std::vector<uint32_t>> sym_iat_slot_rva(import_groups.size());

    const uint32_t iat_start_rva = rdata_cursor;
    for (size_t g = 0; g < import_groups.size(); ++g) {
        group_iat_rva[g] = rdata_cursor;
        sym_iat_slot_rva[g].resize(import_groups[g].symbol_and_export.size(), 0);
        for (size_t s = 0; s < import_groups[g].symbol_and_export.size(); ++s) {
            sym_iat_slot_rva[g][s] = rdata_cursor;
            rdata_cursor += 8;
        }
        rdata_cursor += 8;  // null terminator
    }
    const uint32_t iat_total_size = rdata_cursor - iat_start_rva;
    if (iat_total_size > 0) {
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = iat_start_rva;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = iat_total_size;
    }

    for (size_t g = 0; g < import_groups.size(); ++g) {
        group_ilt_rva[g] = rdata_cursor;
        rdata_cursor += static_cast<uint32_t>((import_groups[g].symbol_and_export.size() + 1) * 8);
    }

    uint32_t import_desc_rva = 0;
    if (!import_groups.empty()) {
        import_desc_rva = rdata_cursor;
        const uint32_t desc_bytes =
            static_cast<uint32_t>((import_groups.size() + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        rdata_cursor += desc_bytes;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = import_desc_rva;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = desc_bytes;
    }

    uint32_t code_cursor = 0x1010;
    for (size_t g = 0; g < import_groups.size(); ++g) {
        rdata_cursor = (rdata_cursor + 1u) & ~1u;
        const uint32_t dll_str_rva = rdata_cursor;
        const std::string& dname = import_groups[g].dll_name;
        std::memcpy(img.data() + rdata_cursor, dname.c_str(), dname.size() + 1);
        rdata_cursor += static_cast<uint32_t>(dname.size() + 1);

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            img.data() + import_desc_rva + g * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        desc->OriginalFirstThunk = group_ilt_rva[g];
        desc->Name = dll_str_rva;
        desc->FirstThunk = group_iat_rva[g];

        for (size_t s = 0; s < import_groups[g].symbol_and_export.size(); ++s) {
            rdata_cursor = (rdata_cursor + 1u) & ~1u;
            const uint32_t hn_rva = rdata_cursor;
            const std::string& sname = import_groups[g].symbol_and_export[s].first;
            const std::string& exp_name = import_groups[g].symbol_and_export[s].second;
            // uint16_t Hint = 0 followed by ASCII symbol name
            img[rdata_cursor + 0] = 0;
            img[rdata_cursor + 1] = 0;
            std::memcpy(img.data() + rdata_cursor + 2, sname.c_str(), sname.size() + 1);
            rdata_cursor += static_cast<uint32_t>(2 + sname.size() + 1);

            *reinterpret_cast<uint64_t*>(img.data() + sym_iat_slot_rva[g][s]) = hn_rva;
            *reinterpret_cast<uint64_t*>(img.data() + group_ilt_rva[g] + s * 8) = hn_rva;

            if (!exp_name.empty()) {
                const uint32_t stub_rva = code_cursor;
                const int32_t disp = static_cast<int32_t>(sym_iat_slot_rva[g][s]) -
                                     static_cast<int32_t>(stub_rva + 6u);
                img[stub_rva + 0] = 0xFF;
                img[stub_rva + 1] = 0x25;
                std::memcpy(img.data() + stub_rva + 2, &disp, sizeof(disp));
                std::memset(img.data() + stub_rva + 6, 0xCC, 10);
                code_cursor += 16;
                exports.push_back(ExportItem{exp_name, stub_rva});
            }
        }
    }

    for (const auto& ce : const_exports) {
        const uint32_t stub_rva = code_cursor;
        img[stub_rva + 0] = 0xB8;  // mov eax, imm32
        std::memcpy(img.data() + stub_rva + 1, &ce.return_u32, sizeof(uint32_t));
        img[stub_rva + 5] = 0xC3;  // ret
        std::memset(img.data() + stub_rva + 6, 0xCC, 10);
        code_cursor += 16;
        exports.push_back(ExportItem{ce.export_name, stub_rva});
    }

    if (!exports.empty()) {
        std::sort(exports.begin(), exports.end(),
                  [](const ExportItem& a, const ExportItem& b) { return a.name < b.name; });
        rdata_cursor = (rdata_cursor + 15u) & ~15u;
        const uint32_t exp_dir_rva = rdata_cursor;
        auto* edir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(img.data() + exp_dir_rva);
        rdata_cursor += sizeof(IMAGE_EXPORT_DIRECTORY);

        const uint32_t n_exp = static_cast<uint32_t>(exports.size());
        const uint32_t eat_rva = rdata_cursor;
        rdata_cursor += n_exp * sizeof(uint32_t);
        const uint32_t enames_rva = rdata_cursor;
        rdata_cursor += n_exp * sizeof(uint32_t);
        const uint32_t eords_rva = rdata_cursor;
        rdata_cursor += n_exp * sizeof(uint16_t);
        rdata_cursor = (rdata_cursor + 1u) & ~1u;

        const uint32_t mod_name_rva = rdata_cursor;
        std::memcpy(img.data() + rdata_cursor, internal_dll_name.c_str(),
                    internal_dll_name.size() + 1);
        rdata_cursor += static_cast<uint32_t>(internal_dll_name.size() + 1);

        for (uint32_t i = 0; i < n_exp; ++i) {
            *reinterpret_cast<uint32_t*>(img.data() + eat_rva + i * 4) = exports[i].func_rva;
            *reinterpret_cast<uint16_t*>(img.data() + eords_rva + i * 2) = static_cast<uint16_t>(i);
            const uint32_t name_rva = rdata_cursor;
            std::memcpy(img.data() + rdata_cursor, exports[i].name.c_str(),
                        exports[i].name.size() + 1);
            rdata_cursor += static_cast<uint32_t>(exports[i].name.size() + 1);
            *reinterpret_cast<uint32_t*>(img.data() + enames_rva + i * 4) = name_rva;
        }

        edir->Name = mod_name_rva;
        edir->Base = 1;
        edir->NumberOfFunctions = n_exp;
        edir->NumberOfNames = n_exp;
        edir->AddressOfFunctions = eat_rva;
        edir->AddressOfNames = enames_rva;
        edir->AddressOfNameOrdinals = eords_rva;

        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = exp_dir_rva;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = rdata_cursor - exp_dir_rva;
    }

    // Minimal valid .reloc block (2 absolute padding entries) so ASLR/relocation succeeds.
    auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(img.data() + 0x3000);
    reloc->VirtualAddress = 0x1000;
    reloc->SizeOfBlock = 12;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x3000;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 12;

    SX_REQUIRE(code_cursor <= 0x2000);
    SX_REQUIRE(rdata_cursor <= 0x3000);
    return img;
}

using ThunkCall4Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

__declspec(noinline) bool VerifySehUnwindAcrossStubThunk(ThunkCall4Fn fn, DWORD expected_code) {
    bool caught_seh = false;
    __try {
        (void)fn(0x10ull, 0x20ull, 0x30ull, 0x40ull);
    } __except (GetExceptionCode() == expected_code ? EXCEPTION_EXECUTE_HANDLER
                                                    : EXCEPTION_CONTINUE_SEARCH) {
        caught_seh = true;
    }
    return caught_seh;
}
#endif

}  // namespace

int main() {
    sx_test::ScopedTempDir sandbox("test_inproc_loader");
    const std::filesystem::path probe_dir =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "iat_probe";
    const std::filesystem::path probe_dll = probe_dir / "iat_probe.dll";
    SX_REQUIRE(std::filesystem::exists(probe_dll));

    // 1. Verify API-set resolution helper.
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-crt-runtime-l1-1-0.dll") == "ucrtbase.dll");
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-core-processthreads-l1-1-0.dll") ==
           "kernel32.dll");
    SX_REQUIRE(scarlettx::ResolveApiSetDll("api-ms-win-core-com-l1-1-0.dll") == "combase.dll");

    // 2. Verify static PE inspection sees both named and ordinal imports.
    scarlettx::DllRedirectionTable redirects;
    scarlettx::PeImageInfo info;
    SX_REQUIRE(scarlettx::InspectPeFile(probe_dll.string(), redirects, info).ok());
    SX_REQUIRE(info.is_pe64);

    bool saw_ord7 = false;
    bool saw_ord42 = false;
    bool saw_overlay_named = false;
    bool saw_xgr = false;
    for (const auto& mod : info.imports) {
        const std::string dll = ToLower(mod.dll_name);
        if (dll == "xordinal_hle.dll") {
            for (const auto& sym : mod.symbols) {
                if (sym.by_ordinal && sym.ordinal == 7) saw_ord7 = true;
            }
        } else if (dll == "xtelemetry_missing.dll") {
            for (const auto& sym : mod.symbols) {
                if (sym.by_ordinal && sym.ordinal == 42) saw_ord42 = true;
            }
        } else if (dll == "xgameoverlay_missing.dll") {
            for (const auto& sym : mod.symbols) {
                if (!sym.by_ordinal && sym.name == "XOverlayShowNotice") saw_overlay_named = true;
            }
        } else if (dll == "xgameruntime.dll") {
            saw_xgr = true;
        }
    }
    SX_REQUIRE(saw_ord7);
    SX_REQUIRE(saw_ord42);
    SX_REQUIRE(saw_overlay_named);
    SX_REQUIRE(saw_xgr);

    // 3. Map in-process with forced relocation, HLE ordinal override, and auto-stub thunks.
    scarlettx::HleSymbolRegistry hle_reg;
    hle_reg.RegisterOrdinal("xordinal_hle.dll", 7, reinterpret_cast<void*>(&FakeOrdinalHleHandler));

    scarlettx::PeMapOptions opts;
    opts.force_relocate = true;
    opts.auto_stub_missing = true;
    opts.default_stub_return = 0x55AA1234ull;
    opts.shim_directory = SCARLETTX_BINARY_DIR;

    scarlettx::MappedPeImage mapped;
    auto st = mapped.MapFile(probe_dll.string(), redirects, hle_reg, opts);
    if (!st.ok()) {
        std::cerr << "[test_inproc_loader] MapFile failed: " << st.message << "\n";
        return 1;
    }

    SX_REQUIRE(mapped.IsMapped());
    SX_REQUIRE(mapped.WasRelocated());
    SX_REQUIRE(mapped.RelocationsApplied() > 0);

    // Verify resolution kinds.
    bool resolved_hle_ord = false;
    bool resolved_host_xgr = false;
    bool resolved_stub_named = false;
    bool resolved_stub_ord = false;
    for (const auto& r : mapped.Resolutions()) {
        const std::string dll = ToLower(r.dll_name);
        if (dll == "xordinal_hle.dll" && r.by_ordinal && r.ordinal == 7) {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HleRegistry);
            resolved_hle_ord = true;
        } else if (dll == "xgameruntime.dll" && r.symbol_name == "XGameRuntimeInitialize") {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HostDll);
            resolved_host_xgr = true;
        } else if (dll == "xgameoverlay_missing.dll" && r.symbol_name == "XOverlayShowNotice") {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::StubThunk);
            resolved_stub_named = true;
        } else if (dll == "xtelemetry_missing.dll" && r.by_ordinal && r.ordinal == 42) {
            SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::StubThunk);
            resolved_stub_ord = true;
        }
    }
    SX_REQUIRE(resolved_hle_ord);
    SX_REQUIRE(resolved_host_xgr);
    SX_REQUIRE(resolved_stub_named);
    SX_REQUIRE(resolved_stub_ord);

    // 4. Execute relocated global pointer probe.
    using ProbeRelocFn = uint64_t (*)();
    auto probe_reloc = reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeRelocatedGlobal"));
    SX_REQUIRE(probe_reloc != nullptr);
    SX_REQUIRE(probe_reloc() == 0x5CA81E7700001234ull);

    // 4.5 The mapped image is known to the host unwinder: without this a C++
    //     exception thrown in guest code terminates the process.
    {
        // Leaf functions and linker thunks have no unwind entry, so take the
        // address of a function the image's own table describes.
        const auto* image = reinterpret_cast<const uint8_t*>(mapped.MappedBase());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        SX_REQUIRE(dir.VirtualAddress != 0 && dir.Size >= sizeof(RUNTIME_FUNCTION));
        const auto* first = reinterpret_cast<const RUNTIME_FUNCTION*>(image + dir.VirtualAddress);
        DWORD64 unwind_base = 0;
        const DWORD64 pc = mapped.MappedBase() + first->BeginAddress;
        SX_REQUIRE(RtlLookupFunctionEntry(pc, &unwind_base, nullptr) != nullptr);
        SX_REQUIRE(unwind_base == mapped.MappedBase());
    }

    // A section table or relocation data that overruns the file or image is rejected.
    {
        std::ifstream in(probe_dll, std::ios::binary);
        const std::vector<char> good((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        const auto nt_of = [](std::vector<char>& pe) {
            return reinterpret_cast<IMAGE_NT_HEADERS64*>(
                pe.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data())->e_lfanew);
        };
        const std::filesystem::path bad_dll = sandbox.path() / "iat_probe_malformed.dll";
        const auto map_bytes = [&](const std::vector<char>& pe) {
            std::ofstream(bad_dll, std::ios::binary)
                .write(pe.data(), static_cast<std::streamsize>(pe.size()));
            scarlettx::MappedPeImage bad;
            return bad.MapFile(bad_dll.string(), redirects, hle_reg, opts);
        };

        auto bad_sections = good;
        nt_of(bad_sections)->FileHeader.NumberOfSections = 0xFFFF;
        SX_REQUIRE(map_bytes(bad_sections).message == "Truncated section table");

        auto bad_block = good;
        const auto* nt = nt_of(bad_block);
        const DWORD reloc_rva =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        size_t block_at = 0;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (reloc_rva >= sec->VirtualAddress &&
                reloc_rva - sec->VirtualAddress < sec->SizeOfRawData) {
                block_at = sec->PointerToRawData + (reloc_rva - sec->VirtualAddress);
            }
        }
        SX_REQUIRE(block_at != 0);
        reinterpret_cast<IMAGE_BASE_RELOCATION*>(bad_block.data() + block_at)->SizeOfBlock =
            0xFFFFFFF0;
        SX_REQUIRE(map_bytes(bad_block).message ==
                   "Relocation block exceeds relocation directory");

        auto bad_dir = good;
        nt_of(bad_dir)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress =
            nt->OptionalHeader.SizeOfImage;
        SX_REQUIRE(map_bytes(bad_dir).message == "Relocation directory exceeds PE image size");

        std::filesystem::remove(bad_dll);
    }

    // 4.6 Guest C++ exceptions and guest thread-local storage.
    {
        using ProbeThrowFn = int (*)(int);
        using ProbeTlsFn = int (*)();
        auto probe_throw = reinterpret_cast<ProbeThrowFn>(mapped.GetExportAddress("ProbeThrowCatch"));
        auto probe_tls = reinterpret_cast<ProbeTlsFn>(mapped.GetExportAddress("ProbeThreadLocalCounter"));
        SX_REQUIRE(probe_throw != nullptr && probe_tls != nullptr);
        for (int i = 0; i < 100; ++i) {
            SX_REQUIRE(probe_throw(i) == i + 1);
        }
        SX_REQUIRE(probe_tls() == 42);
        SX_REQUIRE(probe_tls() == 43);
        int on_thread_first = 0;
        int on_thread_second = 0;
        std::thread worker([&] {
            on_thread_first = probe_tls();
            on_thread_second = probe_tls();
        });
        worker.join();
        SX_REQUIRE(on_thread_first == 42 && on_thread_second == 43);
        SX_REQUIRE(probe_tls() == 44);
    }

    // 5. Execute ordinal-imported HLE handler probe.
    using ProbeOrdFn = uint64_t (*)(uint64_t, uint64_t);
    auto probe_ord = reinterpret_cast<ProbeOrdFn>(mapped.GetExportAddress("ProbeOrdinalHle"));
    SX_REQUIRE(probe_ord != nullptr);
    SX_REQUIRE(probe_ord(42, 99) == 42106ull);

    // 6. Execute auto-stubbed named and ordinal imports and verify captured args + return values.
    auto probe_named_stub =
        reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeMissingNamedStub"));
    auto probe_ord_stub =
        reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeMissingOrdinalStub"));
    SX_REQUIRE(probe_named_stub != nullptr);
    SX_REQUIRE(probe_ord_stub != nullptr);

    SX_REQUIRE(probe_named_stub() == 0x55AA1234ull);
    SX_REQUIRE(probe_ord_stub() == 0x55AA1234ull);
    SX_REQUIRE(mapped.Stubs().TotalHitCount() == 2);

    const auto records = mapped.Stubs().SnapshotRecords();
    bool checked_named_args = false;
    bool checked_ord_args = false;
    for (const auto& rec : records) {
        const std::string dll = ToLower(rec.dll_name);
        if (dll == "xgameoverlay_missing.dll" && rec.symbol_name == "XOverlayShowNotice") {
            SX_REQUIRE(rec.hit_count == 1);
            SX_REQUIRE(rec.last_args[0] == 0x1111ull);
            SX_REQUIRE(rec.last_args[1] == 0x2222ull);
            SX_REQUIRE(rec.last_args[2] == 0x3333ull);
            SX_REQUIRE(rec.last_args[3] == 0x4444ull);
            checked_named_args = true;
        } else if (dll == "xtelemetry_missing.dll" && rec.by_ordinal && rec.ordinal == 42) {
            SX_REQUIRE(rec.hit_count == 1);
            SX_REQUIRE(rec.last_args[0] == 0xAAAAull);
            SX_REQUIRE(rec.last_args[1] == 0xBBBBull);
            SX_REQUIRE(rec.last_args[2] == 0xCCCCull);
            SX_REQUIRE(rec.last_args[3] == 0xDDDDull);
            checked_ord_args = true;
        }
    }
    SX_REQUIRE(checked_named_args);
    SX_REQUIRE(checked_ord_args);

    // 7. Verify Emulator::BootPackage with in_process=true runs ScarlettGuestMain cleanly.
    scarlettx::EmulatorConfig cfg;
    cfg.in_process = true;
    cfg.force_relocate = true;
    cfg.shim_directory = SCARLETTX_BINARY_DIR;
    scarlettx::Emulator emu(cfg);
    auto boot_st = emu.BootPackage(probe_dir.string());
    if (!boot_st.ok()) {
        std::cerr << "[test_inproc_loader] BootPackage(--in-process) failed: "
                  << boot_st.message << "\n";
        return 1;
    }

    // 8. An image with stripped relocations must not load away from its preferred base.
    {
        const std::filesystem::path fixed_dll = sandbox.path() / "test_inproc_loader_fixed.dll";
        std::filesystem::copy_file(probe_dll, fixed_dll,
                                   std::filesystem::copy_options::overwrite_existing);
        {
            std::fstream f(fixed_dll, std::ios::in | std::ios::out | std::ios::binary);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
            f.seekg(dos.e_lfanew);
            f.read(reinterpret_cast<char*>(&nt), sizeof(nt));
            nt.FileHeader.Characteristics |= IMAGE_FILE_RELOCS_STRIPPED;
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {};
            f.seekp(dos.e_lfanew);
            f.write(reinterpret_cast<const char*>(&nt), sizeof(nt));
            SX_REQUIRE(f.good());
        }
        scarlettx::MappedPeImage fixed;
        SX_REQUIRE(!fixed.MapFile(fixed_dll.string(), redirects, hle_reg, opts).ok());
        SX_REQUIRE(!fixed.IsMapped());
        std::filesystem::remove(fixed_dll);
    }

    // 9. Middleware-first vs. System-shim-first loading policy:
    //    - Game-shipped middleware (e.g. Bink2Scarlett.dll in package dir) takes precedence
    //      over ScarlettEdge fallback middleware shims.
    //    - Xbox OS/hardware system DLLs (e.g. xgameruntime.dll) strictly resolve from shim_dir
    //      first even if a decoy/Xbox copy sits in the package dir.
    {
        const std::filesystem::path mw_dir = sandbox.path() / "test_mw_precedence_pkg";
        std::filesystem::create_directories(mw_dir);
        const std::filesystem::path mw_probe = mw_dir / "mw_probe.dll";
        const std::filesystem::path mw_bink = mw_dir / "Bink2Scarlett.dll";
        const std::filesystem::path decoy_xgr = mw_dir / "xgameruntime.dll";

        std::ifstream in(probe_dll, std::ios::binary);
        std::vector<char> patched((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        const auto replace_ascii = [&](const char* from, const char* to) {
            const size_t from_len = std::strlen(from);
            const size_t to_len = std::strlen(to);
            SX_REQUIRE(to_len <= from_len);
            auto it = std::search(patched.begin(), patched.end(), from, from + from_len);
            SX_REQUIRE(it != patched.end());
            std::fill(it, it + static_cast<std::ptrdiff_t>(from_len), '\0');
            std::copy(to, to + to_len, it);
        };
        replace_ascii("xgameoverlay_missing.dll", "Bink2Scarlett.dll");
        replace_ascii("XOverlayShowNotice", "XvcCryptoGetStatus");
        {
            std::ofstream out(mw_probe, std::ios::binary);
            out.write(patched.data(), static_cast<std::streamsize>(patched.size()));
            SX_REQUIRE(out.good());
        }

        // Place a game-shipped middleware DLL exporting XvcCryptoGetStatus in the package dir,
        // and place a decoy xgameruntime.dll (lacking XGameRuntimeInitialize) beside it.
        std::filesystem::copy_file(std::filesystem::path(SCARLETTX_BINARY_DIR) / "xvc_crypto.dll",
                                   mw_bink, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(std::filesystem::path(SCARLETTX_BINARY_DIR) / "pixevt_stub.dll",
                                   decoy_xgr, std::filesystem::copy_options::overwrite_existing);

        scarlettx::MappedPeImage mw_mapped;
        SX_REQUIRE(mw_mapped.MapFile(mw_probe.string(), redirects, hle_reg, opts).ok());
        bool saw_pkg_middleware = false;
        bool saw_shim_xgr = false;
        for (const auto& r : mw_mapped.Resolutions()) {
            const std::string dll = ToLower(r.dll_name);
            if (dll == "bink2scarlett.dll" && r.symbol_name == "XvcCryptoGetStatus") {
                SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HostDll);
                saw_pkg_middleware = true;
            } else if (dll == "xgameruntime.dll" && r.symbol_name == "XGameRuntimeInitialize") {
                SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HostDll);
                saw_shim_xgr = true;
            }
        }
        SX_REQUIRE(saw_pkg_middleware);
        SX_REQUIRE(saw_shim_xgr);
        mw_mapped.Unmap();

        if (HMODULE h = GetModuleHandleA(mw_bink.string().c_str())) {
            FreeLibrary(h);
        }
        std::error_code ec;
        std::filesystem::remove_all(mw_dir, ec);
    }

#if defined(_WIN32)
    // 10. StubThunk W^X PAGE_EXECUTE_READ protection + x64 .pdata RUNTIME_FUNCTION / UNWIND_INFO
    //     registration + C++ exception & Win32 SEH unwinding across StubThunk frames.
    {
        for (const auto& r : mapped.Resolutions()) {
            if (r.kind == scarlettx::ImportResolutionKind::StubThunk) {
                MEMORY_BASIC_INFORMATION mbi{};
                SX_REQUIRE(VirtualQuery(reinterpret_cast<LPCVOID>(r.resolved_address), &mbi,
                                        sizeof(mbi)) == sizeof(mbi));
                SX_REQUIRE(mbi.Protect == PAGE_EXECUTE_READ);
                DWORD64 img_base = 0;
                SX_REQUIRE(RtlLookupFunctionEntry(r.resolved_address + 0x10, &img_base, nullptr) !=
                           nullptr);
            }
        }

        scarlettx::StubThunkTable thunk_table;
        SX_REQUIRE(thunk_table.Initialize().ok());
        void* raw_thunk = nullptr;
        uint32_t named_stub_id = 0xFFFFFFFFu;
        SX_REQUIRE(thunk_table
                       .AllocateStub("xunwind_test.dll", "RaiseInStub", false, 0,
                                     0xCAFEBABE12345678ull, raw_thunk, &named_stub_id)
                       .ok());
        SX_REQUIRE(raw_thunk != nullptr);
        void* raw_ord_thunk = nullptr;
        SX_REQUIRE(thunk_table
                       .AllocateStub("xunwind_test.dll", "", true, 99, 0x77ull, raw_ord_thunk,
                                     nullptr)
                       .ok());
        SX_REQUIRE(raw_ord_thunk != nullptr);

        MEMORY_BASIC_INFORMATION mbi{};
        SX_REQUIRE(VirtualQuery(raw_thunk, &mbi, sizeof(mbi)) == sizeof(mbi));
        SX_REQUIRE(mbi.Protect == PAGE_EXECUTE_READ);

        const DWORD64 stub_pc = reinterpret_cast<DWORD64>(raw_thunk);
        DWORD64 unwind_base = 0;
        const RUNTIME_FUNCTION* rf_entry = RtlLookupFunctionEntry(stub_pc, &unwind_base, nullptr);
        const RUNTIME_FUNCTION* rf_call_ret =
            RtlLookupFunctionEntry(stub_pc + 0x38, &unwind_base, nullptr);
        SX_REQUIRE(rf_entry != nullptr && rf_entry == rf_call_ret);
        const auto* unwind_bytes =
            reinterpret_cast<const uint8_t*>(unwind_base + rf_entry->UnwindData);
        // Version=1, Flags=0, SizeOfProlog=4, CountOfCodes=1, OpCode=2 (UWOP_ALLOC_SMALL), OpInfo=8 (0x48)
        SX_REQUIRE((unwind_bytes[0] & 0x07u) == 1u);
        SX_REQUIRE(unwind_bytes[1] == 4u);
        SX_REQUIRE(unwind_bytes[2] == 1u);
        SX_REQUIRE(((unwind_bytes[5] & 0x0Fu) == 2u) && (((unwind_bytes[5] >> 4) & 0x0Fu) == 8u));

        auto thunk_fn = reinterpret_cast<ThunkCall4Fn>(raw_thunk);
        SX_REQUIRE(thunk_fn(1, 2, 3, 4) == 0xCAFEBABE12345678ull);

        // C++ exception unwind across StubThunk frame (verifying RAII destructor execution).
        thunk_table.SetInterceptCallback(
            [](uint32_t stub_id, const uint64_t* args, uint64_t caller_rip, void* ctx) -> uint64_t {
                const uint32_t expected_id = *static_cast<const uint32_t*>(ctx);
                if (stub_id == expected_id && caller_rip != 0 && args[0] == 0xAA11ull &&
                    args[3] == 0xDD44ull) {
                    throw std::runtime_error("cpp_unwind_across_stub_thunk");
                }
                return 0;
            },
            &named_stub_id);

        bool dtor_ran = false;
        bool caught_cpp = false;
        try {
            struct ScopeProbe {
                bool* flag;
                ~ScopeProbe() { *flag = true; }
            } probe{&dtor_ran};
            (void)thunk_fn(0xAA11ull, 0xBB22ull, 0xCC33ull, 0xDD44ull);
        } catch (const std::runtime_error& ex) {
            caught_cpp = (std::string(ex.what()) == "cpp_unwind_across_stub_thunk");
        }
        SX_REQUIRE(dtor_ran);
        SX_REQUIRE(caught_cpp);

        // Win32 SEH RaiseException unwind across StubThunk frame.
        constexpr DWORD kCustomSehCode = 0xE0580042u;
        thunk_table.SetInterceptCallback(
            [](uint32_t, const uint64_t* args, uint64_t, void*) -> uint64_t {
                if (args[0] == 0x10ull) {
                    RaiseException(0xE0580042u, 0, 0, nullptr);
                }
                return 0;
            },
            nullptr);
        SX_REQUIRE(VerifySehUnwindAcrossStubThunk(thunk_fn, kCustomSehCode));

        thunk_table.SetInterceptCallback(nullptr);
        SX_REQUIRE(thunk_fn(5, 6, 7, 8) == 0xCAFEBABE12345678ull);
        thunk_table.Reset();
        DWORD64 post_reset_base = 0;
        SX_REQUIRE(RtlLookupFunctionEntry(stub_pc, &post_reset_base, nullptr) == nullptr);
    }

    // 11. Secondary package-loaded guest DLLs (both host-loaded static/dynamic DLLs and
    //     zero-staging XVD VFS-loaded DLLs) receive the FULL LookupGuestRuntimeHook IAT table:
    //     VirtualAlloc(MEM_GRAPHICS=0x04000000), CreateFileW/ReadFile/FindFirstFileW on G:\,
    //     recursive LoadLibraryW, RoGetActivationFactory, and FlsAlloc/FlsFree.
    {
        // Confirm raw host Windows VirtualAlloc rejects MEM_GRAPHICS (0x04000000) without our hook.
        SX_REQUIRE(::VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE | 0x04000000u,
                                  PAGE_READWRITE) == nullptr);

        const std::vector<SynthImportGroup> guest_dll_imports = {
            {"KERNEL32.dll",
             {
                 {"VirtualAlloc", "Fwd_VirtualAlloc"},
                 {"VirtualFree", "Fwd_VirtualFree"},
                 {"CreateFileW", "Fwd_CreateFileW"},
                 {"ReadFile", "Fwd_ReadFile"},
                 {"GetFileSizeEx", "Fwd_GetFileSizeEx"},
                 {"CloseHandle", "Fwd_CloseHandle"},
                 {"GetFileAttributesW", "Fwd_GetFileAttributesW"},
                 {"FindFirstFileW", "Fwd_FindFirstFileW"},
                 {"FindClose", "Fwd_FindClose"},
                 {"LoadLibraryW", "Fwd_LoadLibraryW"},
                 {"GetProcAddress", "Fwd_GetProcAddress"},
                 {"FlsAlloc", "Fwd_FlsAlloc"},
                 {"FlsFree", "Fwd_FlsFree"},
             }},
            {"api-ms-win-core-winrt-l1-1-0.dll",
             {
                 {"RoGetActivationFactory", "Fwd_RoGetActivationFactory"},
                 {"WindowsCreateString", "Fwd_WindowsCreateString"},
                 {"WindowsDeleteString", "Fwd_WindowsDeleteString"},
             }},
        };

        const auto sec_dll_bytes =
            BuildSyntheticPe64Dll("SecondaryGuest.dll", 0x0000000181100000ULL,
                                  {guest_dll_imports[0], guest_dll_imports[1],
                                   {"TransitivePkgDep.dll", {{"Fwd_VirtualAlloc", "Sec_TransitiveVAlloc"}}}},
                                  {});
        const auto trans_dep_bytes =
            BuildSyntheticPe64Dll("TransitivePkgDep.dll", 0x0000000181180000ULL, guest_dll_imports, {});
        const auto tert_dll_bytes =
            BuildSyntheticPe64Dll("TertiaryPlugin.dll", 0x0000000181200000ULL, guest_dll_imports, {});
        const auto vfs_dll_bytes =
            BuildSyntheticPe64Dll("VfsPlugin.dll", 0x0000000181300000ULL,
                                  {guest_dll_imports[0], guest_dll_imports[1],
                                   {"KERNEL32.dll", {{"FreeLibrary", "Fwd_FreeLibrary"},
                                                     {"LoadLibraryExW", "Fwd_LoadLibraryExW"}}}},
                                  {});
        const auto vfs_dll2_bytes =
            BuildSyntheticPe64Dll("VfsPlugin2.dll", 0x0000000181380000ULL, guest_dll_imports,
                                  {{"Plugin2Ping", 0x22229999u}});
        // Cyclic VFS DLL pair: VfsCycleA.dll <-> VfsCycleB.dll
        const auto vfs_cycle_a_bytes = BuildSyntheticPe64Dll(
            "VfsCycleA.dll", 0x0000000181400000ULL,
            {{"VfsCycleB.dll", {{"CycleBVal", "CallCycleBFromA"}}}},
            {{"CycleAVal", 0xAAAA1111u}});
        const auto vfs_cycle_b_bytes = BuildSyntheticPe64Dll(
            "VfsCycleB.dll", 0x0000000181480000ULL,
            {{"VfsCycleA.dll", {{"CycleAVal", "CallCycleAFromB"}}}},
            {{"CycleBVal", 0xBBBB2222u}});

        const std::vector<SynthImportGroup> main_host_imports = {
            {"SecondaryGuest.dll", {{"Fwd_LoadLibraryW", "Main_CallSecondaryLoadLibraryW"}}},
        };
        const auto main_dll_bytes = BuildSyntheticPe64Dll(
            "MainGuestEntry.dll", 0x0000000181000000ULL, main_host_imports, {});

        const std::filesystem::path sec_pkg_dir = sandbox.path() / "secondary_guest_pkg";
        std::filesystem::create_directories(sec_pkg_dir);
        const auto write_bin = [](const std::filesystem::path& p, const std::vector<uint8_t>& b) {
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            SX_REQUIRE(ofs.good());
        };
        write_bin(sec_pkg_dir / "MainGuestEntry.dll", main_dll_bytes);
        write_bin(sec_pkg_dir / "SecondaryGuest.dll", sec_dll_bytes);
        write_bin(sec_pkg_dir / "TransitivePkgDep.dll", trans_dep_bytes);
        write_bin(sec_pkg_dir / "TertiaryPlugin.dll", tert_dll_bytes);

        // Mount an encrypted NTFS XVD on PackageVfs::Global() at G: containing Assets\stream_test.bin
        // and multiple VFS plugins (including cyclic VfsCycleA <-> VfsCycleB).
        const uint8_t vfs_key[32] = {
            0x42, 0x13, 0x37, 0x99, 0xAA, 0xBB, 0xCC, 0xDD,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0x00, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45,
            0x67, 0x89, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54,
        };
        std::vector<uint8_t> stream_payload(12288);
        for (size_t i = 0; i < stream_payload.size(); ++i) {
            stream_payload[i] = static_cast<uint8_t>((i * 41u + 13u) & 0xFFu);
        }
        const std::vector<scarlettx::XvdInputFile> vfs_files = {
            {"Assets\\stream_test.bin", stream_payload},
            {"Plugins\\VfsPlugin.dll", vfs_dll_bytes},
            {"Plugins\\VfsPlugin2.dll", vfs_dll2_bytes},
            {"Plugins\\VfsCycleA.dll", vfs_cycle_a_bytes},
            {"Plugins\\VfsCycleB.dll", vfs_cycle_b_bytes},
        };
        const auto xvc_img = scarlettx::XvdContainer::BuildSyntheticNtfsXvc(
            vfs_files, vfs_key, sizeof(vfs_key), /*enable_merkle_tree=*/true);
        const std::filesystem::path xvc_path = sandbox.path() / "secondary_vfs.xvc";
        write_bin(xvc_path, xvc_img);

        auto mounted_xvd = std::make_shared<scarlettx::XvdContainer>();
        SX_REQUIRE(mounted_xvd->OpenFile(xvc_path.string()).ok());
        SX_REQUIRE(mounted_xvd->SetVolumeKey(vfs_key, sizeof(vfs_key)).ok());
        scarlettx::PackageVfs::Global().Reset();
        SX_REQUIRE(scarlettx::PackageVfs::Global().MountXvdContainer("G:", mounted_xvd).ok());

        scarlettx::MappedPeImage main_guest;
        SX_REQUIRE(main_guest
                       .MapFile((sec_pkg_dir / "MainGuestEntry.dll").string(), redirects, hle_reg,
                                opts)
                       .ok());

        HMODULE hSec = GetModuleHandleW(L"SecondaryGuest.dll");
        SX_REQUIRE(hSec != nullptr);

        auto verify_hooked_module = [&](HMODULE mod, bool is_mapped_guest_pe) {
            auto resolve_fn = [&](const char* name) -> void* {
                if (is_mapped_guest_pe) {
                    using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
                    auto gp = reinterpret_cast<GetProcFn>(
                        reinterpret_cast<void*>(GetProcAddress(hSec, "Fwd_GetProcAddress")));
                    SX_REQUIRE(gp != nullptr);
                    return reinterpret_cast<void*>(gp(mod, name));
                }
                return reinterpret_cast<void*>(GetProcAddress(mod, name));
            };

            using VAllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
            using VFreeFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
            using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                                  DWORD, DWORD, HANDLE);
            using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
            using GetFileSizeExFn = BOOL(WINAPI*)(HANDLE, PLARGE_INTEGER);
            using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
            using GetFileAttrWFn = DWORD(WINAPI*)(LPCWSTR);
            using FindFirstWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
            using FindCloseFn = BOOL(WINAPI*)(HANDLE);
            using FlsAllocFn = DWORD(WINAPI*)(PFLS_CALLBACK_FUNCTION);
            using FlsFreeFn = BOOL(WINAPI*)(DWORD);
            using WinCreateStrFn = HRESULT(WINAPI*)( const wchar_t*, uint32_t, void**);
            using WinDeleteStrFn = HRESULT(WINAPI*)(void*);
            using RoGetFacFn = HRESULT(WINAPI*)(void*, const GUID*, void**);

            auto fn_valloc = reinterpret_cast<VAllocFn>(resolve_fn("Fwd_VirtualAlloc"));
            auto fn_vfree = reinterpret_cast<VFreeFn>(resolve_fn("Fwd_VirtualFree"));
            auto fn_create = reinterpret_cast<CreateFileWFn>(resolve_fn("Fwd_CreateFileW"));
            auto fn_read = reinterpret_cast<ReadFileFn>(resolve_fn("Fwd_ReadFile"));
            auto fn_fsize = reinterpret_cast<GetFileSizeExFn>(resolve_fn("Fwd_GetFileSizeEx"));
            auto fn_close = reinterpret_cast<CloseHandleFn>(resolve_fn("Fwd_CloseHandle"));
            auto fn_attr = reinterpret_cast<GetFileAttrWFn>(resolve_fn("Fwd_GetFileAttributesW"));
            auto fn_ffirst = reinterpret_cast<FindFirstWFn>(resolve_fn("Fwd_FindFirstFileW"));
            auto fn_fclose = reinterpret_cast<FindCloseFn>(resolve_fn("Fwd_FindClose"));
            auto fn_fls_alloc = reinterpret_cast<FlsAllocFn>(resolve_fn("Fwd_FlsAlloc"));
            auto fn_fls_free = reinterpret_cast<FlsFreeFn>(resolve_fn("Fwd_FlsFree"));
            auto fn_wcs = reinterpret_cast<WinCreateStrFn>(resolve_fn("Fwd_WindowsCreateString"));
            auto fn_wds = reinterpret_cast<WinDeleteStrFn>(resolve_fn("Fwd_WindowsDeleteString"));
            auto fn_ro = reinterpret_cast<RoGetFacFn>(resolve_fn("Fwd_RoGetActivationFactory"));
            SX_REQUIRE(fn_valloc && fn_vfree && fn_create && fn_read && fn_fsize && fn_close &&
                       fn_attr && fn_ffirst && fn_fclose && fn_fls_alloc && fn_fls_free && fn_wcs &&
                       fn_wds && fn_ro);

            // 11a. VirtualAlloc with MEM_GRAPHICS (0x04000000) succeeds & tracks GPU VA region.
            void* gfx_mem = fn_valloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE | 0x04000000u,
                                      PAGE_READWRITE);
            SX_REQUIRE(gfx_mem != nullptr);
            size_t tracked_sz = 0;
            SX_REQUIRE(scarlettx::IsGuestGraphicsAllocation(gfx_mem, &tracked_sz));
            SX_REQUIRE(tracked_sz == 65536u);
            std::memset(gfx_mem, 0x5A, 65536);
            SX_REQUIRE(fn_vfree(gfx_mem, 0, MEM_RELEASE) == TRUE);
            SX_REQUIRE(!scarlettx::IsGuestGraphicsAllocation(gfx_mem));

            // 11b. CreateFileW / GetFileSizeEx / ReadFile / FindFirstFileW stream G:\ from XVD!
            SX_REQUIRE(fn_attr(L"G:\\Assets\\stream_test.bin") != INVALID_FILE_ATTRIBUTES);
            HANDLE hf = fn_create(L"G:\\Assets\\stream_test.bin", GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            SX_REQUIRE(hf != INVALID_HANDLE_VALUE);
            LARGE_INTEGER fsz{};
            SX_REQUIRE(fn_fsize(hf, &fsz) == TRUE);
            SX_REQUIRE(static_cast<size_t>(fsz.QuadPart) == stream_payload.size());
            std::vector<uint8_t> read_buf(stream_payload.size(), 0);
            DWORD nread = 0;
            SX_REQUIRE(fn_read(hf, read_buf.data(), static_cast<DWORD>(read_buf.size()), &nread,
                               nullptr) == TRUE);
            SX_REQUIRE(nread == stream_payload.size());
            SX_REQUIRE(read_buf == stream_payload);
            SX_REQUIRE(fn_close(hf) == TRUE);

            WIN32_FIND_DATAW wfd{};
            HANDLE hfind = fn_ffirst(L"G:\\Assets\\*.bin", &wfd);
            SX_REQUIRE(hfind != INVALID_HANDLE_VALUE);
            SX_REQUIRE(ToLower(std::filesystem::path(wfd.cFileName).string()) == "stream_test.bin");
            SX_REQUIRE(fn_fclose(hfind) == TRUE);

            // 11c. WinRT RoGetActivationFactory via API-set import resolves clean-room AnalyticsInfo.
            void* hstr = nullptr;
            const wchar_t kClass[] = L"Windows.System.Profile.AnalyticsInfo";
            SX_REQUIRE(SUCCEEDED(fn_wcs(kClass, static_cast<uint32_t>(wcslen(kClass)), &hstr)));
            const GUID kIidInspectable = {
                0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9Cu, 0x5Au, 0xD7u, 0xAAu, 0x65u, 0x10u, 0x1Eu, 0x90u}};
            void* fac = nullptr;
            SX_REQUIRE(SUCCEEDED(fn_ro(hstr, &kIidInspectable, &fac)) && fac != nullptr);
            static_cast<IUnknown*>(fac)->Release();
            SX_REQUIRE(SUCCEEDED(fn_wds(hstr)));

            // 11d. Isolated FLS slot allocation.
            const DWORD fls = fn_fls_alloc(nullptr);
            SX_REQUIRE(fls != FLS_OUT_OF_INDEXES);
            SX_REQUIRE(fn_fls_free(fls) == TRUE);
        };

        // Verify statically-imported SecondaryGuest.dll AND its OS-loaded transitive static
        // dependency TransitivePkgDep.dll (never passed directly to GuestLoadLibraryW!).
        verify_hooked_module(hSec, /*is_mapped_guest_pe=*/false);
        HMODULE hTransDep = GetModuleHandleW(L"TransitivePkgDep.dll");
        SX_REQUIRE(hTransDep != nullptr);
        verify_hooked_module(hTransDep, /*is_mapped_guest_pe=*/false);

        // Verify recursive LoadLibraryW from SecondaryGuest.dll -> TertiaryPlugin.dll (host pkg)
        // AND VFS DLLs (including sequential multi-DLL UAF regression, LoadLibraryx2 + FreeLibraryx1
        // refcount, concurrent multithreaded LoadLibraryW, LOAD_LIBRARY_AS_DATAFILE, and cyclic A<->B!).
        using LoadLibWFn = HMODULE(WINAPI*)(LPCWSTR);
        using FreeLibFn = BOOL(WINAPI*)(HMODULE);
        using LoadLibExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
        using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
        auto call_sec_loadlib =
            reinterpret_cast<LoadLibWFn>(main_guest.GetExportAddress("Main_CallSecondaryLoadLibraryW"));
        auto guest_gp = reinterpret_cast<GetProcFn>(
            reinterpret_cast<void*>(GetProcAddress(hSec, "Fwd_GetProcAddress")));
        SX_REQUIRE(call_sec_loadlib != nullptr && guest_gp != nullptr);

        HMODULE hTert = call_sec_loadlib(L"TertiaryPlugin.dll");
        SX_REQUIRE(hTert != nullptr);
        verify_hooked_module(hTert, /*is_mapped_guest_pe=*/false);

        // Load VfsPlugin.dll FIRST, then load VfsPlugin2.dll SECOND, and verify VfsPlugin.dll
        // remains alive and callable (regression check for MapBytes -> Unmap ClearDynamicVfsDlls).
        HMODULE hVfsPlugin = call_sec_loadlib(L"G:\\Plugins\\VfsPlugin.dll");
        SX_REQUIRE(hVfsPlugin != nullptr);
        HMODULE hVfsPlugin2 = call_sec_loadlib(L"G:\\Plugins\\VfsPlugin2.dll");
        SX_REQUIRE(hVfsPlugin2 != nullptr && hVfsPlugin2 != hVfsPlugin);
        verify_hooked_module(hVfsPlugin, /*is_mapped_guest_pe=*/true);
        verify_hooked_module(hVfsPlugin2, /*is_mapped_guest_pe=*/true);

        auto guest_freelib = reinterpret_cast<FreeLibFn>(
            reinterpret_cast<void*>(guest_gp(hVfsPlugin, "Fwd_FreeLibrary")));
        auto guest_loadlib_ex = reinterpret_cast<LoadLibExWFn>(
            reinterpret_cast<void*>(guest_gp(hVfsPlugin, "Fwd_LoadLibraryExW")));
        SX_REQUIRE(guest_freelib != nullptr && guest_loadlib_ex != nullptr);

        // Concurrent LoadLibraryW x 2 -> ref_count becomes 3; FreeLibrary x 2 leaves VfsPlugin alive!
        HMODULE hDup1 = nullptr;
        HMODULE hDup2 = nullptr;
        std::thread t1([&] { hDup1 = call_sec_loadlib(L"G:\\Plugins\\VfsPlugin.dll"); });
        std::thread t2([&] { hDup2 = call_sec_loadlib(L"G:\\Plugins\\VfsPlugin.dll"); });
        t1.join();
        t2.join();
        SX_REQUIRE(hDup1 == hVfsPlugin && hDup2 == hVfsPlugin);
        SX_REQUIRE(guest_freelib(hDup1) == TRUE);
        SX_REQUIRE(guest_freelib(hDup2) == TRUE);
        // VfsPlugin still has ref_count == 1 and MUST remain callable!
        verify_hooked_module(hVfsPlugin, /*is_mapped_guest_pe=*/true);

        // LOAD_LIBRARY_AS_DATAFILE returns tagged read-only datafile handle without mapping as code.
        HMODULE hDatafile =
            guest_loadlib_ex(L"G:\\Plugins\\VfsPlugin2.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
        SX_REQUIRE(hDatafile != nullptr && hDatafile != hVfsPlugin2);
        SX_REQUIRE((reinterpret_cast<uintptr_t>(hDatafile) & 0x3ull) != 0);
        const auto* df_mz = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(hDatafile) & ~uintptr_t{0x3ull});
        SX_REQUIRE(df_mz[0] == 'M' && df_mz[1] == 'Z');
        SX_REQUIRE(guest_freelib(hDatafile) == TRUE);

        // Cyclic VFS imports: VfsCycleA.dll <-> VfsCycleB.dll resolve without infinite recursion!
        HMODULE hCycleA = call_sec_loadlib(L"G:\\Plugins\\VfsCycleA.dll");
        SX_REQUIRE(hCycleA != nullptr);
        using PingU32Fn = uint32_t (*)();
        auto fn_b_from_a =
            reinterpret_cast<PingU32Fn>(reinterpret_cast<void*>(guest_gp(hCycleA, "CallCycleBFromA")));
        SX_REQUIRE(fn_b_from_a != nullptr && fn_b_from_a() == 0xBBBB2222u);
        HMODULE hCycleB = call_sec_loadlib(L"G:\\Plugins\\VfsCycleB.dll");
        SX_REQUIRE(hCycleB != nullptr);
        auto fn_a_from_b =
            reinterpret_cast<PingU32Fn>(reinterpret_cast<void*>(guest_gp(hCycleB, "CallCycleAFromB")));
        SX_REQUIRE(fn_a_from_b != nullptr && fn_a_from_b() == 0xAAAA1111u);
        SX_REQUIRE(guest_freelib(hCycleB) == TRUE);
        SX_REQUIRE(guest_freelib(hCycleA) == TRUE);

        // Final FreeLibrary drops ref_count 1 -> 0 and unmaps VfsPlugin2 and VfsPlugin cleanly.
        SX_REQUIRE(guest_freelib(hVfsPlugin2) == TRUE);
        SX_REQUIRE(guest_freelib(hVfsPlugin) == TRUE);

        FreeLibrary(hTert);
        main_guest.Unmap();
        scarlettx::PackageVfs::Global().Reset();
    }

    // 12. Package-shipped Microsoft.Xbox.Services.142.GDK.C.Thunks.dll and unversioned
    //     Microsoft.Xbox.Services.C.Thunks.dll win over fallback xsapi.dll when present in
    //     the package directory, and fall back to xsapi.dll cleanly when absent.
    {
        const auto pkg_142_bytes = BuildSyntheticPe64Dll(
            "Microsoft.Xbox.Services.142.GDK.C.Thunks.dll", 0x0000000182100000ULL, {},
            {{"XblInitialize", 0x1420CAFEu}});
        const auto pkg_unv_bytes = BuildSyntheticPe64Dll(
            "Microsoft.Xbox.Services.C.Thunks.dll", 0x0000000182200000ULL, {},
            {{"XblCleanup", 0xC000BEEFu}});

        const std::vector<SynthImportGroup> consumer_imports = {
            {"Microsoft.Xbox.Services.142.GDK.C.Thunks.dll",
             {{"XblInitialize", "Call_142_XblInitialize"}}},
            {"Microsoft.Xbox.Services.C.Thunks.dll", {{"XblCleanup", "Call_Unv_XblCleanup"}}},
        };
        const auto consumer_bytes = BuildSyntheticPe64Dll(
            "ThunksConsumer.dll", 0x0000000182000000ULL, consumer_imports, {});

        // Case A: Both thunks DLLs shipped in package -> package copies win over shim xsapi.dll!
        const std::filesystem::path with_pkg_dir = sandbox.path() / "thunks_pkg_present";
        std::filesystem::create_directories(with_pkg_dir);
        {
            std::ofstream(with_pkg_dir / "ThunksConsumer.dll", std::ios::binary)
                .write(reinterpret_cast<const char*>(consumer_bytes.data()),
                       static_cast<std::streamsize>(consumer_bytes.size()));
            std::ofstream(with_pkg_dir / "Microsoft.Xbox.Services.142.GDK.C.Thunks.dll",
                          std::ios::binary)
                .write(reinterpret_cast<const char*>(pkg_142_bytes.data()),
                       static_cast<std::streamsize>(pkg_142_bytes.size()));
            std::ofstream(with_pkg_dir / "Microsoft.Xbox.Services.C.Thunks.dll", std::ios::binary)
                .write(reinterpret_cast<const char*>(pkg_unv_bytes.data()),
                       static_cast<std::streamsize>(pkg_unv_bytes.size()));
        }

        scarlettx::MappedPeImage mapped_pkg_thunks;
        SX_REQUIRE(mapped_pkg_thunks
                       .MapFile((with_pkg_dir / "ThunksConsumer.dll").string(), redirects, hle_reg,
                                opts)
                       .ok());
        using XblInitFn = uint32_t (*)(const char*);
        using XblCleanFn = uint32_t (*)();
        auto fn_init_pkg =
            reinterpret_cast<XblInitFn>(mapped_pkg_thunks.GetExportAddress("Call_142_XblInitialize"));
        auto fn_clean_pkg =
            reinterpret_cast<XblCleanFn>(mapped_pkg_thunks.GetExportAddress("Call_Unv_XblCleanup"));
        SX_REQUIRE(fn_init_pkg != nullptr && fn_clean_pkg != nullptr);
        SX_REQUIRE(fn_init_pkg("00000000-0000-0000-0000-0000ABCDEF01") == 0x1420CAFEu);
        SX_REQUIRE(fn_clean_pkg() == 0xC000BEEFu);
        mapped_pkg_thunks.Unmap();

        // Case B: Absent from package -> seamlessly falls back to SCARLETTX_BINARY_DIR/xsapi.dll!
        const std::filesystem::path no_pkg_dir = sandbox.path() / "thunks_pkg_absent";
        std::filesystem::create_directories(no_pkg_dir);
        {
            std::ofstream(no_pkg_dir / "ThunksConsumer.dll", std::ios::binary)
                .write(reinterpret_cast<const char*>(consumer_bytes.data()),
                       static_cast<std::streamsize>(consumer_bytes.size()));
        }
        scarlettx::MappedPeImage mapped_fb_thunks;
        SX_REQUIRE(mapped_fb_thunks
                       .MapFile((no_pkg_dir / "ThunksConsumer.dll").string(), redirects, hle_reg,
                                opts)
                       .ok());
        for (const auto& r : mapped_fb_thunks.Resolutions()) {
            const std::string dll = ToLower(r.dll_name);
            if (dll == "microsoft.xbox.services.142.gdk.c.thunks.dll" ||
                dll == "microsoft.xbox.services.c.thunks.dll") {
                SX_REQUIRE(r.kind == scarlettx::ImportResolutionKind::HostDll);
            }
        }
        auto fn_init_fb =
            reinterpret_cast<XblInitFn>(mapped_fb_thunks.GetExportAddress("Call_142_XblInitialize"));
        auto fn_clean_fb =
            reinterpret_cast<XblCleanFn>(mapped_fb_thunks.GetExportAddress("Call_Unv_XblCleanup"));
        SX_REQUIRE(fn_init_fb != nullptr && fn_clean_fb != nullptr);
        SX_REQUIRE(fn_init_fb("00000000-0000-0000-0000-0000ABCDEF01") == 0u);
        SX_REQUIRE(fn_clean_fb() == 0u);
        mapped_fb_thunks.Unmap();
    }
#endif

    std::cout << "[test_inproc_loader] PASSED (relocs=" << mapped.RelocationsApplied()
              << ", imports=" << mapped.Resolutions().size()
              << ", stubs=" << mapped.Stubs().StubCount() << ")\n";
    return 0;
}
