// Drives the guest-facing DirectStorage objects the way a title does: one
// imported function, then raw vtable slots and SDK-layout structures.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gpu/xg_layout.h"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

extern "C" __declspec(dllimport) int32_t WINAPI DStorageGetFactory(const void* riid, void** out);

#define SX_CHECK(...)                                                           \
    do {                                                                        \
        if (!(__VA_ARGS__)) {                                                   \
            std::cerr << "[test_dstorage_guest] FAILED: " #__VA_ARGS__          \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include "sx_check.h"

namespace {

template <typename Ret, typename... Args>
Ret VCall(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = Ret (WINAPI*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

struct Guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
const Guid kAnyIid = {0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};

struct QueueDesc {
    uint64_t source_type;
    uint16_t capacity;
    int8_t priority;
    const char* name;
};

struct Request {
    uint64_t options;
    void* destination;
    uint64_t destination_page_offset;
    uint32_t destination_size;
    uint32_t reserved;
    void* source;
    uint64_t file_offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t cancellation_tag;
    const char* name;
};

constexpr uint64_t kSourceMemory = 1ull << 22;
constexpr uint64_t kZlib = 1ull << 0;
constexpr uint64_t kGDeflateOpt = 1ull << 24;
constexpr int kCreateQueue = 3, kOpenFile = 4, kCreateStatusArray = 5, kSetStagingBufferSize = 7;
constexpr int kEnqueueRequest = 3, kEnqueueStatus = 4, kEnqueueSignal = 5, kSubmit = 6, kCancelRequestsWithTag = 7;
constexpr int kFileClose = 3, kFileGetHandle = 4;
constexpr int kIsComplete = 3, kGetHResult = 4;
constexpr int kRelease = 2;

struct MockFence {
    void* vtable_ptr;
    void* vtable[16];
    std::atomic<uint32_t> ref_count{1};
    std::atomic<uint64_t> completed_val{0};
    HANDLE event = nullptr;

    static uint32_t WINAPI AddRef(void* self) {
        return static_cast<MockFence*>(self)->ref_count.fetch_add(1) + 1;
    }
    static uint32_t WINAPI Release(void* self) {
        return static_cast<MockFence*>(self)->ref_count.fetch_sub(1) - 1;
    }
    static uint64_t WINAPI GetCompletedValue(void* self) {
        return static_cast<MockFence*>(self)->completed_val.load(std::memory_order_acquire);
    }
    static int32_t WINAPI Signal(void* self, uint64_t val) {
        auto* f = static_cast<MockFence*>(self);
        f->completed_val.store(val, std::memory_order_release);
        if (f->event) SetEvent(f->event);
        return 0;
    }

    bool Wait(uint64_t target, DWORD timeout_ms = 5000) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (completed_val.load(std::memory_order_acquire) < target) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            if (WaitForSingleObject(event, static_cast<DWORD>(deadline - now)) == WAIT_TIMEOUT) break;
        }
        return completed_val.load(std::memory_order_acquire) >= target;
    }

    MockFence() {
        vtable_ptr = vtable;
        for (int i = 0; i < 16; ++i) vtable[i] = nullptr;
        vtable[1] = reinterpret_cast<void*>(&AddRef);
        vtable[2] = reinterpret_cast<void*>(&Release);
        vtable[8] = reinterpret_cast<void*>(&GetCompletedValue);
        vtable[10] = reinterpret_cast<void*>(&Signal);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~MockFence() {
        if (event) CloseHandle(event);
    }
};

uint8_t Pattern(size_t i) { return static_cast<uint8_t>(i * 7 + (i >> 8) + 3); }

bool SubmitAndWaitStatus(void* q, void* status, uint32_t index, MockFence& fence, uint64_t& seq) {
    const uint64_t target = ++seq;
    VCall<void>(q, kEnqueueSignal, &fence, target);
    VCall<void>(q, kSubmit);
    return fence.Wait(target) && VCall<int32_t>(status, kIsComplete, index) != 0;
}

Request FileRead(void* file, uint64_t offset, void* destination, uint32_t size) {
    Request r{};
    r.destination = destination;
    r.destination_size = size;
    r.source = file;
    r.file_offset = offset;
    r.source_size = size;
    r.name = "test read";
    return r;
}

bool Matches(const std::vector<uint8_t>& got, size_t offset) {
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != Pattern(offset + i)) return false;
    }
    return true;
}

}  // namespace

int main() {
    sx_test::ScopedTempDir sandbox("test_dstorage_guest");
    constexpr size_t kFileSize = 200000;
    const std::wstring path = (sandbox.path() / L"raw_stream.bin").wstring();
    {
        std::vector<uint8_t> content(kFileSize);
        for (size_t i = 0; i < kFileSize; ++i) content[i] = Pattern(i);
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        SX_CHECK(h != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        SX_CHECK(WriteFile(h, content.data(), static_cast<DWORD>(kFileSize), &written, nullptr) && written == kFileSize);
        CloseHandle(h);
    }

    void* factory = nullptr;
    SX_CHECK(DStorageGetFactory(&kAnyIid, &factory) == 0 && factory != nullptr);
    SX_CHECK(DStorageGetFactory(&kAnyIid, nullptr) < 0);
    void* again = nullptr;
    SX_CHECK(DStorageGetFactory(&kAnyIid, &again) == 0 && again == factory);
    VCall<uint32_t>(again, kRelease);

    QueueDesc desc{};
    desc.capacity = 0x80;
    desc.name = "test queue";
    void* queue = nullptr;
    SX_CHECK(VCall<int32_t>(factory, kCreateQueue, &desc, &kAnyIid, &queue) == 0 && queue != nullptr);
    QueueDesc bad = desc;
    bad.capacity = 0x7F;
    void* none = &bad;
    SX_CHECK(VCall<int32_t>(factory, kCreateQueue, &bad, &kAnyIid, &none) < 0 && none == nullptr);

    void* status = nullptr;
    SX_CHECK(VCall<int32_t>(factory, kCreateStatusArray, 4u, "test status", &kAnyIid, &status) == 0 && status != nullptr);
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) != 0);
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);

    void* file = nullptr;
    SX_CHECK(VCall<int32_t>(factory, kOpenFile, path.c_str(), &kAnyIid, &file) == 0 && file != nullptr);
    void* missing = &bad;
    SX_CHECK(VCall<int32_t>(factory, kOpenFile, L"Z:\\no\\such\\file.bin", &kAnyIid, &missing) < 0 && missing == nullptr);

    // A title sizes the file through a handle it then closes itself.
    {
        const HANDLE h = VCall<HANDLE>(file, kFileGetHandle);
        SX_CHECK(h != nullptr && h != INVALID_HANDLE_VALUE);
        LARGE_INTEGER size{};
        SX_CHECK(GetFileSizeEx(h, &size) && size.QuadPart == static_cast<LONGLONG>(kFileSize));
        SX_CHECK(CloseHandle(h));
    }

    // Two reads and a status write; nothing runs before Submit.
    MockFence wait_fence;
    uint64_t wait_seq = 0;
    std::vector<uint8_t> a(1000), b(50000);
    Request ra = FileRead(file, 100, a.data(), static_cast<uint32_t>(a.size()));
    Request rb = FileRead(file, 123456, b.data(), static_cast<uint32_t>(b.size()));
    VCall<void>(queue, kEnqueueRequest, &ra);
    VCall<void>(queue, kEnqueueRequest, &rb);
    std::memset(&ra, 0xEE, sizeof(ra));  // the title may reuse the structure at once
    VCall<void>(queue, kEnqueueStatus, status, 0u);
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) == 0);
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == static_cast<int32_t>(0x8000000Au));
    SX_CHECK(a[0] == 0 && b[0] == 0);
    SX_CHECK(SubmitAndWaitStatus(queue, status, 0u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
    SX_CHECK(Matches(a, 100) && Matches(b, 123456));

    // A read past the end fails its status entry; the next batch is clean again.
    std::vector<uint8_t> c(64);
    Request rc = FileRead(file, kFileSize - 10, c.data(), static_cast<uint32_t>(c.size()));
    Request rd = FileRead(file, 0, a.data(), static_cast<uint32_t>(a.size()));
    VCall<void>(queue, kEnqueueRequest, &rc);
    VCall<void>(queue, kEnqueueRequest, &rd);
    VCall<void>(queue, kEnqueueStatus, status, 1u);
    SX_CHECK(SubmitAndWaitStatus(queue, status, 1u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) < 0);
    SX_CHECK(Matches(a, 0));
    VCall<void>(queue, kEnqueueRequest, &rb);
    VCall<void>(queue, kEnqueueStatus, status, 2u);
    SX_CHECK(SubmitAndWaitStatus(queue, status, 2u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) == 0);

    // Decompression is not available: the request fails instead of copying raw bytes.
    Request rz = FileRead(file, 0, c.data(), static_cast<uint32_t>(c.size()));
    rz.options = kZlib;
    std::memset(c.data(), 0x5A, c.size());
    VCall<void>(queue, kEnqueueRequest, &rz);
    VCall<void>(queue, kEnqueueStatus, status, 3u);
    SX_CHECK(SubmitAndWaitStatus(queue, status, 3u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 3u) == static_cast<int32_t>(0x80004001u));
    SX_CHECK(c[0] == 0x5A && c[63] == 0x5A);

    // More than half the capacity waiting (65 of 128 entries) submits without a Submit call.
    std::vector<uint8_t> many(63 * 16);
    for (uint32_t i = 0; i < 63; ++i) {
        Request r = FileRead(file, i * 1000, many.data() + i * 16, 16);
        VCall<void>(queue, kEnqueueRequest, &r);
    }
    VCall<void>(queue, kEnqueueStatus, status, 0u);  // 64th entry (== half capacity): still buffered!
    SX_CHECK(many[0] == 0 && many[15] == 0);
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) == 0);
    const uint64_t auto_submit_target = ++wait_seq;
    VCall<void>(queue, kEnqueueSignal, &wait_fence, auto_submit_target);  // 65th entry: auto-submits!
    SX_CHECK(wait_fence.Wait(auto_submit_target));
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) != 0);
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
    for (uint32_t i = 0; i < 63; ++i) {
        SX_CHECK(many[i * 16] == Pattern(i * 1000) && many[i * 16 + 15] == Pattern(i * 1000 + 15));
    }

    // Memory-sourced queue.
    QueueDesc mem_desc = desc;
    mem_desc.source_type = 1;
    void* mem_queue = nullptr;
    SX_CHECK(VCall<int32_t>(factory, kCreateQueue, &mem_desc, &kAnyIid, &mem_queue) == 0 && mem_queue != nullptr);
    std::vector<uint8_t> src(4096), dst(4096);
    for (size_t i = 0; i < src.size(); ++i) src[i] = Pattern(i + 77);
    Request rm{};
    rm.options = kSourceMemory;
    rm.destination = dst.data();
    rm.destination_size = static_cast<uint32_t>(dst.size());
    rm.source = src.data();
    rm.source_size = static_cast<uint32_t>(src.size());
    VCall<void>(mem_queue, kEnqueueRequest, &rm);
    VCall<void>(mem_queue, kEnqueueStatus, status, 1u);
    SX_CHECK(SubmitAndWaitStatus(mem_queue, status, 1u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) == 0);
    SX_CHECK(dst == src);
    // A file request does not belong on it.
    VCall<void>(mem_queue, kEnqueueRequest, &rd);
    VCall<void>(mem_queue, kEnqueueStatus, status, 1u);
    SX_CHECK(SubmitAndWaitStatus(mem_queue, status, 1u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) < 0);

    // A request still queued keeps its file alive; a closed file fails its reads.
    VCall<void>(file, kFileClose);
    VCall<void>(queue, kEnqueueRequest, &rd);
    VCall<void>(queue, kEnqueueStatus, status, 2u);
    SX_CHECK(VCall<uint32_t>(file, kRelease) == 1);
    SX_CHECK(SubmitAndWaitStatus(queue, status, 2u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) < 0);

    // Test EnqueueSignal (slot 5) with a fence.
    MockFence fence;
    VCall<void>(mem_queue, kEnqueueRequest, &rm);
    VCall<void>(mem_queue, kEnqueueSignal, &fence, 999ull);
    SX_CHECK(fence.completed_val.load() == 0);
    VCall<void>(mem_queue, kSubmit);
    SX_CHECK(fence.Wait(999ull));
    SX_CHECK(fence.completed_val.load() == 999ull);

    // Test CancelRequestsWithTag (slot 7).
    std::vector<uint8_t> cancel_dst(4096);
    Request r_cancel = rm;
    r_cancel.destination = cancel_dst.data();
    r_cancel.cancellation_tag = 0xCAFE;
    VCall<void>(mem_queue, kEnqueueRequest, &r_cancel);
    VCall<void>(mem_queue, kCancelRequestsWithTag, 0xFFFFull, 0xCAFEull);
    VCall<void>(mem_queue, kEnqueueStatus, status, 3u);
    SX_CHECK(SubmitAndWaitStatus(mem_queue, status, 3u, wait_fence, wait_seq));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 3u) == static_cast<int32_t>(0x80004004u));

    // Verify spec-compliant RFC 1950 Zlib decompression through the guest DirectStorage memory queue!
    {
        const uint8_t zlib_stream[] = {
            0x78u, 0x01u, 0x0Bu, 0x4Eu, 0x4Eu, 0x2Cu, 0xCAu, 0x49u, 0x2Du, 0x29u, 0x71u, 0x4Du, 0x49u,
            0x4Fu, 0x55u, 0x08u, 0x72u, 0x73u, 0x36u, 0xB4u, 0x34u, 0x35u, 0x54u, 0x70u, 0xCBu, 0xACu,
            0x48u, 0x4Du, 0x51u, 0xF0u, 0x28u, 0x4Du, 0x4Bu, 0xCBu, 0x4Du, 0xCCu, 0x53u, 0x08u, 0x49u,
            0x2Du, 0x2Eu, 0x51u, 0x54u, 0x08u, 0xA6u, 0xBAu, 0x42u, 0x00u, 0xE2u, 0x05u, 0x28u, 0x69u
        };
        const std::string expected_z =
            "ScarlettEdge RFC1951 Fixed Huffman Test! "
            "ScarlettEdge RFC1951 Fixed Huffman Test! "
            "ScarlettEdge RFC1951 Fixed Huffman Test! ";
        std::vector<uint8_t> z_dst(expected_z.size(), 0);
        Request rz_valid{};
        rz_valid.options = kSourceMemory | kZlib;
        rz_valid.destination = z_dst.data();
        rz_valid.destination_size = static_cast<uint32_t>(z_dst.size());
        rz_valid.source = const_cast<uint8_t*>(zlib_stream);
        rz_valid.source_size = static_cast<uint32_t>(sizeof(zlib_stream));
        VCall<void>(mem_queue, kEnqueueRequest, &rz_valid);
        VCall<void>(mem_queue, kEnqueueStatus, status, 0u);
        SX_CHECK(SubmitAndWaitStatus(mem_queue, status, 0u, wait_fence, wait_seq));
        SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
        SX_CHECK(std::memcmp(z_dst.data(), expected_z.data(), expected_z.size()) == 0);
    }

    // Verify kOptSwizzle (XG_SWIZZLE_MODE_4KB_S = 5) direct swizzled texture streaming!
    {
        XG_TEXTURE2D_DESC td{};
        td.Width = 32;
        td.Height = 32;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = 28;  // RGBA8_UNORM
        td.SampleDesc.Count = 1;
        td.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
        XG_RESOURCE_LAYOUT layout{};
        scarlettx::gpu::xg::ComputeTexture2DLayout(td, &layout);
        SX_CHECK(layout.SizeBytes == 4096u);

        std::vector<uint8_t> linear_tex(32u * 32u * 4u);
        for (size_t i = 0; i < linear_tex.size(); ++i) {
            linear_tex[i] = static_cast<uint8_t>((i * 23u + 9u) & 0xFFu);
        }
        std::vector<uint8_t> swizzled_dst(static_cast<size_t>(layout.SizeBytes), 0);

        Request r_swiz{};
        r_swiz.options = kSourceMemory | (static_cast<uint64_t>(XG_SWIZZLE_MODE_4KB_S) << 5u);
        r_swiz.destination = swizzled_dst.data();
        r_swiz.destination_size = static_cast<uint32_t>(swizzled_dst.size());
        r_swiz.reserved = (32u) | (32u << 16u);
        r_swiz.source = linear_tex.data();
        r_swiz.source_size = static_cast<uint32_t>(linear_tex.size());
        VCall<void>(mem_queue, kEnqueueRequest, &r_swiz);
        VCall<void>(mem_queue, kEnqueueStatus, status, 2u);
        SX_CHECK(SubmitAndWaitStatus(mem_queue, status, 2u, wait_fence, wait_seq));
        SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) == 0);

        std::vector<uint8_t> unswizzled(linear_tex.size(), 0);
        SX_CHECK(scarlettx::gpu::xg::CopyFromSubresource(layout, swizzled_dst.data(), 0u, 0u, unswizzled.data(), 32u * 4u));
        SX_CHECK(unswizzled == linear_tex);
    }

    // Verify IDStorageFactoryX::SetStagingBufferSize (slot 7) returns S_OK with zero FlightRecorder vtable misses!
    {
        scarlettx::FlightRecorderOptions fro{};
        fro.output_dir = (sandbox.path() / "flight").string();
        fro.hang_timeout_ms = 0;
        fro.install_veh = false;
        fro.enable_dred = false;
        fro.dump_failing_shaders = false;
        fro.reset_history = true;
        scarlettx::FlightRecorder::Instance().Enable(fro);
        const uint32_t misses_before = scarlettx::FlightRecorder::Instance().GetStats().unique_vtable_misses;
        SX_CHECK(VCall<int32_t>(factory, kSetStagingBufferSize, 64u * 1024u * 1024u) == 0);
        SX_CHECK(VCall<int32_t>(factory, kSetStagingBufferSize, 128u * 1024u * 1024u) == 0);
        const uint32_t misses_after = scarlettx::FlightRecorder::Instance().GetStats().unique_vtable_misses;
        SX_CHECK(misses_after == misses_before);
        scarlettx::FlightRecorder::Instance().Disable();
    }

    // Dynamically load dstorage_xs.dll & xbox_codecs.dll from SCARLETTX_BINARY_DIR:
    //  1) Verify dstorage_xs.dll!DStorageGetFactory(&guid_with_canaries, &fac) with SCARLETTX_REAL_TITLE
    //     unset, "0", and "1" NEVER corrupts stack-allocated GUIDs or surrounding stack canaries!
    //  2) Stream a multi-tile (> 64 KiB) BTYPE=2 Dynamic-Huffman 32-lane GDeflate file through
    //     xcrdapi.dll!DStorageGetFactory (IDStorageQueueX), synchronized purely via MockFence Win32 event!
    {
        const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
        HMODULE hCodecs = LoadLibraryW((bin_dir / L"xbox_codecs.dll").c_str());
        HMODULE hDsXs = LoadLibraryW((bin_dir / L"dstorage_xs.dll").c_str());
        SX_CHECK(hCodecs != nullptr && hDsXs != nullptr);

        using FnGetFactory = int32_t(WINAPI*)(const void*, void**);
        using FnCompressDyn = int32_t(__stdcall*)(const void*, size_t, void*, size_t, size_t*);
        auto fn_xs_get_factory = reinterpret_cast<FnGetFactory>(
            reinterpret_cast<void*>(GetProcAddress(hDsXs, "DStorageGetFactory")));
        auto fn_comp_dyn = reinterpret_cast<FnCompressDyn>(
            reinterpret_cast<void*>(GetProcAddress(hCodecs, "XboxCodecGDeflateCompress32LaneDynamic")));
        SX_CHECK(fn_xs_get_factory != nullptr && fn_comp_dyn != nullptr);

        struct GuardedStackGuid {
            uint64_t canary_lo;
            Guid iid;
            uint64_t canary_hi;
        };
        const Guid expected_iid = {0x6f26ea0cu, 0xc3cdu, 0x48bcu, {0xb1, 0x99, 0x70, 0x0d, 0x52, 0x97, 0x53, 0xf5}};

        auto verify_stack_guid_safe = [&](const char* env_val) -> bool {
            if (env_val) {
                _putenv_s("SCARLETTX_REAL_TITLE", env_val);
                SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", env_val[0] ? env_val : nullptr);
            } else {
                _putenv_s("SCARLETTX_REAL_TITLE", "");
                SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", nullptr);
            }

            GuardedStackGuid stack_box{
                0xDEADBEEFCAFEBABEull,
                expected_iid,
                0x0123456789ABCDEFull,
            };
            void* xs_fac = nullptr;
            if (fn_xs_get_factory(&stack_box.iid, &xs_fac) != 0 || xs_fac != factory) {
                return false;
            }
            if (stack_box.canary_lo != 0xDEADBEEFCAFEBABEull ||
                stack_box.canary_hi != 0x0123456789ABCDEFull ||
                std::memcmp(&stack_box.iid, &expected_iid, sizeof(Guid)) != 0) {
                return false;
            }
            VCall<uint32_t>(xs_fac, kRelease);

            if (fn_xs_get_factory(&stack_box.iid, nullptr) >= 0) {
                return false;
            }
            return stack_box.canary_lo == 0xDEADBEEFCAFEBABEull &&
                   stack_box.canary_hi == 0x0123456789ABCDEFull &&
                   std::memcmp(&stack_box.iid, &expected_iid, sizeof(Guid)) == 0;
        };

        SX_CHECK(verify_stack_guid_safe(nullptr));
        SX_CHECK(verify_stack_guid_safe("0"));
        SX_CHECK(verify_stack_guid_safe("1"));
        _putenv_s("SCARLETTX_REAL_TITLE", "");
        SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", nullptr);

        // Multi-tile (> 64 KiB, 3 tiles = 150,000 B) BTYPE=2 Dynamic-Huffman 32-lane GDeflate file streaming
        std::vector<uint8_t> dyn_plain(150000);
        for (size_t i = 0; i < dyn_plain.size(); ++i) {
            dyn_plain[i] = (i % 40u < 28u) ? static_cast<uint8_t>('A' + ((i / 40u + (i % 7u)) & 0x1Fu))
                                           : Pattern(i * 3u + 17u);
        }
        std::vector<uint8_t> dyn_compressed(dyn_plain.size() + 8192, 0);
        size_t dyn_comp_bytes = 0;
        SX_CHECK(fn_comp_dyn(
                     dyn_plain.data(), dyn_plain.size(), dyn_compressed.data(), dyn_compressed.size(), &dyn_comp_bytes) == 0);
        SX_CHECK(dyn_comp_bytes > 24u && dyn_comp_bytes < dyn_plain.size() / 2u);
        dyn_compressed.resize(dyn_comp_bytes);
        SX_CHECK(dyn_compressed[0] == 0x04u && dyn_compressed[1] == 0xFBu);
        // 3 tiles -> offset table ends at byte 20; lane 0 word 0 low 3 bits == 0b101 (BFINAL=1, BTYPE=2)
        SX_CHECK((dyn_compressed[20] & 0x7u) == 0x5u);

        const std::wstring gd_path = (sandbox.path() / L"btype2_multitile.gdeflate").wstring();
        {
            const HANDLE hg = CreateFileW(gd_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            SX_CHECK(hg != INVALID_HANDLE_VALUE);
            DWORD wr = 0;
            SX_CHECK(WriteFile(hg, dyn_compressed.data(), static_cast<DWORD>(dyn_compressed.size()), &wr, nullptr) &&
                     wr == dyn_compressed.size());
            CloseHandle(hg);
        }

        void* gd_file = nullptr;
        SX_CHECK(VCall<int32_t>(factory, kOpenFile, gd_path.c_str(), &kAnyIid, &gd_file) == 0 && gd_file != nullptr);

        // Stream 1: kZlib (0x04 0xFB GDeflate auto-dispatch)
        std::vector<uint8_t> out_zlib_opt(dyn_plain.size(), 0);
        Request r_gd1{};
        r_gd1.options = kZlib;
        r_gd1.destination = out_zlib_opt.data();
        r_gd1.destination_size = static_cast<uint32_t>(out_zlib_opt.size());
        r_gd1.source = gd_file;
        r_gd1.file_offset = 0;
        r_gd1.source_size = static_cast<uint32_t>(dyn_compressed.size());
        r_gd1.name = "btype2_file_zlib_opt";
        VCall<void>(queue, kEnqueueRequest, &r_gd1);
        VCall<void>(queue, kEnqueueStatus, status, 0u);
        SX_CHECK(SubmitAndWaitStatus(queue, status, 0u, wait_fence, wait_seq));
        SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
        SX_CHECK(out_zlib_opt == dyn_plain);

        // Stream 2: Explicit Xbox DirectStorage GDeflate compression option (kGDeflateOpt)
        std::vector<uint8_t> out_gdef_opt(dyn_plain.size(), 0);
        Request r_gd2 = r_gd1;
        r_gd2.options = kGDeflateOpt;
        r_gd2.destination = out_gdef_opt.data();
        r_gd2.name = "btype2_file_gdeflate_opt";
        VCall<void>(queue, kEnqueueRequest, &r_gd2);
        VCall<void>(queue, kEnqueueStatus, status, 1u);
        SX_CHECK(SubmitAndWaitStatus(queue, status, 1u, wait_fence, wait_seq));
        SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) == 0);
        SX_CHECK(out_gdef_opt == dyn_plain);

        VCall<void>(gd_file, kFileClose);
        SX_CHECK(VCall<uint32_t>(gd_file, kRelease) == 0);
        FreeLibrary(hDsXs);
        FreeLibrary(hCodecs);
    }

    SX_CHECK(VCall<uint32_t>(mem_queue, kRelease) == 0);
    SX_CHECK(VCall<uint32_t>(queue, kRelease) == 0);
    SX_CHECK(VCall<uint32_t>(status, kRelease) == 0);
    VCall<uint32_t>(factory, kRelease);

    std::cout << "[PASS] test_dstorage_guest (factory, SetStagingBufferSize, dstorage_xs stack-GUID safety, BTYPE=2 GDeflate)\n";
    return 0;
}

#else

int main() {
    std::cout << "[SKIP] test_dstorage_guest (Windows only)\n";
    return 0;
}

#endif
