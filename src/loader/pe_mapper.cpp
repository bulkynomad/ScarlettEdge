#include "scarlettx/loader/pe_mapper.h"

#include "guest_modules.h"
#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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
    struct Mapping {
        const char* guest;
        const char* target;
    };
    static constexpr Mapping kSystemShims[] = {
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
        {"microsoft.xbox.services.140.gdk.c.thunks.dll", "xsapi.dll"},
        {"microsoft.xbox.services.141.gdk.c.thunks.dll", "xsapi.dll"},
        {"microsoft.xbox.services.142.gdk.c.thunks.dll", "xsapi.dll"},
        {"xs2_lifting.dll", "xs2_lifting.dll"},
        {"vulkan_backend.dll", "vulkan_backend.dll"},
        {"pixevt.dll", "pixevt_stub.dll"},
        {"pixevt_stub.dll", "pixevt_stub.dll"},
    };
    for (const auto& m : kSystemShims) {
        if (lower_name == m.guest) return m.target;
    }
    return nullptr;
}

// Title-shipped middleware DLLs: prioritize the real DLL inside the game package
// directory when present, falling back to ScarlettEdge HLE shims only when absent.
const char* ResolveFallbackMiddlewareTarget(const std::string& lower_name) {
    struct Mapping {
        const char* guest;
        const char* target;
    };
    static constexpr Mapping kMiddlewareShims[] = {
        {"bink2scarlett.dll", "Bink2Scarlett.dll"},
        {"amd_fidelityfx_loader_xbox.dll", "amd_fidelityfx_loader_xbox.dll"},
        {"amd_fidelityfx_upscaler_xbox.dll", "amd_fidelityfx_upscaler_xbox.dll"},
        {"libhttpclient.gdk.dll", "libHttpClient.GDK.dll"},
        {"xcurl.dll", "XCurl.dll"},
    };
    for (const auto& m : kMiddlewareShims) {
        if (lower_name == m.guest) return m.target;
    }
    return nullptr;
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

// Intercepts guest G:\ or G:/ (including \\?\G:\) paths and resolves them via
// PackageVfs::Global() first (falling back to GuestPackageDirectory if VFS was
// not mounted), ensuring guest I/O never touches a host G:\ drive (e.g. Google Drive).
enum class GuestPathResult {
    NotGuestDrive,
    Resolved,
    Blocked,
};

GuestPathResult RedirectGuestPackagePath(const std::string& raw_path, std::string& out_host_path) {
    if (raw_path.empty()) {
        return GuestPathResult::NotGuestDrive;
    }
    size_t prefix = 0;
    if (raw_path.size() >= 6 &&
        (raw_path[0] == '\\' || raw_path[0] == '/') &&
        (raw_path[1] == '\\' || raw_path[1] == '/') &&
        (raw_path[2] == '?' || raw_path[2] == '.') &&
        (raw_path[3] == '\\' || raw_path[3] == '/')) {
        prefix = 4;
    }
    if (raw_path.size() < prefix + 2) {
        return GuestPathResult::NotGuestDrive;
    }
    const char drive = raw_path[prefix];
    const char drive_upper = static_cast<char>(std::toupper(static_cast<unsigned char>(drive)));
    if ((drive_upper != 'G' && drive_upper != 'T' && drive_upper != 'D') || raw_path[prefix + 1] != ':') {
        return GuestPathResult::NotGuestDrive;
    }
    if (raw_path.size() > prefix + 2 &&
        raw_path[prefix + 2] != '\\' && raw_path[prefix + 2] != '/') {
        return GuestPathResult::NotGuestDrive;
    }

    const std::string vpath = raw_path.substr(prefix);
    if (HasDotDotSegment(vpath)) {
        return GuestPathResult::Blocked;
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
        out_host_path = rel.empty() ? dir.string() : (dir / rel).string();
        return GuestPathResult::Resolved;
    }

    // G:\ and D:\ -> PackageVfs / Package Root
    const std::string g_vpath = (drive_upper == 'D') ? ("G:\\" + rel) : vpath;
    Status st = PackageVfs::Global().ResolvePath(g_vpath, out_host_path);
    if (st.ok()) {
        return GuestPathResult::Resolved;
    }
    if (st.code == StatusCode::InvalidArgument) {
        return GuestPathResult::Blocked;
    }
    const std::string pkg_dir = GetGuestPackageDirectory();
    if (!pkg_dir.empty()) {
        out_host_path = rel.empty() ? pkg_dir : (std::filesystem::path(pkg_dir) / rel).string();
        return GuestPathResult::Resolved;
    }
    return GuestPathResult::Blocked;
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
            HANDLE h = !wide.empty()
                ? ::CreateFileW(wide.c_str(), dwDesiredAccess, dwShareMode,
                                lpSecurityAttributes, dwCreationDisposition,
                                dwFlagsAndAttributes, hTemplateFile)
                : ::CreateFileA(host_path.c_str(), dwDesiredAccess, dwShareMode,
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
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

DWORD WINAPI GuestGetFileAttributesA(LPCSTR lpFileName) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath(lpFileName);
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
            const DWORD attr = wide.empty() ? ::GetFileAttributesA(host_path.c_str())
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
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

BOOL WINAPI GuestGetFileAttributesExA(LPCSTR lpFileName,
                                      GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
    if (lpFileName) {
        const std::string raw_vpath(lpFileName);
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return FALSE;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
            const BOOL ok = wide.empty()
                ? ::GetFileAttributesExA(host_path.c_str(), fInfoLevelId, lpFileInformation)
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(raw_vpath, host_path);
        if (r == GuestPathResult::Blocked) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            RecordGuestVfsResultPreservingLastError(raw_vpath, false, caller_rip);
            return FALSE;
        }
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(lpFileName, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
            return ::FindFirstFileA(host_path.c_str(), lpFindFileData);
        }
    }
    return ::FindFirstFileA(lpFileName, lpFindFileData);
}

HANDLE WINAPI GuestFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (lpFileName) {
        std::string utf8 = WideToUtf8(lpFileName);
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(utf8, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
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
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(lpFileName, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
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
        std::string utf8 = WideToUtf8(lpFileName);
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(utf8, host_path);
        if (r == GuestPathResult::Blocked) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (r == GuestPathResult::Resolved) {
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

BOOL WINAPI GuestGetDiskFreeSpaceExA(LPCSTR lpDirectoryName,
                                     PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                                     PULARGE_INTEGER lpTotalNumberOfBytes,
                                     PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    if (lpDirectoryName) {
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(lpDirectoryName, host_path);
        if (r == GuestPathResult::Resolved) {
            return ::GetDiskFreeSpaceExA(host_path.c_str(), lpFreeBytesAvailableToCaller,
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
        std::string utf8 = WideToUtf8(lpDirectoryName);
        std::string host_path;
        const GuestPathResult r = RedirectGuestPackagePath(utf8, host_path);
        if (r == GuestPathResult::Resolved) {
            const std::wstring wide = Utf8ToWide(host_path);
            if (!wide.empty()) {
                return ::GetDiskFreeSpaceExW(wide.c_str(), lpFreeBytesAvailableToCaller,
                                             lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
            }
        }
    }
    return ::GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller,
                                 lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
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

// A title DLL loaded by the host loader still has to see the guest image when
// it asks which module it runs in.
HMODULE AdoptHostModule(HMODULE mod) {
    if (mod) {
        const DWORD saved_error = GetLastError();
        guest::PatchHostModuleForGuests(mod);
        SetLastError(saved_error);
    }
    return mod;
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
    const std::string base = ExtractBasenameLower(raw);
    if (ResolveSystemShimTarget(base) != nullptr) {
        return nullptr;
    }

    const char* mw_target = ResolveFallbackMiddlewareTarget(base);
    const std::string pkg_dir = GetGuestPackageDirectory();
    std::error_code ec;

    // 1. Check G:\ virtual package path if explicitly specified.
    std::string vfs_resolved;
    const GuestPathResult vr = RedirectGuestPackagePath(raw, vfs_resolved);
    if (vr == GuestPathResult::Resolved && std::filesystem::exists(vfs_resolved, ec)) {
        const std::wstring wide = Utf8ToWide(vfs_resolved);
        if (!wide.empty()) {
            const DWORD eff_flags = dwFlags ? dwFlags : LOAD_WITH_ALTERED_SEARCH_PATH;
            if (HMODULE mod = LoadLibraryExW(wide.c_str(), nullptr, eff_flags)) {
                return AdoptHostModule(mod);
            }
        }
    }

    // 2. Check package directory for ALL non-system DLLs (Oodle, Wwise, FMOD,
    //    engine plugins, plus title-shipped middleware).
    if (!pkg_dir.empty() && !IsCoreHostWindowsDll(base) && raw.find(':') == std::string::npos &&
        !HasDotDotSegment(raw)) {
        std::vector<std::string> candidates;
        if (mw_target) {
            candidates.emplace_back(mw_target);
        }
        candidates.push_back(raw);
        if (base != ToLowerAscii(raw)) {
            candidates.push_back(base);
        }
        for (const std::string& rel : candidates) {
            const auto pkg_candidate = std::filesystem::path(pkg_dir) / rel;
            if (std::filesystem::exists(pkg_candidate, ec)) {
                const std::wstring wide = pkg_candidate.wstring();
                const DWORD eff_flags = dwFlags ? dwFlags : LOAD_WITH_ALTERED_SEARCH_PATH;
                if (HMODULE mod = LoadLibraryExW(wide.c_str(), nullptr, eff_flags)) {
                    return AdoptHostModule(mod);
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
        std::string host_path;
        if (RedirectGuestPackagePath(lpLibFileName, host_path) == GuestPathResult::Blocked) {
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
        std::string host_path;
        if (RedirectGuestPackagePath(narrow, host_path) == GuestPathResult::Blocked) {
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
        if (sym_name == "CreateFileA") return reinterpret_cast<void*>(&GuestCreateFileA);
        if (sym_name == "CreateFileW") return reinterpret_cast<void*>(&GuestCreateFileW);
        if (sym_name == "CreateFile2") return reinterpret_cast<void*>(&GuestCreateFile2);
        if (sym_name == "GetFileAttributesA") return reinterpret_cast<void*>(&GuestGetFileAttributesA);
        if (sym_name == "GetFileAttributesW") return reinterpret_cast<void*>(&GuestGetFileAttributesW);
        if (sym_name == "GetFileAttributesExA") return reinterpret_cast<void*>(&GuestGetFileAttributesExA);
        if (sym_name == "GetFileAttributesExW") return reinterpret_cast<void*>(&GuestGetFileAttributesExW);
        if (sym_name == "FindFirstFileA") return reinterpret_cast<void*>(&GuestFindFirstFileA);
        if (sym_name == "FindFirstFileW") return reinterpret_cast<void*>(&GuestFindFirstFileW);
        if (sym_name == "FindFirstFileExA") return reinterpret_cast<void*>(&GuestFindFirstFileExA);
        if (sym_name == "FindFirstFileExW") return reinterpret_cast<void*>(&GuestFindFirstFileExW);
        if (sym_name == "GetDiskFreeSpaceExA") return reinterpret_cast<void*>(&GuestGetDiskFreeSpaceExA);
        if (sym_name == "GetDiskFreeSpaceExW") return reinterpret_cast<void*>(&GuestGetDiskFreeSpaceExW);
        if (sym_name == "GetSystemInfo") return reinterpret_cast<void*>(&GuestGetSystemInfo);
        if (sym_name == "GetNativeSystemInfo") return reinterpret_cast<void*>(&GuestGetNativeSystemInfo);
        if (sym_name == "SetThreadAffinityMask") return reinterpret_cast<void*>(&GuestSetThreadAffinityMask);
        if (sym_name == "FlsAlloc") return reinterpret_cast<void*>(&GuestFlsAlloc);
        if (sym_name == "FlsFree") return reinterpret_cast<void*>(&GuestFlsFree);
        return nullptr;
    }
    if (target_lower == "combase.dll") {
        if (sym_name == "RoActivateInstance") return reinterpret_cast<void*>(&GuestRoActivateInstance);
        if (sym_name == "RoGetActivationFactory") return reinterpret_cast<void*>(&GuestRoGetActivationFactory);
        return nullptr;
    }
    return nullptr;
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
    for (void* mod : loaded_modules_) {
        if (!mod) continue;
        // Keep ScarlettX HLE system/middleware shims and core host Windows DLLs
        // pinned for process lifetime: HLE shims (xgameruntime, dstorage_xs, xaudio2_9,
        // libHttpClient, etc.) own background worker threads and FLS callbacks that
        // must never be unmapped while the host process is alive.
        wchar_t mod_path[MAX_PATH + 1] = {};
        bool keep_pinned = true;
        if (GetModuleFileNameW(static_cast<HMODULE>(mod), mod_path, MAX_PATH) > 0) {
            const std::string base = ExtractBasenameLower(WideToUtf8(mod_path));
            if (!IsCoreHostWindowsDll(base) &&
                ResolveSystemShimTarget(base) == nullptr &&
                ResolveFallbackMiddlewareTarget(base) == nullptr) {
                keep_pinned = false;
            }
        }
        if (!keep_pinned) {
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
    const std::filesystem::path abs_pe = std::filesystem::absolute(pe_path);
    const std::filesystem::path pkg_dir = abs_pe.parent_path();
    SetGuestPackageDirectory(pkg_dir.string());
    if (!options.shim_directory.empty()) {
        SetGuestShimDirectory(options.shim_directory);
    }
    if (!is_dll_) {
        SetGuestCommandLineForExe(abs_pe.filename().string());
    }

    stubs_.SetVerboseLogging(options.verbose_stub_logging);
    std::unordered_map<std::string, HMODULE> module_cache;

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
            // Prioritize game-shipped middleware DLL inside the package directory.
            for (const std::string& fname : {guest_dll, target_dll}) {
                const auto pkg_candidate = pkg_dir / fname;
                if (std::filesystem::exists(pkg_candidate, ec)) {
                    mod = LoadLibraryExW(pkg_candidate.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                    if (mod) break;
                }
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
            // Non-HLE DLLs (Oodle, Wwise, FMOD, title engine DLLs): check package directory first!
            if (!IsCoreHostWindowsDll(guest_lower)) {
                for (const std::string& fname : {guest_dll, target_dll}) {
                    if (fname.empty() || HasDotDotSegment(fname)) continue;
                    const auto pkg_candidate = pkg_dir / fname;
                    if (std::filesystem::exists(pkg_candidate, ec)) {
                        mod = LoadLibraryExW(pkg_candidate.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                        if (mod) break;
                    }
                }
            }
            if (!mod) {
                mod = LoadLibraryA(target_dll.c_str());
            }
        }

        if (mod) {
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
                    FARPROC proc = by_ordinal
                                       ? ::GetProcAddress(mod, MAKEINTRESOURCEA(ordinal))
                                       : ::GetProcAddress(mod, sym_name.c_str());
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
        guest::RegisterModule(mapped_base_, size_of_image_, abs_pe.string(), !is_dll_);
        state->registered_base = mapped_base_;
        for (void* mod : loaded_modules_) {
            guest::PatchHostModuleForGuests(mod);
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

}  // namespace scarlettx
