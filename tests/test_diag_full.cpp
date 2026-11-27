// Verification test for `scarlettx diag --full --json` and `--full` checklist output.

#include "sx_check.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
struct ProcessOutput {
    bool launched = false;
    DWORD exit_code = 1;
    std::string stdout_text;
};

ProcessOutput RunAndCaptureStdout(const std::filesystem::path& exe_path, const std::string& args_tail) {
    ProcessOutput result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        return result;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_handle = CreateFileA(
        "NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = (nul_handle != INVALID_HANDLE_VALUE) ? nul_handle : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe_path.string() + "\" " + args_tail;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        if (nul_handle != INVALID_HANDLE_VALUE) CloseHandle(nul_handle);
        return result;
    }

    result.launched = true;
    CloseHandle(stdout_write);
    if (nul_handle != INVALID_HANDLE_VALUE) CloseHandle(nul_handle);

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(stdout_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        result.stdout_text.append(buf, bytes_read);
    }
    CloseHandle(stdout_read);

    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

std::string TrimWhitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}
#endif

}  // namespace

int main() {
#if !defined(_WIN32)
    std::cout << "[SKIP] test_diag_full requires Windows x64\n";
    return 0;
#else
    const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
    const std::filesystem::path scarlettx_exe = bin_dir / "scarlettx.exe";
    SX_CHECK(std::filesystem::exists(scarlettx_exe));

    // 1. Validate `scarlettx diag --full --json`
    {
        const auto res = RunAndCaptureStdout(scarlettx_exe, "diag --full --json");
        SX_CHECK(res.launched);
        SX_CHECK(res.exit_code == 0);

        const std::string json = TrimWhitespace(res.stdout_text);
        SX_CHECK(!json.empty());
        SX_CHECK(json.front() == '{');
        SX_CHECK(json.back() == '}');

        const std::vector<std::string> required_keys = {
            "\"adapter_name\"",
            "\"adapter_type\"",
            "\"is_warp\"",
            "\"d3d12_device5\"",
            "\"highest_shader_model\"",
            "\"shader_model_6_6_plus\"",
            "\"dxr_tier\"",
            "\"dxr_tier_name\"",
            "\"mesh_shader_tier\"",
            "\"mesh_shader_tier_name\"",
            "\"vulkan_dll_present\"",
            "\"vulkan_1_3_ready\"",
            "\"vulkan_api_version\"",
            "\"virtual_alloc2_available\"",
            "\"media_foundation_ready\"",
            "\"mfplat_present\"",
            "\"mfreadwrite_present\"",
            "\"xaudio2_host_ready\"",
            "\"xaudio2_host_dll\""
        };
        for (const auto& key : required_keys) {
            SX_CHECK(json.find(key) != std::string::npos);
        }

        // Basic structural check: balanced top-level braces outside strings
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (char c : json) {
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else {
                if (c == '"') {
                    in_string = true;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    SX_CHECK(depth >= 0);
                }
            }
        }
        SX_CHECK(!in_string);
        SX_CHECK(depth == 0);
    }

    // 2. Validate `scarlettx diag --full` human-readable checklist
    {
        const auto res = RunAndCaptureStdout(scarlettx_exe, "diag --full");
        SX_CHECK(res.launched);
        SX_CHECK(res.exit_code == 0);
        SX_CHECK(res.stdout_text.find("Host GPU Adapter") != std::string::npos);
        SX_CHECK(res.stdout_text.find("Highest Shader Model") != std::string::npos);
        SX_CHECK(res.stdout_text.find("DXR Tier") != std::string::npos);
        SX_CHECK(res.stdout_text.find("Mesh Shader Tier") != std::string::npos);
        SX_CHECK(res.stdout_text.find("Vulkan 1.3") != std::string::npos);
        SX_CHECK(res.stdout_text.find("VirtualAlloc2") != std::string::npos);
        SX_CHECK(res.stdout_text.find("Media Foundation") != std::string::npos);
        SX_CHECK(res.stdout_text.find("XAudio2 Host Audio") != std::string::npos);
    }

    // 3. Validate backwards-compatible `scarlettx diag`
    {
        const auto res = RunAndCaptureStdout(scarlettx_exe, "diag");
        SX_CHECK(res.launched);
        SX_CHECK(res.exit_code == 0);
        SX_CHECK(res.stdout_text.find("[diag] Adapter:") != std::string::npos);
    }

    std::cout << "[PASS] test_diag_full (diag --full --json, checklist, and legacy mode verified)\n";
    return 0;
#endif
}
