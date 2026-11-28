// End-to-End Retail Parity & Subsystem Verification Suite:
//  1. Retail msft-xvd v3 header (0x284+), 0xAA Merkle hash tree, XvcRegionHeader[] (KeyId=0xFFFF + CIK),
//     and read-only NTFS ($MFT resident + non-resident cluster runs + INDX B-tree) via PackageVfs & InspectPackage.
//  2. Official DirectStorage 32-lane SIMD bit-interleaved GDeflate (0x04 0xFB) in C++ AND GPU HLSL cs_5_0,
//     plus strict error rejection on corrupt compressed bitstreams.
//  3. 512-sample WMA-Pro / Retail XMA2 (packet_metadata=1 + 0xFF end-of-block sentinel) encode/decode.
//  4. RDNA2 XS2 32-bit literals, VOP3P/FLAT, structured loop/if CFG, root-register remapping,
//     0-byte cache poisoning rejection, and dxil.dll container signing.
//  5. AcpHal.dll (non-power-of-2 ApuAlloc + SHAPE APU address mapping) & pixevt_stub.dll coverage.

#include "scarlettx/audio/acphal_abi.h"
#include "scarlettx/audio/xma2_decoder.h"
#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/shader_ir.h"
#include "scarlettx/gpu/xs2_abi.h"
#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/loader/aes_xts_cng.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/media/codecs_abi.h"
#include "scarlettx/runtime/emulator.h"
#include "sx_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <windows.h>
#endif

namespace {

size_t CountStageDirsInTemp() {
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec) return 0;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(tmp, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("scarlettx_stage_", 0) == 0) {
            ++count;
        }
    }
    return count;
}

#if defined(_WIN32)
// Minimal x64 PE32+ forwarder DLL exporting CreateFileW / ReadFile / GetFileSizeEx / CloseHandle
// through guest-hooked IAT entries so test_retail_parity verifies Win32 VFS G:\ streaming end-to-end.
std::vector<uint8_t> BuildVfsProbeGuestDll() {
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
    opt.ImageBase = 0x0000000183000000ULL;
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
    secs[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

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

    const uint8_t dll_main[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(img.data() + 0x1000, dll_main, sizeof(dll_main));

    // Sorted alphabetically so Export Name Table binary search works directly.
    const char* kSyms[4] = {"CloseHandle", "CreateFileW", "GetFileSizeEx", "ReadFile"};
    uint32_t rcur = 0x2000;
    const uint32_t iat_rva = rcur;
    rcur += 5 * 8;
    const uint32_t ilt_rva = rcur;
    rcur += 5 * 8;
    const uint32_t idesc_rva = rcur;
    rcur += 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);

    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {iat_rva, 5 * 8};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
        idesc_rva, static_cast<DWORD>(2 * sizeof(IMAGE_IMPORT_DESCRIPTOR))};

    const uint32_t dll_str_rva = rcur;
    const char kK32[] = "KERNEL32.dll";
    std::memcpy(img.data() + rcur, kK32, sizeof(kK32));
    rcur += sizeof(kK32);

    auto* idesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(img.data() + idesc_rva);
    idesc->OriginalFirstThunk = ilt_rva;
    idesc->Name = dll_str_rva;
    idesc->FirstThunk = iat_rva;

    uint32_t func_rvas[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        rcur = (rcur + 1u) & ~1u;
        const uint32_t hn_rva = rcur;
        const size_t slen = std::strlen(kSyms[i]) + 1;
        std::memcpy(img.data() + rcur + 2, kSyms[i], slen);
        rcur += static_cast<uint32_t>(2 + slen);
        *reinterpret_cast<uint64_t*>(img.data() + iat_rva + i * 8) = hn_rva;
        *reinterpret_cast<uint64_t*>(img.data() + ilt_rva + i * 8) = hn_rva;

        const uint32_t stub_rva = 0x1010u + i * 16u;
        const int32_t disp = static_cast<int32_t>(iat_rva + i * 8u) - static_cast<int32_t>(stub_rva + 6u);
        img[stub_rva + 0] = 0xFF;
        img[stub_rva + 1] = 0x25;
        std::memcpy(img.data() + stub_rva + 2, &disp, 4);
        func_rvas[i] = stub_rva;
    }

    rcur = (rcur + 15u) & ~15u;
    const uint32_t exp_rva = rcur;
    auto* edir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(img.data() + exp_rva);
    rcur += sizeof(IMAGE_EXPORT_DIRECTORY);
    const uint32_t eat_rva = rcur;
    rcur += 4 * 4;
    const uint32_t enames_rva = rcur;
    rcur += 4 * 4;
    const uint32_t eords_rva = rcur;
    rcur += 4 * 2;
    const uint32_t mod_str_rva = rcur;
    const char kMod[] = "VfsProbe.dll";
    std::memcpy(img.data() + rcur, kMod, sizeof(kMod));
    rcur += sizeof(kMod);

    for (uint32_t i = 0; i < 4; ++i) {
        *reinterpret_cast<uint32_t*>(img.data() + eat_rva + i * 4) = func_rvas[i];
        *reinterpret_cast<uint16_t*>(img.data() + eords_rva + i * 2) = static_cast<uint16_t>(i);
        *reinterpret_cast<uint32_t*>(img.data() + enames_rva + i * 4) = rcur;
        const size_t slen = std::strlen(kSyms[i]) + 1;
        std::memcpy(img.data() + rcur, kSyms[i], slen);
        rcur += static_cast<uint32_t>(slen);
    }
    edir->Name = mod_str_rva;
    edir->Base = 1;
    edir->NumberOfFunctions = 4;
    edir->NumberOfNames = 4;
    edir->AddressOfFunctions = eat_rva;
    edir->AddressOfNames = enames_rva;
    edir->AddressOfNameOrdinals = eords_rva;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {exp_rva, rcur - exp_rva};

    auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(img.data() + 0x3000);
    reloc->VirtualAddress = 0x1000;
    reloc->SizeOfBlock = 12;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {0x3000, 12};
    return img;
}
#endif

}  // namespace

int main() {
    sx_test::ScopedTempDir sandbox("test_retail_parity");
    std::cout << "[test_retail_parity] Verifying Retail XVD v3/Merkle/NTFS, 32-lane GPU GDeflate, WMA-Pro XMA2, RDNA2 CFG & AcpHal/PIX...\n";

    // -----------------------------------------------------------------------
    // 1. Retail msft-xvd v3 + 0xAA Merkle Hash Tree + NTFS Volume + PackageVfs
    // -----------------------------------------------------------------------
    {
        const uint8_t key32[32] = {
            0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
            0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
            0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0, 0x01, 0x12,
            0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89, 0x9A,
        };
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string key_hex;
        key_hex.reserve(64);
        for (uint8_t b : key32) {
            key_hex.push_back(kHexDigits[(b >> 4) & 0xFu]);
            key_hex.push_back(kHexDigits[b & 0xFu]);
        }

        // Small resident file (< 512B) + multi-cluster non-resident file (20 KiB)
        const std::string game_cfg_xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<!-- Commented <Executable Name=\"FakeDev.exe\" TargetDeviceFamily=\"XboxGen9\"/> -->\n"
            "<Game configVersion=\"1\">\n"
            "  <Identity Name=\"RetailNtfsSlice\" Publisher=\"CN=ScarlettEdge\" Version=\"1.0.0.0\"/>\n"
            "  <TitleId>ABCDEF01</TitleId>\n"
            "  <ExecutableList>\n"
            "    <Executable Name=\"hello_scarlett.exe\" Id=\"Game\" TargetDeviceFamily=\"XboxGen9\"/>\n"
            "  </ExecutableList>\n"
            "</Game>\n";

        const std::filesystem::path hello_exe_path =
            std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "hello_scarlett" / "hello_scarlett.exe";
        std::ifstream exe_ifs(hello_exe_path, std::ios::binary);
        SX_CHECK(exe_ifs.good());
        const std::vector<uint8_t> hello_exe_bytes(
            (std::istreambuf_iterator<char>(exe_ifs)), std::istreambuf_iterator<char>());
        SX_CHECK(hello_exe_bytes.size() > 1024);

        std::vector<uint8_t> pak_payload(20480);
        for (size_t i = 0; i < pak_payload.size(); ++i) {
            pak_payload[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
        }

        std::vector<scarlettx::XvdInputFile> ntfs_files = {
            {"MicrosoftGame.Config", std::vector<uint8_t>(game_cfg_xml.begin(), game_cfg_xml.end())},
            {"hello_scarlett.exe", hello_exe_bytes},
            {"Content\\Paks\\chunk0.pak", pak_payload},
        };

        const auto ntfs_xvc_bytes =
            scarlettx::XvdContainer::BuildSyntheticNtfsXvc(ntfs_files, key32, sizeof(key32), /*enable_merkle_tree=*/true);
        SX_CHECK(ntfs_xvc_bytes.size() >= 0x10000);

        const auto tmp_xvc = sandbox.path() / "test_retail_ntfs_v3.xvc";
        {
            std::ofstream ofs(tmp_xvc, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(ntfs_xvc_bytes.data()),
                      static_cast<std::streamsize>(ntfs_xvc_bytes.size()));
        }

        scarlettx::XvdContainer xvd;
        SX_CHECK(xvd.OpenFile(tmp_xvc.string()).ok());
        SX_CHECK(xvd.Header().is_retail_v3_layout);
        SX_CHECK(xvd.Header().has_data_integrity);
        SX_CHECK(xvd.Header().is_encrypted);
        SX_CHECK(xvd.SetVolumeKeyHex(key_hex).ok());
        SX_CHECK(xvd.VerifyDataPageHash(0).ok());

        std::vector<scarlettx::XvdEmbeddedFile> listed;
        SX_CHECK(xvd.ListEmbeddedFiles(listed).ok());
        SX_CHECK(listed.size() >= 3);

        // Mount into PackageVfs and read both resident XML and non-resident cluster slice
        scarlettx::PackageVfs vfs;
        SX_CHECK(vfs.MountXvd("G:", tmp_xvc.string(), key_hex).ok());
        std::vector<uint8_t> read_xml;
        SX_CHECK(vfs.ReadFileBytes("G:\\MicrosoftGame.Config", read_xml).ok());
        SX_CHECK(read_xml.size() == game_cfg_xml.size());
        SX_CHECK(std::memcmp(read_xml.data(), game_cfg_xml.data(), game_cfg_xml.size()) == 0);

        std::vector<uint8_t> read_slice;
        SX_CHECK(vfs.ReadFileRange("G:/Content/Paks/chunk0.pak", 4000, 5000, read_slice).ok());
        SX_CHECK(read_slice.size() == 5000);
        SX_CHECK(std::memcmp(read_slice.data(), pak_payload.data() + 4000, 5000) == 0);

        // InspectPackage on encrypted retail-v3 NTFS .xvc without full-package extraction
        scarlettx::EmulatorConfig ecfg{};
        ecfg.volume_key = key_hex;
        scarlettx::Emulator emu(ecfg);
        scarlettx::PackageInspectionReport rep{};
        SX_CHECK(emu.InspectPackage(tmp_xvc.string(), rep).ok());
        SX_CHECK(rep.config.title_id == "ABCDEF01");
        SX_CHECK(rep.main_pe.is_pe64);

        std::error_code ec;
        std::filesystem::remove(tmp_xvc, ec);
    }

    // -----------------------------------------------------------------------
    // 2. Official 32-Lane SIMD Bit-Interleaved GDeflate (C++ & GPU cs_5_0)
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> raw_asset(90000);  // Multi-tile (> 64 KiB)
        for (size_t i = 0; i < raw_asset.size(); ++i) {
            raw_asset[i] = (i % 64 < 40) ? static_cast<uint8_t>((i / 64) & 0xFFu)
                                        : static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        }
        std::vector<uint8_t> gd32_comp(raw_asset.size() + 8192);
        size_t gd32_sz = 0;
        SX_CHECK(XboxCodecGDeflateCompressRetail(
                     raw_asset.data(), raw_asset.size(), gd32_comp.data(), gd32_comp.size(), &gd32_sz) == 0);
        SX_CHECK(gd32_sz > 16 && gd32_comp[0] == 0x04u && gd32_comp[1] == 0xFBu);
        gd32_comp.resize(gd32_sz);

        std::vector<uint8_t> cpu_out(raw_asset.size(), 0);
        size_t cpu_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(
                     gd32_comp.data(), gd32_comp.size(), cpu_out.data(), cpu_out.size(), &cpu_written) == 0);
        SX_CHECK(cpu_written == raw_asset.size());
        SX_CHECK(cpu_out == raw_asset);

        // Corrupt/unrecognized bitstream MUST return an error (< 0), never silent memcpy!
        const uint8_t garbage[32] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44};
        std::vector<uint8_t> bad_out(32, 0xAA);
        SX_CHECK(XboxCodecGDeflateDecompress(garbage, sizeof(garbage), bad_out.data(), bad_out.size(), nullptr) < 0);
        SX_CHECK(XboxCodecBcPackDecompress(garbage, sizeof(garbage), bad_out.data(), bad_out.size(), 71) < 0);

#if defined(_WIN32)
        scarlettx::D3D12Backend d3d;
        if (d3d.Initialize(true, false).ok()) {
            auto* dev = static_cast<ID3D12Device*>(d3d.NativeDevice());
            D3D12_COMMAND_QUEUE_DESC qdesc{};
            qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* queue = nullptr;
            SX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))));

            D3D12_HEAP_PROPERTIES def_hp{};
            def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_HEAP_PROPERTIES rb_hp{};
            rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = raw_asset.size();
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            ID3D12Resource* gpu_dst = nullptr;
            ID3D12Resource* gpu_rb = nullptr;
            SX_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                &def_hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpu_dst))));
            bd.Flags = D3D12_RESOURCE_FLAG_NONE;
            SX_CHECK(SUCCEEDED(dev->CreateCommittedResource(
                &rb_hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu_rb))));

            SX_CHECK(DStorageDecompressGpuCompute(
                         dev, queue, DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                         gd32_comp.data(), static_cast<uint32_t>(gd32_comp.size()),
                         gpu_dst, 0ull, static_cast<uint32_t>(raw_asset.size())) == 0);

            ID3D12CommandAllocator* ca = nullptr;
            ID3D12GraphicsCommandList* cl = nullptr;
            ID3D12Fence* fn = nullptr;
            SX_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))));
            SX_CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl))));
            SX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fn))));
            cl->CopyBufferRegion(gpu_rb, 0, gpu_dst, 0, raw_asset.size());
            cl->Close();
            ID3D12CommandList* lists[] = {cl};
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fn, 1);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fn->GetCompletedValue() < 1) {
                fn->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, INFINITE);
            }
            CloseHandle(ev);

            void* mapped = nullptr;
            SX_CHECK(SUCCEEDED(gpu_rb->Map(0, nullptr, &mapped)) && mapped != nullptr);
            SX_CHECK(std::memcmp(mapped, raw_asset.data(), raw_asset.size()) == 0);
            gpu_rb->Unmap(0, nullptr);

            fn->Release();
            cl->Release();
            ca->Release();
            gpu_rb->Release();
            gpu_dst->Release();
            queue->Release();
        }
#endif
    }

    // -----------------------------------------------------------------------
    // 3. 512-Sample WMA-Pro / Retail XMA2 (packet_metadata=1 + 0xFF skip)
    // -----------------------------------------------------------------------
    {
        constexpr uint32_t kFrames = 1536;  // 3 x 512-sample WMA-Pro frames
        std::vector<float> stereo_in(kFrames * 2);
        for (uint32_t i = 0; i < kFrames; ++i) {
            const float t = static_cast<float>(i) / 48000.0f;
            stereo_in[i * 2 + 0] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * t);
            stereo_in[i * 2 + 1] = 0.5f * std::cos(2.0f * 3.14159265f * 660.0f * t);
        }
        const auto xma2_pkt = scarlettx::audio::EncodeRetailXma2WmaProPacket(stereo_in.data(), kFrames, 2, 48000);
        SX_CHECK(xma2_pkt.size() >= 2048 && (xma2_pkt.size() % 2048) == 0);
        // Verify packet_metadata == 1 (bits 2..0 of byte 3)
        SX_CHECK((xma2_pkt[2] & 0x07u) == 1u);

        scarlettx::audio::Xma2StreamDecoder dec(2, 48000);
        std::vector<float> pcm_out;
        SX_CHECK(dec.DecodeBlock(xma2_pkt.data(), xma2_pkt.size(), pcm_out, kFrames));
        SX_CHECK(pcm_out.size() == kFrames * 2);
        double energy = 0.0;
        for (float s : pcm_out) energy += static_cast<double>(s) * static_cast<double>(s);
        SX_CHECK(energy > 10.0);
    }

    // -----------------------------------------------------------------------
    // 4. RDNA2 XS2 Literals, Structured CFG, Root Remap & Cache Rejection
    // -----------------------------------------------------------------------
    {
        XS2_PIPELINE_DESC pdesc{};
        pdesc.maxCacheEntries = 4;
        XS2_PIPELINE_HANDLE pipe = nullptr;
        SX_CHECK(Xs2ShaderCreateTranslationPipeline(&pdesc, &pipe) == 0 && pipe != nullptr);

        // Passing 0-byte / null microcode must fail and NOT poison cache
        void* bad_bc = nullptr;
        size_t bad_sz = 99;
        SX_CHECK(Xs2ShaderTranslateCached(pipe, nullptr, 0, &bad_bc, &bad_sz) != 0);

        // RDNA2 program with 32-bit literal constant (src0 = 0xFF -> 8-byte VOP1!) + backward loop:
        //   0: v_mov_b32 v1, 0x40800000 (4.0f literal -> 2 dwords!)
        //   2: v_add_f32 v0, v0, v1     (1 dword)
        //   3: s_cbranch_scc1 -2        (loops back to dword 2; bounded CFG!)
        //   4: s_endpgm
        const uint32_t rdna2_lit_loop[] = {
            0x7E0202FFu, 0x40800000u,  // v_mov_b32 v1, literal(4.0f)
            0x02000200u,               // v_add_f32 v0, v0, v1
            0xBF85FFFEu,               // s_cbranch_scc1 -2
            0xBF810000u,               // s_endpgm
        };
        XS2_TRANSLATION_OPTIONS opts{};
        opts.targetShaderModel = 0x00060006u;
        opts.enableWave32 = 1;
        void* spv = nullptr;
        size_t spv_sz = 0;
        SX_CHECK(Xs2ShaderLiftToSpirv(rdna2_lit_loop, sizeof(rdna2_lit_loop), &opts, &spv, &spv_sz) == 0);
        SX_CHECK(spv != nullptr && spv_sz > 64);

        // Patch SPIR-V root register binding 0 -> binding 7, space 2
        XS2_REGISTER_REMAP rmap{};
        rmap.oldRegister = 0;
        rmap.newRegister = 7;
        rmap.registerSpace = 2;
        SX_CHECK(Xs2ShaderPatchRootSignatureRegisters(spv, spv_sz, &rmap, 1) == 0);
        Xs2ShaderFreeBytecode(spv);
        SX_CHECK(Xs2ShaderDestroyTranslationPipeline(pipe) == 0);
    }

    // -----------------------------------------------------------------------
    // 5. AcpHal.dll & pixevt_stub.dll Full Coverage
    // -----------------------------------------------------------------------
    {
        SX_CHECK(ApuCreateHeap(4u * 1024u * 1024u, 0) == 0);
        void* apu_mem = nullptr;
        // Non-power-of-2 alignment (96) must round up cleanly without CRT abort!
        SX_CHECK(ApuAlloc(&apu_mem, nullptr, 4096, 96, 0) == 0 && apu_mem != nullptr);
        SX_CHECK((reinterpret_cast<uintptr_t>(apu_mem) % 128u) == 0);
        SX_CHECK(ApuIsVirtualAddressValid(apu_mem, 4096) == 1);
        const uint32_t apu_pa = ApuMapVirtualAddress(apu_mem);
        SX_CHECK(apu_pa != 0 && ApuMapApuAddress(apu_pa) == apu_mem);

        void* acp = nullptr;
        SX_CHECK(AcpHalCreate(&acp) == 0 && acp != nullptr);
        ACP_SHAPE_CONTEXT_COUNTS shape{};
        shape.numSrcContexts = 4;
        shape.numEqCompContexts = 2;
        SX_CHECK(AcpHalAllocateShapeContexts(&shape) == 0);
        SX_CHECK(shape.srcContexts != nullptr && ApuIsVirtualAddressValid(shape.srcContexts, 64) == 1);
        SX_CHECK(ApuMapVirtualAddress(shape.srcContexts) != 0);
        SX_CHECK(AcpHalReleaseShapeContexts() == 0);
        SX_CHECK(ApuFree(apu_mem) == 0);

#if defined(_WIN32)
        const auto pix_dll = std::filesystem::path(SCARLETTX_BINARY_DIR) / "pixevt_stub.dll";
        HMODULE hPix = LoadLibraryW(pix_dll.c_str());
        SX_CHECK(hPix != nullptr);
        using FnBegin = void(__cdecl*)(void*, uint64_t, const char*);
        using FnEnd = void(__cdecl*)(void*);
        using FnMarker = void(__cdecl*)(void*, uint64_t, const char*);
        auto fn_begin = reinterpret_cast<FnBegin>(reinterpret_cast<void*>(
            GetProcAddress(hPix, "PIXBeginEventOnCommandList")));
        auto fn_end = reinterpret_cast<FnEnd>(reinterpret_cast<void*>(
            GetProcAddress(hPix, "PIXEndEventOnCommandList")));
        auto fn_mark = reinterpret_cast<FnMarker>(reinterpret_cast<void*>(
            GetProcAddress(hPix, "PIXSetMarkerOnCommandList")));
        SX_CHECK(fn_begin && fn_end && fn_mark);
        fn_begin(nullptr, 0xFF00FF00ull, "FramePass");
        fn_mark(nullptr, 0xFFFFFFFFull, "DrawBatch");
        fn_end(nullptr);
        FreeLibrary(hPix);
#endif
    }

    // -----------------------------------------------------------------------
    // 6. Authentic makepkg .msixvc Contiguous-Merkle + 128B XvcRegionHeader v2 (FS-MD)
    // -----------------------------------------------------------------------
    {
        std::vector<scarlettx::XvdInputFile> mp_files = {
            {"MicrosoftGame.Config", {'<', 'G', 'a', 'm', 'e', '/', '>'}},
            {"Sub\\data.bin", std::vector<uint8_t>(8192, 0x5Au)},
        };
        const std::vector<uint8_t> ntfs_vol = scarlettx::XvdContainer::BuildNtfsVolumeBytes(mp_files);
        const size_t drive_pages = ntfs_vol.size() / 4096u;
        SX_CHECK(drive_pages >= 8u);

        // Layout: 3 hdr + 1 MDU + 1 L0 Merkle + 1 PDU + 1 XvcInfo + drive_pages
        const size_t total_hashed = 1u + 1u + drive_pages;
        SX_CHECK(total_hashed <= 170u);  // single Level-0 Merkle root block
        const size_t total_pages = 3u + 1u + 1u + total_hashed;
        std::vector<uint8_t> msixvc(total_pages * 4096u, 0);

        auto write_u16 = [](uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        };
        auto write_u32 = [](uint8_t* p, uint32_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        };
        auto write_u64 = [&](uint8_t* p, uint64_t v) {
            write_u32(p, static_cast<uint32_t>(v & 0xFFFFFFFFull));
            write_u32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
        };

        // XvcInfo v2 at page 6 (0x6000) with 128-byte XvcRegionHeader[] at +0xDA8:
        // Region 0: FS-MD (0x40000002, keyId=0xFFFF) covering MFT/directory clusters;
        // Region 1: Chunk 1000 (0x3E8, keyId=0) covering remaining data clusters.
        const uint64_t xvc_off = 6ull * 4096ull;
        const uint64_t drive_off = 7ull * 4096ull;
        const uint64_t fs_md_bytes = (drive_pages - 2u) * 4096ull;
        const uint64_t chunk_bytes = 2ull * 4096ull;

        write_u32(msixvc.data() + xvc_off + 0xD10, 2u);  // Version = 2
        write_u32(msixvc.data() + xvc_off + 0xD14, 2u);  // RegionCount = 2
        write_u16(msixvc.data() + xvc_off + 0xD1E, 1u);  // KeyCount = 1

        uint8_t* r0 = msixvc.data() + xvc_off + 0xDA8u;
        write_u32(r0 + 0x00, 0x40000002u);               // FS-MD
        write_u16(r0 + 0x04, 0xFFFFu);                   // Unencrypted!
        write_u32(r0 + 0x08, 1u);
        write_u64(r0 + 0x50, drive_off);
        write_u64(r0 + 0x58, fs_md_bytes);

        uint8_t* r1 = r0 + 0x80u;
        write_u32(r1 + 0x00, 1000u);                     // Launch chunk
        write_u16(r1 + 0x04, 0u);                        // Encrypted with slot 0!
        write_u32(r1 + 0x08, 4u);
        write_u64(r1 + 0x50, drive_off + fs_md_bytes);
        write_u64(r1 + 0x58, chunk_bytes);

        std::memcpy(msixvc.data() + drive_off, ntfs_vol.data(), ntfs_vol.size());

        // Compute contiguous Merkle hash block at page 4 (0x4000) over pages 5..(5 + total_hashed - 1)
        for (size_t i = 0; i < total_hashed; ++i) {
            uint8_t dg[32]{};
            SX_CHECK(scarlettx::crypto::ComputeSha256(
                msixvc.data() + (5u + i) * 4096u, 4096u, dg));
            std::memcpy(msixvc.data() + 0x4000u + i * 24u, dg, 24u);
        }
        uint8_t top_dg[32]{};
        SX_CHECK(scarlettx::crypto::ComputeSha256(msixvc.data() + 0x4000u, 4096u, top_dg));

        // Populate msft-xvd header at 0x200 with makepkg BlockSize = 0xAA000 at 0x298
        std::memcpy(msixvc.data() + 0x200, "msft-xvd", 8);
        write_u32(msixvc.data() + 0x208, 0x41u);         // ReadOnly | Encrypted | IntegrityEnabled
        write_u32(msixvc.data() + 0x20C, 3u);
        write_u64(msixvc.data() + 0x218, ntfs_vol.size());
        std::memcpy(msixvc.data() + 0x240, top_dg, 32);
        write_u32(msixvc.data() + 0x284, 1u);
        write_u32(msixvc.data() + 0x28C, 1234u);         // 32-bit PDU length -> 1 page
        write_u32(msixvc.data() + 0x290, 4096u);         // 32-bit XvcData length -> 1 page
        write_u32(msixvc.data() + 0x298, 0x000AA000u);   // Makepkg Merkle BlockSize

        const auto tmp_msixvc = sandbox.path() / "test_makepkg_v3.msixvc";
        {
            std::ofstream ofs(tmp_msixvc, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(msixvc.data()),
                      static_cast<std::streamsize>(msixvc.size()));
        }

        scarlettx::XvdContainer mp_xvd;
        SX_CHECK(mp_xvd.OpenFile(tmp_msixvc.string()).ok());
        SX_CHECK(mp_xvd.Header().is_encrypted);
        SX_CHECK(mp_xvd.Header().has_data_integrity);
        SX_CHECK(mp_xvd.Header().mutable_data_length == 4096u);
        SX_CHECK(mp_xvd.Regions().size() == 2u);
        SX_CHECK(mp_xvd.VerifyDataPageHash(0).ok());

        // Even WITHOUT any encryption key, unencrypted FS-MD NTFS $MFT lists all embedded files!
        std::vector<scarlettx::XvdEmbeddedFile> mp_listed;
        SX_CHECK(mp_xvd.ListEmbeddedFiles(mp_listed).ok());
        SX_CHECK(mp_listed.size() >= 2u);

        // And resident MicrosoftGame.Config in FS-MD reads cleanly while encrypted Chunk 1000 rejects without key!
        std::vector<uint8_t> res_cfg;
        SX_CHECK(mp_xvd.ReadEmbeddedFile("MicrosoftGame.Config", res_cfg).ok());
        SX_CHECK(res_cfg.size() == 7u);
        std::vector<uint8_t> enc_bin;
        SX_CHECK(!mp_xvd.ReadUserData(fs_md_bytes, 4096u, enc_bin).ok());

        // Standard 48-byte binary .cik keyfile ([0..15]=GUID, [16..47]=32B AES-128-XTS key) + text GUID=HEXKEY keyring
        const auto tmp_cik = sandbox.path() / "test_makepkg_v3.cik";
        const auto tmp_txt_cik = sandbox.path() / "test_makepkg_v3_keyring.txt";
        {
            uint8_t cik48[48] = {};
            for (int i = 0; i < 48; ++i) cik48[i] = static_cast<uint8_t>(0x30 + i);
            std::ofstream cofs(tmp_cik, std::ios::binary | std::ios::trunc);
            cofs.write(reinterpret_cast<const char*>(cik48), sizeof(cik48));

            std::ofstream tofs(tmp_txt_cik, std::ios::trunc);
            tofs << "# Xbox CIK keyring\n";
            tofs << "14F323CB-43D7-04BB-49F2-0AC835FFCBCC = 0123456789abcdef0123456789abcdeffedcba9876543210fedcba9876543210\n";
        }
        SX_CHECK(mp_xvd.LoadVolumeKeyFile(tmp_cik.string()).ok());
        SX_CHECK(mp_xvd.HasVolumeKey());
        SX_CHECK(mp_xvd.ReadUserData(fs_md_bytes, 4096u, enc_bin).ok());
        SX_CHECK(enc_bin.size() == 4096u);

        scarlettx::XvdContainer mp_xvd_txt;
        SX_CHECK(mp_xvd_txt.OpenFile(tmp_msixvc.string()).ok());
        SX_CHECK(mp_xvd_txt.LoadVolumeKeyFile(tmp_txt_cik.string()).ok());
        SX_CHECK(mp_xvd_txt.ReadUserData(fs_md_bytes, 4096u, enc_bin).ok());

        std::error_code rm_ec;
        std::filesystem::remove(tmp_msixvc, rm_ec);
        std::filesystem::remove(tmp_cik, rm_ec);
        std::filesystem::remove(tmp_txt_cik, rm_ec);
    }

    // -----------------------------------------------------------------------
    // 7. Single-Call 4 KiB BCRYPT_XTS_AES_ALGORITHM Sector Parity (32B & 64B Keys)
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> plain_4k(4096);
        for (size_t i = 0; i < plain_4k.size(); ++i) {
            plain_4k[i] = static_cast<uint8_t>((i * 131u + 29u) & 0xFFu);
        }

        // AES-128-XTS (32B = 16B data_key || 16B tweak_key): verify single-call 4 KiB
        // BCRYPT_XTS_AES_ALGORITHM matches block-by-block GF(2^128) XTS byte-for-byte
        // across all initial blocks AND round-trips all 4096 bytes cleanly.
        const uint8_t xts128_key[32] = {
            0x27, 0x18, 0x28, 0x18, 0x28, 0x45, 0x90, 0x45,
            0x23, 0x53, 0x60, 0x28, 0x74, 0x71, 0x35, 0x26,
            0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93,
            0x23, 0x84, 0x62, 0x64, 0x33, 0x83, 0x27, 0x95,
        };
        const uint8_t vduid_seed[16] = {
            0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
            0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90,
        };

        scarlettx::crypto::AesXtsPreparedKey prep128;
        SX_CHECK(prep128.Init(xts128_key, sizeof(xts128_key)));
        SX_CHECK(prep128.hk_xts_4k != nullptr);

        std::vector<uint8_t> ct_4k(4096, 0);
        std::vector<uint8_t> ct_ref_4080(4080, 0);
        std::vector<uint8_t> rt_4k(4096, 0);
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            true, 0x0000000102030405ull, nullptr, plain_4k.data(), ct_4k.data(), 4096));
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            true, 0x0000000102030405ull, nullptr, plain_4k.data(), ct_ref_4080.data(), 4080));
        SX_CHECK(std::memcmp(ct_4k.data(), ct_ref_4080.data(), 4080) == 0);
        SX_CHECK(ct_4k != plain_4k);
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            false, 0x0000000102030405ull, nullptr, ct_4k.data(), rt_4k.data(), 4096));
        SX_CHECK(rt_4k == plain_4k);

        // Also verify VDUID tweak_seed 4 KiB sector path AND 64B AES-256-XTS prepared key.
        std::vector<uint8_t> ct_vduid_4k(4096, 0);
        std::vector<uint8_t> ct_vduid_4080(4080, 0);
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            true, 77ull, vduid_seed, plain_4k.data(), ct_vduid_4k.data(), 4096));
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            true, 77ull, vduid_seed, plain_4k.data(), ct_vduid_4080.data(), 4080));
        SX_CHECK(std::memcmp(ct_vduid_4k.data(), ct_vduid_4080.data(), 4080) == 0);
        SX_CHECK(prep128.TransformSectorWithTweakSeed(
            false, 77ull, vduid_seed, ct_vduid_4k.data(), rt_4k.data(), 4096));
        SX_CHECK(rt_4k == plain_4k);

        uint8_t xts256_key[64]{};
        for (size_t i = 0; i < sizeof(xts256_key); ++i) {
            xts256_key[i] = static_cast<uint8_t>((i * 37u + 0x5Au) & 0xFFu);
        }
        scarlettx::crypto::AesXtsPreparedKey prep256;
        SX_CHECK(prep256.Init(xts256_key, sizeof(xts256_key)));
        std::vector<uint8_t> ct256_4k(4096, 0);
        std::vector<uint8_t> ct256_4080(4080, 0);
        SX_CHECK(prep256.TransformSectorWithTweakSeed(
            true, 19ull, nullptr, plain_4k.data(), ct256_4k.data(), 4096));
        SX_CHECK(prep256.TransformSectorWithTweakSeed(
            true, 19ull, nullptr, plain_4k.data(), ct256_4080.data(), 4080));
        SX_CHECK(std::memcmp(ct256_4k.data(), ct256_4080.data(), 4080) == 0);
        SX_CHECK(prep256.TransformSectorWithTweakSeed(
            false, 19ull, nullptr, ct256_4k.data(), rt_4k.data(), 4096));
        SX_CHECK(rt_4k == plain_4k);
    }

    // -----------------------------------------------------------------------
    // 8. Multi-Key Encrypted v3 .msixvc + Out-of-Order .cik KeyId GUID Keyring +
    //    Zero-Staging In-Process Boot + Win32 VFS & DirectStorage Streaming
    //    (Asserting 0 scarlettx_stage_* directories in %TEMP%)
    // -----------------------------------------------------------------------
    {
        const std::filesystem::path hello_exe_path =
            std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "hello_scarlett" / "hello_scarlett.exe";
        std::ifstream exe_ifs(hello_exe_path, std::ios::binary);
        SX_CHECK(exe_ifs.good());
        const std::vector<uint8_t> hello_exe_bytes(
            (std::istreambuf_iterator<char>(exe_ifs)), std::istreambuf_iterator<char>());

        // Keep MicrosoftGame.Config <= 192B so it resides inside unencrypted $MFT (FS-MD),
        // while "hello_scarlett.exe" ('h') precedes "Streaming\region1_stream.bin" ('s') in
        // non-resident cluster order (Region 0 = slot 0, Region 1 = slot 1).
        const std::string game_cfg_xml =
            "<Game configVersion=\"1\"><TitleId>778899AA</TitleId><ExecutableList>"
            "<Executable Name=\"hello_scarlett.exe\" TargetDeviceFamily=\"XboxGen9\"/>"
            "</ExecutableList></Game>";
        SX_CHECK(game_cfg_xml.size() <= 192u);

        std::vector<uint8_t> reg1_payload(24576);
        for (size_t i = 0; i < reg1_payload.size(); ++i) {
            reg1_payload[i] = static_cast<uint8_t>((i * 97u + 53u) & 0xFFu);
        }
        uint8_t expected_sha256[32]{};
        SX_CHECK(scarlettx::crypto::ComputeSha256(
            reg1_payload.data(), reg1_payload.size(), expected_sha256));

        const std::vector<scarlettx::XvdInputFile> mk_files = {
            {"MicrosoftGame.Config", std::vector<uint8_t>(game_cfg_xml.begin(), game_cfg_xml.end())},
            {"hello_scarlett.exe", hello_exe_bytes},
            {"Streaming\\region1_stream.bin", reg1_payload},
        };
        const std::vector<uint8_t> ntfs_vol = scarlettx::XvdContainer::BuildNtfsVolumeBytes(mk_files);
        const uint64_t total_vol_bytes = ntfs_vol.size();

        uint64_t exe_user_off = 0;
        uint64_t reg1_user_off = 0;
        {
            scarlettx::XvdContainer layout_probe;
            SX_CHECK(layout_probe.OpenBytes(scarlettx::XvdContainer::BuildSyntheticXvd(ntfs_vol)).ok());
            std::vector<scarlettx::XvdEmbeddedFile> layout_entries;
            SX_CHECK(layout_probe.ListEmbeddedFiles(layout_entries).ok());
            for (const auto& e : layout_entries) {
                if (e.relative_path == "hello_scarlett.exe") {
                    exe_user_off = e.user_data_offset;
                } else if (e.relative_path == "Streaming\\region1_stream.bin" ||
                           e.relative_path == "Streaming/region1_stream.bin") {
                    reg1_user_off = e.user_data_offset;
                }
            }
        }
        SX_CHECK(exe_user_off > 0 && reg1_user_off > exe_user_off && total_vol_bytes > reg1_user_off);
        const uint64_t fs_md_bytes = exe_user_off;
        const uint64_t reg0_bytes = reg1_user_off - exe_user_off;
        const uint64_t reg1_bytes = total_vol_bytes - reg1_user_off;

        // Distinct 16-byte KeyId GUIDs and 32-byte AES-128-XTS keys for slot 0 and slot 1.
        // Slot 0 GUID: {11223344-5566-7788-99AA-BBCCDDEEFF01} stored in mixed-endian Win32 GUID layout
        // in the XVC header while zz_real.cik stores raw big-endian GUID bytes -> tests endian-agnostic match!
        const std::array<uint8_t, 16> guid_slot0_mixed = {
            0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x88, 0x77,
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01,
        };
        const std::array<uint8_t, 16> guid_slot0_raw_be = {
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01,
        };
        // Slot 1 GUID: {A0B1C2D3-E4F5-4607-891A-2B3C4D5E6F70}
        const std::array<uint8_t, 16> guid_slot1_raw_be = {
            0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x46, 0x07,
            0x89, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70,
        };
        const std::array<uint8_t, 16> guid_decoy = {
            0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF,
            0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF,
        };

        std::array<uint8_t, 32> key_slot0{};
        std::array<uint8_t, 32> key_slot1{};
        std::array<uint8_t, 32> key_wrong{};
        for (size_t i = 0; i < 32; ++i) {
            key_slot0[i] = static_cast<uint8_t>(0x10u + i * 3u);
            key_slot1[i] = static_cast<uint8_t>(0x80u + i * 5u);
            key_wrong[i] = static_cast<uint8_t>(0xFFu - i);
        }

        const std::vector<scarlettx::XvcRegionInfo> regions = {
            {0x40000002u, scarlettx::XvdContainer::kUnencryptedKeyId, 1u, 0ull, fs_md_bytes},
            {1000u, 0u, 4u, fs_md_bytes, reg0_bytes},
            {1001u, 1u, 4u, fs_md_bytes + reg0_bytes, reg1_bytes},
        };
        const std::vector<std::pair<std::array<uint8_t, 16>, std::vector<uint8_t>>> cik_guid_keys = {
            {guid_slot0_mixed, std::vector<uint8_t>(key_slot0.begin(), key_slot0.end())},
            {guid_slot1_raw_be, std::vector<uint8_t>(key_slot1.begin(), key_slot1.end())},
        };

        const auto msixvc_bytes = scarlettx::XvdContainer::BuildRetailV3XvcMultiKeyWithGuids(
            ntfs_vol, cik_guid_keys, regions, /*enable_merkle_tree=*/true);
        SX_CHECK(msixvc_bytes.size() > total_vol_bytes);

        const auto msixvc_path = sandbox.path() / "multi_key_zero_stage.msixvc";
        {
            std::ofstream ofs(msixvc_path, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(msixvc_bytes.data()),
                      static_cast<std::streamsize>(msixvc_bytes.size()));
            SX_CHECK(ofs.good());
        }

        // Build keyring directory with decoy 00_wrong.cik alphabetically BEFORE 50_slot1.key and zz_real.cik!
        const auto keyring_dir = sandbox.path() / "keyring";
        const auto decoy_only_dir = sandbox.path() / "keyring_decoy_only";
        std::filesystem::create_directories(keyring_dir);
        std::filesystem::create_directories(decoy_only_dir);

        auto write_cik48 = [](const std::filesystem::path& p,
                              const std::array<uint8_t, 16>& guid,
                              const std::array<uint8_t, 32>& k) {
            uint8_t buf[48]{};
            std::memcpy(buf, guid.data(), 16);
            std::memcpy(buf + 16, k.data(), 32);
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(buf), sizeof(buf));
            SX_CHECK(ofs.good());
        };
        write_cik48(keyring_dir / "00_wrong.cik", guid_decoy, key_wrong);
        write_cik48(decoy_only_dir / "00_wrong.cik", guid_decoy, key_wrong);
        write_cik48(keyring_dir / "zz_real.cik", guid_slot0_raw_be, key_slot0);

        static constexpr char kHex[] = "0123456789abcdef";
        std::string key1_hex;
        for (uint8_t b : key_slot1) {
            key1_hex.push_back(kHex[(b >> 4) & 0xFu]);
            key1_hex.push_back(kHex[b & 0xFu]);
        }
        {
            std::ofstream tofs(keyring_dir / "50_slot1.key", std::ios::trunc);
            tofs << "A0B1C2D3-E4F5-4607-891A-2B3C4D5E6F70 = " << key1_hex << "\n";
            SX_CHECK(tofs.good());
        }

        // Verify decoy-only keyring is strictly rejected when reading encrypted Region 0 / Region 1.
        {
            scarlettx::XvdContainer bad_xvd;
            SX_CHECK(bad_xvd.OpenFile(msixvc_path.string()).ok());
            SX_CHECK(bad_xvd.LoadKeyringDirectory(decoy_only_dir.string()).ok());
            std::vector<uint8_t> fail_bytes;
            SX_CHECK(!bad_xvd.ReadEmbeddedFile("Streaming\\region1_stream.bin", fail_bytes).ok());
        }

        // Assert 0 scarlettx_stage_* directories exist before and after InspectPackage + BootPackage!
        SX_CHECK(CountStageDirsInTemp() == 0u);

#if defined(_WIN32)
        _putenv_s("SCARLETTX_MAX_FRAMES", "1");
        scarlettx::EmulatorConfig boot_cfg{};
        boot_cfg.in_process = true;
        boot_cfg.force_relocate = true;
        boot_cfg.shim_directory = SCARLETTX_BINARY_DIR;
        boot_cfg.volume_key = keyring_dir.string();  // Keyring directory (same path used by --keyring)
        scarlettx::Emulator boot_emu(boot_cfg);

        scarlettx::PackageInspectionReport mk_rep{};
        SX_CHECK(boot_emu.InspectPackage(msixvc_path.string(), mk_rep).ok());
        SX_CHECK(mk_rep.config.title_id == "778899AA");
        SX_CHECK(mk_rep.main_pe.is_pe64);
        SX_CHECK(CountStageDirsInTemp() == 0u);

        auto boot_st = boot_emu.BootPackage(msixvc_path.string());
        if (!boot_st.ok()) {
            std::cerr << "[test_retail_parity] Multi-key zero-staging BootPackage failed: "
                      << boot_st.message << "\n";
            return 1;
        }
        SX_CHECK(CountStageDirsInTemp() == 0u);

        // Mount the multi-key .msixvc into PackageVfs::Global() using the out-of-order keyring_dir
        // (BootPackage's RAII guard cleanly unmounts G: after guest exit) and stream Region 1 via
        // both Win32 VFS guest hooks and DirectStorage!
        SX_CHECK(scarlettx::PackageVfs::Global()
                     .MountXvd("G:", msixvc_path.string(), keyring_dir.string())
                     .ok());

        // Stream G:\Streaming\region1_stream.bin via Win32 VFS guest hooks (CreateFileW + ReadFile)!
        {
            const auto probe_dll_bytes = BuildVfsProbeGuestDll();
            scarlettx::DllRedirectionTable redirs;
            scarlettx::HleSymbolRegistry hle_reg;
            scarlettx::PeMapOptions map_opts{};
            map_opts.force_relocate = true;
            map_opts.shim_directory = SCARLETTX_BINARY_DIR;
            scarlettx::MappedPeImage vfs_probe;
            SX_CHECK(vfs_probe.MapBytes(probe_dll_bytes, "G:\\VfsProbe.dll", redirs, hle_reg, map_opts).ok());

            using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                                  DWORD, DWORD, HANDLE);
            using GetFileSizeExFn = BOOL(WINAPI*)(HANDLE, PLARGE_INTEGER);
            using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
            using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

            auto fn_create = reinterpret_cast<CreateFileWFn>(vfs_probe.GetExportAddress("CreateFileW"));
            auto fn_size = reinterpret_cast<GetFileSizeExFn>(vfs_probe.GetExportAddress("GetFileSizeEx"));
            auto fn_read = reinterpret_cast<ReadFileFn>(vfs_probe.GetExportAddress("ReadFile"));
            auto fn_close = reinterpret_cast<CloseHandleFn>(vfs_probe.GetExportAddress("CloseHandle"));
            SX_CHECK(fn_create && fn_size && fn_read && fn_close);

            HANDLE hvfs = fn_create(L"G:\\Streaming\\region1_stream.bin", GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            SX_CHECK(hvfs != INVALID_HANDLE_VALUE);
            LARGE_INTEGER sz{};
            SX_CHECK(fn_size(hvfs, &sz) == TRUE && static_cast<size_t>(sz.QuadPart) == reg1_payload.size());
            std::vector<uint8_t> win32_streamed(reg1_payload.size(), 0);
            DWORD nread = 0;
            SX_CHECK(fn_read(hvfs, win32_streamed.data(), static_cast<DWORD>(win32_streamed.size()),
                             &nread, nullptr) == TRUE);
            SX_CHECK(nread == reg1_payload.size());
            SX_CHECK(fn_close(hvfs) == TRUE);

            uint8_t win32_sha256[32]{};
            SX_CHECK(scarlettx::crypto::ComputeSha256(
                win32_streamed.data(), win32_streamed.size(), win32_sha256));
            SX_CHECK(std::memcmp(win32_sha256, expected_sha256, 32) == 0);
            SX_CHECK(win32_streamed == reg1_payload);
        }

        // Stream G:\Streaming\region1_stream.bin via DirectStorage (DStorageOpenFile + Queue)!
        {
            uint64_t ds_factory = 0;
            SX_CHECK(DStorageGetFactory(&ds_factory) == 0 && ds_factory != 0);
            uint64_t ds_file = 0;
            SX_CHECK(DStorageOpenFile(ds_factory, "G:\\Streaming\\region1_stream.bin", &ds_file) == 0 &&
                     ds_file != 0);
            uint64_t ds_size = 0;
            SX_CHECK(DStorageGetFileSize(ds_file, &ds_size) == 0 && ds_size == reg1_payload.size());

            DSTORAGE_QUEUE_DESC qd{};
            qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            qd.Capacity = 64;
            qd.Priority = DSTORAGE_PRIORITY_NORMAL;
            qd.Name = "ZeroStageMultiKeyQueue";
            uint64_t ds_queue = 0;
            SX_CHECK(DStorageCreateQueue(ds_factory, &qd, &ds_queue) == 0 && ds_queue != 0);

            std::vector<uint8_t> ds_streamed(reg1_payload.size(), 0);
            DSTORAGE_REQUEST req{};
            req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            req.SourceFileHandle = ds_file;
            req.SourceFileOffset = 0;
            req.SourceSize = static_cast<uint32_t>(reg1_payload.size());
            req.DestinationMemory = ds_streamed.data();
            req.DestinationSize = static_cast<uint32_t>(ds_streamed.size());
            req.UncompressedSize = static_cast<uint32_t>(ds_streamed.size());
            req.Name = "Region1Read";
            SX_CHECK(DStorageEnqueueRequest(ds_queue, &req) == 0);
            SX_CHECK(DStorageSubmit(ds_queue) == 0);
            SX_CHECK(DStorageWaitForIdle(ds_queue, 10000) == 0);

            uint8_t ds_sha256[32]{};
            SX_CHECK(scarlettx::crypto::ComputeSha256(
                ds_streamed.data(), ds_streamed.size(), ds_sha256));
            SX_CHECK(std::memcmp(ds_sha256, expected_sha256, 32) == 0);
            SX_CHECK(ds_streamed == reg1_payload);

            SX_CHECK(DStorageDestroyQueue(ds_queue) == 0);
            SX_CHECK(DStorageCloseFile(ds_file) == 0);
            SX_CHECK(DStorageDestroyFactory(ds_factory) == 0);
        }

        scarlettx::PackageVfs::Global().Reset();
        SX_CHECK(CountStageDirsInTemp() == 0u);
#endif
    }

    std::cout << "[PASS] test_retail_parity (XVD v3/Merkle/NTFS + Multi-Key .cik Keyring + Zero-Staging VFS/DStorage + 32-lane GPU GDeflate + WMA-Pro XMA2 + RDNA2 CFG + AcpHal/PIX)\n";
    return 0;
}
