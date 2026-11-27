// Drives the guest-facing DirectStorage objects the way a title does: one
// imported function, then raw vtable slots and SDK-layout structures.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/gpu/xg_layout.h"

#if defined(_WIN32)
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
constexpr int kCreateQueue = 3, kOpenFile = 4, kCreateStatusArray = 5;
constexpr int kEnqueueRequest = 3, kEnqueueStatus = 4, kEnqueueSignal = 5, kSubmit = 6, kCancelRequestsWithTag = 7;
constexpr int kFileClose = 3, kFileGetHandle = 4;
constexpr int kIsComplete = 3, kGetHResult = 4;
constexpr int kRelease = 2;

struct MockFence {
    void* vtable_ptr;
    void* vtable[16];
    std::atomic<uint32_t> ref_count{1};
    std::atomic<uint64_t> completed_val{0};

    static uint32_t WINAPI AddRef(void* self) {
        return static_cast<MockFence*>(self)->ref_count.fetch_add(1) + 1;
    }
    static uint32_t WINAPI Release(void* self) {
        return static_cast<MockFence*>(self)->ref_count.fetch_sub(1) - 1;
    }
    static uint64_t WINAPI GetCompletedValue(void* self) {
        return static_cast<MockFence*>(self)->completed_val.load();
    }
    static int32_t WINAPI Signal(void* self, uint64_t val) {
        static_cast<MockFence*>(self)->completed_val.store(val);
        return 0;
    }

    MockFence() {
        vtable_ptr = vtable;
        for (int i = 0; i < 16; ++i) vtable[i] = nullptr;
        vtable[1] = reinterpret_cast<void*>(&AddRef);
        vtable[2] = reinterpret_cast<void*>(&Release);
        vtable[8] = reinterpret_cast<void*>(&GetCompletedValue);
        vtable[10] = reinterpret_cast<void*>(&Signal);
    }
};

uint8_t Pattern(size_t i) { return static_cast<uint8_t>(i * 7 + (i >> 8) + 3); }

bool WaitComplete(void* status, uint32_t index) {
    for (int i = 0; i < 5000; ++i) {
        if (VCall<int32_t>(status, kIsComplete, index)) return true;
        Sleep(1);
    }
    return false;
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
    constexpr size_t kFileSize = 200000;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    SX_CHECK(GetTempPathW(MAX_PATH, dir) != 0);
    SX_CHECK(GetTempFileNameW(dir, L"sxd", 0, path) != 0);
    {
        std::vector<uint8_t> content(kFileSize);
        for (size_t i = 0; i < kFileSize; ++i) content[i] = Pattern(i);
        const HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    SX_CHECK(VCall<int32_t>(factory, kOpenFile, static_cast<const wchar_t*>(path), &kAnyIid, &file) == 0 && file != nullptr);
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
    std::vector<uint8_t> a(1000), b(50000);
    Request ra = FileRead(file, 100, a.data(), static_cast<uint32_t>(a.size()));
    Request rb = FileRead(file, 123456, b.data(), static_cast<uint32_t>(b.size()));
    VCall<void>(queue, kEnqueueRequest, &ra);
    VCall<void>(queue, kEnqueueRequest, &rb);
    std::memset(&ra, 0xEE, sizeof(ra));  // the title may reuse the structure at once
    VCall<void>(queue, kEnqueueStatus, status, 0u);
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) == 0);
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == static_cast<int32_t>(0x8000000Au));
    Sleep(20);
    SX_CHECK(VCall<int32_t>(status, kIsComplete, 0u) == 0);
    VCall<void>(queue, kSubmit);
    SX_CHECK(WaitComplete(status, 0));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
    SX_CHECK(Matches(a, 100) && Matches(b, 123456));

    // A read past the end fails its status entry; the next batch is clean again.
    std::vector<uint8_t> c(64);
    Request rc = FileRead(file, kFileSize - 10, c.data(), static_cast<uint32_t>(c.size()));
    Request rd = FileRead(file, 0, a.data(), static_cast<uint32_t>(a.size()));
    VCall<void>(queue, kEnqueueRequest, &rc);
    VCall<void>(queue, kEnqueueRequest, &rd);
    VCall<void>(queue, kEnqueueStatus, status, 1u);
    VCall<void>(queue, kSubmit);
    SX_CHECK(WaitComplete(status, 1));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) < 0);
    SX_CHECK(Matches(a, 0));
    VCall<void>(queue, kEnqueueRequest, &rb);
    VCall<void>(queue, kEnqueueStatus, status, 2u);
    VCall<void>(queue, kSubmit);
    SX_CHECK(WaitComplete(status, 2));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) == 0);

    // Decompression is not available: the request fails instead of copying raw bytes.
    Request rz = FileRead(file, 0, c.data(), static_cast<uint32_t>(c.size()));
    rz.options = kZlib;
    std::memset(c.data(), 0x5A, c.size());
    VCall<void>(queue, kEnqueueRequest, &rz);
    VCall<void>(queue, kEnqueueStatus, status, 3u);
    VCall<void>(queue, kSubmit);
    SX_CHECK(WaitComplete(status, 3));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 3u) == static_cast<int32_t>(0x80004001u));
    SX_CHECK(c[0] == 0x5A && c[63] == 0x5A);

    // More than half the capacity waiting (65 of 128 entries) submits without a Submit call.
    std::vector<uint8_t> many(64 * 16);
    for (uint32_t i = 0; i < 64; ++i) {
        Request r = FileRead(file, i * 1000, many.data() + i * 16, 16);
        VCall<void>(queue, kEnqueueRequest, &r);
    }
    Sleep(20);
    SX_CHECK(many[0] == 0 && many[15] == 0);
    VCall<void>(queue, kEnqueueStatus, status, 0u);
    SX_CHECK(WaitComplete(status, 0));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 0u) == 0);
    for (uint32_t i = 0; i < 64; ++i) {
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
    VCall<void>(mem_queue, kSubmit);
    SX_CHECK(WaitComplete(status, 1));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) == 0);
    SX_CHECK(dst == src);
    // A file request does not belong on it.
    VCall<void>(mem_queue, kEnqueueRequest, &rd);
    VCall<void>(mem_queue, kEnqueueStatus, status, 1u);
    VCall<void>(mem_queue, kSubmit);
    SX_CHECK(WaitComplete(status, 1));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 1u) < 0);

    // A request still queued keeps its file alive; a closed file fails its reads.
    VCall<void>(file, kFileClose);
    VCall<void>(queue, kEnqueueRequest, &rd);
    VCall<void>(queue, kEnqueueStatus, status, 2u);
    SX_CHECK(VCall<uint32_t>(file, kRelease) == 1);
    VCall<void>(queue, kSubmit);
    SX_CHECK(WaitComplete(status, 2));
    SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) < 0);

    // Test EnqueueSignal (slot 5) with a fence.
    MockFence fence;
    VCall<void>(mem_queue, kEnqueueRequest, &rm);
    VCall<void>(mem_queue, kEnqueueSignal, &fence, 999ull);
    SX_CHECK(fence.completed_val.load() == 0);
    VCall<void>(mem_queue, kSubmit);
    for (int i = 0; i < 5000 && fence.completed_val.load() < 999ull; ++i) Sleep(1);
    SX_CHECK(fence.completed_val.load() == 999ull);

    // Test CancelRequestsWithTag (slot 7).
    std::vector<uint8_t> cancel_dst(4096);
    Request r_cancel = rm;
    r_cancel.destination = cancel_dst.data();
    r_cancel.cancellation_tag = 0xCAFE;
    VCall<void>(mem_queue, kEnqueueRequest, &r_cancel);
    VCall<void>(mem_queue, kCancelRequestsWithTag, 0xFFFFull, 0xCAFEull);
    VCall<void>(mem_queue, kEnqueueStatus, status, 3u);
    VCall<void>(mem_queue, kSubmit);
    SX_CHECK(WaitComplete(status, 3));
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
        VCall<void>(mem_queue, kSubmit);
        SX_CHECK(WaitComplete(status, 0));
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
        VCall<void>(mem_queue, kSubmit);
        SX_CHECK(WaitComplete(status, 2));
        SX_CHECK(VCall<int32_t>(status, kGetHResult, 2u) == 0);

        std::vector<uint8_t> unswizzled(linear_tex.size(), 0);
        SX_CHECK(scarlettx::gpu::xg::CopyFromSubresource(layout, swizzled_dst.data(), 0u, 0u, unswizzled.data(), 32u * 4u));
        SX_CHECK(unswizzled == linear_tex);
    }

    SX_CHECK(VCall<uint32_t>(mem_queue, kRelease) == 0);
    SX_CHECK(VCall<uint32_t>(queue, kRelease) == 0);
    SX_CHECK(VCall<uint32_t>(status, kRelease) == 0);
    VCall<uint32_t>(factory, kRelease);
    SX_CHECK(DeleteFileW(path));

    std::cout << "[PASS] test_dstorage_guest (factory, file, queues, status array, signal, cancellation)\n";
    return 0;
}

#else

int main() {
    std::cout << "[SKIP] test_dstorage_guest (Windows only)\n";
    return 0;
}

#endif
