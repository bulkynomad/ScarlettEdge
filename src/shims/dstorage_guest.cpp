// Guest-facing DirectStorage objects for real titles.
//
// A title imports a single function, XCRDAPI.dll!DStorageGetFactory, and
// drives everything else through COM-style objects: the factory, queues,
// files and status arrays. Requests are read from host files or PackageVfs
// on one worker thread per queue, in the order they were enqueued.
#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/io/deflate_rfc1951.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/media/codecs_abi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kAbort = static_cast<int32_t>(0x80004004u);
constexpr int32_t kPending = static_cast<int32_t>(0x8000000Au);
constexpr int32_t kOutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kStagingBufferTooSmall = static_cast<int32_t>(0x89240007u);

constexpr uint16_t kMinQueueCapacity = 0x80;
constexpr uint16_t kMaxQueueCapacity = 0x2000;

// DSTORAGE_REQUEST_OPTIONS bits.
constexpr uint64_t kOptZlib = 1ull << 0;
constexpr uint64_t kOptBcpack = 0xFull << 1;
constexpr uint64_t kOptSwizzle = 0xFFFFull << 5;
constexpr uint64_t kOptDestinationPhysical = 1ull << 21;
constexpr uint64_t kOptSourceMemory = 1ull << 22;
constexpr uint64_t kOptSourcePhysical = 1ull << 23;
constexpr uint64_t kOptCompressionFormatMask = 0xFFull << 24;
constexpr uint8_t kCompressionFormatNone = 0u;
constexpr uint8_t kCompressionFormatGDeflate = 1u;
constexpr uint64_t kOptUnsupported = kOptDestinationPhysical | kOptSourcePhysical;

bool TraceEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

#define DS_LOG(...)                                                \
    do {                                                           \
        std::fprintf(stderr, "[ScarlettX][dstorage] " __VA_ARGS__); \
        std::fputc('\n', stderr);                                  \
        std::fflush(stderr);                                       \
    } while (0)

#define DS_TRACE(limit, ...)                                             \
    do {                                                                 \
        static std::atomic<int> ds_trace_count{0};                       \
        if (TraceEnabled() && ds_trace_count.fetch_add(1) < (limit)) {   \
            DS_LOG(__VA_ARGS__);                                         \
        }                                                                \
    } while (0)

int32_t HresultFromWin32(DWORD error) {
    return error == 0 ? kOk : static_cast<int32_t>((error & 0xFFFFu) | 0x80070000u);
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

uint32_t BcPackModeToDxgiFormat(uint32_t bcpack_mode) {
    switch (bcpack_mode) {
        case 1u: return 71u;  // DXGI_FORMAT_BC1_UNORM
        case 2u: return 74u;  // DXGI_FORMAT_BC2_UNORM
        case 3u: return 77u;  // DXGI_FORMAT_BC3_UNORM
        case 4u: return 80u;  // DXGI_FORMAT_BC4_UNORM
        case 5u: return 83u;  // DXGI_FORMAT_BC5_UNORM
        case 6u: return 95u;  // DXGI_FORMAT_BC6H_UF16
        case 7u: return 98u;  // DXGI_FORMAT_BC7_UNORM
        case 8u: return 99u;  // DXGI_FORMAT_BC7_UNORM_SRGB
        default: return 28u;  // DXGI_FORMAT_R8G8B8A8_UNORM
    }
}

// DSTORAGE_QUEUE_DESC
struct QueueDesc {
    uint64_t source_type;  // 0 = file, 1 = memory
    uint16_t capacity;
    int8_t priority;
    const char* name;
};
static_assert(sizeof(QueueDesc) == 0x18 && offsetof(QueueDesc, name) == 0x10, "DSTORAGE_QUEUE_DESC layout");

// DSTORAGE_REQUEST (GDK dstorage_xs.h)
struct Request {
    uint64_t options;
    void* destination;
    uint16_t destination_page_offset;
    uint16_t destination_pad[3];
    uint32_t destination_size;
    uint32_t reserved;
    void* source;                      // IDStorageFileX* or the source memory
    uint64_t file_offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t cancellation_tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48, "DSTORAGE_REQUEST size");
static_assert(offsetof(Request, destination_size) == 0x18 && offsetof(Request, source) == 0x20 &&
                  offsetof(Request, source_size) == 0x30 && offsetof(Request, name) == 0x40,
              "DSTORAGE_REQUEST layout");

// DSTORAGE_ERROR_RECORD (GDK dstorage_xs.h)
struct GuestErrorFirstFailure {
    int32_t hresult;
    int32_t command_type;  // -1 = NONE, 0 = REQUEST, 1 = STATUS, 2 = SIGNAL
    Request request;
};

struct GuestErrorRecord {
    uint32_t failure_count;
    GuestErrorFirstFailure first_failure;
};
static_assert(sizeof(GuestErrorRecord) == 0x58, "DSTORAGE_ERROR_RECORD layout");

struct Object {
    void* const* vtbl = nullptr;
    std::atomic<int32_t> refs{1};
    void (*destroy)(Object*) = nullptr;
};

uint32_t WINAPI ObjectAddRef(Object* self) {
    return static_cast<uint32_t>(self->refs.fetch_add(1) + 1);
}

uint32_t WINAPI ObjectRelease(Object* self) {
    const int32_t left = self->refs.fetch_sub(1) - 1;
    if (left == 0 && self->destroy) self->destroy(self);
    return static_cast<uint32_t>(left);
}

int32_t WINAPI ObjectQueryInterface(Object* self, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    ObjectAddRef(self);
    *out = self;
    return kOk;
}

const char* const kInterfaceNames[] = {"IDStorageFactoryX", "IDStorageQueueX", "IDStorageFileX",
                                       "IDStorageStatusArrayX"};

template <int Interface, int Slot>
intptr_t WINAPI UnnamedSlot(Object* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        kInterfaceNames[Interface], Slot, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1) < 4) {
        DS_LOG("%s slot %d is not implemented (0x%llx, 0x%llx, 0x%llx)", kInterfaceNames[Interface], Slot,
               static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
               static_cast<unsigned long long>(a3));
    }
    return static_cast<intptr_t>(kNotImpl);
}

#define DS_SLOT(fn) reinterpret_cast<void*>(&fn)

// ---- status array -----------------------------------------------------------

struct StatusEntry {
    std::atomic<bool> complete{true};
    std::atomic<int32_t> hr{kOk};
};

struct StatusArray : Object {
    uint32_t capacity = 0;
    StatusEntry* entries = nullptr;
};

void DestroyStatusArray(Object* self) {
    auto* array = static_cast<StatusArray*>(self);
    delete[] array->entries;
    delete array;
}

int32_t WINAPI StatusIsComplete(Object* self, uint32_t index) {
    auto* array = static_cast<StatusArray*>(self);
    return index < array->capacity && array->entries[index].complete.load() ? 1 : 0;
}

int32_t WINAPI StatusGetHResult(Object* self, uint32_t index) {
    auto* array = static_cast<StatusArray*>(self);
    if (index >= array->capacity) return kInvalidArg;
    const StatusEntry& entry = array->entries[index];
    return entry.complete.load() ? entry.hr.load() : kPending;
}

void* const kStatusArrayVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(StatusIsComplete),   // 3, confirmed
    DS_SLOT(StatusGetHResult),   // 4, confirmed
    DS_SLOT((UnnamedSlot<3, 5>)),
};

// ---- file -------------------------------------------------------------------

struct File : Object {
    std::mutex mu;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool is_vfs = false;
    bool closed = false;
    std::string vfs_path;
    uint64_t vfs_size = 0;
};

void CloseFileLocked(File* file) {
    file->closed = true;
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
}

void DestroyFile(Object* self) {
    auto* file = static_cast<File*>(self);
    {
        std::lock_guard<std::mutex> lock(file->mu);
        CloseFileLocked(file);
    }
    delete file;
}

void WINAPI FileClose(Object* self) {
    auto* file = static_cast<File*>(self);
    std::lock_guard<std::mutex> lock(file->mu);
    CloseFileLocked(file);
}

// The title owns the returned handle and closes it.
HANDLE WINAPI FileGetHandle(Object* self) {
    auto* file = static_cast<File*>(self);
    std::lock_guard<std::mutex> lock(file->mu);
    if (file->closed) return INVALID_HANDLE_VALUE;

    if (file->handle == INVALID_HANDLE_VALUE && file->is_vfs) {
        wchar_t tmp_dir[MAX_PATH] = {};
        wchar_t tmp_file[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp_dir) != 0 && GetTempFileNameW(tmp_dir, L"sxv", 0, tmp_file) != 0) {
            HANDLE h = CreateFileW(
                tmp_file,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                if (file->vfs_size <= 4ull * 1024ull * 1024ull) {
                    std::vector<uint8_t> bytes;
                    if (scarlettx::PackageVfs::Global().ReadFileRange(file->vfs_path, 0, file->vfs_size, bytes).ok() &&
                        !bytes.empty()) {
                        DWORD wr = 0;
                        (void)WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wr, nullptr);
                    }
                } else {
                    DWORD ret_bytes = 0;
                    (void)DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret_bytes, nullptr);
                    LARGE_INTEGER li{};
                    li.QuadPart = static_cast<LONGLONG>(file->vfs_size);
                    if (SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
                        (void)SetEndOfFile(h);
                    }
                }
                LARGE_INTEGER zero{};
                (void)SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
                file->handle = h;
            }
        }
    }

    HANDLE copy = nullptr;
    if (file->handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), file->handle, GetCurrentProcess(), &copy, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return INVALID_HANDLE_VALUE;
    }
    return copy;
}

void* const kFileVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(FileClose),      // 3, confirmed
    DS_SLOT(FileGetHandle),  // 4, confirmed
    DS_SLOT((UnnamedSlot<2, 5>)),
};

// ---- queue ------------------------------------------------------------------

struct Entry {
    bool is_status = false;
    bool is_signal = false;
    bool cancelled = false;
    Request request{};
    File* file = nullptr;           // held until the request has run
    StatusArray* status = nullptr;  // held until the status is written
    uint32_t index = 0;
    void* fence = nullptr;          // held until the signal is written
    uint64_t fence_value = 0;
};

void ReleaseEntry(const Entry& entry) {
    if (entry.file) ObjectRelease(entry.file);
    if (entry.status) ObjectRelease(entry.status);
    if (entry.fence) {
        void** vtbl = *reinterpret_cast<void***>(entry.fence);
        using ReleaseFn = uint32_t (WINAPI*)(void*);
        reinterpret_cast<ReleaseFn>(vtbl[2])(entry.fence);
    }
}

struct Queue : Object {
    uint64_t source_type = 0;
    uint16_t capacity = 0;
    std::mutex mu;
    std::condition_variable wake;
    std::vector<Entry> pending;
    std::deque<Entry> submitted;
    std::thread worker;
    bool stop = false;
    int32_t first_failure = kOk;   // since the last status write; worker thread only
    GuestErrorRecord error_record{0u, {kOk, -1, {}}};
};

std::atomic<uint32_t> g_staging_buffer_size{32u * 1024u * 1024u};

int32_t RunRequest(const Queue* queue, const Entry& entry) {
    const Request& r = entry.request;
    if (r.options & kOptUnsupported) return kNotImpl;
    const bool from_memory = (r.options & kOptSourceMemory) != 0;
    if (from_memory != (queue->source_type == 1)) return kInvalidArg;
    if (r.source_size == 0) return kOk;
    const bool is_zlib = (r.options & kOptZlib) != 0;
    const uint8_t comp_fmt = static_cast<uint8_t>((r.options & kOptCompressionFormatMask) >> 24);
    if (comp_fmt != kCompressionFormatNone && comp_fmt != kCompressionFormatGDeflate) {
        return kNotImpl;
    }
    const bool is_gdeflate = (comp_fmt == kCompressionFormatGDeflate);
    const uint32_t bcpack_fmt = static_cast<uint32_t>((r.options & kOptBcpack) >> 1);
    const uint32_t swizzle_opt = static_cast<uint32_t>((r.options & kOptSwizzle) >> 5);
    const bool compressed = is_zlib || is_gdeflate || (bcpack_fmt != 0);
    if (!r.destination || !r.source || (!compressed && swizzle_opt == 0 && r.destination_size < r.source_size)) {
        return kInvalidArg;
    }

    const uint32_t staging_limit = g_staging_buffer_size.load(std::memory_order_acquire);
    if (!from_memory) {
        if (staging_limit == 0u) {
            return kStagingBufferTooSmall;
        }
        if (compressed || swizzle_opt != 0) {
            const uint32_t needed_intermediate =
                (bcpack_fmt != 0u && (is_gdeflate || is_zlib))
                    ? ((r.intermediate_size != 0u) ? r.intermediate_size : r.destination_size)
                    : 0u;
            if (r.source_size > staging_limit || needed_intermediate > staging_limit) {
                return kStagingBufferTooSmall;
            }
        }
    }

    // Only apply destination_page_offset when DestinationIsPhysicalPages is set;
    // virtual-memory requests leave uninitialized stack bytes in destination_page_offset / pad.
    uint8_t* dst_ptr = static_cast<uint8_t*>(r.destination) +
                       ((r.options & kOptDestinationPhysical) ? static_cast<size_t>(r.destination_page_offset) : 0u);
    std::vector<uint8_t> raw_buf;
    std::vector<uint8_t> swizzle_linear_buf;
    uint8_t* decode_dst = dst_ptr;
    uint32_t decode_cap = r.destination_size;
    if (swizzle_opt != 0) {
        decode_cap = std::max<uint32_t>(r.destination_size, r.source_size);
        swizzle_linear_buf.assign(decode_cap, 0u);
        decode_dst = swizzle_linear_buf.data();
    }

    uint8_t* io_target = (compressed || swizzle_opt != 0) ? nullptr : dst_ptr;
    if (compressed || swizzle_opt != 0) {
        raw_buf.resize(r.source_size);
        io_target = raw_buf.data();
    }

    if (from_memory) {
        std::memcpy(io_target, r.source, r.source_size);
    } else {
        if (!entry.file) return HresultFromWin32(ERROR_INVALID_HANDLE);
        HANDLE dup_handle = INVALID_HANDLE_VALUE;
        bool use_vfs = false;
        std::string vfs_path;
        uint64_t vfs_size = 0;
        {
            std::lock_guard<std::mutex> lock(entry.file->mu);
            if (entry.file->closed) {
                return HresultFromWin32(ERROR_INVALID_HANDLE);
            }
            if (entry.file->is_vfs && entry.file->handle == INVALID_HANDLE_VALUE) {
                use_vfs = true;
                vfs_path = entry.file->vfs_path;
                vfs_size = entry.file->vfs_size;
            } else if (entry.file->handle == INVALID_HANDLE_VALUE ||
                       !DuplicateHandle(GetCurrentProcess(), entry.file->handle, GetCurrentProcess(),
                                        &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                return HresultFromWin32(ERROR_INVALID_HANDLE);
            }
        }

        if (use_vfs) {
            if (r.file_offset > vfs_size || r.source_size > vfs_size - r.file_offset) {
                return HresultFromWin32(ERROR_HANDLE_EOF);
            }
            std::vector<uint8_t> vfs_slice;
            uint32_t done = 0u;
            while (done < r.source_size) {
                const uint32_t chunk = std::min<uint32_t>(r.source_size - done, staging_limit);
                auto st = scarlettx::PackageVfs::Global().ReadFileRange(
                    vfs_path, r.file_offset + done, chunk, vfs_slice);
                if (!st.ok()) return HresultFromWin32(ERROR_READ_FAULT);
                if (vfs_slice.size() != chunk) return HresultFromWin32(ERROR_HANDLE_EOF);
                std::memcpy(io_target + done, vfs_slice.data(), chunk);
                done += chunk;
            }
        } else {
            HANDLE io_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            uint32_t done = 0u;
            int32_t read_hr = kOk;
            while (done < r.source_size) {
                const uint32_t chunk = std::min<uint32_t>(r.source_size - done, staging_limit);
                const uint64_t cur_off = r.file_offset + done;
                if (io_ev) ResetEvent(io_ev);
                OVERLAPPED at{};
                at.Offset = static_cast<DWORD>(cur_off);
                at.OffsetHigh = static_cast<DWORD>(cur_off >> 32);
                at.hEvent = io_ev;
                DWORD read = 0;
                BOOL ok = ReadFile(dup_handle, io_target + done, chunk, &read, &at);
                if (!ok) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        ok = GetOverlappedResult(dup_handle, &at, &read, TRUE);
                    }
                }
                if (!ok) {
                    const DWORD final_err = GetLastError();
                    read_hr = HresultFromWin32(final_err != 0 ? final_err : ERROR_READ_FAULT);
                    break;
                }
                if (read != chunk) {
                    read_hr = HresultFromWin32(ERROR_HANDLE_EOF);
                    break;
                }
                done += chunk;
            }
            if (io_ev) CloseHandle(io_ev);
            CloseHandle(dup_handle);
            if (read_hr < 0) return read_hr;
        }
    }

    size_t decoded_bytes = r.source_size;
    if (is_gdeflate || is_zlib) {
        const bool is_gdeflate_magic =
            raw_buf.size() >= 12u && raw_buf[0] == 0x04u && raw_buf[1] == 0xFBu;
        const bool is_rfc1950_zlib =
            scarlettx::io::LooksLikeRfc1950ZlibHeader(raw_buf.data(), raw_buf.size());

        std::vector<uint8_t> intermediate_buf;
        uint8_t* stage_dst = decode_dst;
        uint32_t stage_cap = decode_cap;
        if (bcpack_fmt != 0) {
            stage_cap = (r.intermediate_size != 0u) ? r.intermediate_size : decode_cap;
            intermediate_buf.assign(stage_cap, 0u);
            stage_dst = intermediate_buf.data();
        }

        if (is_gdeflate || is_gdeflate_magic) {
            size_t written = 0;
            const int32_t gd_hr = XboxCodecGDeflateDecompress(
                raw_buf.data(), raw_buf.size(), stage_dst, stage_cap, &written);
            if (gd_hr < 0) return gd_hr;
            decoded_bytes = written;
        } else if (is_rfc1950_zlib) {
            size_t written = 0;
            const int32_t zl_hr = XboxCodecGDeflateDecompress(
                raw_buf.data(), raw_buf.size(), stage_dst, stage_cap, &written);
            if (zl_hr < 0) return kNotImpl;
            decoded_bytes = std::min<size_t>(written, stage_cap);
        } else {
            return kNotImpl;
        }

        if (bcpack_fmt != 0) {
            const int32_t bc_hr = XboxCodecBcPackDecompress(
                stage_dst, decoded_bytes, decode_dst, decode_cap, bcpack_fmt);
            if (bc_hr < 0) return bc_hr;
            decoded_bytes = decode_cap;
        }
    } else if (bcpack_fmt != 0) {
        const bool is_framed_bcpack =
            (raw_buf.size() >= 8 && std::memcmp(raw_buf.data(), "BCPK", 4) == 0) ||
            (raw_buf.size() >= 12 && raw_buf[0] == 0x04u && raw_buf[1] == 0xFBu) ||
            scarlettx::io::LooksLikeRfc1950ZlibHeader(raw_buf.data(), raw_buf.size());
        if (is_framed_bcpack) {
            const int32_t bc_hr = XboxCodecBcPackDecompress(
                raw_buf.data(), raw_buf.size(), decode_dst, decode_cap, bcpack_fmt);
            if (bc_hr < 0) return bc_hr;
            decoded_bytes = decode_cap;
        } else {
            return kNotImpl;
        }
    } else if (swizzle_opt != 0) {
        std::memcpy(decode_dst, raw_buf.data(), std::min<size_t>(raw_buf.size(), decode_cap));
    }

    if (swizzle_opt != 0) {
        const uint32_t mode = swizzle_opt & 0xFFu;
        const uint32_t dxgi_fmt = (bcpack_fmt != 0) ? BcPackModeToDxgiFormat(bcpack_fmt) : 28u;
        const uint32_t bpe = scarlettx::gpu::xg::FormatBytesPerElement(dxgi_fmt);
        const bool is_bc = scarlettx::gpu::xg::IsBlockCompressed(dxgi_fmt);

        uint32_t tex_w = static_cast<uint32_t>(r.reserved & 0xFFFFu);
        uint32_t tex_h = static_cast<uint32_t>(r.reserved >> 16u);
        if (tex_w == 0 || tex_h == 0) {
            const uint32_t elem_count =
                std::max<uint32_t>(1u, static_cast<uint32_t>(decoded_bytes / std::max<uint32_t>(1u, bpe)));
            const auto side_elems =
                std::max<uint32_t>(1u, static_cast<uint32_t>(std::sqrt(static_cast<double>(elem_count)) + 0.5));
            tex_w = side_elems * (is_bc ? 4u : 1u);
            tex_h = tex_w;
        }

        XG_TEXTURE2D_DESC td{};
        td.Width = tex_w;
        td.Height = tex_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_fmt;
        td.SampleDesc.Count = 1;
        td.SwizzleMode = mode;

        XG_RESOURCE_LAYOUT layout{};
        scarlettx::gpu::xg::ComputeTexture2DLayout(td, &layout);
        if (scarlettx::gpu::xg::IsAddressable(layout) && layout.SizeBytes <= r.destination_size) {
            const uint32_t row_pitch = layout.Plane[0].MipLayout[0].WidthElements * bpe;
            if (!scarlettx::gpu::xg::CopyIntoSubresource(layout, dst_ptr, 0u, 0u, decode_dst, row_pitch)) {
                return kInvalidArg;
            }
            return kOk;
        }
        if (r.destination_size < decoded_bytes) return kInvalidArg;
        std::memcpy(dst_ptr, decode_dst, decoded_bytes);
    }
    return kOk;
}

void QueueRun(Queue* queue) {
    std::unique_lock<std::mutex> lock(queue->mu);
    for (;;) {
        queue->wake.wait(lock, [queue] { return queue->stop || !queue->submitted.empty(); });
        if (queue->stop) return;
        const Entry entry = queue->submitted.front();
        queue->submitted.pop_front();
        lock.unlock();
        if (entry.is_status) {
            StatusEntry& slot = entry.status->entries[entry.index];
            slot.hr.store(queue->first_failure);
            slot.complete.store(true);
            queue->first_failure = kOk;
        } else if (entry.is_signal) {
            void** vtbl = *reinterpret_cast<void***>(entry.fence);
            using SignalFn = int32_t (WINAPI*)(void*, uint64_t);
            reinterpret_cast<SignalFn>(vtbl[10])(entry.fence, entry.fence_value);
        } else {
            const int32_t hr = entry.cancelled ? kAbort : RunRequest(queue, entry);
            if (hr < 0) {
                if (queue->first_failure >= 0) queue->first_failure = hr;
                {
                    std::lock_guard<std::mutex> err_lock(queue->mu);
                    if (queue->error_record.failure_count == 0u) {
                        queue->error_record.first_failure.hresult = hr;
                        queue->error_record.first_failure.command_type = 0;  // DSTORAGE_COMMAND_TYPE_REQUEST
                        queue->error_record.first_failure.request = entry.request;
                    }
                    ++queue->error_record.failure_count;
                }
                static std::atomic<int> reported{0};
                if (reported.fetch_add(1) < 16) {
                    DS_LOG("request failed: hr=0x%08X options=0x%llx offset=%llu size=%u", static_cast<unsigned>(hr),
                           static_cast<unsigned long long>(entry.request.options),
                           static_cast<unsigned long long>(entry.request.file_offset), entry.request.source_size);
                }
            }
        }
        ReleaseEntry(entry);
        scarlettx::FlightRecorder::Instance().BeatProgress("DirectStorageBatch");
        lock.lock();
    }
}

void SubmitLocked(Queue* queue) {
    if (queue->pending.empty()) return;
    queue->submitted.insert(queue->submitted.end(), queue->pending.begin(), queue->pending.end());
    queue->pending.clear();
    if (!queue->worker.joinable()) queue->worker = std::thread(&QueueRun, queue);
    queue->wake.notify_one();
}

void EnqueueLocked(Queue* queue, const Entry& entry) {
    queue->pending.push_back(entry);
    if (queue->pending.size() * 2 > queue->capacity) SubmitLocked(queue);
}

void DestroyQueue(Object* self) {
    auto* queue = static_cast<Queue*>(self);
    {
        std::lock_guard<std::mutex> lock(queue->mu);
        queue->stop = true;
    }
    queue->wake.notify_all();
    if (queue->worker.joinable()) queue->worker.join();
    for (const Entry& entry : queue->pending) ReleaseEntry(entry);
    for (const Entry& entry : queue->submitted) ReleaseEntry(entry);
    delete queue;
}

void WINAPI QueueEnqueueRequest(Object* self, const Request* request) {
    auto* queue = static_cast<Queue*>(self);
    if (!request) return;
    DS_TRACE(8, "EnqueueRequest options=0x%llx file/source=%p offset=%llu size=%u -> %p (%u)",
             static_cast<unsigned long long>(request->options), request->source,
             static_cast<unsigned long long>(request->file_offset), request->source_size, request->destination,
             request->destination_size);
    Entry entry;
    entry.request = *request;
    if (!(request->options & kOptSourceMemory) && request->source) {
        entry.file = static_cast<File*>(static_cast<Object*>(request->source));
        ObjectAddRef(entry.file);
    }
    std::lock_guard<std::mutex> lock(queue->mu);
    EnqueueLocked(queue, entry);
}

void WINAPI QueueEnqueueStatus(Object* self, Object* status_array, uint32_t index) {
    auto* queue = static_cast<Queue*>(self);
    auto* array = static_cast<StatusArray*>(status_array);
    if (!array || index >= array->capacity) {
        DS_LOG("EnqueueStatus ignored: array=%p index=%u", static_cast<void*>(array), index);
        return;
    }
    array->entries[index].complete.store(false);
    Entry entry;
    entry.is_status = true;
    entry.status = array;
    entry.index = index;
    ObjectAddRef(array);
    std::lock_guard<std::mutex> lock(queue->mu);
    EnqueueLocked(queue, entry);
}

void WINAPI QueueSubmit(Object* self) {
    auto* queue = static_cast<Queue*>(self);
    std::lock_guard<std::mutex> lock(queue->mu);
    SubmitLocked(queue);
}

void WINAPI QueueEnqueueSignal(Object* self, void* fence, uint64_t value) {
    auto* queue = static_cast<Queue*>(self);
    if (!fence) {
        DS_LOG("EnqueueSignal ignored: fence is null");
        return;
    }
    void** vtbl = *reinterpret_cast<void***>(fence);
    using AddRefFn = uint32_t (WINAPI*)(void*);
    reinterpret_cast<AddRefFn>(vtbl[1])(fence);

    Entry entry;
    entry.is_signal = true;
    entry.fence = fence;
    entry.fence_value = value;
    std::lock_guard<std::mutex> lock(queue->mu);
    EnqueueLocked(queue, entry);
}

void WINAPI QueueCancelRequestsWithTag(Object* self, uint64_t mask, uint64_t value) {
    auto* queue = static_cast<Queue*>(self);
    std::lock_guard<std::mutex> lock(queue->mu);
    for (auto& entry : queue->pending) {
        if (!entry.is_status && !entry.is_signal && (entry.request.cancellation_tag & mask) == value) {
            entry.cancelled = true;
        }
    }
    for (auto& entry : queue->submitted) {
        if (!entry.is_status && !entry.is_signal && (entry.request.cancellation_tag & mask) == value) {
            entry.cancelled = true;
        }
    }
}

int32_t WINAPI QueueRetrieveErrorRecord(Object* self, GuestErrorRecord* out_record) {
    if (!out_record) return kPointer;
    auto* queue = static_cast<Queue*>(self);
    std::lock_guard<std::mutex> lock(queue->mu);
    *out_record = queue->error_record;
    queue->error_record = GuestErrorRecord{0u, {kOk, -1, {}}};
    return kOk;
}

void* const kQueueVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(QueueEnqueueRequest),         // 3, confirmed
    DS_SLOT(QueueEnqueueStatus),          // 4, confirmed
    DS_SLOT(QueueEnqueueSignal),          // 5, confirmed
    DS_SLOT(QueueSubmit),                 // 6, confirmed
    DS_SLOT(QueueCancelRequestsWithTag),  // 7, confirmed
    DS_SLOT(QueueRetrieveErrorRecord),    // 8, RetrieveErrorRecord
    DS_SLOT((UnnamedSlot<1, 9>)),
    DS_SLOT((UnnamedSlot<1, 10>)), DS_SLOT((UnnamedSlot<1, 11>)),
};

// ---- factory ----------------------------------------------------------------

int32_t WINAPI FactoryCreateQueue(Object* /*self*/, const QueueDesc* desc, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (!desc || desc->source_type > 1 || desc->capacity < kMinQueueCapacity || desc->capacity > kMaxQueueCapacity) {
        return kInvalidArg;
    }
    auto* queue = new (std::nothrow) Queue();
    if (!queue) return kOutOfMemory;
    queue->vtbl = kQueueVtbl;
    queue->destroy = &DestroyQueue;
    queue->source_type = desc->source_type;
    queue->capacity = desc->capacity;
    DS_TRACE(8, "CreateQueue source=%llu capacity=%u name=%s -> %p", static_cast<unsigned long long>(desc->source_type),
             desc->capacity, desc->name ? desc->name : "", static_cast<void*>(queue));
    *out = static_cast<Object*>(queue);
    return kOk;
}

int32_t WINAPI FactoryOpenFile(Object* /*self*/, const wchar_t* path, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (!path) return kInvalidArg;

    std::wstring resolved_path(path);
    const std::string utf8_path = WideToUtf8(resolved_path);
    const bool is_virtual_drive =
        resolved_path.size() >= 2 && resolved_path[1] == L':' &&
        (resolved_path[0] == L'G' || resolved_path[0] == L'g' ||
         resolved_path[0] == L'D' || resolved_path[0] == L'd');

    // If path is mounted in PackageVfs::Global() (e.g., embedded .xvc/.xvd or when host G: is Google Drive),
    // try resolving through PackageVfs first before touching a potentially colliding host G: drive.
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (is_virtual_drive && !utf8_path.empty() && scarlettx::PackageVfs::Global().Exists(utf8_path)) {
        std::string host_resolved;
        if (scarlettx::PackageVfs::Global().ResolvePath(utf8_path, host_resolved).ok() && !host_resolved.empty()) {
            const std::wstring whost = Utf8ToWide(host_resolved);
            handle = CreateFileW(
                whost.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            uint64_t vfs_sz = 0;
            if (scarlettx::PackageVfs::Global().GetFileSize(utf8_path, vfs_sz)) {
                auto* file = new (std::nothrow) File();
                if (!file) return kOutOfMemory;
                file->vtbl = kFileVtbl;
                file->destroy = &DestroyFile;
                file->is_vfs = true;
                file->vfs_path = utf8_path;
                file->vfs_size = vfs_sz;
                DS_TRACE(32, "OpenFile (PackageVfs) %ls (%llu bytes) -> %p",
                         path, static_cast<unsigned long long>(vfs_sz), static_cast<void*>(file));
                *out = static_cast<Object*>(file);
                return kOk;
            }
        }
    }

    if (handle == INVALID_HANDLE_VALUE) {
        handle = CreateFileW(
            resolved_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    }
    const DWORD initial_err = (handle == INVALID_HANDLE_VALUE) ? GetLastError() : 0;

    if (handle == INVALID_HANDLE_VALUE && is_virtual_drive && std::getenv("SCARLETTX_PACKAGE_ROOT")) {
        const char* pkg = std::getenv("SCARLETTX_PACKAGE_ROOT");
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pkg, -1, nullptr, 0);
        if (wlen > 1) {
            std::wstring wpkg(static_cast<size_t>(wlen - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pkg, -1, wpkg.data(), wlen);
            size_t rel_pos = 2;
            while (rel_pos < resolved_path.size() &&
                   (resolved_path[rel_pos] == L'\\' || resolved_path[rel_pos] == L'/')) {
                ++rel_pos;
            }
            const auto candidate = std::filesystem::path(wpkg) / resolved_path.substr(rel_pos);
            handle = CreateFileW(
                candidate.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        }
    }

    if (handle == INVALID_HANDLE_VALUE && !utf8_path.empty() && scarlettx::PackageVfs::Global().Exists(utf8_path)) {
        uint64_t vfs_sz = 0;
        if (scarlettx::PackageVfs::Global().GetFileSize(utf8_path, vfs_sz)) {
            auto* file = new (std::nothrow) File();
            if (!file) return kOutOfMemory;
            file->vtbl = kFileVtbl;
            file->destroy = &DestroyFile;
            file->is_vfs = true;
            file->vfs_path = utf8_path;
            file->vfs_size = vfs_sz;
            DS_TRACE(32, "OpenFile (PackageVfs fallback) %ls -> %p", path, static_cast<void*>(file));
            *out = static_cast<Object*>(file);
            return kOk;
        }
    }

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        const int32_t hr = HresultFromWin32(err != 0 ? err : initial_err);
        DS_LOG("OpenFile failed: %ls hr=0x%08X", path, static_cast<unsigned>(hr));
        scarlettx::FlightRecorder::Instance().RecordVfsAccess(
            utf8_path.c_str(), false, hr, SCARLETTX_CALLER_RIP());
        return hr;
    }
    if (is_virtual_drive && !utf8_path.empty()) {
        scarlettx::FlightRecorder::Instance().RecordVfsAccess(
            utf8_path.c_str(), true, 0, SCARLETTX_CALLER_RIP());
    }
    auto* file = new (std::nothrow) File();
    if (!file) {
        CloseHandle(handle);
        return kOutOfMemory;
    }
    file->vtbl = kFileVtbl;
    file->destroy = &DestroyFile;
    file->handle = handle;
    DS_TRACE(32, "OpenFile %ls -> %p", path, static_cast<void*>(file));
    *out = static_cast<Object*>(file);
    return kOk;
}

int32_t WINAPI FactoryCreateStatusArray(Object* /*self*/, uint32_t capacity, const char* name, const void* /*riid*/,
                                        void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (capacity == 0) return kInvalidArg;
    auto* array = new (std::nothrow) StatusArray();
    StatusEntry* entries = new (std::nothrow) StatusEntry[capacity];
    if (!array || !entries) {
        delete array;
        delete[] entries;
        return kOutOfMemory;
    }
    array->vtbl = kStatusArrayVtbl;
    array->destroy = &DestroyStatusArray;
    array->capacity = capacity;
    array->entries = entries;
    DS_TRACE(8, "CreateStatusArray capacity=%u name=%s -> %p", capacity, name ? name : "", static_cast<void*>(array));
    *out = static_cast<Object*>(array);
    return kOk;
}

int32_t WINAPI FactorySetStagingBufferSize(Object* /*self*/, uint32_t size) {
    if (size == 0u) return kInvalidArg;
    g_staging_buffer_size.store(size, std::memory_order_release);
    DS_TRACE(8, "SetStagingBufferSize size=%u", size);
    return kOk;
}

void* const kFactoryVtbl[] = {
    DS_SLOT(ObjectQueryInterface),
    DS_SLOT(ObjectAddRef),
    DS_SLOT(ObjectRelease),
    DS_SLOT(FactoryCreateQueue),           // 3, confirmed
    DS_SLOT(FactoryOpenFile),              // 4, confirmed
    DS_SLOT(FactoryCreateStatusArray),     // 5, confirmed
    DS_SLOT((UnnamedSlot<0, 6>)),          // 6, SetDebugFlags
    DS_SLOT(FactorySetStagingBufferSize),  // 7, SetStagingBufferSize
    DS_SLOT((UnnamedSlot<0, 8>)),
    DS_SLOT((UnnamedSlot<0, 9>)),  DS_SLOT((UnnamedSlot<0, 10>)), DS_SLOT((UnnamedSlot<0, 11>)),
};

Object g_factory{kFactoryVtbl};

}  // namespace

extern "C" __declspec(dllexport) int32_t WINAPI ScarlettXcrdGetFactory(const void* riid, void** out) {
    if (!out) return kPointer;
    if (riid) {
        const auto* g = static_cast<const uint32_t*>(riid);
        DS_TRACE(4, "DStorageGetFactory riid=%08x-%08x-%08x-%08x", g[0], g[1], g[2], g[3]);
    }
    ObjectAddRef(&g_factory);
    *out = &g_factory;
    return kOk;
}

extern "C" __declspec(dllexport) int32_t WINAPI DStorageGetFactory(const void* riid, void** out) {
    return ScarlettXcrdGetFactory(riid, out);
}

#else  // !_WIN32

extern "C" int32_t ScarlettXcrdGetFactory(const void*, void** out) {
    if (out) *out = nullptr;
    return static_cast<int32_t>(0x80004001u);
}

extern "C" int32_t DStorageGetFactory(const void* riid, void** out) {
    return ScarlettXcrdGetFactory(riid, out);
}

#endif
