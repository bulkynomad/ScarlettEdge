// Guest-facing DirectStorage objects for real titles.
//
// A title imports a single function, XCRDAPI.dll!DStorageGetFactory, and
// drives everything else through COM-style objects: the factory, queues,
// files and status arrays. Requests are read from host files on one worker
// thread per queue, in the order they were enqueued.
//
// Structure layouts follow the public GDK reference for dstorage_xs.h and
// match what a title builds. A vtable slot has a name here only if a title's
// call confirmed it; the others report themselves when called.
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/media/codecs_abi.h"
#include "scarlettx/io/deflate_rfc1951.h"

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
#include <windows.h>

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kAbort = static_cast<int32_t>(0x80004004u);
constexpr int32_t kPending = static_cast<int32_t>(0x8000000Au);
constexpr int32_t kOutOfMemory = static_cast<int32_t>(0x8007000Eu);

constexpr uint16_t kMinQueueCapacity = 0x80;
constexpr uint16_t kMaxQueueCapacity = 0x2000;

// DSTORAGE_REQUEST_OPTIONS bits.
constexpr uint64_t kOptZlib = 1ull << 0;
constexpr uint64_t kOptBcpack = 0xFull << 1;
constexpr uint64_t kOptSwizzle = 0xFFFFull << 5;
constexpr uint64_t kOptDestinationPhysical = 1ull << 21;
constexpr uint64_t kOptSourceMemory = 1ull << 22;
constexpr uint64_t kOptSourcePhysical = 1ull << 23;
constexpr uint64_t kOptUnsupported =
    kOptDestinationPhysical | kOptSourcePhysical;

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

// Traces the first calls of a kind only; requests arrive by the thousand.
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

// DSTORAGE_QUEUE_DESC
struct QueueDesc {
    uint64_t source_type;  // 0 = file, 1 = memory
    uint16_t capacity;
    int8_t priority;
    const char* name;
};
static_assert(sizeof(QueueDesc) == 0x18 && offsetof(QueueDesc, name) == 0x10, "DSTORAGE_QUEUE_DESC layout");

// DSTORAGE_REQUEST
struct Request {
    uint64_t options;
    void* destination;
    uint64_t destination_page_offset;  // UINT16 in the SDK, padded
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

// What the title holds: a pointer to the vtable pointer.
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

// Every object has one interface, so any interface id answers with it.
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
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1) < 4) {
        DS_LOG("%s slot %d is not implemented (0x%llx, 0x%llx, 0x%llx)", kInterfaceNames[Interface], Slot,
               static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
               static_cast<unsigned long long>(a3));
    }
    return 0;
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
};

void CloseFileLocked(File* file) {
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
}

void DestroyFile(Object* self) {
    auto* file = static_cast<File*>(self);
    CloseFileLocked(file);
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
    std::vector<Entry> pending;    // enqueued, not yet submitted
    std::deque<Entry> submitted;
    std::thread worker;
    bool stop = false;
    int32_t first_failure = kOk;   // since the last status write; worker thread only
};

int32_t RunRequest(const Queue* queue, const Entry& entry) {
    const Request& r = entry.request;
    if (r.options & kOptUnsupported) return kNotImpl;
    const bool from_memory = (r.options & kOptSourceMemory) != 0;
    if (from_memory != (queue->source_type == 1)) return kInvalidArg;
    if (r.source_size == 0) return kOk;
    const bool is_zlib = (r.options & kOptZlib) != 0;
    const uint32_t bcpack_fmt = static_cast<uint32_t>((r.options & kOptBcpack) >> 1);
    const uint32_t swizzle_opt = static_cast<uint32_t>((r.options & kOptSwizzle) >> 5);
    const bool compressed = is_zlib || (bcpack_fmt != 0);
    if (!r.destination || !r.source || (!compressed && swizzle_opt == 0 && r.destination_size < r.source_size)) {
        return kInvalidArg;
    }

    uint8_t* dst_ptr = static_cast<uint8_t*>(r.destination) + r.destination_page_offset;
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
        HANDLE file_handle = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(entry.file->mu);
            file_handle = entry.file->handle;
        }
        if (file_handle == INVALID_HANDLE_VALUE) return HresultFromWin32(ERROR_INVALID_HANDLE);
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(r.file_offset);
        at.OffsetHigh = static_cast<DWORD>(r.file_offset >> 32);
        at.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD read = 0;
        BOOL ok = ReadFile(file_handle, io_target, r.source_size, &read, &at);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(file_handle, &at, &read, TRUE);
            }
        }
        const DWORD final_err = ok ? 0 : GetLastError();
        if (at.hEvent) CloseHandle(at.hEvent);
        if (!ok) return HresultFromWin32(final_err);
        if (read != r.source_size) return HresultFromWin32(ERROR_HANDLE_EOF);
    }

    size_t decoded_bytes = r.source_size;
    if (is_zlib) {
        // Dispatch tile-framed GDeflate streams (0x04 0xFB) and spec-compliant RFC 1950 Zlib streams;
        // return E_NOTIMPL for unrecognized/unframed payloads without overwriting destination memory.
        if (raw_buf.size() >= 8 && raw_buf[0] == 0x04u && raw_buf[1] == 0xFBu) {
            size_t written = 0;
            const int32_t gd_hr = XboxCodecGDeflateDecompress(
                raw_buf.data(), raw_buf.size(), decode_dst, decode_cap, &written);
            if (gd_hr < 0) return gd_hr;
            decoded_bytes = written;
        } else if (scarlettx::io::LooksLikeRfc1950ZlibHeader(raw_buf.data(), raw_buf.size())) {
            std::vector<uint8_t> staged(decode_cap, 0u);
            size_t written = 0;
            if (!scarlettx::io::InflateZlibRfc1950(
                    raw_buf.data(), raw_buf.size(), staged.data(), staged.size(), written)) {
                return kNotImpl;
            }
            decoded_bytes = std::min<size_t>(written, decode_cap);
            std::memcpy(decode_dst, staged.data(), decoded_bytes);
        } else {
            return kNotImpl;
        }
    } else if (bcpack_fmt != 0) {
        if (raw_buf.size() >= 8 && std::memcmp(raw_buf.data(), "BCPK", 4) == 0) {
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
        const uint32_t dxgi_fmt = (bcpack_fmt != 0) ? bcpack_fmt : 28u;  // default RGBA8_UNORM
        const uint32_t bpe = scarlettx::gpu::xg::FormatBytesPerElement(dxgi_fmt);
        const bool is_bc = scarlettx::gpu::xg::IsBlockCompressed(dxgi_fmt);

        uint32_t tex_w = static_cast<uint32_t>(r.reserved & 0xFFFFu);
        uint32_t tex_h = static_cast<uint32_t>(r.reserved >> 16u);
        if (tex_w == 0 || tex_h == 0) {
            const uint32_t elem_count = std::max<uint32_t>(1u, static_cast<uint32_t>(decoded_bytes / std::max<uint32_t>(1u, bpe)));
            const auto side_elems = std::max<uint32_t>(1u, static_cast<uint32_t>(std::sqrt(static_cast<double>(elem_count)) + 0.5));
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
                static std::atomic<int> reported{0};
                if (reported.fetch_add(1) < 16) {
                    DS_LOG("request failed: hr=0x%08X options=0x%llx offset=%llu size=%u", static_cast<unsigned>(hr),
                           static_cast<unsigned long long>(entry.request.options),
                           static_cast<unsigned long long>(entry.request.file_offset), entry.request.source_size);
                }
            }
        }
        ReleaseEntry(entry);
        lock.lock();
    }
}

// Caller holds queue->mu.
void SubmitLocked(Queue* queue) {
    if (queue->pending.empty()) return;
    queue->submitted.insert(queue->submitted.end(), queue->pending.begin(), queue->pending.end());
    queue->pending.clear();
    if (!queue->worker.joinable()) queue->worker = std::thread(&QueueRun, queue);
    queue->wake.notify_one();
}

// Caller holds queue->mu. The console submits on its own once more than
// half of the queue's capacity is waiting.
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

void* const kQueueVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(QueueEnqueueRequest),         // 3, confirmed
    DS_SLOT(QueueEnqueueStatus),          // 4, confirmed
    DS_SLOT(QueueEnqueueSignal),          // 5, confirmed
    DS_SLOT(QueueSubmit),                 // 6, confirmed
    DS_SLOT(QueueCancelRequestsWithTag),  // 7, confirmed
    DS_SLOT((UnnamedSlot<1, 8>)),  DS_SLOT((UnnamedSlot<1, 9>)),
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
    HANDLE handle = CreateFileW(
        resolved_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE && resolved_path.size() >= 2 && resolved_path[1] == L':') {
        const wchar_t drv = resolved_path[0];
        if ((drv == L'G' || drv == L'g' || drv == L'D' || drv == L'd') &&
            std::getenv("SCARLETTX_PACKAGE_ROOT")) {
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
    }
    if (handle == INVALID_HANDLE_VALUE) {
        const int32_t hr = HresultFromWin32(GetLastError());
        DS_LOG("OpenFile failed: %ls hr=0x%08X", path, static_cast<unsigned>(hr));
        return hr;
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

void* const kFactoryVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(FactoryCreateQueue),        // 3, confirmed
    DS_SLOT(FactoryOpenFile),           // 4, confirmed
    DS_SLOT(FactoryCreateStatusArray),  // 5, confirmed
    DS_SLOT((UnnamedSlot<0, 6>)),  DS_SLOT((UnnamedSlot<0, 7>)),  DS_SLOT((UnnamedSlot<0, 8>)),
    DS_SLOT((UnnamedSlot<0, 9>)),  DS_SLOT((UnnamedSlot<0, 10>)), DS_SLOT((UnnamedSlot<0, 11>)),
};

// The factory is a process-wide singleton and is never destroyed.
Object g_factory{kFactoryVtbl};

}  // namespace

extern "C" __declspec(dllexport) int32_t WINAPI DStorageGetFactory(const void* riid, void** out) {
    if (!out) return kPointer;
    if (riid) {
        const auto* g = static_cast<const uint32_t*>(riid);
        DS_TRACE(4, "DStorageGetFactory riid=%08x-%08x-%08x-%08x", g[0], g[1], g[2], g[3]);
    }
    ObjectAddRef(&g_factory);
    *out = &g_factory;
    return kOk;
}

#else  // !_WIN32

extern "C" int32_t DStorageGetFactory(const void*, void** out) {
    if (out) *out = nullptr;
    return static_cast<int32_t>(0x80004001u);
}

#endif
