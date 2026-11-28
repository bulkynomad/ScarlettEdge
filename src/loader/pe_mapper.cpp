#include "scarlettx/loader/pe_mapper.h"

#include "guest_modules.h"
#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

#if defined(_WIN32)
// Host-process registrations made for a mapped image; undone in Unmap().
struct LoaderState {
    guest::TlsBinding tls;
    PRUNTIME_FUNCTION function_table = nullptr;
    uint8_t* registered_base = nullptr;
};
#endif

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string MakeNamedKey(const std::string& dll, const std::string& sym) {
    return ToLowerAscii(dll) + "!" + sym;
}

std::string MakeOrdinalKey(const std::string& dll, uint16_t ordinal) {
    return ToLowerAscii(dll) + "!#" + std::to_string(ordinal);
}

#if defined(_WIN32)
constexpr DWORD kGuestExitExceptionCode = 0xE0535845u;
constexpr uint64_t kDefaultSecurityCookie = 0x00002B992DDFA232ull;

thread_local uint32_t g_guest_entry_depth = 0;
thread_local bool g_guest_exit_called = false;
thread_local int g_guest_exit_code = 0;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int len = MultiByteToWideChar(cp, flags, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        cp = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(cp, flags, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    }
    if (len <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(cp, flags, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const wchar_t* ws) {
    if (!ws || !*ws) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
    return out;
}

using GuestAtExitFn = void(__cdecl*)();

std::mutex& GuestAtExitMutex() {
    static std::mutex m;
    return m;
}

std::vector<GuestAtExitFn>& GuestAtExitTable() {
    static std::vector<GuestAtExitFn> table;
    return table;
}

int __cdecl GuestIsolatedAtExit(GuestAtExitFn fn) {
    if (!fn) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(GuestAtExitMutex());
    GuestAtExitTable().push_back(fn);
    return 0;
}

using GuestTlsAtExitCallback = void(__stdcall*)(void*, unsigned long, void*);

void __cdecl GuestRegisterThreadLocalExeAtExit(GuestTlsAtExitCallback /*cb*/) {
    // Isolate guest mainCRTStartup TLS destructor registration from host ucrtbase.dll's
    // single process-wide slot (which otherwise __fastfail's on repeated boots or calls
    // into unmapped guest PE memory when the host runner process exits).
}

void __cdecl GuestNoopCexit() {
    // Prevent guest CRT shutdown from tearing down shared host ucrtbase stdio/heap state.
}

void RunAndClearGuestAtExitInRange(const uint8_t* base, size_t size) {
    if (!base || size == 0) {
        return;
    }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    const uintptr_t hi = lo + size;
    std::vector<GuestAtExitFn> to_run;
    {
        std::lock_guard<std::mutex> lock(GuestAtExitMutex());
        auto& table = GuestAtExitTable();
        for (auto it = table.rbegin(); it != table.rend();) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(*it);
            if (addr >= lo && addr < hi) {
                to_run.push_back(*it);
                it = std::vector<GuestAtExitFn>::reverse_iterator(table.erase(std::next(it).base()));
            } else {
                ++it;
            }
        }
    }
    for (GuestAtExitFn fn : to_run) {
        fn();
    }
}

struct GuestFlsEntry {
    DWORD index = FLS_OUT_OF_INDEXES;
    PFLS_CALLBACK_FUNCTION callback = nullptr;
};

std::mutex& GuestFlsMutex() {
    static std::mutex m;
    return m;
}

std::vector<GuestFlsEntry>& GuestFlsTable() {
    static std::vector<GuestFlsEntry> table;
    return table;
}

DWORD WINAPI GuestFlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback) {
    const DWORD idx = ::FlsAlloc(lpCallback);
    if (idx != FLS_OUT_OF_INDEXES && lpCallback != nullptr) {
        std::lock_guard<std::mutex> lock(GuestFlsMutex());
        GuestFlsTable().push_back({idx, lpCallback});
    }
    return idx;
}

BOOL WINAPI GuestFlsFree(DWORD dwFlsIndex) {
    {
        std::lock_guard<std::mutex> lock(GuestFlsMutex());
        auto& table = GuestFlsTable();
        for (auto it = table.begin(); it != table.end();) {
            if (it->index == dwFlsIndex) {
                it = table.erase(it);
            } else {
                ++it;
            }
        }
    }
    return ::FlsFree(dwFlsIndex);
}

PVOID WINAPI GuestFlsGetValue(DWORD dwFlsIndex) {
    return ::FlsGetValue(dwFlsIndex);
}

BOOL WINAPI GuestFlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData) {
    return ::FlsSetValue(dwFlsIndex, lpFlsData);
}

struct GuestGraphicsRegion {
    uintptr_t base = 0;
    size_t size = 0;
    DWORD raw_flags = 0;
};

std::shared_mutex& GuestGraphicsMutex() {
    static std::shared_mutex m;
    return m;
}

std::unordered_map<uintptr_t, GuestGraphicsRegion>& GuestGraphicsTable() {
    static std::unordered_map<uintptr_t, GuestGraphicsRegion> t;
    return t;
}

void NotifyKernelXExternalAlloc(void* va, size_t size, bool register_alloc) {
    if (!va) return;
    HMODULE kx = ::GetModuleHandleW(L"kernelx.dll");
    if (!kx) return;
    if (register_alloc) {
        if (size == 0) return;
        FARPROC proc = ::GetProcAddress(kx, "KernelXRegisterExternalAllocation");
        if (!proc) return;
        using RegFn = int32_t(__stdcall*)(void*, uint64_t, uint64_t*);
        uint64_t gpu_va = 0;
        (void)reinterpret_cast<RegFn>(reinterpret_cast<void*>(proc))(
            va, static_cast<uint64_t>(size), &gpu_va);
    } else {
        FARPROC proc = ::GetProcAddress(kx, "KernelXUnregisterExternalAllocation");
        if (!proc) return;
        using UnregFn = int32_t(__stdcall*)(void*, uint64_t);
        (void)reinterpret_cast<UnregFn>(reinterpret_cast<void*>(proc))(
            va, static_cast<uint64_t>(size));
    }
}

LPVOID WINAPI GuestVirtualAllocEx(HANDLE hProcess,
                                  LPVOID lpAddress,
                                  SIZE_T dwSize,
                                  DWORD flAllocationType,
                                  DWORD flProtect) {
    constexpr DWORD kXboxMemGraphics = 0x04000000u;
    constexpr DWORD kXboxPage64kMask = 0x20400000u;
    constexpr DWORD kXboxConsoleBits = kXboxMemGraphics | 0x00400000u | 0x20000000u | 0x40000000u;

    if ((flAllocationType & kXboxConsoleBits) == 0u) {
        return ::VirtualAllocEx(hProcess, lpAddress, dwSize, flAllocationType, flProtect);
    }
    if (dwSize == 0) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    size_t page = 0x1000u;
    if ((flAllocationType & kXboxPage64kMask) == kXboxPage64kMask ||
        (flAllocationType & 0x00400000u) != 0u) {
        page = 0x10000u;
    } else if ((flAllocationType & 0x20000000u) != 0u ||
               (flAllocationType & 0x40000000u) != 0u) {
        page = 0x200000u;
    }

    constexpr DWORD kValidWin32AllocFlags =
        MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET_UNDO;
    DWORD clean_alloc = flAllocationType & kValidWin32AllocFlags;
    if (clean_alloc == 0u) {
        clean_alloc = MEM_RESERVE | MEM_COMMIT;
    }
    if (!lpAddress && (clean_alloc & MEM_COMMIT) != 0u) {
        clean_alloc |= MEM_RESERVE;
    }

    DWORD clean_protect = flProtect & 0x7FFu;
    if ((clean_protect & 0xFFu) == 0u) {
        clean_protect = (clean_alloc == MEM_RESERVE) ? PAGE_NOACCESS : PAGE_READWRITE;
    }

    const size_t aligned_size = (static_cast<size_t>(dwSize) + page - 1u) & ~(page - 1u);
    void* result = nullptr;
    if (!lpAddress && page > 0x10000u &&
        (hProcess == ::GetCurrentProcess() || hProcess == reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))) {
        struct Win32AddrReq {
            PVOID lowest;
            PVOID highest;
            SIZE_T alignment;
        };
        struct alignas(8) Win32ExtParam {
            ULONG64 type;
            PVOID ptr;
        };
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        static const VirtualAlloc2Fn va2 = []() -> VirtualAlloc2Fn {
            HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
            return kb ? reinterpret_cast<VirtualAlloc2Fn>(
                            reinterpret_cast<void*>(::GetProcAddress(kb, "VirtualAlloc2")))
                      : nullptr;
        }();
        if (va2) {
            Win32AddrReq req{nullptr, nullptr, static_cast<SIZE_T>(page)};
            Win32ExtParam ext{1ull, &req};
            result = va2(hProcess, nullptr, aligned_size, clean_alloc, clean_protect, &ext, 1u);
        }
    }
    if (!result) {
        result = ::VirtualAllocEx(hProcess, lpAddress, aligned_size, clean_alloc, clean_protect);
    }
    if (result && (flAllocationType & kXboxMemGraphics) != 0u && (clean_alloc & MEM_RESERVE) != 0u) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(result);
        {
            std::unique_lock<std::shared_mutex> lk(GuestGraphicsMutex());
            GuestGraphicsTable()[base] = GuestGraphicsRegion{base, aligned_size, flAllocationType};
        }
        NotifyKernelXExternalAlloc(result, aligned_size, true);
    }
    return result;
}

LPVOID WINAPI GuestVirtualAlloc(LPVOID lpAddress,
                                SIZE_T dwSize,
                                DWORD flAllocationType,
                                DWORD flProtect) {
    return GuestVirtualAllocEx(::GetCurrentProcess(), lpAddress, dwSize, flAllocationType, flProtect);
}

BOOL WINAPI GuestVirtualFreeEx(HANDLE hProcess,
                               LPVOID lpAddress,
                               SIZE_T dwSize,
                               DWORD dwFreeType) {
    size_t tracked_size = 0;
    if (lpAddress && (dwFreeType & MEM_RELEASE) != 0u) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(lpAddress);
        std::unique_lock<std::shared_mutex> lk(GuestGraphicsMutex());
        auto it = GuestGraphicsTable().find(base);
        if (it != GuestGraphicsTable().end()) {
            tracked_size = it->second.size;
            GuestGraphicsTable().erase(it);
        }
    }
    const BOOL ok = ::VirtualFreeEx(hProcess, lpAddress, (dwFreeType & MEM_RELEASE) ? 0 : dwSize, dwFreeType);
    if (ok && tracked_size > 0) {
        NotifyKernelXExternalAlloc(lpAddress, tracked_size, false);
    }
    return ok;
}

BOOL WINAPI GuestVirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    return GuestVirtualFreeEx(::GetCurrentProcess(), lpAddress, dwSize, dwFreeType);
}

void FreeGuestFlsInRange(const uint8_t* base, size_t size) {
    if (!base || size == 0) {
        return;
    }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    const uintptr_t hi = lo + size;
    std::vector<DWORD> to_free;
    {
        std::lock_guard<std::mutex> lock(GuestFlsMutex());
        auto& table = GuestFlsTable();
        for (auto it = table.begin(); it != table.end();) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(it->callback);
            if (addr >= lo && addr < hi) {
                to_free.push_back(it->index);
                it = table.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Free guest FLS slots while the guest PE image is still mapped so their
    // callbacks run safely and ntdll never jumps into unmapped memory on host exit.
    for (DWORD idx : to_free) {
        ::FlsFree(idx);
    }
}

std::mutex& GuestLoaderPathsMutex() {
    static std::mutex m;
    return m;
}

std::string& GuestShimDirectoryStorage() {
    static std::string dir;
    return dir;
}

std::string& GuestPackageDirectoryStorage() {
    static std::string dir;
    return dir;
}

std::string GetGuestShimDirectory() {
    std::lock_guard<std::mutex> lk(GuestLoaderPathsMutex());
    return GuestShimDirectoryStorage();
}

void SetGuestShimDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(GuestLoaderPathsMutex());
    GuestShimDirectoryStorage() = dir;
}

std::string GetGuestPackageDirectory() {
    std::lock_guard<std::mutex> lk(GuestLoaderPathsMutex());
    return GuestPackageDirectoryStorage();
}

void SetGuestPackageDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(GuestLoaderPathsMutex());
    GuestPackageDirectoryStorage() = dir;
}

// Persistent static buffers so guest pointers returned by GetCommandLineA/W
// remain valid for the entire lifetime of the process without leaking host argv.
char g_guest_cmdline_a[1024] = "\"G:\\Game.exe\"";
wchar_t g_guest_cmdline_w[1024] = L"\"G:\\Game.exe\"";

void SetGuestCommandLineForExe(const std::string& exe_basename) {
    if (exe_basename.empty()) {
        return;
    }
    const std::string cmd_a = "\"G:\\" + exe_basename + "\"";
    const std::wstring cmd_w = Utf8ToWide(cmd_a);
    std::lock_guard<std::mutex> lk(GuestLoaderPathsMutex());
    const size_t copy_a = std::min<size_t>(cmd_a.size(), sizeof(g_guest_cmdline_a) - 1u);
    std::memcpy(g_guest_cmdline_a, cmd_a.data(), copy_a);
    g_guest_cmdline_a[copy_a] = '\0';

    const size_t max_w = (sizeof(g_guest_cmdline_w) / sizeof(wchar_t)) - 1u;
    const size_t copy_w = std::min<size_t>(cmd_w.size(), max_w);
    if (copy_w > 0) {
        std::memcpy(g_guest_cmdline_w, cmd_w.data(), copy_w * sizeof(wchar_t));
    }
    g_guest_cmdline_w[copy_w] = L'\0';
}

LPSTR WINAPI GuestGetCommandLineA() {
    return g_guest_cmdline_a;
}

LPWSTR WINAPI GuestGetCommandLineW() {
    return g_guest_cmdline_w;
}

VOID WINAPI GuestExitProcess(UINT uExitCode) {
    if (g_guest_entry_depth > 0) {
        if (!g_guest_exit_called) {
            g_guest_exit_called = true;
            g_guest_exit_code = static_cast<int>(uExitCode);
        }
        const ULONG_PTR args[1] = {static_cast<ULONG_PTR>(uExitCode)};
        RaiseException(kGuestExitExceptionCode, EXCEPTION_NONCONTINUABLE, 1, args);
    }
    ::ExitProcess(uExitCode);
}

void __cdecl GuestCrtExit(int status) {
    if (g_guest_entry_depth > 0) {
        std::fflush(nullptr);
        GuestExitProcess(static_cast<UINT>(status));
    }
    std::exit(status);
}

void __cdecl GuestCrtImmediateExit(int status) {
    if (g_guest_entry_depth > 0) {
        GuestExitProcess(static_cast<UINT>(status));
    }
    std::_Exit(status);
}

int FilterGuestEntryException(unsigned long code, EXCEPTION_POINTERS* ep, unsigned long exit_magic) {
    if (code == exit_magic) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    if (FlightRecorder::IsEnabled()) {
        FlightRecorder::Instance().CaptureCrashReport(ep, "UnhandledGuestEntryException", nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int InvokeGuestFnCatchExit(int (*fn)()) {
    int rc = 0;
    __try {
        rc = fn();
    } __except (FilterGuestEntryException(GetExceptionCode(), GetExceptionInformation(), kGuestExitExceptionCode)) {
        rc = g_guest_exit_code;
    }
    return rc;
}

int InvokeGuestEntryGuarded(int (*fn)()) {
    ++g_guest_entry_depth;
    g_guest_exit_called = false;
    g_guest_exit_code = 0;
    int rc = 0;
    __try {
        rc = InvokeGuestFnCatchExit(fn);
    } __finally {
        --g_guest_entry_depth;
    }
    return g_guest_exit_called ? g_guest_exit_code : rc;
}

// OS / hardware system DLLs: always prioritize ScarlettEdge HLE shims over
// anything in the title directory or Windows System32.
const char* ResolveSystemShimTarget(const std::string& lower_name) {
    return LookupSystemShimTarget(lower_name);
}

// Title-shipped middleware & GDK C thunk DLLs: prioritize the real DLL inside the game
// package/VFS when present, falling back to ScarlettEdge HLE shims only when absent.
const char* ResolveFallbackMiddlewareTarget(const std::string& lower_name) {
    return LookupFallbackMiddlewareTarget(lower_name);
}

bool IsCoreHostWindowsDll(const std::string& lower_base) {
    if (StartsWith(lower_base, "api-ms-win-") || StartsWith(lower_base, "ext-ms-win-")) {
        return true;
    }
    static constexpr const char* kCoreDlls[] = {
        "ntdll.dll",       "kernel32.dll",  "kernelbase.dll", "ucrtbase.dll",
        "msvcrt.dll",      "user32.dll",    "gdi32.dll",      "advapi32.dll",
        "combase.dll",     "ole32.dll",     "oleaut32.dll",   "rpcrt4.dll",
        "ws2_32.dll",      "bcrypt.dll",    "crypt32.dll",    "shell32.dll",
        "shlwapi.dll",     "sechost.dll",   "imm32.dll",      "winmm.dll",
        "dxgi.dll",        "d3d12.dll",     "twinapi.appcore.dll",
    };
    for (const char* core : kCoreDlls) {
        if (lower_base == core) {
            return true;
        }
    }
    return false;
}

std::string ExtractBasenameLower(const std::string& path_str) {
    const size_t pos = path_str.find_last_of("\\/");
    const std::string base = (pos == std::string::npos) ? path_str : path_str.substr(pos + 1);
    return ToLowerAscii(base);
}

bool HasDotDotSegment(const std::string& path) {
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
            ++i;
        }
        const size_t start = i;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') {
            ++i;
        }
        if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') {
            return true;
        }
    }
    return false;
}

// Intercepts guest G:\ / D:\ / T:\ (and relative paths when package root is virtual G:\)
// and resolves them via PackageVfs::Global() first (streaming directly from mounted XvdContainers
// without %TEMP% extraction, or resolving to host mount / GuestPackageDirectory when directory-backed).
enum class GuestPathResult {
    NotGuestDrive,
    ResolvedHost,
    ResolvedXvdVfs,
    Blocked,
};

struct GuestResolvedPath {
    GuestPathResult kind = GuestPathResult::NotGuestDrive;
    std::string host_path;
    std::string vfs_path;
    VfsEntryInfo vfs_info{};
};

bool IsVirtualGDriveRoot(const std::string& dir) {
    if (dir.size() < 2 || dir[1] != ':') return false;
    const char d = static_cast<char>(std::toupper(static_cast<unsigned char>(dir[0])));
    if (d != 'G' && d != 'D') return false;
    for (size_t i = 2; i < dir.size(); ++i) {
        if (dir[i] != '\\' && dir[i] != '/') return false;
    }
    return true;
}

GuestResolvedPath ClassifyGuestPackagePath(const std::string& raw_path) {
    GuestResolvedPath res{};
    if (raw_path.empty()) {
        return res;
    }
    size_t prefix = 0;
    if (raw_path.size() >= 6 &&
        (raw_path[0] == '\\' || raw_path[0] == '/') &&
        (raw_path[1] == '\\' || raw_path[1] == '/') &&
        (raw_path[2] == '?' || raw_path[2] == '.') &&
        (raw_path[3] == '\\' || raw_path[3] == '/')) {
        prefix = 4;
    }

    bool explicit_guest_drive = false;
    char drive_upper = '\0';
    std::string vpath;
    if (raw_path.size() >= prefix + 2 && raw_path[prefix + 1] == ':') {
        const char drive = raw_path[prefix];
        drive_upper = static_cast<char>(std::toupper(static_cast<unsigned char>(drive)));
        if ((drive_upper == 'G' || drive_upper == 'T' || drive_upper == 'D') &&
            (raw_path.size() == prefix + 2 ||
             raw_path[prefix + 2] == '\\' || raw_path[prefix + 2] == '/')) {
            explicit_guest_drive = true;
            vpath = raw_path.substr(prefix);
        } else {
            return res;
        }
    } else if (prefix == 0 && raw_path[0] != '\\' && raw_path[0] != '/' &&
               raw_path.find(':') == std::string::npos &&
               IsVirtualGDriveRoot(GetGuestPackageDirectory())) {
        // In zero-staging XVD mode (GuestPackageDirectory == "G:\"), relative paths
        // resolve against G:\ in PackageVfs if present there!
        if (HasDotDotSegment(raw_path)) {
            return res;
        }
        const std::string cand_vpath = "G:\\" + raw_path;
        VfsEntryInfo vinfo{};
        if (PackageVfs::Global().Stat(cand_vpath, vinfo) && vinfo.exists && vinfo.is_xvd_backed) {
            res.kind = GuestPathResult::ResolvedXvdVfs;
            res.vfs_path = cand_vpath;
            res.vfs_info = std::move(vinfo);
            return res;
        }
        return res;
    } else {
        return res;
    }

    if (!explicit_guest_drive) {
        return res;
    }
    if (HasDotDotSegment(vpath)) {
        res.kind = GuestPathResult::Blocked;
        return res;
    }

    size_t rel_pos = 2;
    while (rel_pos < vpath.size() && (vpath[rel_pos] == '\\' || vpath[rel_pos] == '/')) {
        ++rel_pos;
    }
    const std::string rel = vpath.substr(rel_pos);

    // T:\ -> Title Scratch Drive (%TEMP%\scarlettx_scratch\<title_id> or SCARLETTX_SCRATCH_ROOT)
    if (drive_upper == 'T') {
        std::filesystem::path scratch_root;
        if (const char* env_scratch = std::getenv("SCARLETTX_SCRATCH_ROOT");
            env_scratch && env_scratch[0] != '\0') {
            scratch_root = std::filesystem::path(env_scratch);
        } else {
            std::error_code ec;
            scratch_root = std::filesystem::temp_directory_path(ec) / "scarlettx_scratch";
        }
        const char* tid = std::getenv("SCARLETTX_TITLE_ID");
        const std::string sub = (tid && tid[0] != '\0') ? std::string(tid) : std::string("default");
        const std::filesystem::path dir = scratch_root / sub;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        res.kind = GuestPathResult::ResolvedHost;
        res.host_path = rel.empty() ? dir.string() : (dir / rel).string();
        return res;
    }

    // G:\ and D:\ -> PackageVfs (XVD streaming first, then host directory mount, then GuestPackageDirectory)
    const std::string g_vpath = rel.empty() ? std::string("G:") : ("G:\\" + rel);
    res.vfs_path = g_vpath;
    if (PackageVfs::Global().Stat(g_vpath, res.vfs_info) && res.vfs_info.exists) {
        if (res.vfs_info.is_xvd_backed) {
            res.kind = GuestPathResult::ResolvedXvdVfs;
            return res;
        }
        if (!res.vfs_info.host_path.empty()) {
            res.kind = GuestPathResult::ResolvedHost;
            res.host_path = res.vfs_info.host_path;
            return res;
        }
    }

    Status st = PackageVfs::Global().ResolvePath(g_vpath, res.host_path);
    if (st.ok()) {
        res.kind = GuestPathResult::ResolvedHost;
        return res;
    }
    if (st.code == StatusCode::InvalidArgument) {
        res.kind = GuestPathResult::Blocked;
        return res;
    }

    const std::string pkg_dir = GetGuestPackageDirectory();
    if (!pkg_dir.empty() && !IsVirtualGDriveRoot(pkg_dir)) {
        res.kind = GuestPathResult::ResolvedHost;
        res.host_path = rel.empty() ? pkg_dir : (std::filesystem::path(pkg_dir) / rel).string();
        return res;
    }

    res.kind = GuestPathResult::Blocked;
    return res;
}

GuestPathResult RedirectGuestPackagePath(const std::string& raw_path, std::string& out_host_path) {
    out_host_path.clear();
    const GuestResolvedPath r = ClassifyGuestPackagePath(raw_path);
    if (r.kind == GuestPathResult::ResolvedHost) {
        out_host_path = r.host_path;
    }
    return r.kind;
}

void RecordGuestVfsResultPreservingLastError(const std::string& raw_vpath,
                                             bool ok,
                                             uint64_t caller_rip) {
    if (!FlightRecorder::IsEnabled()) {
        return;
    }
    const DWORD saved_err = ::GetLastError();
    FlightRecorder::Instance().RecordVfsAccess(
        raw_vpath.c_str(), ok, ok ? 0 : static_cast<int32_t>(saved_err), caller_rip);
    ::SetLastError(saved_err);
}

// ---------------------------------------------------------------------------
// Zero-Staging Win32 VFS Streaming Handles & Directory Enumeration State
// ---------------------------------------------------------------------------
struct GuestVfsFileState {
    std::string vfs_path;
    uint64_t size = 0;
    bool is_directory = false;
    std::mutex pos_mu;
    uint64_t position = 0;
};

struct GuestVfsFindState {
    std::mutex mu;
    std::vector<VfsDirEntry> matches;
    size_t cursor = 0;
};

std::atomic<size_t> g_active_vfs_files{0};
std::atomic<size_t> g_active_vfs_finds{0};

std::shared_mutex& GuestVfsHandlesMutex() {
    static std::shared_mutex m;
    return m;
}

std::unordered_map<HANDLE, std::shared_ptr<GuestVfsFileState>>& GuestVfsFileMap() {
    static std::unordered_map<HANDLE, std::shared_ptr<GuestVfsFileState>> m;
    return m;
}

std::unordered_map<HANDLE, std::shared_ptr<GuestVfsFindState>>& GuestVfsFindMap() {
    static std::unordered_map<HANDLE, std::shared_ptr<GuestVfsFindState>> m;
    return m;
}

std::shared_ptr<GuestVfsFileState> LookupVfsFileHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || g_active_vfs_files.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
    auto it = GuestVfsFileMap().find(h);
    return it == GuestVfsFileMap().end() ? nullptr : it->second;
}

std::shared_ptr<GuestVfsFindState> LookupVfsFindHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || g_active_vfs_finds.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
    auto it = GuestVfsFindMap().find(h);
    return it == GuestVfsFindMap().end() ? nullptr : it->second;
}

HANDLE OpenVirtualXvdFileHandle(const GuestResolvedPath& resolved,
                                DWORD dwDesiredAccess,
                                DWORD dwCreationDisposition,
                                DWORD dwFlagsAndAttributes) {
    constexpr DWORD kWriteAccessMask =
        GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
    if ((dwDesiredAccess & kWriteAccessMask) != 0u) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (dwCreationDisposition == CREATE_NEW ||
        dwCreationDisposition == CREATE_ALWAYS ||
        dwCreationDisposition == TRUNCATE_EXISTING) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (resolved.vfs_info.is_directory &&
        (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) == 0u) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE tok = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!tok) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    auto st = std::make_shared<GuestVfsFileState>();
    st->vfs_path = resolved.vfs_path;
    st->size = resolved.vfs_info.size_bytes;
    st->is_directory = resolved.vfs_info.is_directory;
    {
        std::unique_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
        GuestVfsFileMap()[tok] = std::move(st);
        g_active_vfs_files.fetch_add(1, std::memory_order_release);
    }
    ::SetLastError(dwCreationDisposition == OPEN_ALWAYS ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return tok;
}

uint64_t HashVfsPath64(const std::string& p) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : p) {
        const unsigned char lc = static_cast<unsigned char>(std::tolower(c == '/' ? '\\' : c));
        h ^= lc;
        h *= 1099511628211ull;
    }
    return h;
}

FILETIME DefaultVfsFileTime() {
    FILETIME ft{};
    ft.dwLowDateTime = 0x00000000u;
    ft.dwHighDateTime = 0x01DA0000u;
    return ft;
}

bool WildcardMatchCaseInsensitive(const std::string& pattern, const std::string& candidate) {
    if (pattern.empty() || pattern == "*" || pattern == "*.*") {
        return true;
    }
    size_t p = 0;
    size_t s = 0;
    size_t star_idx = std::string::npos;
    size_t match_idx = 0;
    while (s < candidate.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' ||
             std::tolower(static_cast<unsigned char>(pattern[p])) ==
                 std::tolower(static_cast<unsigned char>(candidate[s])))) {
            ++p;
            ++s;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_idx = p++;
            match_idx = s;
        } else if (star_idx != std::string::npos) {
            p = star_idx + 1;
            s = ++match_idx;
        } else {
            return false;
        }
    }
    while (p < pattern.size() &&
           (pattern[p] == '*' ||
            (p + 1 < pattern.size() && pattern[p] == '.' && pattern[p + 1] == '*'))) {
        if (pattern[p] == '.') p += 2;
        else ++p;
    }
    return p == pattern.size();
}

void FillFindDataA(const VfsDirEntry& ent, LPWIN32_FIND_DATAA out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->dwFileAttributes = ent.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    const FILETIME ft = DefaultVfsFileTime();
    out->ftCreationTime = ft;
    out->ftLastAccessTime = ft;
    out->ftLastWriteTime = ft;
    out->nFileSizeHigh = static_cast<DWORD>((ent.size_bytes >> 32) & 0xFFFFFFFFull);
    out->nFileSizeLow = static_cast<DWORD>(ent.size_bytes & 0xFFFFFFFFull);
    const size_t copy_len = (std::min)(ent.name.size(), static_cast<size_t>(MAX_PATH - 1));
    std::memcpy(out->cFileName, ent.name.data(), copy_len);
    out->cFileName[copy_len] = '\0';
}

void FillFindDataW(const VfsDirEntry& ent, LPWIN32_FIND_DATAW out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->dwFileAttributes = ent.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    const FILETIME ft = DefaultVfsFileTime();
    out->ftCreationTime = ft;
    out->ftLastAccessTime = ft;
    out->ftLastWriteTime = ft;
    out->nFileSizeHigh = static_cast<DWORD>((ent.size_bytes >> 32) & 0xFFFFFFFFull);
    out->nFileSizeLow = static_cast<DWORD>(ent.size_bytes & 0xFFFFFFFFull);
    const std::wstring wname = Utf8ToWide(ent.name);
    const size_t copy_len = (std::min)(wname.size(), static_cast<size_t>(MAX_PATH - 1));
    if (copy_len > 0) {
        std::memcpy(out->cFileName, wname.data(), copy_len * sizeof(wchar_t));
    }
    out->cFileName[copy_len] = L'\0';
}

bool TryOpenVfsFindHandle(const std::string& raw_search_path,
                          HANDLE& out_handle,
                          VfsDirEntry& out_first,
                          bool& out_handled) {
    out_handle = INVALID_HANDLE_VALUE;
    out_handled = false;
    if (raw_search_path.empty()) return false;

    // Extract drive and parent directory vs leaf pattern (before ClassifyGuestPackagePath,
    // because '*' / '?' are valid in FindFirstFile patterns).
    std::string s = raw_search_path;
    if (s.size() >= 6 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/') &&
        (s[2] == '?' || s[2] == '.') && (s[3] == '\\' || s[3] == '/')) {
        s.erase(0, 4);
    }
    if (s.size() < 2 || s[1] != ':') {
        return false;
    }
    const char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (drv != 'G' && drv != 'D') {
        return false;
    }
    if (HasDotDotSegment(s)) {
        out_handled = true;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }

    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 2 && s.back() == '\\') {
        s.pop_back();
    }
    std::string parent_vdir = "G:";
    std::string pattern = "*";
    const size_t sep = s.find_last_of('\\');
    if (sep != std::string::npos) {
        const std::string dir_part = s.substr(2, sep - 2);
        parent_vdir = dir_part.empty() ? std::string("G:") : ("G:" + dir_part);
        pattern = s.substr(sep + 1);
    } else if (s.size() > 2) {
        pattern = s.substr(2);
    }

    VfsEntryInfo pinfo{};
    if (!PackageVfs::Global().Stat(parent_vdir, pinfo) || !pinfo.exists || !pinfo.is_xvd_backed) {
        return false;
    }

    out_handled = true;
    std::vector<VfsDirEntry> all_entries;
    if (!PackageVfs::Global().ListDirectory(parent_vdir, all_entries).ok()) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return false;
    }

    std::vector<VfsDirEntry> matched;
    const bool is_sub = (parent_vdir.size() > 3);
    if (is_sub && (pattern == "*" || pattern == "*.*")) {
        matched.push_back(VfsDirEntry{".", 0, true});
        matched.push_back(VfsDirEntry{"..", 0, true});
    }
    for (const auto& ent : all_entries) {
        if (WildcardMatchCaseInsensitive(pattern, ent.name)) {
            matched.push_back(ent);
        }
    }
    if (matched.empty()) {
        ::SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    HANDLE tok = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!tok) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    out_first = matched.front();
    auto st = std::make_shared<GuestVfsFindState>();
    st->matches = std::move(matched);
    st->cursor = 1;
    {
        std::unique_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
        GuestVfsFindMap()[tok] = std::move(st);
        g_active_vfs_finds.fetch_add(1, std::memory_order_release);
    }
    out_handle = tok;
    ::SetLastError(ERROR_SUCCESS);
    return true;
}

HANDLE WINAPI GuestCreateFileA(LPCSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            HANDLE h = OpenVirtualXvdFileHandle(
                r, dwDesiredAccess, dwCreationDisposition, dwFlagsAndAttributes);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            HANDLE h = !wide.empty()
                ? ::CreateFileW(wide.c_str(), dwDesiredAccess, dwShareMode,
                                lpSecurityAttributes, dwCreationDisposition,
                                dwFlagsAndAttributes, hTemplateFile)
                : ::CreateFileA(r.host_path.c_str(), dwDesiredAccess, dwShareMode,
                                lpSecurityAttributes, dwCreationDisposition,
                                dwFlagsAndAttributes, hTemplateFile);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
    }
    return ::CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI GuestCreateFileW(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath = WideToUtf8(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            HANDLE h = OpenVirtualXvdFileHandle(
                r, dwDesiredAccess, dwCreationDisposition, dwFlagsAndAttributes);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            if (wide.empty()) {
                SetLastError(ERROR_INVALID_NAME);
                RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
                return INVALID_HANDLE_VALUE;
            }
            HANDLE h = ::CreateFileW(wide.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                     dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
    }
    return ::CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI GuestCreateFile2(LPCWSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               DWORD dwCreationDisposition,
                               LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath = WideToUtf8(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            const DWORD flags = pCreateExParams ? pCreateExParams->dwFileFlags : 0u;
            HANDLE h = OpenVirtualXvdFileHandle(r, dwDesiredAccess, dwCreationDisposition, flags);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            if (wide.empty()) {
                SetLastError(ERROR_INVALID_NAME);
                RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
                return INVALID_HANDLE_VALUE;
            }
            HANDLE h = ::CreateFile2(wide.c_str(), dwDesiredAccess, dwShareMode, dwCreationDisposition,
                                     pCreateExParams);
            RecordGuestVfsResultPreservingLastError(raw_vpath, h != INVALID_HANDLE_VALUE, caller_rip);
            return h;
        }
    }
    return ::CreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition,
                         pCreateExParams);
}

BOOL WINAPI GuestReadFile(HANDLE hFile,
                          LPVOID lpBuffer,
                          DWORD nNumberOfBytesToRead,
                          LPDWORD lpNumberOfBytesRead,
                          LPOVERLAPPED lpOverlapped) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        if (vf->is_directory || (nNumberOfBytesToRead > 0 && !lpBuffer)) {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        auto signal_ov_event = [&]() {
            if (lpOverlapped && lpOverlapped->hEvent) {
                HANDLE ev = reinterpret_cast<HANDLE>(
                    reinterpret_cast<uintptr_t>(lpOverlapped->hEvent) & ~uintptr_t{1});
                if (ev) ::SetEvent(ev);
            }
        };
        uint64_t read_off = 0;
        std::unique_lock<std::mutex> pos_lk(vf->pos_mu, std::defer_lock);
        if (lpOverlapped) {
            read_off = (static_cast<uint64_t>(lpOverlapped->OffsetHigh) << 32) |
                       static_cast<uint64_t>(lpOverlapped->Offset);
        } else {
            pos_lk.lock();
            read_off = vf->position;
        }
        if (nNumberOfBytesToRead == 0) {
            if (lpOverlapped) {
                lpOverlapped->Internal = 0;
                lpOverlapped->InternalHigh = 0;
                signal_ov_event();
            }
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        if (read_off >= vf->size) {
            if (lpOverlapped) {
                lpOverlapped->Internal = static_cast<ULONG_PTR>(0xC0000011ull);  // STATUS_END_OF_FILE
                lpOverlapped->InternalHigh = 0;
                signal_ov_event();
                ::SetLastError(ERROR_HANDLE_EOF);
                return FALSE;
            }
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        const size_t to_read = static_cast<size_t>(
            (std::min)(static_cast<uint64_t>(nNumberOfBytesToRead), vf->size - read_off));
        size_t got = 0;
        Status st = PackageVfs::Global().ReadFileRangeToBuffer(
            vf->vfs_path, read_off, to_read, lpBuffer, got);
        if (!st.ok()) {
            if (lpOverlapped) {
                lpOverlapped->Internal = static_cast<ULONG_PTR>(0xC0000185ull);
                lpOverlapped->InternalHigh = 0;
            }
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        if (!lpOverlapped) {
            vf->position = read_off + got;
        } else {
            lpOverlapped->Internal = 0;
            lpOverlapped->InternalHigh = got;
            signal_ov_event();
        }
        if (lpNumberOfBytesRead) {
            *lpNumberOfBytesRead = static_cast<DWORD>(got);
        }
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

BOOL WINAPI GuestGetOverlappedResult(HANDLE hFile,
                                     LPOVERLAPPED lpOverlapped,
                                     LPDWORD lpNumberOfBytesTransferred,
                                     BOOL bWait) {
    if (LookupVfsFileHandle(hFile)) {
        if (!lpOverlapped || !lpNumberOfBytesTransferred) {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        *lpNumberOfBytesTransferred = static_cast<DWORD>(lpOverlapped->InternalHigh);
        if (lpOverlapped->Internal == static_cast<ULONG_PTR>(0xC0000011ull)) {
            ::SetLastError(ERROR_HANDLE_EOF);
            return FALSE;
        }
        if (static_cast<intptr_t>(lpOverlapped->Internal) < 0) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
}

BOOL WINAPI GuestGetOverlappedResultEx(HANDLE hFile,
                                       LPOVERLAPPED lpOverlapped,
                                       LPDWORD lpNumberOfBytesTransferred,
                                       DWORD dwMilliseconds,
                                       BOOL bAlertable) {
    if (LookupVfsFileHandle(hFile)) {
        return GuestGetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred,
                                        dwMilliseconds != 0 ? TRUE : FALSE);
    }
    return ::GetOverlappedResultEx(hFile, lpOverlapped, lpNumberOfBytesTransferred,
                                   dwMilliseconds, bAlertable);
}

BOOL WINAPI GuestSetFilePointerEx(HANDLE hFile,
                                  LARGE_INTEGER liDistanceToMove,
                                  PLARGE_INTEGER lpNewFilePointer,
                                  DWORD dwMoveMethod) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        std::lock_guard<std::mutex> lk(vf->pos_mu);
        int64_t base = 0;
        if (dwMoveMethod == FILE_BEGIN) {
            base = 0;
        } else if (dwMoveMethod == FILE_CURRENT) {
            base = static_cast<int64_t>(vf->position);
        } else if (dwMoveMethod == FILE_END) {
            base = static_cast<int64_t>(vf->size);
        } else {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const int64_t next = base + liDistanceToMove.QuadPart;
        if (next < 0) {
            ::SetLastError(ERROR_NEGATIVE_SEEK);
            return FALSE;
        }
        vf->position = static_cast<uint64_t>(next);
        if (lpNewFilePointer) {
            lpNewFilePointer->QuadPart = next;
        }
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::SetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
}

DWORD WINAPI GuestSetFilePointer(HANDLE hFile,
                                 LONG lDistanceToMove,
                                 PLONG lpDistanceToMoveHigh,
                                 DWORD dwMoveMethod) {
    if (LookupVfsFileHandle(hFile)) {
        LARGE_INTEGER dist{};
        if (lpDistanceToMoveHigh) {
            dist.LowPart = static_cast<DWORD>(lDistanceToMove);
            dist.HighPart = *lpDistanceToMoveHigh;
        } else {
            dist.QuadPart = static_cast<LONGLONG>(lDistanceToMove);
        }
        LARGE_INTEGER out_pos{};
        if (!GuestSetFilePointerEx(hFile, dist, &out_pos, dwMoveMethod)) {
            return INVALID_SET_FILE_POINTER;
        }
        if (lpDistanceToMoveHigh) {
            *lpDistanceToMoveHigh = out_pos.HighPart;
        }
        ::SetLastError(ERROR_SUCCESS);
        return out_pos.LowPart;
    }
    return ::SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

BOOL WINAPI GuestGetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        if (!lpFileSize) {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        lpFileSize->QuadPart = static_cast<LONGLONG>(vf->size);
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::GetFileSizeEx(hFile, lpFileSize);
}

DWORD WINAPI GuestGetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        if (lpFileSizeHigh) {
            *lpFileSizeHigh = static_cast<DWORD>((vf->size >> 32) & 0xFFFFFFFFull);
        }
        ::SetLastError(ERROR_SUCCESS);
        return static_cast<DWORD>(vf->size & 0xFFFFFFFFull);
    }
    return ::GetFileSize(hFile, lpFileSizeHigh);
}

BOOL WINAPI GuestGetFileInformationByHandle(HANDLE hFile,
                                            LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        if (!lpFileInformation) {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::memset(lpFileInformation, 0, sizeof(*lpFileInformation));
        lpFileInformation->dwFileAttributes =
            vf->is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        const FILETIME ft = DefaultVfsFileTime();
        lpFileInformation->ftCreationTime = ft;
        lpFileInformation->ftLastAccessTime = ft;
        lpFileInformation->ftLastWriteTime = ft;
        lpFileInformation->dwVolumeSerialNumber = 0x5CA81E77u;
        lpFileInformation->nFileSizeHigh = static_cast<DWORD>((vf->size >> 32) & 0xFFFFFFFFull);
        lpFileInformation->nFileSizeLow = static_cast<DWORD>(vf->size & 0xFFFFFFFFull);
        lpFileInformation->nNumberOfLinks = 1;
        const uint64_t fidx = HashVfsPath64(vf->vfs_path);
        lpFileInformation->nFileIndexHigh = static_cast<DWORD>((fidx >> 32) & 0xFFFFFFFFull);
        lpFileInformation->nFileIndexLow = static_cast<DWORD>(fidx & 0xFFFFFFFFull);
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::GetFileInformationByHandle(hFile, lpFileInformation);
}

BOOL WINAPI GuestGetFileInformationByHandleEx(HANDLE hFile,
                                              FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                                              LPVOID lpFileInformation,
                                              DWORD dwBufferSize) {
    if (auto vf = LookupVfsFileHandle(hFile)) {
        if (!lpFileInformation) {
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (FileInformationClass == FileBasicInfo) {
            if (dwBufferSize < sizeof(FILE_BASIC_INFO)) {
                ::SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }
            auto* info = static_cast<FILE_BASIC_INFO*>(lpFileInformation);
            std::memset(info, 0, sizeof(*info));
            info->FileAttributes =
                vf->is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        if (FileInformationClass == FileStandardInfo) {
            if (dwBufferSize < sizeof(FILE_STANDARD_INFO)) {
                ::SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }
            auto* info = static_cast<FILE_STANDARD_INFO*>(lpFileInformation);
            std::memset(info, 0, sizeof(*info));
            info->AllocationSize.QuadPart = static_cast<LONGLONG>((vf->size + 4095ull) & ~4095ull);
            info->EndOfFile.QuadPart = static_cast<LONGLONG>(vf->size);
            info->NumberOfLinks = 1;
            info->Directory = vf->is_directory ? TRUE : FALSE;
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return ::GetFileInformationByHandleEx(
        hFile, FileInformationClass, lpFileInformation, dwBufferSize);
}

DWORD WINAPI GuestGetFileType(HANDLE hFile) {
    if (LookupVfsFileHandle(hFile)) {
        ::SetLastError(ERROR_SUCCESS);
        return FILE_TYPE_DISK;
    }
    return ::GetFileType(hFile);
}

BOOL WINAPI GuestDuplicateHandle(HANDLE hSourceProcessHandle,
                                 HANDLE hSourceHandle,
                                 HANDLE hTargetProcessHandle,
                                 LPHANDLE lpTargetHandle,
                                 DWORD dwDesiredAccess,
                                 BOOL bInheritHandle,
                                 DWORD dwOptions) {
    const BOOL ok = ::DuplicateHandle(hSourceProcessHandle, hSourceHandle, hTargetProcessHandle,
                                      lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
    if (ok && lpTargetHandle && *lpTargetHandle) {
        if (auto vf = LookupVfsFileHandle(hSourceHandle)) {
            std::unique_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
            GuestVfsFileMap()[*lpTargetHandle] = vf;
            g_active_vfs_files.fetch_add(1, std::memory_order_release);
        }
    }
    return ok;
}

BOOL WINAPI GuestCloseHandle(HANDLE hObject) {
    if (hObject && hObject != INVALID_HANDLE_VALUE &&
        g_active_vfs_files.load(std::memory_order_acquire) > 0) {
        bool was_vfs = false;
        {
            std::unique_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
            auto it = GuestVfsFileMap().find(hObject);
            if (it != GuestVfsFileMap().end()) {
                GuestVfsFileMap().erase(it);
                g_active_vfs_files.fetch_sub(1, std::memory_order_release);
                was_vfs = true;
            }
        }
        if (was_vfs) {
            return ::CloseHandle(hObject);
        }
    }
    return ::CloseHandle(hObject);
}

DWORD WINAPI GuestGetFileAttributesA(LPCSTR lpFileName) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_SUCCESS);
            RecordGuestVfsResultPreservingLastError(raw_vpath, true, caller_rip);
            return r.vfs_info.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            const DWORD attr = wide.empty() ? ::GetFileAttributesA(r.host_path.c_str())
                                            : ::GetFileAttributesW(wide.c_str());
            RecordGuestVfsResultPreservingLastError(raw_vpath, attr != INVALID_FILE_ATTRIBUTES, caller_rip);
            return attr;
        }
    }
    return ::GetFileAttributesA(lpFileName);
}

DWORD WINAPI GuestGetFileAttributesW(LPCWSTR lpFileName) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath = WideToUtf8(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_SUCCESS);
            RecordGuestVfsResultPreservingLastError(raw_vpath, true, caller_rip);
            return r.vfs_info.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            if (wide.empty()) {
                SetLastError(ERROR_INVALID_NAME);
                RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
                return INVALID_FILE_ATTRIBUTES;
            }
            const DWORD attr = ::GetFileAttributesW(wide.c_str());
            RecordGuestVfsResultPreservingLastError(raw_vpath, attr != INVALID_FILE_ATTRIBUTES, caller_rip);
            return attr;
        }
    }
    return ::GetFileAttributesW(lpFileName);
}

BOOL FillXvdAttributeData(const VfsEntryInfo& vinfo,
                          GET_FILEEX_INFO_LEVELS fInfoLevelId,
                          LPVOID lpFileInformation) {
    if (fInfoLevelId != GetFileExInfoStandard || !lpFileInformation) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(lpFileInformation);
    std::memset(data, 0, sizeof(*data));
    data->dwFileAttributes = vinfo.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    const FILETIME ft = DefaultVfsFileTime();
    data->ftCreationTime = ft;
    data->ftLastAccessTime = ft;
    data->ftLastWriteTime = ft;
    data->nFileSizeHigh = static_cast<DWORD>((vinfo.size_bytes >> 32) & 0xFFFFFFFFull);
    data->nFileSizeLow = static_cast<DWORD>(vinfo.size_bytes & 0xFFFFFFFFull);
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI GuestGetFileAttributesExA(LPCSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            const BOOL ok = FillXvdAttributeData(r.vfs_info, fInfoLevelId, lpFileInformation);
            RecordGuestVfsResultPreservingLastError(raw_vpath, ok != FALSE, caller_rip);
            return ok;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            const BOOL ok = wide.empty()
                ? ::GetFileAttributesExA(r.host_path.c_str(), fInfoLevelId, lpFileInformation)
                : ::GetFileAttributesExW(wide.c_str(), fInfoLevelId, lpFileInformation);
            RecordGuestVfsResultPreservingLastError(raw_vpath, ok != FALSE, caller_rip);
            return ok;
        }
    }
    return ::GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL WINAPI GuestGetFileAttributesExW(LPCWSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath = WideToUtf8(lpFileName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(raw_vpath);
        if (r.kind == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            const BOOL ok = FillXvdAttributeData(r.vfs_info, fInfoLevelId, lpFileInformation);
            RecordGuestVfsResultPreservingLastError(raw_vpath, ok != FALSE, caller_rip);
            return ok;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            if (wide.empty()) {
                SetLastError(ERROR_INVALID_NAME);
                RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
                return FALSE;
            }
            const BOOL ok = ::GetFileAttributesExW(wide.c_str(), fInfoLevelId, lpFileInformation);
            RecordGuestVfsResultPreservingLastError(raw_vpath, ok != FALSE, caller_rip);
            return ok;
        }
    }
    return ::GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

HANDLE WINAPI GuestFindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    if (lpFileName) {
        HANDLE vfs_h = INVALID_HANDLE_VALUE;
        VfsDirEntry first{};
        bool handled = false;
        if (TryOpenVfsFindHandle(lpFileName, vfs_h, first, handled)) {
            FillFindDataA(first, lpFindFileData);
            return vfs_h;
        }
        if (handled) {
            return INVALID_HANDLE_VALUE;
        }
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(lpFileName, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::ResolvedHost) {
            return ::FindFirstFileA(host_path.c_str(), lpFindFileData);
        }
    }
    return ::FindFirstFileA(lpFileName, lpFindFileData);
}

HANDLE WINAPI GuestFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (lpFileName) {
        const std::string utf8 = WideToUtf8(lpFileName);
        HANDLE vfs_h = INVALID_HANDLE_VALUE;
        VfsDirEntry first{};
        bool handled = false;
        if (TryOpenVfsFindHandle(utf8, vfs_h, first, handled)) {
            FillFindDataW(first, lpFindFileData);
            return vfs_h;
        }
        if (handled) {
            return INVALID_HANDLE_VALUE;
        }
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(utf8, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(host_path);
            if (!wide.empty()) {
                return ::FindFirstFileW(wide.c_str(), lpFindFileData);
            }
        }
    }
    return ::FindFirstFileW(lpFileName, lpFindFileData);
}

HANDLE WINAPI GuestFindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
                                    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
                                    LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
    if (lpFileName) {
        HANDLE vfs_h = INVALID_HANDLE_VALUE;
        VfsDirEntry first{};
        bool handled = false;
        if (TryOpenVfsFindHandle(lpFileName, vfs_h, first, handled)) {
            FillFindDataA(first, static_cast<LPWIN32_FIND_DATAA>(lpFindFileData));
            return vfs_h;
        }
        if (handled) {
            return INVALID_HANDLE_VALUE;
        }
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(lpFileName, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::ResolvedHost) {
            return ::FindFirstFileExA(host_path.c_str(), fInfoLevelId, lpFindFileData,
                                      fSearchOp, lpSearchFilter, dwAdditionalFlags);
        }
    }
    return ::FindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData,
                              fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

HANDLE WINAPI GuestFindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
                                    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
                                    LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
    if (lpFileName) {
        const std::string utf8 = WideToUtf8(lpFileName);
        HANDLE vfs_h = INVALID_HANDLE_VALUE;
        VfsDirEntry first{};
        bool handled = false;
        if (TryOpenVfsFindHandle(utf8, vfs_h, first, handled)) {
            FillFindDataW(first, static_cast<LPWIN32_FIND_DATAW>(lpFindFileData));
            return vfs_h;
        }
        if (handled) {
            return INVALID_HANDLE_VALUE;
        }
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(utf8, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(host_path);
            if (!wide.empty()) {
                return ::FindFirstFileExW(wide.c_str(), fInfoLevelId, lpFindFileData,
                                          fSearchOp, lpSearchFilter, dwAdditionalFlags);
            }
        }
    }
    return ::FindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData,
                              fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

BOOL WINAPI GuestFindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    if (auto vf = LookupVfsFindHandle(hFindFile)) {
        std::lock_guard<std::mutex> lk(vf->mu);
        if (vf->cursor >= vf->matches.size()) {
            ::SetLastError(ERROR_NO_MORE_FILES);
            return FALSE;
        }
        FillFindDataA(vf->matches[vf->cursor++], lpFindFileData);
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::FindNextFileA(hFindFile, lpFindFileData);
}

BOOL WINAPI GuestFindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    if (auto vf = LookupVfsFindHandle(hFindFile)) {
        std::lock_guard<std::mutex> lk(vf->mu);
        if (vf->cursor >= vf->matches.size()) {
            ::SetLastError(ERROR_NO_MORE_FILES);
            return FALSE;
        }
        FillFindDataW(vf->matches[vf->cursor++], lpFindFileData);
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return ::FindNextFileW(hFindFile, lpFindFileData);
}

BOOL WINAPI GuestFindClose(HANDLE hFindFile) {
    if (hFindFile && hFindFile != INVALID_HANDLE_VALUE &&
        g_active_vfs_finds.load(std::memory_order_acquire) > 0) {
        bool was_vfs = false;
        {
            std::unique_lock<std::shared_mutex> lk(GuestVfsHandlesMutex());
            auto it = GuestVfsFindMap().find(hFindFile);
            if (it != GuestVfsFindMap().end()) {
                GuestVfsFindMap().erase(it);
                g_active_vfs_finds.fetch_sub(1, std::memory_order_release);
                was_vfs = true;
            }
        }
        if (was_vfs) {
            return ::CloseHandle(hFindFile);
        }
    }
    return ::FindClose(hFindFile);
}

BOOL WINAPI GuestGetDiskFreeSpaceExA(LPCSTR lpDirectoryName,
                                     PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                                     PULARGE_INTEGER lpTotalNumberOfBytes,
                                     PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    if (lpDirectoryName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(lpDirectoryName);
        if (r.kind == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            constexpr ULONGLONG kReadOnlyVolTotal = 100ull << 30;  // 100 GiB read-only XVC volume
            if (lpFreeBytesAvailableToCaller) lpFreeBytesAvailableToCaller->QuadPart = 0;
            if (lpTotalNumberOfBytes) lpTotalNumberOfBytes->QuadPart = kReadOnlyVolTotal;
            if (lpTotalNumberOfFreeBytes) lpTotalNumberOfFreeBytes->QuadPart = 0;
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            return ::GetDiskFreeSpaceExA(r.host_path.c_str(), lpFreeBytesAvailableToCaller,
                                         lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
        }
    }
    return ::GetDiskFreeSpaceExA(lpDirectoryName, lpFreeBytesAvailableToCaller,
                                 lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

BOOL WINAPI GuestGetDiskFreeSpaceExW(LPCWSTR lpDirectoryName,
                                     PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                                     PULARGE_INTEGER lpTotalNumberOfBytes,
                                     PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    if (lpDirectoryName) {
        const std::string utf8 = WideToUtf8(lpDirectoryName);
        const GuestResolvedPath r = ClassifyGuestPackagePath(utf8);
        if (r.kind == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            constexpr ULONGLONG kReadOnlyVolTotal = 100ull << 30;
            if (lpFreeBytesAvailableToCaller) lpFreeBytesAvailableToCaller->QuadPart = 0;
            if (lpTotalNumberOfBytes) lpTotalNumberOfBytes->QuadPart = kReadOnlyVolTotal;
            if (lpTotalNumberOfFreeBytes) lpTotalNumberOfFreeBytes->QuadPart = 0;
            ::SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            if (!wide.empty()) {
                return ::GetDiskFreeSpaceExW(wide.c_str(), lpFreeBytesAvailableToCaller,
                                             lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
            }
        }
    }
    return ::GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller,
                                 lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

BOOL WINAPI GuestDeleteFileA(LPCSTR lpFileName) {
    if (lpFileName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(lpFileName);
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty() ? ::DeleteFileA(r.host_path.c_str()) : ::DeleteFileW(wide.c_str());
        }
    }
    return ::DeleteFileA(lpFileName);
}

BOOL WINAPI GuestDeleteFileW(LPCWSTR lpFileName) {
    if (lpFileName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(WideToUtf8(lpFileName));
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty() ? FALSE : ::DeleteFileW(wide.c_str());
        }
    }
    return ::DeleteFileW(lpFileName);
}

BOOL WINAPI GuestCreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (lpPathName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(lpPathName);
        if (r.kind == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(r.vfs_info.is_directory ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty()
                       ? ::CreateDirectoryA(r.host_path.c_str(), lpSecurityAttributes)
                       : ::CreateDirectoryW(wide.c_str(), lpSecurityAttributes);
        }
    }
    return ::CreateDirectoryA(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI GuestCreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (lpPathName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(WideToUtf8(lpPathName));
        if (r.kind == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(r.vfs_info.is_directory ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty() ? FALSE : ::CreateDirectoryW(wide.c_str(), lpSecurityAttributes);
        }
    }
    return ::CreateDirectoryW(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI GuestRemoveDirectoryA(LPCSTR lpPathName) {
    if (lpPathName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(lpPathName);
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty() ? ::RemoveDirectoryA(r.host_path.c_str())
                                : ::RemoveDirectoryW(wide.c_str());
        }
    }
    return ::RemoveDirectoryA(lpPathName);
}

BOOL WINAPI GuestRemoveDirectoryW(LPCWSTR lpPathName) {
    if (lpPathName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(WideToUtf8(lpPathName));
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (r.kind == GuestPathResult::ResolvedHost) {
            const std::wstring wide = Utf8ToWide(r.host_path);
            return wide.empty() ? FALSE : ::RemoveDirectoryW(wide.c_str());
        }
    }
    return ::RemoveDirectoryW(lpPathName);
}

// Zen 2 Xbox Series X|S CPU topology (8 logical cores / 0xFF mask) & safe thread affinity clamping
void WINAPI GuestGetSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
    ::GetSystemInfo(lpSystemInfo);
    if (lpSystemInfo) {
        if (lpSystemInfo->dwNumberOfProcessors > 8u) {
            lpSystemInfo->dwNumberOfProcessors = 8u;
        }
        lpSystemInfo->dwActiveProcessorMask = (static_cast<DWORD_PTR>(1) << lpSystemInfo->dwNumberOfProcessors) - 1u;
    }
}

void WINAPI GuestGetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
    ::GetNativeSystemInfo(lpSystemInfo);
    if (lpSystemInfo) {
        if (lpSystemInfo->dwNumberOfProcessors > 8u) {
            lpSystemInfo->dwNumberOfProcessors = 8u;
        }
        lpSystemInfo->dwActiveProcessorMask = (static_cast<DWORD_PTR>(1) << lpSystemInfo->dwNumberOfProcessors) - 1u;
    }
}

DWORD_PTR WINAPI GuestSetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
    DWORD_PTR proc_mask = 0;
    DWORD_PTR sys_mask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask != 0) {
        DWORD_PTR clamped = dwThreadAffinityMask & proc_mask;
        if (clamped == 0) {
            // Wrap requested bit index into available host cores instead of failing with ERROR_INVALID_PARAMETER
            clamped = proc_mask;
        }
        return ::SetThreadAffinityMask(hThread, clamped);
    }
    return ::SetThreadAffinityMask(hThread, dwThreadAffinityMask);
}

// A title DLL loaded by the host loader receives the full guest IAT hook table.
HMODULE AdoptHostModule(HMODULE mod, bool force_package_module = false) {
    if (mod) {
        const DWORD saved_error = GetLastError();
        guest::PatchHostModuleForGuests(mod, force_package_module);
        SetLastError(saved_error);
    }
    return mod;
}

std::mutex& DynamicVfsDllMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<HMODULE, std::shared_ptr<MappedPeImage>>& DynamicVfsDllTable() {
    static std::unordered_map<HMODULE, std::shared_ptr<MappedPeImage>> t;
    return t;
}

void ClearDynamicVfsDlls() {
    std::unordered_map<HMODULE, std::shared_ptr<MappedPeImage>> local;
    {
        std::lock_guard<std::mutex> lk(DynamicVfsDllMutex());
        local.swap(DynamicVfsDllTable());
    }
    local.clear();
}

// Loads/maps a guest DLL directly from PackageVfs (zero-staging XVD/MSIXVC streaming!).
HMODULE TryMapGuestDllFromPackageVfs(const std::string& vfs_path,
                                     std::shared_ptr<MappedPeImage>* out_owned = nullptr) {
    if (vfs_path.empty()) return nullptr;
    using GetModHandleAFn = HMODULE(WINAPI*)(LPCSTR);
    const auto guest_gmh = reinterpret_cast<GetModHandleAFn>(
        guest::LookupImportOverride("kernel32.dll", "GetModuleHandleA"));
    if (guest_gmh) {
        const std::string base = ExtractBasenameLower(vfs_path);
        if (HMODULE already = guest_gmh(base.c_str())) {
            if (guest::FindModuleBase(already) == reinterpret_cast<void*>(already)) {
                return already;
            }
        }
    }

    std::vector<uint8_t> dll_bytes;
    if (!PackageVfs::Global().ReadFileBytes(vfs_path, dll_bytes).ok() || dll_bytes.empty()) {
        return nullptr;
    }

    auto img = std::make_shared<MappedPeImage>();
    DllRedirectionTable redirects;
    HleSymbolRegistry empty_reg;
    PeMapOptions opts;
    opts.auto_stub_missing = true;
    opts.shim_directory = GetGuestShimDirectory();
    opts.package_directory_override = GetGuestPackageDirectory();
    if (!img->MapBytes(dll_bytes, vfs_path, redirects, empty_reg, opts).ok()) {
        return nullptr;
    }
    HMODULE hmod = reinterpret_cast<HMODULE>(img->MappedBase());
    if (out_owned) {
        *out_owned = std::move(img);
    } else {
        std::lock_guard<std::mutex> lk(DynamicVfsDllMutex());
        DynamicVfsDllTable()[hmod] = std::move(img);
    }
    return hmod;
}

HMODULE TryLoadSystemShimA(const char* name) {
    if (!name || !*name) return nullptr;
    const std::string shim_dir = GetGuestShimDirectory();
    if (shim_dir.empty()) return nullptr;
    const std::string base = ExtractBasenameLower(name);
    const char* target = ResolveSystemShimTarget(base);
    if (!target) return nullptr;
    const auto candidate = std::filesystem::path(shim_dir) / target;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
        const std::wstring wide_cand = candidate.wstring();
        HMODULE mod = LoadLibraryW(wide_cand.c_str());
        const DWORD error = mod ? 0 : GetLastError();
        if (mod) {
            std::fprintf(stderr, "[ScarlettX][loader] Redirected dynamic load '%s' -> '%s' (handle=%p)\n",
                         name, candidate.string().c_str(), static_cast<void*>(mod));
        } else {
            std::fprintf(stderr, "[ScarlettX][loader] Redirected dynamic load '%s' -> '%s' FAILED (error %lu)\n",
                         name, candidate.string().c_str(), error);
            SetLastError(error);
        }
        return mod;
    }
    return nullptr;
}

HMODULE TryLoadPackageOrFallbackMiddlewareA(const char* name, DWORD dwFlags = 0) {
    if (!name || !*name) return nullptr;
    const std::string raw(name);
    std::string base = ExtractBasenameLower(raw);
    if (base.find('.') == std::string::npos) {
        base += ".dll";
    }
    if (ResolveSystemShimTarget(base) != nullptr) {
        return nullptr;
    }

    const char* mw_target = ResolveFallbackMiddlewareTarget(base);
    const std::string pkg_dir = GetGuestPackageDirectory();
    std::error_code ec;

    // 1. Check G:\ virtual package path if explicitly specified (both XVD VFS and host mount).
    const GuestResolvedPath vr = ClassifyGuestPackagePath(raw);
    if (vr.kind == GuestPathResult::ResolvedXvdVfs && !vr.vfs_info.is_directory) {
        if (HMODULE vmod = TryMapGuestDllFromPackageVfs(vr.vfs_path)) {
            return vmod;
        }
    } else if (vr.kind == GuestPathResult::ResolvedHost &&
               std::filesystem::exists(vr.host_path, ec)) {
        const std::wstring wide = Utf8ToWide(vr.host_path);
        if (!wide.empty()) {
            const DWORD eff_flags = dwFlags ? dwFlags : LOAD_WITH_ALTERED_SEARCH_PATH;
            if (HMODULE mod = LoadLibraryExW(wide.c_str(), nullptr, eff_flags)) {
                return AdoptHostModule(mod, /*force_package_module=*/true);
            }
        }
    }

    // 2. Check PackageVfs G:\ and host package directory for ALL non-system DLLs
    //    (GameAssembly.dll, Oodle, Wwise, FMOD, engine plugins, plus title-shipped
    //    middleware / Microsoft.Xbox.Services.*.GDK.C.Thunks.dll).
    //    Always prioritize the guest-requested filename BEFORE mw_target (e.g. xsapi.dll)!
    if (!IsCoreHostWindowsDll(base) && raw.find(':') == std::string::npos &&
        !HasDotDotSegment(raw)) {
        std::vector<std::string> candidates;
        candidates.push_back(raw);
        if (raw.find('.') == std::string::npos) {
            candidates.push_back(raw + ".dll");
        }
        if (base != ToLowerAscii(raw)) {
            candidates.push_back(base);
        }
        if (mw_target && ToLowerAscii(mw_target) == base) {
            candidates.emplace_back(mw_target);
        }

        // 2a. Check mounted XVD in PackageVfs first (zero-staging .xvc / .msixvc).
        for (const std::string& rel : candidates) {
            const std::string vpath = "G:\\" + rel;
            VfsEntryInfo vinfo{};
            if (PackageVfs::Global().Stat(vpath, vinfo) && vinfo.exists &&
                vinfo.is_xvd_backed && !vinfo.is_directory) {
                if (HMODULE vmod = TryMapGuestDllFromPackageVfs(vpath)) {
                    return vmod;
                }
            }
        }

        // 2b. Check host package directory when directory-backed.
        if (!pkg_dir.empty() && !IsVirtualGDriveRoot(pkg_dir)) {
            for (const std::string& rel : candidates) {
                const auto pkg_candidate = std::filesystem::path(pkg_dir) / rel;
                if (std::filesystem::exists(pkg_candidate, ec)) {
                    const std::wstring wide = pkg_candidate.wstring();
                    const DWORD eff_flags = dwFlags ? dwFlags : LOAD_WITH_ALTERED_SEARCH_PATH;
                    if (HMODULE mod = LoadLibraryExW(wide.c_str(), nullptr, eff_flags)) {
                        return AdoptHostModule(mod, /*force_package_module=*/true);
                    }
                }
            }
        }
    }

    // 3. Fallback to ScarlettEdge HLE middleware shim when package does not ship it.
    if (mw_target) {
        const std::string shim_dir = GetGuestShimDirectory();
        if (!shim_dir.empty()) {
            const auto shim_candidate = std::filesystem::path(shim_dir) / mw_target;
            if (std::filesystem::exists(shim_candidate, ec)) {
                const std::wstring wide = shim_candidate.wstring();
                return LoadLibraryW(wide.c_str());
            }
        }
    }
    return nullptr;
}

HMODULE WINAPI GuestLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (HMODULE mod = TryLoadSystemShimA(lpLibFileName)) {
        return mod;
    }
    if (HMODULE mod = TryLoadPackageOrFallbackMiddlewareA(lpLibFileName, dwFlags)) {
        return mod;
    }
    if (lpLibFileName) {
        const GuestResolvedPath r = ClassifyGuestPackagePath(lpLibFileName);
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
    }
    return AdoptHostModule(LoadLibraryExA(lpLibFileName, hFile, dwFlags));
}

HMODULE WINAPI GuestLoadLibraryA(LPCSTR lpLibFileName) {
    return GuestLoadLibraryExA(lpLibFileName, nullptr, 0);
}

HMODULE WINAPI GuestLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName && *lpLibFileName) {
        const std::string narrow = WideToUtf8(lpLibFileName);
        if (HMODULE mod = TryLoadSystemShimA(narrow.c_str())) {
            return mod;
        }
        if (HMODULE mod = TryLoadPackageOrFallbackMiddlewareA(narrow.c_str(), dwFlags)) {
            return mod;
        }
        const GuestResolvedPath r = ClassifyGuestPackagePath(narrow);
        if (r.kind == GuestPathResult::Blocked || r.kind == GuestPathResult::ResolvedXvdVfs) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
    }
    return AdoptHostModule(LoadLibraryExW(lpLibFileName, hFile, dwFlags));
}

HMODULE WINAPI GuestLoadLibraryW(LPCWSTR lpLibFileName) {
    return GuestLoadLibraryExW(lpLibFileName, nullptr, 0);
}

BOOL WINAPI GuestFreeLibrary(HMODULE hLibModule) {
    if (!hLibModule) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    {
        std::shared_ptr<MappedPeImage> dynamic_dll;
        {
            std::lock_guard<std::mutex> lk(DynamicVfsDllMutex());
            auto it = DynamicVfsDllTable().find(hLibModule);
            if (it != DynamicVfsDllTable().end()) {
                dynamic_dll = std::move(it->second);
                DynamicVfsDllTable().erase(it);
            }
        }
        if (dynamic_dll) {
            dynamic_dll->Unmap();
            return TRUE;
        }
    }
    if (void* guest_base = guest::FindModuleBase(hLibModule)) {
        if (guest_base == reinterpret_cast<void*>(hLibModule)) {
            return TRUE;
        }
    }
    return ::FreeLibrary(hLibModule);
}

FARPROC WINAPI GuestGetProcAddress(HMODULE hModule, LPCSTR lpProcName);

void* ResolvePeExportFromImage(uint8_t* image_base,
                               uint32_t image_size,
                               uint32_t exp_rva,
                               uint32_t exp_size,
                               const char* name_or_ord,
                               int depth) {
    if (!image_base || !name_or_ord || exp_rva == 0 || exp_size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        depth > 8) {
        return nullptr;
    }
    if (static_cast<size_t>(exp_rva) > image_size || exp_size > image_size - exp_rva) {
        return nullptr;
    }
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image_base + exp_rva);
    if (exp->NumberOfFunctions == 0 || exp->AddressOfFunctions >= image_size ||
        static_cast<size_t>(exp->NumberOfFunctions) * sizeof(uint32_t) >
            image_size - exp->AddressOfFunctions) {
        return nullptr;
    }
    const auto* funcs = reinterpret_cast<const uint32_t*>(image_base + exp->AddressOfFunctions);

    uint32_t func_rva = 0;
    bool found = false;
    const uintptr_t raw_arg = reinterpret_cast<uintptr_t>(name_or_ord);
    if ((raw_arg >> 16) == 0) {
        const uint16_t ordinal = static_cast<uint16_t>(raw_arg & 0xFFFFu);
        if (ordinal < exp->Base) {
            return nullptr;
        }
        const uint32_t idx = static_cast<uint32_t>(ordinal - exp->Base);
        if (idx >= exp->NumberOfFunctions) {
            return nullptr;
        }
        func_rva = funcs[idx];
        found = (func_rva != 0);
    } else {
        if (exp->NumberOfNames == 0 || exp->AddressOfNames >= image_size ||
            exp->AddressOfNameOrdinals >= image_size ||
            static_cast<size_t>(exp->NumberOfNames) * sizeof(uint32_t) >
                image_size - exp->AddressOfNames ||
            static_cast<size_t>(exp->NumberOfNames) * sizeof(uint16_t) >
                image_size - exp->AddressOfNameOrdinals) {
            return nullptr;
        }
        const auto* names = reinterpret_cast<const uint32_t*>(image_base + exp->AddressOfNames);
        const auto* ordinals =
            reinterpret_cast<const uint16_t*>(image_base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (names[i] >= image_size) {
                continue;
            }
            const char* cand = reinterpret_cast<const char*>(image_base + names[i]);
            if (std::strcmp(name_or_ord, cand) == 0) {
                const uint16_t ord_idx = ordinals[i];
                if (ord_idx < exp->NumberOfFunctions) {
                    func_rva = funcs[ord_idx];
                    found = (func_rva != 0);
                }
                break;
            }
        }
    }

    if (!found || func_rva >= image_size) {
        return nullptr;
    }

    // Handle PE export forwarders pointing inside [exp_rva, exp_rva + exp_size).
    if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) {
        const size_t max_len = static_cast<size_t>(exp_rva + exp_size - func_rva);
        const char* fwd_ptr = reinterpret_cast<const char*>(image_base + func_rva);
        size_t fwd_len = 0;
        while (fwd_len < max_len && fwd_ptr[fwd_len] != '\0') {
            ++fwd_len;
        }
        if (fwd_len == 0 || fwd_len == max_len) {
            return nullptr;
        }
        const std::string fwd_spec(fwd_ptr, fwd_len);
        const size_t dot = fwd_spec.find_last_of('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= fwd_spec.size()) {
            return nullptr;
        }
        const std::string fwd_dll = fwd_spec.substr(0, dot) + ".dll";
        const std::string fwd_sym = fwd_spec.substr(dot + 1);

        using GetModHandleAFn = HMODULE(WINAPI*)(LPCSTR);
        const auto guest_gmh = reinterpret_cast<GetModHandleAFn>(
            guest::LookupImportOverride("kernel32.dll", "GetModuleHandleA"));
        HMODULE target_mod = guest_gmh ? guest_gmh(fwd_dll.c_str()) : GetModuleHandleA(fwd_dll.c_str());
        if (!target_mod) {
            target_mod = GuestLoadLibraryA(fwd_dll.c_str());
        }
        if (!target_mod) {
            return nullptr;
        }

        if (fwd_sym[0] == '#') {
            char* end = nullptr;
            const unsigned long ord = std::strtoul(fwd_sym.c_str() + 1, &end, 10);
            if (!end || *end != '\0' || ord == 0 || ord > 0xFFFFul) {
                return nullptr;
            }
            if (guest::FindModuleBase(target_mod) == reinterpret_cast<void*>(target_mod)) {
                auto* tbase = reinterpret_cast<uint8_t*>(target_mod);
                const auto* tdos = reinterpret_cast<const IMAGE_DOS_HEADER*>(tbase);
                const auto* tnt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(tbase + tdos->e_lfanew);
                const auto& texp = tnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                return ResolvePeExportFromImage(tbase, tnt->OptionalHeader.SizeOfImage,
                                                texp.VirtualAddress, texp.Size,
                                                MAKEINTRESOURCEA(static_cast<WORD>(ord)), depth + 1);
            }
            return reinterpret_cast<void*>(
                ::GetProcAddress(target_mod, MAKEINTRESOURCEA(static_cast<WORD>(ord))));
        }

        if (guest::FindModuleBase(target_mod) == reinterpret_cast<void*>(target_mod)) {
            auto* tbase = reinterpret_cast<uint8_t*>(target_mod);
            const auto* tdos = reinterpret_cast<const IMAGE_DOS_HEADER*>(tbase);
            const auto* tnt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(tbase + tdos->e_lfanew);
            const auto& texp = tnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            return ResolvePeExportFromImage(tbase, tnt->OptionalHeader.SizeOfImage,
                                            texp.VirtualAddress, texp.Size,
                                            fwd_sym.c_str(), depth + 1);
        }
        return reinterpret_cast<void*>(::GetProcAddress(target_mod, fwd_sym.c_str()));
    }

    return image_base + func_rva;
}

HMODULE GetCachedXsapiModule() {
    static std::mutex mu;
    static HMODULE cached = nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (cached) {
        HMODULE check = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(cached), &check) &&
            check == cached) {
            return cached;
        }
        cached = nullptr;
    }
    cached = GetModuleHandleW(L"xsapi.dll");
    if (!cached) {
        cached = TryLoadSystemShimA("xsapi.dll");
    }
    return cached;
}

// Clean-room WinRT IInspectable / IActivationFactory broker for common Xbox / UWP
// runtime classes: Windows.Gaming.Input.Gamepad, Windows.System.Profile.AnalyticsInfo,
// and Windows.ApplicationModel.Package.
enum class BrokerWinRtKind {
    None = 0,
    Gamepad,
    AnalyticsInfo,
    AnalyticsVersionInfo,
    Package,
    PackageId,
};

constexpr GUID kWinRtIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
constexpr GUID kWinRtIidIInspectable = {
    0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9Cu, 0x5Au, 0xD7u, 0xAAu, 0x65u, 0x10u, 0x1Eu, 0x90u}};
constexpr GUID kWinRtIidIAgileObject = {
    0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0u, 0xFFu, 0xEEu, 0x64u, 0xCAu, 0x8Fu, 0x5Bu, 0x90u}};
constexpr GUID kWinRtIidIActivationFactory = {
    0x00000035u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};

// Windows.Gaming.Input.Gamepad
constexpr GUID kWinRtIidIGamepadStatics = {
    0x8BBCE529u, 0xD49Cu, 0x39E9u, {0x95u, 0x60u, 0xE4u, 0x7Du, 0xDEu, 0x96u, 0xB7u, 0xC8u}};
constexpr GUID kWinRtIidIGamepadStatics2 = {
    0x42676DC5u, 0x0856u, 0x47C4u, {0x92u, 0x13u, 0xB3u, 0x95u, 0x50u, 0x4Cu, 0x3Au, 0x3Cu}};
constexpr GUID kWinRtIidIGamepad = {
    0xBC7BB43Cu, 0x0A69u, 0x3903u, {0x9Eu, 0x9Du, 0xA5u, 0x0Fu, 0x86u, 0xA4u, 0x5Du, 0xE5u}};

// Windows.System.Profile.AnalyticsInfo / AnalyticsVersionInfo
constexpr GUID kWinRtIidIAnalyticsInfoStatics = {
    0x1D5EE066u, 0x188Du, 0x5BA9u, {0x43u, 0x87u, 0xACu, 0xAEu, 0xB0u, 0xE7u, 0xE3u, 0x05u}};
constexpr GUID kWinRtIidIAnalyticsVersionInfo = {
    0x926130B8u, 0x9955u, 0x4C74u, {0xBDu, 0xC1u, 0x7Cu, 0xD0u, 0xDEu, 0xCFu, 0x9Bu, 0x03u}};

// Windows.ApplicationModel.Package / PackageId
constexpr GUID kWinRtIidIPackageStatics = {
    0x4E534BDFu, 0x2960u, 0x4878u, {0x97u, 0xA4u, 0x96u, 0x24u, 0xDEu, 0xB7u, 0x2Fu, 0x2Du}};
constexpr GUID kWinRtIidIPackage = {
    0x163C792Fu, 0xBD75u, 0x413Cu, {0xBFu, 0x23u, 0xB1u, 0xFEu, 0x7Bu, 0x95u, 0xD8u, 0x25u}};
constexpr GUID kWinRtIidIPackageId = {
    0x1ADB665Eu, 0x37C7u, 0x4790u, {0x99u, 0x80u, 0xDDu, 0x7Au, 0xE7u, 0x4Eu, 0x8Bu, 0xB2u}};

bool BrokerGuidEquals(const GUID* a, const GUID& b) {
    return a && std::memcmp(a, &b, sizeof(GUID)) == 0;
}

void* BrokerCoTaskMemAlloc(size_t bytes) {
    static HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    using FnAlloc = void*(WINAPI*)(size_t);
    static const FnAlloc alloc_fn =
        ole32 ? reinterpret_cast<FnAlloc>(reinterpret_cast<void*>(GetProcAddress(ole32, "CoTaskMemAlloc")))
              : nullptr;
    return alloc_fn ? alloc_fn(bytes) : nullptr;
}

HRESULT BrokerCreateHString(const wchar_t* text, void** out_hstr) {
    if (!out_hstr) return E_POINTER;
    *out_hstr = nullptr;
    if (!text) return S_OK;
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using FnCreateStr = HRESULT(WINAPI*)(const wchar_t*, uint32_t, void**);
    static const FnCreateStr create_str =
        combase ? reinterpret_cast<FnCreateStr>(reinterpret_cast<void*>(
                      GetProcAddress(combase, "WindowsCreateString")))
                : nullptr;
    if (!create_str) return E_NOTIMPL;
    return create_str(text, static_cast<uint32_t>(std::wcslen(text)), out_hstr);
}

BrokerWinRtKind ClassifyBrokerWinRtClass(const wchar_t* cls) {
    if (!cls || !*cls) return BrokerWinRtKind::None;
    if (std::wcscmp(cls, L"Windows.Gaming.Input.Gamepad") == 0) return BrokerWinRtKind::Gamepad;
    if (std::wcscmp(cls, L"Windows.System.Profile.AnalyticsInfo") == 0) return BrokerWinRtKind::AnalyticsInfo;
    if (std::wcscmp(cls, L"Windows.ApplicationModel.Package") == 0) return BrokerWinRtKind::Package;
    return BrokerWinRtKind::None;
}

struct BrokerGamepadVibration {
    double left_motor = 0.0;
    double right_motor = 0.0;
    double left_trigger = 0.0;
    double right_trigger = 0.0;
};

struct BrokerGamepadReading {
    uint64_t timestamp = 0;
    uint32_t buttons = 0;
    uint32_t reserved = 0;
    double left_trigger = 0.0;
    double right_trigger = 0.0;
    double left_thumbstick_x = 0.0;
    double left_thumbstick_y = 0.0;
    double right_thumbstick_x = 0.0;
    double right_thumbstick_y = 0.0;
};

struct BrokerPackageVersion {
    uint16_t major = 1;
    uint16_t minor = 0;
    uint16_t build = 0;
    uint16_t revision = 0;
};

struct BrokerWinRtInstance {
    const void* const* vtbl = nullptr;
    volatile LONG refs = 1;
    BrokerWinRtKind kind = BrokerWinRtKind::None;
    GUID primary_iid{};
    const wchar_t* class_name = L"";
    BrokerGamepadVibration vibration{};
};

struct BrokerWinRtFactory;

struct BrokerWinRtStaticsView {
    const void* const* vtbl = nullptr;
    BrokerWinRtFactory* owner = nullptr;
};

struct BrokerWinRtFactory {
    const void* const* vtbl = nullptr;
    BrokerWinRtStaticsView statics{};
    volatile LONG refs = 1;
    BrokerWinRtKind kind = BrokerWinRtKind::None;
    GUID statics_iid{};
    GUID statics_iid2{};
    bool has_statics_iid2 = false;
    volatile LONG next_event_token = 1;
};

BrokerWinRtInstance* CreateBrokerWinRtInstance(BrokerWinRtKind kind);

HRESULT WINAPI BrokerInst_QueryInterface(void* self_ptr, const GUID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!self_ptr || !riid) return E_INVALIDARG;
    auto* self = static_cast<BrokerWinRtInstance*>(self_ptr);
    if (BrokerGuidEquals(riid, kWinRtIidIUnknown) ||
        BrokerGuidEquals(riid, kWinRtIidIInspectable) ||
        BrokerGuidEquals(riid, kWinRtIidIAgileObject) ||
        BrokerGuidEquals(riid, self->primary_iid)) {
        InterlockedIncrement(&self->refs);
        *ppv = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG WINAPI BrokerInst_AddRef(void* self_ptr) {
    if (!self_ptr) return 0;
    return static_cast<ULONG>(InterlockedIncrement(&static_cast<BrokerWinRtInstance*>(self_ptr)->refs));
}

ULONG WINAPI BrokerInst_Release(void* self_ptr) {
    if (!self_ptr) return 0;
    auto* self = static_cast<BrokerWinRtInstance*>(self_ptr);
    const LONG rem = InterlockedDecrement(&self->refs);
    if (rem == 0) {
        delete self;
    }
    return static_cast<ULONG>(rem);
}

HRESULT WINAPI BrokerInst_GetIids(void* self_ptr, ULONG* iid_count, GUID** iids) {
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = nullptr;
    if (!self_ptr) return E_INVALIDARG;
    auto* self = static_cast<BrokerWinRtInstance*>(self_ptr);
    auto* mem = static_cast<GUID*>(BrokerCoTaskMemAlloc(sizeof(GUID)));
    if (!mem) return E_OUTOFMEMORY;
    *mem = self->primary_iid;
    *iid_count = 1;
    *iids = mem;
    return S_OK;
}

HRESULT WINAPI BrokerInst_GetRuntimeClassName(void* self_ptr, void** out_hstr) {
    if (!out_hstr) return E_POINTER;
    *out_hstr = nullptr;
    if (!self_ptr) return E_INVALIDARG;
    auto* self = static_cast<BrokerWinRtInstance*>(self_ptr);
    return BrokerCreateHString(self->class_name, out_hstr);
}

HRESULT WINAPI BrokerInst_GetTrustLevel(void* /*self_ptr*/, int32_t* trust_level) {
    if (!trust_level) return E_POINTER;
    *trust_level = 0;  // BaseTrust
    return S_OK;
}

HRESULT WINAPI BrokerWinRt_NotImpl(void* /*self_ptr*/) {
    return E_NOTIMPL;
}

// IGamepad methods (slots 6..8)
HRESULT WINAPI BrokerGamepad_GetVibration(void* self_ptr, BrokerGamepadVibration* out_vib) {
    if (!out_vib) return E_POINTER;
    if (!self_ptr) return E_INVALIDARG;
    *out_vib = static_cast<BrokerWinRtInstance*>(self_ptr)->vibration;
    return S_OK;
}

HRESULT WINAPI BrokerGamepad_PutVibration(void* self_ptr, BrokerGamepadVibration vib) {
    if (!self_ptr) return E_INVALIDARG;
    static_cast<BrokerWinRtInstance*>(self_ptr)->vibration = vib;
    return S_OK;
}

HRESULT WINAPI BrokerGamepad_GetCurrentReading(void* self_ptr, BrokerGamepadReading* out_reading) {
    if (!out_reading) return E_POINTER;
    if (!self_ptr) return E_INVALIDARG;
    *out_reading = BrokerGamepadReading{};
    out_reading->timestamp = GetTickCount64() * 10000ull;
    return S_OK;
}

// IAnalyticsVersionInfo methods (slots 6..7)
HRESULT WINAPI BrokerAnalyticsVersion_GetDeviceFamily(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"Windows.Xbox", out_hstr);
}

HRESULT WINAPI BrokerAnalyticsVersion_GetDeviceFamilyVersion(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"2814751249596416", out_hstr);
}

// IPackage methods (slots 6..9)
HRESULT WINAPI BrokerPackage_GetId(void* /*self_ptr*/, void** out_pkg_id) {
    if (!out_pkg_id) return E_POINTER;
    *out_pkg_id = CreateBrokerWinRtInstance(BrokerWinRtKind::PackageId);
    return *out_pkg_id ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI BrokerPackage_GetInstalledLocation(void* /*self_ptr*/, void** out_folder) {
    if (!out_folder) return E_POINTER;
    *out_folder = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI BrokerPackage_GetDependencies(void* /*self_ptr*/, void** out_deps) {
    if (!out_deps) return E_POINTER;
    *out_deps = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI BrokerPackage_GetIsFramework(void* /*self_ptr*/, uint8_t* out_is_framework) {
    if (!out_is_framework) return E_POINTER;
    *out_is_framework = 0;
    return S_OK;
}

// IPackageId methods (slots 6..13)
HRESULT WINAPI BrokerPackageId_GetName(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"ScarlettX.Title", out_hstr);
}

HRESULT WINAPI BrokerPackageId_GetVersion(void* /*self_ptr*/, BrokerPackageVersion* out_ver) {
    if (!out_ver) return E_POINTER;
    *out_ver = BrokerPackageVersion{1, 0, 0, 0};
    return S_OK;
}

HRESULT WINAPI BrokerPackageId_GetArchitecture(void* /*self_ptr*/, int32_t* out_arch) {
    if (!out_arch) return E_POINTER;
    *out_arch = 9;  // ProcessorArchitecture_X64
    return S_OK;
}

HRESULT WINAPI BrokerPackageId_GetResourceId(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"", out_hstr);
}

HRESULT WINAPI BrokerPackageId_GetPublisher(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"CN=ScarlettX", out_hstr);
}

HRESULT WINAPI BrokerPackageId_GetPublisherId(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"scarlettx0000", out_hstr);
}

HRESULT WINAPI BrokerPackageId_GetFullName(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"ScarlettX.Title_1.0.0.0_x64__scarlettx0000", out_hstr);
}

HRESULT WINAPI BrokerPackageId_GetFamilyName(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"ScarlettX.Title_scarlettx0000", out_hstr);
}

const void* const kBrokerGamepadInstVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerInst_QueryInterface),
    reinterpret_cast<const void*>(&BrokerInst_AddRef),
    reinterpret_cast<const void*>(&BrokerInst_Release),
    reinterpret_cast<const void*>(&BrokerInst_GetIids),
    reinterpret_cast<const void*>(&BrokerInst_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerInst_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerGamepad_GetVibration),
    reinterpret_cast<const void*>(&BrokerGamepad_PutVibration),
    reinterpret_cast<const void*>(&BrokerGamepad_GetCurrentReading),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerAnalyticsVersionVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerInst_QueryInterface),
    reinterpret_cast<const void*>(&BrokerInst_AddRef),
    reinterpret_cast<const void*>(&BrokerInst_Release),
    reinterpret_cast<const void*>(&BrokerInst_GetIids),
    reinterpret_cast<const void*>(&BrokerInst_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerInst_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerAnalyticsVersion_GetDeviceFamily),
    reinterpret_cast<const void*>(&BrokerAnalyticsVersion_GetDeviceFamilyVersion),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerPackageInstVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerInst_QueryInterface),
    reinterpret_cast<const void*>(&BrokerInst_AddRef),
    reinterpret_cast<const void*>(&BrokerInst_Release),
    reinterpret_cast<const void*>(&BrokerInst_GetIids),
    reinterpret_cast<const void*>(&BrokerInst_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerInst_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerPackage_GetId),
    reinterpret_cast<const void*>(&BrokerPackage_GetInstalledLocation),
    reinterpret_cast<const void*>(&BrokerPackage_GetDependencies),
    reinterpret_cast<const void*>(&BrokerPackage_GetIsFramework),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerPackageIdInstVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerInst_QueryInterface),
    reinterpret_cast<const void*>(&BrokerInst_AddRef),
    reinterpret_cast<const void*>(&BrokerInst_Release),
    reinterpret_cast<const void*>(&BrokerInst_GetIids),
    reinterpret_cast<const void*>(&BrokerInst_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerInst_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerPackageId_GetName),
    reinterpret_cast<const void*>(&BrokerPackageId_GetVersion),
    reinterpret_cast<const void*>(&BrokerPackageId_GetArchitecture),
    reinterpret_cast<const void*>(&BrokerPackageId_GetResourceId),
    reinterpret_cast<const void*>(&BrokerPackageId_GetPublisher),
    reinterpret_cast<const void*>(&BrokerPackageId_GetPublisherId),
    reinterpret_cast<const void*>(&BrokerPackageId_GetFullName),
    reinterpret_cast<const void*>(&BrokerPackageId_GetFamilyName),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

BrokerWinRtInstance* CreateBrokerWinRtInstance(BrokerWinRtKind kind) {
    auto* inst = new (std::nothrow) BrokerWinRtInstance();
    if (!inst) return nullptr;
    inst->kind = kind;
    switch (kind) {
        case BrokerWinRtKind::Gamepad:
            inst->vtbl = kBrokerGamepadInstVtbl;
            inst->primary_iid = kWinRtIidIGamepad;
            inst->class_name = L"Windows.Gaming.Input.Gamepad";
            break;
        case BrokerWinRtKind::AnalyticsInfo:
        case BrokerWinRtKind::AnalyticsVersionInfo:
            inst->kind = BrokerWinRtKind::AnalyticsVersionInfo;
            inst->vtbl = kBrokerAnalyticsVersionVtbl;
            inst->primary_iid = kWinRtIidIAnalyticsVersionInfo;
            inst->class_name = L"Windows.System.Profile.AnalyticsVersionInfo";
            break;
        case BrokerWinRtKind::Package:
            inst->vtbl = kBrokerPackageInstVtbl;
            inst->primary_iid = kWinRtIidIPackage;
            inst->class_name = L"Windows.ApplicationModel.Package";
            break;
        case BrokerWinRtKind::PackageId:
            inst->vtbl = kBrokerPackageIdInstVtbl;
            inst->primary_iid = kWinRtIidIPackageId;
            inst->class_name = L"Windows.ApplicationModel.PackageId";
            break;
        default:
            delete inst;
            return nullptr;
    }
    return inst;
}

HRESULT BrokerFactoryQueryInterfaceCommon(BrokerWinRtFactory* fac, const GUID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!fac || !riid) return E_INVALIDARG;
    if (BrokerGuidEquals(riid, kWinRtIidIUnknown) ||
        BrokerGuidEquals(riid, kWinRtIidIInspectable) ||
        BrokerGuidEquals(riid, kWinRtIidIAgileObject) ||
        BrokerGuidEquals(riid, kWinRtIidIActivationFactory)) {
        InterlockedIncrement(&fac->refs);
        *ppv = fac;
        return S_OK;
    }
    if (BrokerGuidEquals(riid, fac->statics_iid) ||
        (fac->has_statics_iid2 && BrokerGuidEquals(riid, fac->statics_iid2))) {
        InterlockedIncrement(&fac->refs);
        *ppv = &fac->statics;
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT WINAPI BrokerFac_QueryInterface(void* self_ptr, const GUID* riid, void** ppv) {
    return BrokerFactoryQueryInterfaceCommon(static_cast<BrokerWinRtFactory*>(self_ptr), riid, ppv);
}

ULONG WINAPI BrokerFac_AddRef(void* self_ptr) {
    if (!self_ptr) return 0;
    return static_cast<ULONG>(InterlockedIncrement(&static_cast<BrokerWinRtFactory*>(self_ptr)->refs));
}

ULONG WINAPI BrokerFac_Release(void* self_ptr) {
    if (!self_ptr) return 0;
    auto* fac = static_cast<BrokerWinRtFactory*>(self_ptr);
    const LONG rem = InterlockedDecrement(&fac->refs);
    if (rem == 0) delete fac;
    return static_cast<ULONG>(rem);
}

HRESULT BrokerFactoryGetIidsCommon(BrokerWinRtFactory* fac, ULONG* iid_count, GUID** iids) {
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = nullptr;
    if (!fac) return E_INVALIDARG;
    const ULONG count = fac->has_statics_iid2 ? 3u : 2u;
    auto* mem = static_cast<GUID*>(BrokerCoTaskMemAlloc(sizeof(GUID) * count));
    if (!mem) return E_OUTOFMEMORY;
    mem[0] = kWinRtIidIActivationFactory;
    mem[1] = fac->statics_iid;
    if (fac->has_statics_iid2) {
        mem[2] = fac->statics_iid2;
    }
    *iid_count = count;
    *iids = mem;
    return S_OK;
}

HRESULT WINAPI BrokerFac_GetIids(void* self_ptr, ULONG* iid_count, GUID** iids) {
    return BrokerFactoryGetIidsCommon(static_cast<BrokerWinRtFactory*>(self_ptr), iid_count, iids);
}

HRESULT WINAPI BrokerFac_GetRuntimeClassName(void* /*self_ptr*/, void** out_hstr) {
    if (!out_hstr) return E_POINTER;
    *out_hstr = nullptr;
    return static_cast<HRESULT>(0x8000000Eu);  // E_ILLEGAL_METHOD_CALL per WinRT IActivationFactory spec
}

HRESULT WINAPI BrokerFac_GetTrustLevel(void* /*self_ptr*/, int32_t* trust_level) {
    if (!trust_level) return E_POINTER;
    *trust_level = 0;  // BaseTrust
    return S_OK;
}

HRESULT WINAPI BrokerFac_ActivateInstance(void* self_ptr, void** out_instance) {
    if (!out_instance) return E_POINTER;
    *out_instance = nullptr;
    if (!self_ptr) return E_INVALIDARG;
    auto* fac = static_cast<BrokerWinRtFactory*>(self_ptr);
    *out_instance = CreateBrokerWinRtInstance(fac->kind);
    return *out_instance ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI BrokerStatics_QueryInterface(void* self_ptr, const GUID* riid, void** ppv) {
    auto* view = static_cast<BrokerWinRtStaticsView*>(self_ptr);
    return BrokerFactoryQueryInterfaceCommon(view ? view->owner : nullptr, riid, ppv);
}

ULONG WINAPI BrokerStatics_AddRef(void* self_ptr) {
    auto* view = static_cast<BrokerWinRtStaticsView*>(self_ptr);
    return (view && view->owner) ? BrokerFac_AddRef(view->owner) : 0;
}

ULONG WINAPI BrokerStatics_Release(void* self_ptr) {
    auto* view = static_cast<BrokerWinRtStaticsView*>(self_ptr);
    return (view && view->owner) ? BrokerFac_Release(view->owner) : 0;
}

HRESULT WINAPI BrokerStatics_GetIids(void* self_ptr, ULONG* iid_count, GUID** iids) {
    auto* view = static_cast<BrokerWinRtStaticsView*>(self_ptr);
    return BrokerFactoryGetIidsCommon(view ? view->owner : nullptr, iid_count, iids);
}

// IGamepadStatics methods (slots 6..10)
HRESULT WINAPI BrokerGamepadStatics_AddEvent(void* self_ptr, void* /*handler*/, int64_t* out_token) {
    if (!out_token) return E_POINTER;
    auto* view = static_cast<BrokerWinRtStaticsView*>(self_ptr);
    if (!view || !view->owner) return E_INVALIDARG;
    *out_token = static_cast<int64_t>(InterlockedIncrement(&view->owner->next_event_token));
    return S_OK;
}

HRESULT WINAPI BrokerGamepadStatics_RemoveEvent(void* /*self_ptr*/, int64_t /*token*/) {
    return S_OK;
}

HRESULT WINAPI BrokerGamepadStatics_GetGamepads(void* /*self_ptr*/, void** out_vec) {
    if (!out_vec) return E_POINTER;
    *out_vec = nullptr;
    return E_NOTIMPL;
}

// IAnalyticsInfoStatics methods (slots 6..7)
HRESULT WINAPI BrokerAnalyticsStatics_GetVersionInfo(void* /*self_ptr*/, void** out_ver_info) {
    if (!out_ver_info) return E_POINTER;
    *out_ver_info = CreateBrokerWinRtInstance(BrokerWinRtKind::AnalyticsVersionInfo);
    return *out_ver_info ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI BrokerAnalyticsStatics_GetDeviceForm(void* /*self_ptr*/, void** out_hstr) {
    return BrokerCreateHString(L"Xbox", out_hstr);
}

// IPackageStatics methods (slot 6)
HRESULT WINAPI BrokerPackageStatics_GetCurrent(void* /*self_ptr*/, void** out_pkg) {
    if (!out_pkg) return E_POINTER;
    *out_pkg = CreateBrokerWinRtInstance(BrokerWinRtKind::Package);
    return *out_pkg ? S_OK : E_OUTOFMEMORY;
}

const void* const kBrokerFactoryVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerFac_QueryInterface),
    reinterpret_cast<const void*>(&BrokerFac_AddRef),
    reinterpret_cast<const void*>(&BrokerFac_Release),
    reinterpret_cast<const void*>(&BrokerFac_GetIids),
    reinterpret_cast<const void*>(&BrokerFac_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerFac_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerFac_ActivateInstance),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerGamepadStaticsVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerStatics_QueryInterface),
    reinterpret_cast<const void*>(&BrokerStatics_AddRef),
    reinterpret_cast<const void*>(&BrokerStatics_Release),
    reinterpret_cast<const void*>(&BrokerStatics_GetIids),
    reinterpret_cast<const void*>(&BrokerFac_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerFac_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerGamepadStatics_AddEvent),
    reinterpret_cast<const void*>(&BrokerGamepadStatics_RemoveEvent),
    reinterpret_cast<const void*>(&BrokerGamepadStatics_AddEvent),
    reinterpret_cast<const void*>(&BrokerGamepadStatics_RemoveEvent),
    reinterpret_cast<const void*>(&BrokerGamepadStatics_GetGamepads),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerAnalyticsStaticsVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerStatics_QueryInterface),
    reinterpret_cast<const void*>(&BrokerStatics_AddRef),
    reinterpret_cast<const void*>(&BrokerStatics_Release),
    reinterpret_cast<const void*>(&BrokerStatics_GetIids),
    reinterpret_cast<const void*>(&BrokerFac_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerFac_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerAnalyticsStatics_GetVersionInfo),
    reinterpret_cast<const void*>(&BrokerAnalyticsStatics_GetDeviceForm),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

const void* const kBrokerPackageStaticsVtbl[16] = {
    reinterpret_cast<const void*>(&BrokerStatics_QueryInterface),
    reinterpret_cast<const void*>(&BrokerStatics_AddRef),
    reinterpret_cast<const void*>(&BrokerStatics_Release),
    reinterpret_cast<const void*>(&BrokerStatics_GetIids),
    reinterpret_cast<const void*>(&BrokerFac_GetRuntimeClassName),
    reinterpret_cast<const void*>(&BrokerFac_GetTrustLevel),
    reinterpret_cast<const void*>(&BrokerPackageStatics_GetCurrent),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
    reinterpret_cast<const void*>(&BrokerWinRt_NotImpl),
};

HRESULT TryBrokerLocalWinRtActivateInstance(const wchar_t* cls, void** out_instance) {
    const BrokerWinRtKind kind = ClassifyBrokerWinRtClass(cls);
    if (kind == BrokerWinRtKind::None) return REGDB_E_CLASSNOTREG;
    *out_instance = CreateBrokerWinRtInstance(kind);
    return *out_instance ? S_OK : E_OUTOFMEMORY;
}

HRESULT TryBrokerLocalWinRtGetActivationFactory(const wchar_t* cls, const GUID* iid, void** out_factory) {
    const BrokerWinRtKind kind = ClassifyBrokerWinRtClass(cls);
    if (kind == BrokerWinRtKind::None) return REGDB_E_CLASSNOTREG;
    auto* fac = new (std::nothrow) BrokerWinRtFactory();
    if (!fac) return E_OUTOFMEMORY;
    fac->vtbl = kBrokerFactoryVtbl;
    fac->statics.owner = fac;
    fac->kind = kind;
    switch (kind) {
        case BrokerWinRtKind::Gamepad:
            fac->statics.vtbl = kBrokerGamepadStaticsVtbl;
            fac->statics_iid = kWinRtIidIGamepadStatics;
            fac->statics_iid2 = kWinRtIidIGamepadStatics2;
            fac->has_statics_iid2 = true;
            break;
        case BrokerWinRtKind::AnalyticsInfo:
            fac->statics.vtbl = kBrokerAnalyticsStaticsVtbl;
            fac->statics_iid = kWinRtIidIAnalyticsInfoStatics;
            break;
        case BrokerWinRtKind::Package:
            fac->statics.vtbl = kBrokerPackageStaticsVtbl;
            fac->statics_iid = kWinRtIidIPackageStatics;
            break;
        default:
            delete fac;
            return REGDB_E_CLASSNOTREG;
    }
    const GUID* query_iid = iid ? iid : &kWinRtIidIActivationFactory;
    const HRESULT hr = BrokerFac_QueryInterface(fac, query_iid, out_factory);
    BrokerFac_Release(fac);
    return hr;
}

// WinRT RoActivateInstance / RoGetActivationFactory guest IAT broker:
// Handles core clean-room WinRT classes (Gamepad, AnalyticsInfo, Package),
// intercepts Xbox/Game Core WinRT runtime classes via xsapi.dll's XblWinRtActivateClassByName,
// and delegates all remaining desktop Windows runtime classes cleanly to host combase.dll.
HRESULT WINAPI GuestRoActivateInstance(void* activatableClassId, void** instance) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (!instance) return E_POINTER;
    *instance = nullptr;
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using FnGetRaw = const wchar_t*(WINAPI*)(void*, uint32_t*);
    using FnHostActivate = HRESULT(WINAPI*)(void*, void**);
    auto get_raw = combase ? reinterpret_cast<FnGetRaw>(reinterpret_cast<void*>(
                                 GetProcAddress(combase, "WindowsGetStringRawBuffer")))
                           : nullptr;
    const wchar_t* cls = nullptr;
    if (activatableClassId && get_raw) {
        uint32_t len = 0;
        cls = get_raw(activatableClassId, &len);
        if (cls && len > 0) {
            const HRESULT local_hr = TryBrokerLocalWinRtActivateInstance(cls, instance);
            if (local_hr != REGDB_E_CLASSNOTREG) {
                FlightRecorder::Instance().RecordWinRtActivation(
                    "RoActivateInstance", cls, local_hr, true, caller_rip);
                return local_hr;
            }
            if (HMODULE xsapi_mod = GetCachedXsapiModule()) {
                using FnBroker = int32_t(__stdcall*)(const wchar_t*, const void*, int32_t, void**);
                auto broker = reinterpret_cast<FnBroker>(reinterpret_cast<void*>(
                    GetProcAddress(xsapi_mod, "XblWinRtActivateClassByName")));
                if (broker && broker(cls, nullptr, 0, instance) == 0 && *instance) {
                    FlightRecorder::Instance().RecordWinRtActivation("RoActivateInstance", cls, S_OK, true, caller_rip);
                    return S_OK;
                }
            }
        }
    }
    auto host_act = combase ? reinterpret_cast<FnHostActivate>(reinterpret_cast<void*>(
                                  GetProcAddress(combase, "RoActivateInstance")))
                            : nullptr;
    const HRESULT hr = host_act ? host_act(activatableClassId, instance) : REGDB_E_CLASSNOTREG;
    FlightRecorder::Instance().RecordWinRtActivation("RoActivateInstance", cls, hr, false, caller_rip);
    return hr;
}

HRESULT WINAPI GuestRoGetActivationFactory(void* activatableClassId, const GUID* iid, void** factory) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (!factory) return E_POINTER;
    *factory = nullptr;
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using FnGetRaw = const wchar_t*(WINAPI*)(void*, uint32_t*);
    using FnHostGetFac = HRESULT(WINAPI*)(void*, const GUID*, void**);
    auto get_raw = combase ? reinterpret_cast<FnGetRaw>(reinterpret_cast<void*>(
                                 GetProcAddress(combase, "WindowsGetStringRawBuffer")))
                           : nullptr;
    const wchar_t* cls = nullptr;
    if (activatableClassId && get_raw) {
        uint32_t len = 0;
        cls = get_raw(activatableClassId, &len);
        if (cls && len > 0) {
            const HRESULT local_hr = TryBrokerLocalWinRtGetActivationFactory(cls, iid, factory);
            if (local_hr != REGDB_E_CLASSNOTREG) {
                FlightRecorder::Instance().RecordWinRtActivation(
                    "RoGetActivationFactory", cls, local_hr, true, caller_rip);
                return local_hr;
            }
            if (HMODULE xsapi_mod = GetCachedXsapiModule()) {
                using FnBroker = int32_t(__stdcall*)(const wchar_t*, const void*, int32_t, void**);
                auto broker = reinterpret_cast<FnBroker>(reinterpret_cast<void*>(
                    GetProcAddress(xsapi_mod, "XblWinRtActivateClassByName")));
                if (broker && broker(cls, iid, 1, factory) == 0 && *factory) {
                    FlightRecorder::Instance().RecordWinRtActivation("RoGetActivationFactory", cls, S_OK, true, caller_rip);
                    return S_OK;
                }
            }
        }
    }
    auto host_fac = combase ? reinterpret_cast<FnHostGetFac>(reinterpret_cast<void*>(
                                  GetProcAddress(combase, "RoGetActivationFactory")))
                            : nullptr;
    const HRESULT hr = host_fac ? host_fac(activatableClassId, iid, factory) : REGDB_E_CLASSNOTREG;
    FlightRecorder::Instance().RecordWinRtActivation("RoGetActivationFactory", cls, hr, false, caller_rip);
    return hr;
}

HRESULT WINAPI GuestWindowsCreateString(const wchar_t* sourceString,
                                        uint32_t length,
                                        void** string) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = HRESULT(WINAPI*)(const wchar_t*, uint32_t, void**);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsCreateString")))
                                 : nullptr;
    return fn ? fn(sourceString, length, string) : E_NOTIMPL;
}

HRESULT WINAPI GuestWindowsCreateStringReference(const wchar_t* sourceString,
                                                 uint32_t length,
                                                 void* hstringHeader,
                                                 void** string) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = HRESULT(WINAPI*)(const wchar_t*, uint32_t, void*, void**);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsCreateStringReference")))
                                 : nullptr;
    return fn ? fn(sourceString, length, hstringHeader, string) : E_NOTIMPL;
}

HRESULT WINAPI GuestWindowsDeleteString(void* string) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = HRESULT(WINAPI*)(void*);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsDeleteString")))
                                 : nullptr;
    return fn ? fn(string) : S_OK;
}

const wchar_t* WINAPI GuestWindowsGetStringRawBuffer(void* string, uint32_t* length) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = const wchar_t*(WINAPI*)(void*, uint32_t*);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsGetStringRawBuffer")))
                                 : nullptr;
    if (fn) return fn(string, length);
    if (length) *length = 0;
    return L"";
}

uint32_t WINAPI GuestWindowsGetStringLen(void* string) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = uint32_t(WINAPI*)(void*);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsGetStringLen")))
                                 : nullptr;
    return fn ? fn(string) : 0u;
}

BOOL WINAPI GuestWindowsIsStringEmpty(void* string) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = BOOL(WINAPI*)(void*);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsIsStringEmpty")))
                                 : nullptr;
    return fn ? fn(string) : TRUE;
}

HRESULT WINAPI GuestWindowsDuplicateString(void* string, void** newString) {
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    using Fn = HRESULT(WINAPI*)(void*, void**);
    static const Fn fn = combase ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                       GetProcAddress(combase, "WindowsDuplicateString")))
                                 : nullptr;
    return fn ? fn(string, newString) : E_NOTIMPL;
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI ScarlettX_GuestRoActivateInstance(
    void* activatableClassId, void** instance) {
    return GuestRoActivateInstance(activatableClassId, instance);
}

extern "C" __declspec(dllexport) HRESULT WINAPI ScarlettX_GuestRoGetActivationFactory(
    void* activatableClassId, const GUID* iid, void** factory) {
    return GuestRoGetActivationFactory(activatableClassId, iid, factory);
}

namespace {

void* LookupGuestCoreHook(const std::string& target_lower, const std::string& sym_name) {
    if (target_lower == "kernel32.dll" || target_lower == "kernelbase.dll") {
        if (sym_name == "LoadLibraryExA") return reinterpret_cast<void*>(&GuestLoadLibraryExA);
        if (sym_name == "LoadLibraryA") return reinterpret_cast<void*>(&GuestLoadLibraryA);
        if (sym_name == "LoadLibraryExW") return reinterpret_cast<void*>(&GuestLoadLibraryExW);
        if (sym_name == "LoadLibraryW") return reinterpret_cast<void*>(&GuestLoadLibraryW);
        if (sym_name == "FreeLibrary") return reinterpret_cast<void*>(&GuestFreeLibrary);
        if (sym_name == "GetProcAddress") return reinterpret_cast<void*>(&GuestGetProcAddress);
        if (sym_name == "GetCommandLineA") return reinterpret_cast<void*>(&GuestGetCommandLineA);
        if (sym_name == "GetCommandLineW") return reinterpret_cast<void*>(&GuestGetCommandLineW);
        if (sym_name == "ExitProcess") return reinterpret_cast<void*>(&GuestExitProcess);
        if (sym_name == "VirtualAlloc") return reinterpret_cast<void*>(&GuestVirtualAlloc);
        if (sym_name == "VirtualAllocEx") return reinterpret_cast<void*>(&GuestVirtualAllocEx);
        if (sym_name == "VirtualFree") return reinterpret_cast<void*>(&GuestVirtualFree);
        if (sym_name == "VirtualFreeEx") return reinterpret_cast<void*>(&GuestVirtualFreeEx);
        if (sym_name == "CreateFileA") return reinterpret_cast<void*>(&GuestCreateFileA);
        if (sym_name == "CreateFileW") return reinterpret_cast<void*>(&GuestCreateFileW);
        if (sym_name == "CreateFile2") return reinterpret_cast<void*>(&GuestCreateFile2);
        if (sym_name == "ReadFile") return reinterpret_cast<void*>(&GuestReadFile);
        if (sym_name == "GetOverlappedResult") return reinterpret_cast<void*>(&GuestGetOverlappedResult);
        if (sym_name == "GetOverlappedResultEx") return reinterpret_cast<void*>(&GuestGetOverlappedResultEx);
        if (sym_name == "SetFilePointerEx") return reinterpret_cast<void*>(&GuestSetFilePointerEx);
        if (sym_name == "SetFilePointer") return reinterpret_cast<void*>(&GuestSetFilePointer);
        if (sym_name == "GetFileSizeEx") return reinterpret_cast<void*>(&GuestGetFileSizeEx);
        if (sym_name == "GetFileSize") return reinterpret_cast<void*>(&GuestGetFileSize);
        if (sym_name == "GetFileInformationByHandle") return reinterpret_cast<void*>(&GuestGetFileInformationByHandle);
        if (sym_name == "GetFileInformationByHandleEx") return reinterpret_cast<void*>(&GuestGetFileInformationByHandleEx);
        if (sym_name == "GetFileType") return reinterpret_cast<void*>(&GuestGetFileType);
        if (sym_name == "DuplicateHandle") return reinterpret_cast<void*>(&GuestDuplicateHandle);
        if (sym_name == "CloseHandle") return reinterpret_cast<void*>(&GuestCloseHandle);
        if (sym_name == "GetFileAttributesA") return reinterpret_cast<void*>(&GuestGetFileAttributesA);
        if (sym_name == "GetFileAttributesW") return reinterpret_cast<void*>(&GuestGetFileAttributesW);
        if (sym_name == "GetFileAttributesExA") return reinterpret_cast<void*>(&GuestGetFileAttributesExA);
        if (sym_name == "GetFileAttributesExW") return reinterpret_cast<void*>(&GuestGetFileAttributesExW);
        if (sym_name == "FindFirstFileA") return reinterpret_cast<void*>(&GuestFindFirstFileA);
        if (sym_name == "FindFirstFileW") return reinterpret_cast<void*>(&GuestFindFirstFileW);
        if (sym_name == "FindFirstFileExA") return reinterpret_cast<void*>(&GuestFindFirstFileExA);
        if (sym_name == "FindFirstFileExW") return reinterpret_cast<void*>(&GuestFindFirstFileExW);
        if (sym_name == "FindNextFileA") return reinterpret_cast<void*>(&GuestFindNextFileA);
        if (sym_name == "FindNextFileW") return reinterpret_cast<void*>(&GuestFindNextFileW);
        if (sym_name == "FindClose") return reinterpret_cast<void*>(&GuestFindClose);
        if (sym_name == "GetDiskFreeSpaceExA") return reinterpret_cast<void*>(&GuestGetDiskFreeSpaceExA);
        if (sym_name == "GetDiskFreeSpaceExW") return reinterpret_cast<void*>(&GuestGetDiskFreeSpaceExW);
        if (sym_name == "DeleteFileA") return reinterpret_cast<void*>(&GuestDeleteFileA);
        if (sym_name == "DeleteFileW") return reinterpret_cast<void*>(&GuestDeleteFileW);
        if (sym_name == "CreateDirectoryA") return reinterpret_cast<void*>(&GuestCreateDirectoryA);
        if (sym_name == "CreateDirectoryW") return reinterpret_cast<void*>(&GuestCreateDirectoryW);
        if (sym_name == "RemoveDirectoryA") return reinterpret_cast<void*>(&GuestRemoveDirectoryA);
        if (sym_name == "RemoveDirectoryW") return reinterpret_cast<void*>(&GuestRemoveDirectoryW);
        if (sym_name == "GetSystemInfo") return reinterpret_cast<void*>(&GuestGetSystemInfo);
        if (sym_name == "GetNativeSystemInfo") return reinterpret_cast<void*>(&GuestGetNativeSystemInfo);
        if (sym_name == "SetThreadAffinityMask") return reinterpret_cast<void*>(&GuestSetThreadAffinityMask);
        if (sym_name == "FlsAlloc") return reinterpret_cast<void*>(&GuestFlsAlloc);
        if (sym_name == "FlsFree") return reinterpret_cast<void*>(&GuestFlsFree);
        if (sym_name == "FlsGetValue") return reinterpret_cast<void*>(&GuestFlsGetValue);
        if (sym_name == "FlsSetValue") return reinterpret_cast<void*>(&GuestFlsSetValue);
        return nullptr;
    }
    if (target_lower == "combase.dll") {
        if (sym_name == "RoActivateInstance") return reinterpret_cast<void*>(&GuestRoActivateInstance);
        if (sym_name == "RoGetActivationFactory") return reinterpret_cast<void*>(&GuestRoGetActivationFactory);
        if (sym_name == "WindowsCreateString") return reinterpret_cast<void*>(&GuestWindowsCreateString);
        if (sym_name == "WindowsCreateStringReference") return reinterpret_cast<void*>(&GuestWindowsCreateStringReference);
        if (sym_name == "WindowsDeleteString") return reinterpret_cast<void*>(&GuestWindowsDeleteString);
        if (sym_name == "WindowsGetStringRawBuffer") return reinterpret_cast<void*>(&GuestWindowsGetStringRawBuffer);
        if (sym_name == "WindowsGetStringLen") return reinterpret_cast<void*>(&GuestWindowsGetStringLen);
        if (sym_name == "WindowsIsStringEmpty") return reinterpret_cast<void*>(&GuestWindowsIsStringEmpty);
        if (sym_name == "WindowsDuplicateString") return reinterpret_cast<void*>(&GuestWindowsDuplicateString);
        return nullptr;
    }
    return nullptr;
}

void* LookupGuestRuntimeHook(const std::string& target_lower, const std::string& sym_name) {
    if (target_lower == "ucrtbase.dll" || target_lower == "msvcrt.dll") {
        if (sym_name == "_crt_atexit" || sym_name == "atexit" || sym_name == "_crt_at_quick_exit") {
            return reinterpret_cast<void*>(&GuestIsolatedAtExit);
        }
        if (sym_name == "_register_thread_local_exe_atexit_callback") {
            return reinterpret_cast<void*>(&GuestRegisterThreadLocalExeAtExit);
        }
        if (sym_name == "_cexit" || sym_name == "_c_exit") {
            return reinterpret_cast<void*>(&GuestNoopCexit);
        }
        if (sym_name == "exit") {
            return reinterpret_cast<void*>(&GuestCrtExit);
        }
        if (sym_name == "_exit" || sym_name == "quick_exit") {
            return reinterpret_cast<void*>(&GuestCrtImmediateExit);
        }
        return nullptr;
    }
    return LookupGuestCoreHook(target_lower, sym_name);
}

FARPROC WINAPI GuestGetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (!hModule || !lpProcName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // 1. Mapped guest PE64 image export table resolution.
    if (void* guest_base = guest::FindModuleBase(hModule)) {
        if (guest_base == reinterpret_cast<void*>(hModule)) {
            auto* base = static_cast<uint8_t*>(guest_base);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE &&
                    nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                    nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
                    const auto& exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                    void* addr = ResolvePeExportFromImage(
                        base, nt->OptionalHeader.SizeOfImage, exp.VirtualAddress, exp.Size,
                        lpProcName, 0);
                    if (addr) {
                        return reinterpret_cast<FARPROC>(addr);
                    }
                }
            }
            SetLastError(ERROR_PROC_NOT_FOUND);
            return nullptr;
        }
    }

    // 2. Preserve guest runtime hooks & module overrides on dynamic GetProcAddress lookups.
    if ((reinterpret_cast<uintptr_t>(lpProcName) >> 16) != 0) {
        static const HMODULE kKernel32 = GetModuleHandleW(L"kernel32.dll");
        static const HMODULE kKernelBase = GetModuleHandleW(L"kernelbase.dll");
        static const HMODULE kNtdll = GetModuleHandleW(L"ntdll.dll");
        static const HMODULE kCombase = GetModuleHandleW(L"combase.dll");
        const std::string sym(lpProcName);
        if (hModule == kKernel32 || hModule == kKernelBase) {
            if (void* hk = LookupGuestRuntimeHook("kernel32.dll", sym)) {
                return reinterpret_cast<FARPROC>(hk);
            }
            if (void* ov = guest::LookupImportOverride("kernel32.dll", sym)) {
                return reinterpret_cast<FARPROC>(ov);
            }
        } else if (hModule == kNtdll) {
            if (void* ov = guest::LookupImportOverride("ntdll.dll", sym)) {
                return reinterpret_cast<FARPROC>(ov);
            }
        } else if (hModule == kCombase) {
            if (void* hk = LookupGuestRuntimeHook("combase.dll", sym)) {
                return reinterpret_cast<FARPROC>(hk);
            }
        }
    }

    FARPROC proc = ::GetProcAddress(hModule, lpProcName);
    if (proc) {
        return proc;
    }

    // 3. If hModule is a loaded ScarlettX HLE shim and export is missing, log cleanly.
    const DWORD saved_err = GetLastError();
    wchar_t mod_path[MAX_PATH + 1] = {};
    if (GetModuleFileNameW(hModule, mod_path, MAX_PATH) > 0) {
        const std::string base = ExtractBasenameLower(WideToUtf8(mod_path));
        if (ResolveSystemShimTarget(base) != nullptr ||
            ResolveFallbackMiddlewareTarget(base) != nullptr) {
            if ((reinterpret_cast<uintptr_t>(lpProcName) >> 16) == 0) {
                const auto ord = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpProcName) & 0xFFFFu);
                std::fprintf(stderr,
                             "[ScarlettX][loader] Missing HLE shim export on GetProcAddress: %s!#%u\n",
                             base.c_str(),
                             static_cast<unsigned>(ord));
                FlightRecorder::Instance().RecordStubHit(
                    base.c_str(), "", true, ord, nullptr, 0, SCARLETTX_CALLER_RIP());
            } else {
                std::fprintf(stderr,
                             "[ScarlettX][loader] Missing HLE shim export on GetProcAddress: %s!%s\n",
                             base.c_str(), lpProcName);
                FlightRecorder::Instance().RecordStubHit(
                    base.c_str(), lpProcName, false, 0, nullptr, 0, SCARLETTX_CALLER_RIP());
            }
        }
    }
    SetLastError(saved_err);
    return nullptr;
}

DWORD SectionCharsToPageProtect(uint32_t ch) {
    const bool exec = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (ch & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (ch & IMAGE_SCN_MEM_WRITE) != 0;
    if (exec) {
        if (write) return PAGE_EXECUTE_READWRITE;
        if (read) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    if (read) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
#endif

}  // namespace

void* LookupGuestDllRuntimeHook(const std::string& target_dll_lower, const std::string& symbol) {
#if defined(_WIN32)
    if (target_dll_lower == "ucrtbase.dll" || target_dll_lower == "msvcrt.dll") {
        if (symbol == "exit") return reinterpret_cast<void*>(&GuestCrtExit);
        if (symbol == "_exit" || symbol == "quick_exit") return reinterpret_cast<void*>(&GuestCrtImmediateExit);
        return nullptr;
    }
    return LookupGuestCoreHook(target_dll_lower, symbol);
#else
    (void)target_dll_lower;
    (void)symbol;
    return nullptr;
#endif
}

std::string GetGuestPackageDirectoryForPatch() {
#if defined(_WIN32)
    return GetGuestPackageDirectory();
#else
    return {};
#endif
}

std::string GetGuestShimDirectoryForPatch() {
#if defined(_WIN32)
    return GetGuestShimDirectory();
#else
    return {};
#endif
}

bool IsGuestGraphicsAllocation(const void* address, size_t* out_size) noexcept {
    if (out_size) *out_size = 0;
#if defined(_WIN32)
    if (!address) return false;
    const uintptr_t va = reinterpret_cast<uintptr_t>(address);
    std::shared_lock<std::shared_mutex> lk(GuestGraphicsMutex());
    for (const auto& [base, reg] : GuestGraphicsTable()) {
        if (va >= base && va < base + reg.size) {
            if (out_size) *out_size = reg.size;
            return true;
        }
    }
#else
    (void)address;
#endif
    return false;
}

std::string ResolveApiSetDll(const std::string& dll_name) {
    const std::string lower = ToLowerAscii(dll_name);
    if (StartsWith(lower, "api-ms-win-crt-")) {
        return "ucrtbase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-com-")) {
        return "combase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-winrt-")) {
        return "combase.dll";
    }
    if (StartsWith(lower, "api-ms-win-security-")) {
        return "advapi32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-registry-")) {
        return "advapi32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-psm-appnotify-")) {
        return "twinapi.appcore.dll";
    }
    if (StartsWith(lower, "ext-ms-win-rtcore-ntuser-")) {
        return "user32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-rtlsupport-")) {
        return "ntdll.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-synch-l1-2-") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-3") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-4") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-5") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-6") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-7") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-8") ||
        StartsWith(lower, "api-ms-win-core-memory-l1-1-9") ||
        StartsWith(lower, "api-ms-win-core-path-") ||
        StartsWith(lower, "api-ms-win-core-version-") ||
        StartsWith(lower, "api-ms-win-core-file-l2-") ||
        StartsWith(lower, "api-ms-win-core-fibers-")) {
        return "kernelbase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-") || StartsWith(lower, "ext-ms-win-core-")) {
        return "kernel32.dll";
    }
    return dll_name;
}

void HleSymbolRegistry::RegisterSymbol(const std::string& dll_name,
                                       const std::string& symbol_name,
                                       void* address) {
    named_symbols_[MakeNamedKey(dll_name, symbol_name)] = address;
}

void HleSymbolRegistry::RegisterOrdinal(const std::string& dll_name,
                                        uint16_t ordinal,
                                        void* address) {
    ordinal_symbols_[MakeOrdinalKey(dll_name, ordinal)] = address;
}

void* HleSymbolRegistry::LookupSymbol(const std::string& dll_name,
                                      const std::string& symbol_name) const {
    auto it = named_symbols_.find(MakeNamedKey(dll_name, symbol_name));
    return it == named_symbols_.end() ? nullptr : it->second;
}

void* HleSymbolRegistry::LookupOrdinal(const std::string& dll_name, uint16_t ordinal) const {
    auto it = ordinal_symbols_.find(MakeOrdinalKey(dll_name, ordinal));
    return it == ordinal_symbols_.end() ? nullptr : it->second;
}

MappedPeImage::~MappedPeImage() {
    Unmap();
}

void MappedPeImage::Unmap() {
#if defined(_WIN32)
    if (mapped_base_ && size_of_image_ > 0) {
        RunAndClearGuestAtExitInRange(mapped_base_, size_of_image_);
        FreeGuestFlsInRange(mapped_base_, size_of_image_);
    }
    if (mapped_base_ && is_dll_ && dll_attached_ && entry_point_rva_ != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto fn = reinterpret_cast<DllMainFn>(mapped_base_ + entry_point_rva_);
        fn(reinterpret_cast<HINSTANCE>(mapped_base_), DLL_PROCESS_DETACH, nullptr);
        dll_attached_ = false;
    }
    if (auto* state = static_cast<LoaderState*>(loader_state_)) {
        if (state->registered_base) {
            guest::UnregisterModule(state->registered_base);
        }
        if (state->function_table) {
            RtlDeleteFunctionTable(state->function_table);
        }
        guest::DetachTls(&state->tls);
        delete state;
        loader_state_ = nullptr;
    }
    for (auto& gdll : mapped_guest_dlls_) {
        if (gdll) {
            gdll->Unmap();
        }
    }
    mapped_guest_dlls_.clear();
    if (!is_dll_) {
        ClearDynamicVfsDlls();
    }

    const std::string shim_dir_str = GetGuestShimDirectory();
    std::filesystem::path norm_shim_dir;
    if (!shim_dir_str.empty()) {
        std::error_code ec;
        norm_shim_dir = std::filesystem::weakly_canonical(shim_dir_str, ec);
        if (ec) norm_shim_dir = std::filesystem::path(shim_dir_str);
    }

    for (void* mod : loaded_modules_) {
        if (!mod) continue;
        // Keep ScarlettX HLE system/middleware shims (in shim_directory) and core host
        // Windows DLLs pinned for process lifetime: HLE shims own background worker
        // threads and FLS callbacks that must never be unmapped while alive.
        // Package-local DLLs loaded from the game directory are freed cleanly on Unmap().
        wchar_t mod_path[MAX_PATH + 1] = {};
        bool keep_pinned = true;
        if (GetModuleFileNameW(static_cast<HMODULE>(mod), mod_path, MAX_PATH) > 0) {
            const std::string utf8_mod = WideToUtf8(mod_path);
            const std::string base = ExtractBasenameLower(utf8_mod);
            if (!IsCoreHostWindowsDll(base) && ResolveSystemShimTarget(base) == nullptr) {
                if (ResolveFallbackMiddlewareTarget(base) == nullptr) {
                    keep_pinned = false;
                } else if (!norm_shim_dir.empty()) {
                    std::error_code ec;
                    const auto mod_parent =
                        std::filesystem::weakly_canonical(std::filesystem::path(utf8_mod).parent_path(), ec);
                    if (!ec && ToLowerAscii(mod_parent.string()) != ToLowerAscii(norm_shim_dir.string())) {
                        keep_pinned = false;
                    }
                }
            }
        }
        if (!keep_pinned) {
            size_t mod_image_size = 0;
            const auto* mod_bytes = reinterpret_cast<const uint8_t*>(mod);
            const auto* mod_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod_bytes);
            if (mod_dos->e_magic == IMAGE_DOS_SIGNATURE && mod_dos->e_lfanew > 0) {
                const auto* mod_nt =
                    reinterpret_cast<const IMAGE_NT_HEADERS64*>(mod_bytes + mod_dos->e_lfanew);
                if (mod_nt->Signature == IMAGE_NT_SIGNATURE &&
                    mod_nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                    mod_image_size = mod_nt->OptionalHeader.SizeOfImage;
                }
            }
            if (mod_image_size > 0) {
                FreeGuestFlsInRange(mod_bytes, mod_image_size);
            }
            FreeLibrary(static_cast<HMODULE>(mod));
        }
    }
    loaded_modules_.clear();
    if (mapped_base_) {
        VirtualFree(mapped_base_, 0, MEM_RELEASE);
        mapped_base_ = nullptr;
    }
#endif
    preferred_image_base_ = 0;
    size_of_image_ = 0;
    entry_point_rva_ = 0;
    export_dir_rva_ = 0;
    export_dir_size_ = 0;
    relocations_applied_ = 0;
    is_dll_ = false;
    dll_attached_ = false;
    resolutions_.clear();
    stubs_.Reset();
}

Status MappedPeImage::MapFile(const std::string& pe_path,
                              const DllRedirectionTable& redirects,
                              const HleSymbolRegistry& hle_registry,
                              const PeMapOptions& options) {
    Unmap();
#if !defined(_WIN32)
    (void)pe_path;
    (void)redirects;
    (void)hle_registry;
    (void)options;
    return Status::Error(StatusCode::NotImplemented,
                         "In-process PE64 mapping currently requires Windows x64");
#else
    std::ifstream ifs(pe_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open PE64 file: " + pe_path);
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
    return MapBytes(raw, pe_path, redirects, hle_registry, options);
#endif
}

Status MappedPeImage::MapBytes(const std::vector<uint8_t>& raw,
                               const std::string& pe_path,
                               const DllRedirectionTable& redirects,
                               const HleSymbolRegistry& hle_registry,
                               const PeMapOptions& options) {
    Unmap();
#if !defined(_WIN32)
    (void)raw;
    (void)pe_path;
    (void)redirects;
    (void)hle_registry;
    (void)options;
    return Status::Error(StatusCode::NotImplemented,
                         "In-process PE64 mapping currently requires Windows x64");
#else
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        return Status::Error(StatusCode::ParseError, "PE64 file smaller than DOS header");
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid DOS signature");
    }
    if (dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) {
        return Status::Error(StatusCode::ParseError, "Truncated NT64 headers");
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid PE signature");
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Expected x86-64 PE32+ image for Scarlett");
    }
    const size_t section_table = static_cast<size_t>(dos->e_lfanew) +
                                 offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                 nt->FileHeader.SizeOfOptionalHeader;
    if (section_table > raw.size() ||
        static_cast<size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) >
            raw.size() - section_table) {
        return Status::Error(StatusCode::ParseError, "Truncated section table");
    }

    preferred_image_base_ = nt->OptionalHeader.ImageBase;
    size_of_image_ = nt->OptionalHeader.SizeOfImage;
    entry_point_rva_ = nt->OptionalHeader.AddressOfEntryPoint;
    is_dll_ = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

    if (size_of_image_ == 0) {
        return Status::Error(StatusCode::ParseError, "Invalid zero SizeOfImage");
    }

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        const auto& exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        export_dir_rva_ = exp.VirtualAddress;
        export_dir_size_ = exp.Size;
    }

    // 1. Allocate image memory.
    void* base = nullptr;
    void* blocker = nullptr;
    if (!options.force_relocate) {
        base = VirtualAlloc(reinterpret_cast<LPVOID>(preferred_image_base_), size_of_image_,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!base) {
        base = VirtualAlloc(nullptr, size_of_image_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (options.force_relocate &&
            reinterpret_cast<uint64_t>(base) == preferred_image_base_) {
            blocker = base;
            base = VirtualAlloc(nullptr, size_of_image_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            VirtualFree(blocker, 0, MEM_RELEASE);
        }
    }
    if (!base) {
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc failed for guest PE64 image");
    }
    mapped_base_ = static_cast<uint8_t*>(base);
    if (WasRelocated() && (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0) {
        std::ostringstream msg;
        msg << "Image with stripped relocations cannot be mapped away from its preferred base 0x"
            << std::hex << preferred_image_base_;
        Unmap();
        return Status::Error(StatusCode::UnsupportedFormat, msg.str());
    }

    // 2. Copy headers and sections (bounds-checking both VirtualSize for .bss and SizeOfRawData).
    const size_t headers_copy =
        std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, std::min<size_t>(raw.size(), size_of_image_));
    std::memcpy(mapped_base_, raw.data(), headers_copy);

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        const size_t virt_span = std::max<size_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
        if (static_cast<size_t>(sec.VirtualAddress) > size_of_image_ ||
            virt_span > size_of_image_ - sec.VirtualAddress) {
            Unmap();
            return Status::Error(StatusCode::ParseError, "Section bounds exceed PE image size");
        }
        if (sec.SizeOfRawData == 0) {
            continue;
        }
        if (static_cast<size_t>(sec.PointerToRawData) > raw.size() ||
            sec.SizeOfRawData > raw.size() - sec.PointerToRawData) {
            Unmap();
            return Status::Error(StatusCode::ParseError, "Section bounds exceed PE image size");
        }
        std::memcpy(mapped_base_ + sec.VirtualAddress,
                    raw.data() + sec.PointerToRawData,
                    sec.SizeOfRawData);
    }

    // 3. Apply base relocations if mapped away from preferred ImageBase.
    const int64_t delta = static_cast<int64_t>(MappedBase() - preferred_image_base_);
    if (delta != 0 && nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        const auto& reloc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir.VirtualAddress != 0 && reloc_dir.Size > 0) {
            if (static_cast<size_t>(reloc_dir.VirtualAddress) > size_of_image_ ||
                reloc_dir.Size > size_of_image_ - reloc_dir.VirtualAddress) {
                Unmap();
                return Status::Error(StatusCode::ParseError,
                                     "Relocation directory exceeds PE image size");
            }
            size_t offset = 0;
            while (offset + sizeof(IMAGE_BASE_RELOCATION) <= reloc_dir.Size) {
                const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                    mapped_base_ + reloc_dir.VirtualAddress + offset);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                    break;
                }
                if (block->SizeOfBlock > reloc_dir.Size - offset) {
                    Unmap();
                    return Status::Error(StatusCode::ParseError,
                                         "Relocation block exceeds relocation directory");
                }
                const size_t entry_count =
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
                for (size_t e = 0; e < entry_count; ++e) {
                    const uint16_t type = entries[e] >> 12;
                    const uint16_t rel_off = entries[e] & 0x0FFFu;
                    const size_t target_rva = static_cast<size_t>(block->VirtualAddress) + rel_off;
                    if (type == IMAGE_REL_BASED_ABSOLUTE) {
                        continue;
                    } else if (type == IMAGE_REL_BASED_DIR64) {
                        if (target_rva + sizeof(uint64_t) <= size_of_image_) {
                            uint64_t val = 0;
                            std::memcpy(&val, mapped_base_ + target_rva, sizeof(val));
                            val = static_cast<uint64_t>(static_cast<int64_t>(val) + delta);
                            std::memcpy(mapped_base_ + target_rva, &val, sizeof(val));
                            ++relocations_applied_;
                        }
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        if (target_rva + sizeof(uint32_t) <= size_of_image_) {
                            uint32_t val = 0;
                            std::memcpy(&val, mapped_base_ + target_rva, sizeof(val));
                            val = static_cast<uint32_t>(static_cast<int64_t>(val) + delta);
                            std::memcpy(mapped_base_ + target_rva, &val, sizeof(val));
                            ++relocations_applied_;
                        }
                    }
                }
                offset += block->SizeOfBlock;
            }
        }
    }

    // 3.5 Initialize /GS SecurityCookie in IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG so TLS callbacks,
    //     DllMain, and exported guest entry points never trip FAST_FAIL_STACK_COOKIE_CHECK_FAILURE.
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG) {
        const auto& lc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
        constexpr size_t kMinLoadConfigWithCookie =
            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) + sizeof(ULONGLONG);
        if (lc_dir.VirtualAddress != 0 &&
            static_cast<size_t>(lc_dir.VirtualAddress) <= size_of_image_ &&
            kMinLoadConfigWithCookie <= size_of_image_ - lc_dir.VirtualAddress) {
            uint32_t lc_size = 0;
            std::memcpy(&lc_size, mapped_base_ + lc_dir.VirtualAddress, sizeof(lc_size));
            if (lc_size >= kMinLoadConfigWithCookie) {
                uint64_t cookie_va = 0;
                std::memcpy(&cookie_va,
                            mapped_base_ + lc_dir.VirtualAddress +
                                offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie),
                            sizeof(cookie_va));
                if (cookie_va >= MappedBase() &&
                    cookie_va - MappedBase() + sizeof(uint64_t) <= size_of_image_) {
                    uint8_t* cookie_ptr = mapped_base_ + (cookie_va - MappedBase());
                    uint64_t cur_cookie = 0;
                    std::memcpy(&cur_cookie, cookie_ptr, sizeof(cur_cookie));
                    if (cur_cookie == 0 || cur_cookie == kDefaultSecurityCookie ||
                        (cur_cookie >> 48) != 0) {
                        LARGE_INTEGER qpc{};
                        QueryPerformanceCounter(&qpc);
                        uint64_t new_cookie =
                            (static_cast<uint64_t>(GetCurrentProcessId()) ^
                             (static_cast<uint64_t>(GetCurrentThreadId()) << 16) ^
                             static_cast<uint64_t>(qpc.QuadPart) ^
                             reinterpret_cast<uintptr_t>(cookie_ptr)) &
                            0x0000FFFFFFFFFFFFull;
                        if (new_cookie == 0 || new_cookie == kDefaultSecurityCookie) {
                            new_cookie = kDefaultSecurityCookie ^ 1ull;
                        }
                        std::memcpy(cookie_ptr, &new_cookie, sizeof(new_cookie));
                    }
                }
            }
        }
    }

    // 4. Resolve Import Address Table (IAT) and Delay-Load Import Table:
    const bool is_virtual_g_pe =
        pe_path.size() >= 2 && pe_path[1] == ':' &&
        (pe_path[0] == 'G' || pe_path[0] == 'g' || pe_path[0] == 'D' || pe_path[0] == 'd');

    std::string registered_pe_path = pe_path;
    std::filesystem::path pkg_dir;
    std::string exe_basename;
    if (is_virtual_g_pe) {
        std::replace(registered_pe_path.begin(), registered_pe_path.end(), '/', '\\');
        const size_t last_sep = registered_pe_path.find_last_of('\\');
        exe_basename = (last_sep == std::string::npos)
                           ? registered_pe_path.substr(2)
                           : registered_pe_path.substr(last_sep + 1);
        const std::string vdir = (last_sep == std::string::npos || last_sep <= 2)
                                     ? std::string("G:\\")
                                     : registered_pe_path.substr(0, last_sep);
        SetGuestPackageDirectory(options.package_directory_override.empty()
                                     ? vdir
                                     : options.package_directory_override);
    } else {
        const std::filesystem::path abs_pe = std::filesystem::absolute(pe_path);
        registered_pe_path = abs_pe.string();
        pkg_dir = options.package_directory_override.empty()
                      ? abs_pe.parent_path()
                      : std::filesystem::path(options.package_directory_override);
        exe_basename = abs_pe.filename().string();
        SetGuestPackageDirectory(pkg_dir.string());
    }
    if (!options.shim_directory.empty()) {
        SetGuestShimDirectory(options.shim_directory);
    }
    if (!is_dll_) {
        SetGuestCommandLineForExe(exe_basename);
    }

    stubs_.SetVerboseLogging(options.verbose_stub_logging);
    std::unordered_map<std::string, HMODULE> module_cache;
    std::vector<HMODULE> package_host_modules;

    auto try_load_from_package = [&](const std::string& guest_dll,
                                     const std::string& target_dll,
                                     bool allow_target_name) -> HMODULE {
        std::vector<std::string> names;
        names.push_back(guest_dll);
        if (allow_target_name && ToLowerAscii(target_dll) != ToLowerAscii(guest_dll)) {
            names.push_back(target_dll);
        }
        std::error_code ec;
        // 1. Check PackageVfs embedded XVD files first (zero-staging .xvc / .msixvc).
        for (const std::string& fname : names) {
            if (fname.empty() || HasDotDotSegment(fname)) continue;
            const std::string vpath = "G:\\" + fname;
            VfsEntryInfo vinfo{};
            if (PackageVfs::Global().Stat(vpath, vinfo) && vinfo.exists &&
                vinfo.is_xvd_backed && !vinfo.is_directory) {
                std::shared_ptr<MappedPeImage> owned_dll;
                if (HMODULE vmod = TryMapGuestDllFromPackageVfs(vpath, &owned_dll)) {
                    if (owned_dll) {
                        mapped_guest_dlls_.push_back(std::move(owned_dll));
                    }
                    return vmod;
                }
            }
        }
        // 2. Check host package directory when directory-backed.
        if (!pkg_dir.empty()) {
            for (const std::string& fname : names) {
                if (fname.empty() || HasDotDotSegment(fname)) continue;
                const auto pkg_candidate = pkg_dir / fname;
                if (std::filesystem::exists(pkg_candidate, ec)) {
                    HMODULE mod = LoadLibraryExW(
                        pkg_candidate.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                    if (mod) {
                        package_host_modules.push_back(mod);
                        loaded_modules_.push_back(mod);
                        return mod;
                    }
                }
            }
        }
        return nullptr;
    };

    auto load_target_module = [&](const std::string& guest_dll,
                                  const std::string& target_dll,
                                  bool is_hle) -> HMODULE {
        const std::string guest_lower = ToLowerAscii(guest_dll);
        const std::string target_lower = ToLowerAscii(target_dll);
        auto it = module_cache.find(guest_lower);
        if (it != module_cache.end()) {
            return it->second;
        }

        HMODULE mod = nullptr;
        bool already_recorded = false;
        std::error_code ec;
        const bool is_apiset =
            StartsWith(guest_lower, "api-ms-win-") || StartsWith(guest_lower, "ext-ms-win-");
        const bool is_fallback_mw =
            ResolveFallbackMiddlewareTarget(guest_lower) != nullptr ||
            ResolveFallbackMiddlewareTarget(target_lower) != nullptr;

        if (is_apiset) {
            // Try Windows' native PEB ApiSetSchema resolution first so exact host
            // forwarders (e.g. VirtualAlloc2 / MapViewOfFile3 in kernelbase.dll) resolve.
            mod = LoadLibraryA(guest_dll.c_str());
            if (!mod) {
                mod = LoadLibraryA(target_dll.c_str());
            }
        } else if (is_fallback_mw) {
            // Prioritize game-shipped middleware / GDK C thunk DLL inside the package/VFS.
            // Only check target_dll in package when it has the same name (case variant), never
            // hijacking Microsoft.Xbox.Services.*.GDK.C.Thunks.dll with a package xsapi.dll!
            mod = try_load_from_package(guest_dll, target_dll, target_lower == guest_lower);
            if (mod) {
                already_recorded = true;
            }
            if (!mod && !options.shim_directory.empty()) {
                const auto candidate = std::filesystem::path(options.shim_directory) / target_dll;
                if (std::filesystem::exists(candidate, ec)) {
                    mod = LoadLibraryW(candidate.wstring().c_str());
                }
            }
            if (!mod) {
                mod = LoadLibraryA(target_dll.c_str());
            }
        } else if (is_hle) {
            // Core Xbox OS/hardware system HLE shims resolve strictly from shim_directory first.
            if (!options.shim_directory.empty()) {
                const auto candidate = std::filesystem::path(options.shim_directory) / target_dll;
                if (std::filesystem::exists(candidate, ec)) {
                    mod = LoadLibraryW(candidate.wstring().c_str());
                }
            }
            if (!mod) {
                mod = LoadLibraryA(target_dll.c_str());
            }
        } else {
            // Non-HLE DLLs (GameAssembly.dll, Oodle, Wwise, FMOD, title engine DLLs):
            // check PackageVfs and package directory first!
            if (!IsCoreHostWindowsDll(guest_lower)) {
                mod = try_load_from_package(guest_dll, target_dll, true);
                if (mod) {
                    already_recorded = true;
                }
            }
            if (!mod) {
                mod = LoadLibraryA(target_dll.c_str());
            }
        }

        if (mod && !already_recorded) {
            loaded_modules_.push_back(mod);
        }
        module_cache[guest_lower] = mod;
        return mod;
    };

    auto bind_thunk_chain = [&](const std::string& guest_dll,
                                uint32_t orig_rva,
                                uint32_t iat_rva,
                                HMODULE* out_module_handle) -> Status {
        if (iat_rva == 0 || iat_rva >= size_of_image_ || orig_rva >= size_of_image_) {
            return Status::Ok();
        }
        std::string target_dll;
        bool is_hle = false;
        if (!redirects.Resolve(guest_dll, target_dll, is_hle)) {
            target_dll = ResolveApiSetDll(guest_dll);
            is_hle = false;
        }
        const std::string target_lower = ToLowerAscii(target_dll);

        size_t thunk_idx = 0;
        for (;; ++thunk_idx) {
            const size_t cur_orig_off =
                static_cast<size_t>(orig_rva) + thunk_idx * sizeof(IMAGE_THUNK_DATA64);
            const size_t cur_iat_off =
                static_cast<size_t>(iat_rva) + thunk_idx * sizeof(IMAGE_THUNK_DATA64);
            if (cur_orig_off + sizeof(IMAGE_THUNK_DATA64) > size_of_image_ ||
                cur_iat_off + sizeof(IMAGE_THUNK_DATA64) > size_of_image_) {
                return Status::Error(StatusCode::ParseError,
                                     "Import thunk table exceeds PE image size");
            }

            const auto* orig_thunk =
                reinterpret_cast<const IMAGE_THUNK_DATA64*>(mapped_base_ + cur_orig_off);
            auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(mapped_base_ + cur_iat_off);
            if (orig_thunk->u1.AddressOfData == 0) {
                break;
            }

            const bool by_ordinal = IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal) != 0;
            uint16_t ordinal = 0;
            std::string sym_name;
            if (by_ordinal) {
                ordinal = static_cast<uint16_t>(IMAGE_ORDINAL64(orig_thunk->u1.Ordinal));
            } else {
                const uint64_t ibn_rva = orig_thunk->u1.AddressOfData;
                if (ibn_rva + 3 > size_of_image_) {
                    return Status::Error(StatusCode::ParseError,
                                         "Import-by-name RVA exceeds PE image size");
                }
                const auto* ibn =
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(mapped_base_ + ibn_rva);
                sym_name = reinterpret_cast<const char*>(ibn->Name);
            }

            PeImportResolution res;
            res.dll_name = guest_dll;
            res.target_dll = target_dll;
            res.symbol_name = sym_name;
            res.ordinal = ordinal;
            res.by_ordinal = by_ordinal;

            // (a) Check explicit in-process HLE symbol registry first.
            void* resolved = by_ordinal ? hle_registry.LookupOrdinal(guest_dll, ordinal)
                                        : hle_registry.LookupSymbol(guest_dll, sym_name);
            if (!resolved && target_lower != ToLowerAscii(guest_dll)) {
                resolved = by_ordinal ? hle_registry.LookupOrdinal(target_dll, ordinal)
                                      : hle_registry.LookupSymbol(target_dll, sym_name);
            }
            if (resolved) {
                res.kind = ImportResolutionKind::HleRegistry;
            }

            // (a.5) Guest CRT isolation, dynamic loader, VFS G:\ path safety,
            //       GetCommandLine, ExitProcess, and WinRT activation broker hooks.
            if (!resolved && !by_ordinal) {
                if (void* hook = LookupGuestRuntimeHook(target_lower, sym_name)) {
                    resolved = hook;
                    res.kind = ImportResolutionKind::HleRegistry;
                }
            }

            // (a.6) Module queries the OS loader cannot answer for a mapped image.
            if (!resolved && !by_ordinal) {
                if (void* replacement = guest::LookupImportOverride(target_lower, sym_name)) {
                    resolved = replacement;
                    res.kind = ImportResolutionKind::HleRegistry;
                }
            }

            // (b) Check host / package / HLE shim DLL via LoadLibrary + GetProcAddress.
            if (!resolved) {
                HMODULE mod = load_target_module(guest_dll, target_dll, is_hle);
                if (mod) {
                    if (out_module_handle) {
                        *out_module_handle = mod;
                    }
                    FARPROC proc = nullptr;
                    if (guest::FindModuleBase(mod) == reinterpret_cast<void*>(mod)) {
                        auto* mbase = reinterpret_cast<uint8_t*>(mod);
                        const auto* mdos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mbase);
                        const auto* mnt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mbase + mdos->e_lfanew);
                        const auto& mexp = mnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                        proc = reinterpret_cast<FARPROC>(ResolvePeExportFromImage(
                            mbase, mnt->OptionalHeader.SizeOfImage, mexp.VirtualAddress, mexp.Size,
                            by_ordinal ? MAKEINTRESOURCEA(ordinal) : sym_name.c_str(), 0));
                    } else {
                        proc = by_ordinal
                                   ? ::GetProcAddress(mod, MAKEINTRESOURCEA(ordinal))
                                   : ::GetProcAddress(mod, sym_name.c_str());
                    }
                    if (proc) {
                        resolved = reinterpret_cast<void*>(proc);
                        res.kind = ImportResolutionKind::HostDll;
                    }
                }
            }

            // (c) Fallback to dynamically generated x86-64 stub thunk.
            if (!resolved) {
                if (!options.auto_stub_missing) {
                    return Status::Error(
                        StatusCode::NotFound,
                        "Unresolved import: " + guest_dll + "!" +
                            (by_ordinal ? ("#" + std::to_string(ordinal)) : sym_name));
                }
                uint32_t stub_id = 0;
                Status st = stubs_.AllocateStub(guest_dll, sym_name, by_ordinal, ordinal,
                                                options.default_stub_return, resolved, &stub_id);
                if (!st.ok()) {
                    return st;
                }
                res.kind = ImportResolutionKind::StubThunk;
                res.stub_id = stub_id;
            }

            iat_thunk->u1.Function = reinterpret_cast<ULONGLONG>(resolved);
            res.resolved_address = reinterpret_cast<uint64_t>(resolved);
            resolutions_.push_back(std::move(res));
        }
        return Status::Ok();
    };

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp_dir.VirtualAddress != 0) {
            if (imp_dir.VirtualAddress >= size_of_image_) {
                Unmap();
                return Status::Error(StatusCode::ParseError,
                                     "Import directory exceeds PE image size");
            }
            size_t desc_off = imp_dir.VirtualAddress;
            while (desc_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= size_of_image_) {
                const auto* desc =
                    reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(mapped_base_ + desc_off);
                if (desc->Name == 0) {
                    break;
                }
                if (desc->FirstThunk == 0) {
                    desc_off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
                    continue;
                }
                if (desc->Name >= size_of_image_) {
                    Unmap();
                    return Status::Error(StatusCode::ParseError,
                                         "Import DLL name RVA exceeds PE image size");
                }
                const std::string guest_dll(reinterpret_cast<const char*>(mapped_base_ + desc->Name));
                const uint32_t orig_rva =
                    desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk;
                Status st = bind_thunk_chain(guest_dll, orig_rva, desc->FirstThunk, nullptr);
                if (!st.ok()) {
                    Unmap();
                    return st;
                }
                desc_off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            }
        }
    }

    // 4.2 Pre-bind Delay-Load Import Descriptors (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT / DataDirectory[13]).
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) {
        const auto& delay_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay_dir.VirtualAddress != 0 && delay_dir.VirtualAddress < size_of_image_) {
            auto to_rva = [&](DWORD val, bool rva_based) -> uint32_t {
                if (rva_based || val == 0) {
                    return val;
                }
                const uint64_t uval = static_cast<uint64_t>(val);
                if (uval >= preferred_image_base_ &&
                    uval - preferred_image_base_ < size_of_image_) {
                    return static_cast<uint32_t>(uval - preferred_image_base_);
                }
                return val;
            };

            size_t ddesc_off = delay_dir.VirtualAddress;
            while (ddesc_off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= size_of_image_) {
                const auto* ddesc =
                    reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(mapped_base_ + ddesc_off);
                if (ddesc->DllNameRVA == 0) {
                    break;
                }
                const bool rva_based = (ddesc->Attributes.AllAttributes & 1u) != 0;
                const uint32_t dll_rva = to_rva(ddesc->DllNameRVA, rva_based);
                const uint32_t iat_rva = to_rva(ddesc->ImportAddressTableRVA, rva_based);
                const uint32_t int_rva = to_rva(ddesc->ImportNameTableRVA, rva_based);
                const uint32_t mod_rva = to_rva(ddesc->ModuleHandleRVA, rva_based);
                if (dll_rva == 0 || dll_rva >= size_of_image_ || iat_rva == 0 || int_rva == 0) {
                    ddesc_off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
                    continue;
                }
                const std::string guest_dll(reinterpret_cast<const char*>(mapped_base_ + dll_rva));
                HMODULE bound_mod = nullptr;
                Status st = bind_thunk_chain(guest_dll, int_rva, iat_rva, &bound_mod);
                if (!st.ok()) {
                    Unmap();
                    return st;
                }
                if (bound_mod && mod_rva != 0 && mod_rva + sizeof(HMODULE) <= size_of_image_) {
                    std::memcpy(mapped_base_ + mod_rva, &bound_mod, sizeof(bound_mod));
                }
                ddesc_off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            }
        }
    }

    // 4.5 Host-process registrations: implicit TLS (while the index variable is
    //     still writable), unwind tables, and the guest module registry.
    {
        auto* state = new LoaderState();
        loader_state_ = state;
        const auto& dirs = nt->OptionalHeader.DataDirectory;
        const DWORD dir_count = nt->OptionalHeader.NumberOfRvaAndSizes;
        if (dir_count > IMAGE_DIRECTORY_ENTRY_TLS && dirs[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress != 0) {
            const auto& tls_dd = dirs[IMAGE_DIRECTORY_ENTRY_TLS];
            if (static_cast<size_t>(tls_dd.VirtualAddress) > size_of_image_ ||
                sizeof(IMAGE_TLS_DIRECTORY64) > size_of_image_ - tls_dd.VirtualAddress) {
                Unmap();
                return Status::Error(StatusCode::ParseError,
                                     "TLS directory exceeds PE image size");
            }
            Status tls_st = guest::AttachTls(
                mapped_base_, mapped_base_ + tls_dd.VirtualAddress, !is_dll_, &state->tls);
            if (!tls_st.ok()) {
                Unmap();
                return tls_st;
            }
        }
        if (dir_count > IMAGE_DIRECTORY_ENTRY_EXCEPTION &&
            dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress != 0) {
            const auto& exc_dir = dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (static_cast<size_t>(exc_dir.VirtualAddress) > size_of_image_ ||
                exc_dir.Size > size_of_image_ - exc_dir.VirtualAddress) {
                Unmap();
                return Status::Error(StatusCode::ParseError,
                                     "Exception directory (.pdata) exceeds PE image size");
            }
            auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(mapped_base_ + exc_dir.VirtualAddress);
            DWORD count = exc_dir.Size / sizeof(RUNTIME_FUNCTION);
            while (count > 0 &&
                   table[count - 1].BeginAddress == 0 &&
                   table[count - 1].EndAddress == 0 &&
                   table[count - 1].UnwindData == 0) {
                --count;
            }
            if (count != 0 &&
                RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(mapped_base_))) {
                state->function_table = table;
            }
        }
        guest::RegisterModule(mapped_base_, size_of_image_, registered_pe_path, !is_dll_);
        state->registered_base = mapped_base_;
        for (void* mod : loaded_modules_) {
            const bool force_pkg =
                std::find(package_host_modules.begin(), package_host_modules.end(),
                          static_cast<HMODULE>(mod)) != package_host_modules.end();
            guest::PatchHostModuleForGuests(mod, force_pkg);
        }
    }

    // 5. Apply per-section memory protections and flush instruction cache.
    DWORD old_prot = 0;
    VirtualProtect(mapped_base_, headers_copy, PAGE_READONLY, &old_prot);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        const size_t prot_size = std::max<size_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
        if (prot_size == 0) {
            continue;
        }
        const DWORD prot = SectionCharsToPageProtect(sec.Characteristics);
        VirtualProtect(mapped_base_ + sec.VirtualAddress, prot_size, prot, &old_prot);
    }
    FlushInstructionCache(GetCurrentProcess(), mapped_base_, size_of_image_);

    // 6. Execute TLS callbacks if present.
    if (options.run_tls_callbacks &&
        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_TLS) {
        const auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls_dir.VirtualAddress != 0) {
            const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
                mapped_base_ + tls_dir.VirtualAddress);
            if (tls->AddressOfCallBacks != 0) {
                const auto* cb = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
                while (cb && *cb) {
                    (*cb)(mapped_base_, DLL_PROCESS_ATTACH, nullptr);
                    ++cb;
                }
            }
            if (auto* state = static_cast<LoaderState*>(loader_state_)) {
                guest::EnableTlsCallbacks(&state->tls);
            }
        }
    }

    // 7. For DLL images, invoke DllMain(DLL_PROCESS_ATTACH).
    if (is_dll_ && entry_point_rva_ != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto fn = reinterpret_cast<DllMainFn>(mapped_base_ + entry_point_rva_);
        if (!fn(reinterpret_cast<HINSTANCE>(mapped_base_), DLL_PROCESS_ATTACH, nullptr)) {
            Unmap();
            return Status::Error(StatusCode::DeviceError,
                                 "Guest DLL DllMain(DLL_PROCESS_ATTACH) returned FALSE");
        }
        dll_attached_ = true;
    }

    return Status::Ok();
#endif
}

void* MappedPeImage::EntryPointAddress() const noexcept {
    if (!mapped_base_ || entry_point_rva_ == 0 || entry_point_rva_ >= size_of_image_) {
        return nullptr;
    }
    return mapped_base_ + entry_point_rva_;
}

void* MappedPeImage::GetExportAddress(const std::string& export_name) const {
#if defined(_WIN32)
    if (!mapped_base_ || export_dir_rva_ == 0 || export_name.empty()) {
        return nullptr;
    }
    return ResolvePeExportFromImage(mapped_base_, size_of_image_, export_dir_rva_,
                                    export_dir_size_, export_name.c_str(), 0);
#else
    (void)export_name;
    return nullptr;
#endif
}

void* MappedPeImage::GetExportAddress(uint16_t ordinal) const {
#if defined(_WIN32)
    if (!mapped_base_ || export_dir_rva_ == 0 || ordinal == 0) {
        return nullptr;
    }
    return ResolvePeExportFromImage(mapped_base_, size_of_image_, export_dir_rva_,
                                    export_dir_size_, MAKEINTRESOURCEA(ordinal), 0);
#else
    (void)ordinal;
    return nullptr;
#endif
}

Status MappedPeImage::CallExportedEntry(const std::string& export_name, int& out_exit_code) const {
    out_exit_code = -1;
    void* addr = GetExportAddress(export_name);
    if (!addr) {
        return Status::Error(StatusCode::NotFound,
                             "Exported guest entry not found: " + export_name);
    }
    using GuestEntryFn = int (*)();
    auto fn = reinterpret_cast<GuestEntryFn>(addr);
#if defined(_WIN32)
    out_exit_code = InvokeGuestEntryGuarded(fn);
#else
    out_exit_code = fn();
#endif
    return Status::Ok();
}

Status MappedPeImage::CallEntryPoint(int& out_exit_code) const {
    out_exit_code = -1;
    void* addr = EntryPointAddress();
    if (!addr) {
        return Status::Error(StatusCode::NotFound, "PE image has no entry point");
    }
    using NativeEntryFn = int (*)();
    auto fn = reinterpret_cast<NativeEntryFn>(addr);
#if defined(_WIN32)
    out_exit_code = InvokeGuestEntryGuarded(fn);
#else
    out_exit_code = fn();
#endif
    return Status::Ok();
}

void TryAbortActiveInProcessGuest(uint32_t exit_code) noexcept {
#if defined(_WIN32)
    if (g_guest_entry_depth > 0) {
        GuestExitProcess(static_cast<UINT>(exit_code));
    }
#else
    (void)exit_code;
#endif
}

}  // namespace scarlettx
