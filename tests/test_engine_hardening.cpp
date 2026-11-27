#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/core/flight_triage.h"
#include "scarlettx/gpu/xs2_abi.h"
#include "scarlettx/kernel/kernelx_abi.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/media/codecs_abi.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[test_engine_hardening] FAILED at line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(x) Check((x), #x, __LINE__)

void SetEnvVar(const char* name, const char* val) {
#if defined(_WIN32)
    _putenv_s(name, val ? val : "");
#else
    if (val && val[0] != '\0') setenv(name, val, 1);
    else unsetenv(name);
#endif
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "scarlettx_test_engine_hardening";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // 4E.1: Versioned shader disk cache + corruption auto-eviction + rootSigHash key separation
    {
        const fs::path cache_dir = base / "shader_cache";
        fs::create_directories(cache_dir, ec);
        SetEnvVar("SCARLETTX_SHADER_CACHE_DIR", cache_dir.string().c_str());

        XS2_PIPELINE_DESC desc{};
        desc.maxCacheEntries = 64;
        XS2_PIPELINE_HANDLE pipe = nullptr;
        CHECK(Xs2ShaderCreateTranslationPipeline(&desc, &pipe) == 0 && pipe != nullptr);

        // Minimal valid RDNA2 s_endpgm shader
        const uint32_t shader_words[4] = {0xBF810000u, 0xBF800000u, 0xBF810000u, 0x00000000u};
        void* bc1 = nullptr;
        size_t sz1 = 0;
        CHECK(Xs2ShaderTranslateCachedEx(pipe, shader_words, sizeof(shader_words), 0x1111222233334444ull, &bc1, &sz1) == 0);
        CHECK(bc1 != nullptr && sz1 > 0);
        delete[] static_cast<uint8_t*>(bc1);

        // Different rootSigHash produces a distinct versioned .sxc cache file
        void* bc2 = nullptr;
        size_t sz2 = 0;
        CHECK(Xs2ShaderTranslateCachedEx(pipe, shader_words, sizeof(shader_words), 0xAAAABBBBCCCCDDDDull, &bc2, &sz2) == 0);
        CHECK(bc2 != nullptr && sz2 > 0);
        delete[] static_cast<uint8_t*>(bc2);

        size_t sxc_count = 0;
        fs::path first_sxc;
        for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
            if (entry.path().extension() == ".sxc") {
                ++sxc_count;
                if (first_sxc.empty()) first_sxc = entry.path();
            }
        }
        CHECK(sxc_count == 2);

        // Corrupt the first .sxc file and verify auto-eviction + clean re-translation on cache clear
        if (!first_sxc.empty()) {
            std::ofstream ofs(first_sxc, std::ios::binary | std::ios::trunc);
            ofs << "CORRUPT_STALE_CACHE_BYTES_0123456789";
        }
        CHECK(Xs2ShaderClearPipelineCache(pipe) == 0);
        void* bc3 = nullptr;
        size_t sz3 = 0;
        CHECK(Xs2ShaderTranslateCachedEx(pipe, shader_words, sizeof(shader_words), 0x1111222233334444ull, &bc3, &sz3) == 0);
        CHECK(bc3 != nullptr && sz3 == sz1);
        delete[] static_cast<uint8_t*>(bc3);

        CHECK(Xs2ShaderDestroyTranslationPipeline(pipe) == 0);
        SetEnvVar("SCARLETTX_SHADER_CACHE_DIR", "");
    }

    // 4E.2: Multi-entry 48B binary .cik keyring & LoadKeyringDirectory
    {
        const fs::path ring_dir = base / "keyrings";
        fs::create_directories(ring_dir, ec);

        // 2x 48-byte records in one .cik file (96 bytes)
        std::vector<uint8_t> multi_cik(96, 0x3Au);
        for (size_t i = 0; i < 32; ++i) {
            multi_cik[16 + i] = static_cast<uint8_t>(0x10u + i);
            multi_cik[48 + 16 + i] = static_cast<uint8_t>(0x50u + i);
        }
        {
            std::ofstream ofs(ring_dir / "01_base_and_dlc.cik", std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(multi_cik.data()), static_cast<std::streamsize>(multi_cik.size()));
        }
        // Single 32-byte raw .key file
        std::vector<uint8_t> extra_key(32, 0x7Cu);
        {
            std::ofstream ofs(ring_dir / "02_patch.key", std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(extra_key.data()), static_cast<std::streamsize>(extra_key.size()));
        }

        scarlettx::XvdContainer xvd1;
        CHECK(xvd1.LoadVolumeKeyFile((ring_dir / "01_base_and_dlc.cik").string()).ok());
        CHECK(xvd1.CikKeyCount() == 2);

        scarlettx::XvdContainer xvd2;
        size_t loaded = 0;
        CHECK(xvd2.LoadKeyringDirectory(ring_dir.string(), &loaded).ok());
        CHECK(loaded == 3 && xvd2.CikKeyCount() == 3);
    }

    // 4E.3: KernelX VirtualAlloc2 alias churn telemetry
    {
        KERNELX_ALIAS_TELEMETRY before{};
        CHECK(KernelXQueryAliasTelemetry(&before) == 0);

        void* base_mem = nullptr;
        const uint64_t alloc_sz = 128ull * 1024ull;
        uint64_t gpu_va = 0;
        CHECK(KernelXAllocateMemory(alloc_sz, KERNELX_PAGE_64KB, KERNELX_MEM_GPU_VISIBLE, &base_mem, &gpu_va) == 0 && base_mem != nullptr);

        void* alias_va = nullptr;
        CHECK(KernelXCreateMemoryAlias(base_mem, 0, 65536ull, &alias_va) == 0 && alias_va != nullptr);

        KERNELX_ALIAS_TELEMETRY during{};
        CHECK(KernelXQueryAliasTelemetry(&during) == 0);
        CHECK(during.totalAliasesCreated == before.totalAliasesCreated + 1);
        CHECK(during.activeAliases == before.activeAliases + 1);
        CHECK(during.peakAliases >= during.activeAliases);
        CHECK(during.activeAliasedBytes >= 65536ull);

        CHECK(KernelXFreeMemoryAlias(alias_va) == 0);
        KERNELX_ALIAS_TELEMETRY after{};
        CHECK(KernelXQueryAliasTelemetry(&after) == 0);
        CHECK(after.totalAliasesFreed == before.totalAliasesFreed + 1);
        CHECK(after.activeAliases == before.activeAliases);
        CHECK(KernelXFreeMemory(base_mem) == 0);
    }

    // 4E.4: Optional Windows Media Foundation H.264/HEVC runtime probe
    {
        MF_MEDIA_CAPABILITIES caps{};
        CHECK(XboxCodecProbeMediaFoundation(&caps) == 0);
        CHECK(caps.backendName[0] != '\0');
    }

    // 4E.5: p95 / p99 frame-time & stutter profiler in FlightRecorder + scarlettx triage
    {
        const fs::path flight_dir = base / "flight_pacing";
        fs::create_directories(flight_dir, ec);

        scarlettx::FlightRecorderOptions opts;
        opts.output_dir = flight_dir.string();
        opts.hang_timeout_ms = 0;
        scarlettx::FlightRecorder::Instance().Enable(opts);

        for (int i = 0; i < 94; ++i) {
            scarlettx::FlightRecorder::Instance().RecordFrameTimeUs(16600ull);  // 16.6 ms
        }
        for (int i = 0; i < 5; ++i) {
            scarlettx::FlightRecorder::Instance().RecordFrameTimeUs(25000ull);  // 25.0 ms
        }
        scarlettx::FlightRecorder::Instance().RecordFrameTimeUs(48000ull);      // 48.0 ms stutter!

        const auto fp = scarlettx::FlightRecorder::Instance().GetFramePacingSummary();
        CHECK(fp.frames_recorded == 100);
        CHECK(fp.stutter_count == 1);
        CHECK(fp.p50_ms >= 16.0 && fp.p50_ms <= 17.0);
        CHECK(fp.p95_ms >= 24.0 && fp.p95_ms <= 26.0);
        CHECK(fp.max_ms >= 47.9 && fp.max_ms <= 48.1);

        CHECK(scarlettx::FlightRecorder::Instance().WriteSummaryReport());
        scarlettx::FlightRecorder::Instance().Disable();

        scarlettx::FlightTriageReport rep;
        CHECK(scarlettx::TriageFlightDirectory(flight_dir.string(), rep).ok());
        CHECK(rep.verdict == "CLEAN_RUN");
        CHECK(rep.frames_recorded == 100);
        CHECK(rep.stutter_count == 1);
        CHECK(rep.max_ms >= 47.9);
        const std::string txt = scarlettx::FormatFlightTriageText(rep, false);
        CHECK(txt.find("Frame Pacing:") != std::string::npos);
        CHECK(txt.find("stutters=1") != std::string::npos);
    }

    fs::remove_all(base, ec);

    if (g_failures != 0) {
        std::cerr << "[test_engine_hardening] " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[test_engine_hardening] PASS (all 5 Track 4E milestones verified)\n";
    return 0;
}
