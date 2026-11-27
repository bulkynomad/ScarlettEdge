#include "scarlettx/runtime/virtual_drive.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace scarlettx {
namespace vfs {

#if defined(_WIN32)

namespace {

// Fixed buffers: the crash path must not allocate.
char g_device[3] = {0, ':', 0};
char g_target[MAX_PATH + 1] = {};
char g_marker[MAX_PATH + 32] = {};
std::atomic<bool> g_mapped{false};
std::atomic<bool> g_handlers_installed{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

void NormalizeDosPath(const char* src, char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        for (; src[i] != '\0' && src[i] != '\r' && src[i] != '\n' && i + 1 < dst_cap; ++i) {
            dst[i] = (src[i] == '/') ? '\\' : src[i];
        }
    }
    while (i > 3 && dst[i - 1] == '\\') {
        --i;
    }
    dst[i] = '\0';
}

bool MarkerPath(char letter, char* out, size_t out_size) {
    char temp[MAX_PATH + 1] = {};
    const DWORD len = GetTempPathA(MAX_PATH, temp);
    if (len == 0 || len > MAX_PATH) {
        return false;
    }
    return std::snprintf(out, out_size, "%sscarlettx_drive_%c.txt", temp, letter) > 0;
}

bool SamePath(const char* a, const char* b) {
    char norm_a[MAX_PATH + 16] = {};
    char norm_b[MAX_PATH + 16] = {};
    NormalizeDosPath(a, norm_a, sizeof(norm_a));
    NormalizeDosPath(b, norm_b, sizeof(norm_b));
    return _stricmp(norm_a, norm_b) == 0;
}

// QueryDosDevice reports "<4-character NT prefix>C:<path>".
bool DriveTargets(const char* device, const char* target) {
    char current[MAX_PATH + 16] = {};
    if (QueryDosDeviceA(device, current, sizeof(current) - 1) == 0) {
        return false;
    }
    const char* path = current;
    if (std::strlen(current) > 4 && current[1] == '?' && current[2] == '?') {
        path = current + 4;
    }
    return SamePath(path, target);
}

// Serialises recovery and mapping of one drive letter across processes, so a
// process cannot act on a marker another one is about to replace. Recursive
// for the owning thread.
class DriveLock {
public:
    explicit DriveLock(char letter) {
        char name[40] = {};
        std::snprintf(name, sizeof(name), "ScarlettX_PackageDrive_%c", letter);
        handle_ = CreateMutexA(nullptr, FALSE, name);
        if (handle_) {
            const DWORD wait = WaitForSingleObject(handle_, 10000);
            owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~DriveLock() {
        if (handle_) {
            if (owned_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
    }
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

private:
    HANDLE handle_ = nullptr;
    bool owned_ = false;
};

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    UnmapPackageDrive();
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI ConsoleHandler(DWORD) {
    UnmapPackageDrive();
    return FALSE;
}

}  // namespace

bool RecoverStaleDrive(char letter) {
    DriveLock lock(letter);
    char marker[MAX_PATH + 32] = {};
    if (!MarkerPath(letter, marker, sizeof(marker))) {
        return false;
    }
    std::FILE* file = std::fopen(marker, "rb");
    if (!file) {
        return false;
    }
    unsigned long pid = 0;
    char raw_target[MAX_PATH + 1] = {};
    const bool parsed = std::fscanf(file, "%lu", &pid) == 1 && std::fgetc(file) == 10 &&
                        std::fgets(raw_target, sizeof(raw_target), file) != nullptr;
    std::fclose(file);
    if (!parsed) {
        DeleteFileA(marker);
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return false;
    }
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        if (alive) {
            return false;  // another instance owns the mapping
        }
    }

    char norm_target[MAX_PATH + 1] = {};
    NormalizeDosPath(raw_target, norm_target, sizeof(norm_target));
    if (norm_target[0] == '\0') {
        DeleteFileA(marker);
        return false;
    }

    const char device[3] = {letter, ':', 0};
    bool removed = false;
    if (DriveTargets(device, norm_target)) {
        removed = DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, device,
                                   norm_target) != 0;
        if (!removed && std::strcmp(norm_target, raw_target) != 0) {
            removed = DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, device,
                                       raw_target) != 0;
        }
    }
    if (removed || !DriveTargets(device, norm_target)) {
        DeleteFileA(marker);
    }
    return removed;
}

bool MapPackageDrive(char letter, const std::string& target) {
    if (g_mapped.load() || target.empty() || target.size() > MAX_PATH || letter < 'A' ||
        letter > 'Z') {
        return false;
    }
    char norm_target[MAX_PATH + 1] = {};
    NormalizeDosPath(target.c_str(), norm_target, sizeof(norm_target));
    if (norm_target[0] == '\0') {
        return false;
    }

    DriveLock lock(letter);
    RecoverStaleDrive(letter);
    if (GetLogicalDrives() & (1u << (letter - 'A'))) {
        return false;
    }
    const char device[3] = {letter, ':', 0};
    if (!DefineDosDeviceA(0, device, norm_target)) {
        return false;
    }
    g_device[0] = letter;
    std::memcpy(g_target, norm_target, std::strlen(norm_target) + 1);
    g_marker[0] = 0;
    if (MarkerPath(letter, g_marker, sizeof(g_marker))) {
        if (std::FILE* file = std::fopen(g_marker, "wb")) {
            std::fprintf(file, "%lu%c%s", static_cast<unsigned long>(GetCurrentProcessId()), 10,
                         g_target);
            std::fclose(file);
        } else {
            g_marker[0] = 0;
        }
    }
    g_mapped.store(true);
    if (!g_handlers_installed.exchange(true)) {
        g_previous_filter = SetUnhandledExceptionFilter(&CrashFilter);
        SetConsoleCtrlHandler(&ConsoleHandler, TRUE);
        std::atexit(&UnmapPackageDrive);
    }
    return true;
}

void UnmapPackageDrive() {
    if (!g_mapped.exchange(false)) {
        return;
    }
    DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, g_device, g_target);
    if (g_marker[0]) {
        DeleteFileA(g_marker);
    }
}

#else  // !_WIN32

bool MapPackageDrive(char, const std::string&) { return false; }
void UnmapPackageDrive() {}
bool RecoverStaleDrive(char) { return false; }

#endif

}  // namespace vfs
}  // namespace scarlettx
