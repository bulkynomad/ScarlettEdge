#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#endif

// Always-on test check. Unlike assert(), the condition is evaluated and
// enforced in Release (NDEBUG) builds as well.
#ifndef SX_REQUIRE
#define SX_REQUIRE(cond)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                  \
            std::fflush(stderr);                                                  \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)
#endif

#ifndef SX_CHECK
#define SX_CHECK(cond) SX_REQUIRE(cond)
#endif

namespace sx_test {

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* tag) {
        static std::atomic<uint32_t> s_seq{0};
        uint64_t pid = 0;
#if defined(_WIN32)
        pid = static_cast<uint64_t>(GetCurrentProcessId());
#endif
        std::error_code ec;
        path_ = std::filesystem::temp_directory_path(ec) /
                (std::string("scarlettx_") + (tag ? tag : "test") + "_" +
                 std::to_string(pid) + "_" + std::to_string(s_seq.fetch_add(1)));
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace sx_test
