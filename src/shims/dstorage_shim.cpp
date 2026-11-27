#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/io/deflate_rfc1951.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/media/codecs_abi.h"

#include <algorithm>
#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kE_DSTORAGE_FILE_NOT_FOUND = static_cast<int32_t>(0x80070002u);
constexpr int32_t kE_DSTORAGE_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8007007Au);

constexpr uint8_t kCodecMagic[4] = {'S', 'X', 'C', 'R'};

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t CompressLzRle(
    const uint8_t* src,
    uint32_t src_size,
    uint8_t* dst,
    uint32_t dst_cap,
    uint32_t* out_size) {
    if (!out_size) return kE_INVALIDARG;
    if (src_size > 0 && !src) return kE_INVALIDARG;

    std::vector<uint8_t> tmp;
    tmp.reserve(src_size + 16);
    tmp.insert(tmp.end(), kCodecMagic, kCodecMagic + 4);
    tmp.push_back(static_cast<uint8_t>(src_size & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 8) & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 16) & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 24) & 0xFFu));

    uint32_t i = 0;
    while (i < src_size) {
        uint32_t run = 1;
        while (i + run < src_size && src[i + run] == src[i] && run < 130) {
            ++run;
        }
        if (run >= 3) {
            tmp.push_back(static_cast<uint8_t>(0x80u | (run - 3u)));
            tmp.push_back(src[i]);
            i += run;
            continue;
        }

        uint32_t lit_start = i;
        while (i < src_size && (i - lit_start) < 128) {
            if (i + 2 < src_size && src[i] == src[i + 1] && src[i] == src[i + 2]) {
                break;
            }
            ++i;
        }
        uint32_t lit_len = i - lit_start;
        tmp.push_back(static_cast<uint8_t>(lit_len - 1u));
        tmp.insert(tmp.end(), src + lit_start, src + lit_start + lit_len);
    }

    *out_size = static_cast<uint32_t>(tmp.size());
    if (!dst || dst_cap < tmp.size()) {
        return kE_DSTORAGE_BUFFER_TOO_SMALL;
    }
    std::memcpy(dst, tmp.data(), tmp.size());
    return kS_OK;
}

int32_t DecompressLzRle(
    const uint8_t* src,
    uint32_t src_size,
    uint8_t* dst,
    uint32_t dst_cap,
    uint32_t* out_size) {
    if (!src || src_size < 8 || !out_size) return kE_INVALIDARG;
    if (std::memcmp(src, kCodecMagic, 4) != 0) {
        return kE_FAIL;
    }
    const uint32_t expected = ReadLe32(src + 4);
    if (!dst || dst_cap < expected) {
        *out_size = expected;
        return kE_DSTORAGE_BUFFER_TOO_SMALL;
    }

    uint32_t in_pos = 8;
    uint32_t out_pos = 0;
    while (in_pos < src_size && out_pos < expected) {
        const uint8_t ctrl = src[in_pos++];
        if (ctrl & 0x80u) {
            if (in_pos >= src_size) return kE_FAIL;
            const uint32_t run = static_cast<uint32_t>(ctrl & 0x7Fu) + 3u;
            const uint8_t val = src[in_pos++];
            if (out_pos + run > expected) return kE_FAIL;
            std::memset(dst + out_pos, val, run);
            out_pos += run;
        } else {
            const uint32_t lit = static_cast<uint32_t>(ctrl) + 1u;
            if (in_pos + lit > src_size || out_pos + lit > expected) return kE_FAIL;
            std::memcpy(dst + out_pos, src + in_pos, lit);
            in_pos += lit;
            out_pos += lit;
        }
    }

    if (out_pos != expected) {
        return kE_FAIL;
    }
    *out_size = out_pos;
    return kS_OK;
}

int32_t DecompressUnifiedGDeflateOrZlib(
    const uint8_t* src,
    uint32_t src_size,
    uint8_t* dst,
    uint32_t dst_cap,
    uint32_t* out_size) {
    if (!src || src_size == 0 || !out_size) return kE_INVALIDARG;

    if (src_size >= 8u && std::memcmp(src, kCodecMagic, 4) == 0) {
        return DecompressLzRle(src, src_size, dst, dst_cap, out_size);
    }

    if (!dst || dst_cap == 0) return kE_DSTORAGE_BUFFER_TOO_SMALL;

    if (src_size >= 8u && src[0] == 0x04u && src[1] == 0xFBu) {
        size_t written = 0;
        const int32_t hr = XboxCodecGDeflateDecompress(src, src_size, dst, dst_cap, &written);
        if (hr == 0 && written > 0) {
            *out_size = static_cast<uint32_t>(written);
            return kS_OK;
        }
        return hr < 0 ? hr : kE_FAIL;
    }

    if (scarlettx::io::LooksLikeRfc1950ZlibHeader(src, src_size)) {
        size_t written = 0;
        if (scarlettx::io::InflateZlibRfc1950(src, src_size, dst, dst_cap, written) && written > 0) {
            *out_size = static_cast<uint32_t>(written);
            return kS_OK;
        }
        return kE_FAIL;
    }

    return kE_FAIL;
}

struct FileObject {
    bool is_xvd = false;
    bool is_vfs = false;
    std::string path;
    uint64_t size = 0;
    scarlettx::XvdContainer xvd;
#if defined(_WIN32)
    void* hFile = nullptr;
    ~FileObject() {
        if (hFile) {
            CloseHandle(static_cast<HANDLE>(hFile));
            hFile = nullptr;
        }
    }
#endif
};

struct StatusArrayObject {
    std::mutex mu;
    std::string name;
    std::vector<uint8_t> completed;
    std::vector<int32_t> hresults;
};

enum class QueueItemKind {
    kRequest,
    kTextureRegion,
    kSignal,
    kStatus,
};

struct QueueItem {
    QueueItemKind kind = QueueItemKind::kRequest;
    DSTORAGE_REQUEST req{};
    DSTORAGE_TEXTURE_REGION_DESC tex_req{};
    std::string req_name;
    uint64_t* fence_ptr = nullptr;
    uint64_t fence_val = 0;
    DSTORAGE_SIGNAL_CALLBACK callback = nullptr;
    void* callback_ctx = nullptr;
    uint64_t status_array = 0;
    uint32_t status_index = 0;
    bool cancelled = false;
};

struct QueueObject {
    DSTORAGE_QUEUE_DESC desc{};
    std::string name;
    std::mutex mu;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::condition_variable space_cv;
    std::deque<QueueItem> entries;
    std::size_t submitted_count = 0;
    bool stop = false;
    bool busy = false;
    DSTORAGE_ERROR_RECORD error_record{};
    int32_t last_batch_hr = kS_OK;
    std::thread worker;
    std::thread::id worker_id;
};

struct DStorageState {
    std::mutex mu;
    DSTORAGE_CONFIGURATION config{};
    uint64_t next_handle = 1;
    std::unordered_map<uint64_t, bool> factories;
    std::unordered_map<uint64_t, std::shared_ptr<FileObject>> files;
    std::unordered_map<uint64_t, std::shared_ptr<StatusArrayObject>> status_arrays;
    std::unordered_map<uint64_t, std::shared_ptr<QueueObject>> queues;
};

DStorageState& State() {
    static DStorageState s;
    return s;
}

void SubmitLocked(QueueObject& q) {
    if (q.submitted_count == q.entries.size()) return;
    q.submitted_count = q.entries.size();
    q.cv.notify_one();
}

int32_t EnqueueItem(QueueObject& q, QueueItem item) {
    std::unique_lock<std::mutex> lock(q.mu);
    const auto capacity = q.desc.Capacity;
    const auto has_room = [&]() {
        return capacity == 0 || q.entries.size() + static_cast<std::size_t>(q.busy) < capacity;
    };
    if (!q.stop && !has_room() && std::this_thread::get_id() == q.worker_id) {
        return kE_FAIL;
    }
    q.space_cv.wait(lock, [&]() { return q.stop || has_room(); });
    if (q.stop) return kE_HANDLE;

    q.entries.push_back(std::move(item));
    const auto staged_count = q.entries.size() - q.submitted_count;
    if (capacity != 0 && staged_count >= (capacity + 1u) / 2u) {
        SubmitLocked(q);
    }
    return kS_OK;
}

int32_t ReadSourcePayload(
    uint32_t source_type,
    uint64_t file_handle,
    uint64_t file_offset,
    const void* source_memory,
    uint32_t source_size,
    std::vector<uint8_t>& out_raw) {
    if (source_type == DSTORAGE_REQUEST_SOURCE_FILE) {
        std::shared_ptr<FileObject> file;
        {
            std::lock_guard<std::mutex> lk(State().mu);
            auto it = State().files.find(file_handle);
            if (it == State().files.end()) return kE_HANDLE;
            file = it->second;
        }
        if (file_offset > file->size || source_size > file->size - file_offset) {
            return kE_INVALIDARG;
        }
        if (file->is_xvd) {
            auto st = file->xvd.ReadUserData(file_offset, source_size, out_raw);
            if (!st.ok()) return kE_FAIL;
        } else if (file->is_vfs) {
            if (source_size == 0) {
                out_raw.clear();
                return kS_OK;
            }
            auto st = scarlettx::PackageVfs::Global().ReadFileRange(file->path, file_offset, source_size, out_raw);
            if (!st.ok() || out_raw.size() != source_size) {
                return kE_FAIL;
            }
        } else {
            out_raw.resize(source_size);
            bool read_done = false;
#if defined(_WIN32)
            if (file->hFile && file->hFile != INVALID_HANDLE_VALUE && source_size > 0) {
                HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (ev) {
                    OVERLAPPED ov{};
                    ov.Offset = static_cast<DWORD>(file_offset & 0xFFFFFFFFull);
                    ov.OffsetHigh = static_cast<DWORD>(file_offset >> 32);
                    ov.hEvent = ev;
                    DWORD bytes_read = 0;
                    BOOL ok = ReadFile(static_cast<HANDLE>(file->hFile), out_raw.data(),
                                       static_cast<DWORD>(source_size), &bytes_read, &ov);
                    if (!ok && GetLastError() == ERROR_IO_PENDING) {
                        ok = GetOverlappedResult(static_cast<HANDLE>(file->hFile), &ov, &bytes_read, TRUE);
                    }
                    CloseHandle(ev);
                    if (ok && bytes_read == source_size) {
                        read_done = true;
                    }
                }
            }
#endif
            if (!read_done) {
                std::ifstream ifs(file->path, std::ios::binary);
                if (!ifs) return kE_DSTORAGE_FILE_NOT_FOUND;
                ifs.seekg(static_cast<std::streamoff>(file_offset), std::ios::beg);
                ifs.read(reinterpret_cast<char*>(out_raw.data()), static_cast<std::streamsize>(source_size));
                if (static_cast<uint32_t>(ifs.gcount()) != source_size) return kE_FAIL;
            }
        }
        return kS_OK;
    }
    if (source_type == DSTORAGE_REQUEST_SOURCE_MEMORY) {
        if (!source_memory && source_size > 0) return kE_INVALIDARG;
        out_raw.resize(source_size);
        if (source_size > 0) {
            std::memcpy(out_raw.data(), source_memory, source_size);
        }
        return kS_OK;
    }
    return kE_INVALIDARG;
}

#if defined(_WIN32)
struct GpuLzPacket {
    uint32_t dst_start;
    uint32_t length;
    uint32_t src_offset_or_val;
    uint32_t is_rle;
};

struct GpuGDeflateTileDesc {
    uint32_t comp_off;
    uint32_t comp_len;
    uint32_t out_off;
    uint32_t out_cap;
};

const std::vector<uint8_t>& GetGpuLzComputeBytecode() {
    static const std::vector<uint8_t> bytecode = []() -> std::vector<uint8_t> {
        static const char kHlsl[] = R"(
cbuffer Params : register(b0) {
    uint g_UncompressedBytes;
    uint g_PacketCount;
    uint g_CompressedBytes;
    uint g_Pad;
};
ByteAddressBuffer g_Packets    : register(t0);
ByteAddressBuffer g_Compressed : register(t1);
RWByteAddressBuffer g_Output   : register(u0);

uint ReadCompressedByte(uint byteIdx) {
    if (byteIdx >= g_CompressedBytes) return 0u;
    uint word = g_Compressed.Load(byteIdx & ~3u);
    uint shift = (byteIdx & 3u) * 8u;
    return (word >> shift) & 0xFFu;
}

uint DecodeOutputByte(uint outByteIdx) {
    if (outByteIdx >= g_UncompressedBytes || g_PacketCount == 0u) return 0u;
    uint lo = 0u;
    uint hi = g_PacketCount - 1u;
    [loop]
    for (uint iter = 0u; iter < 24u && lo < hi; ++iter) {
        uint mid = (lo + hi + 1u) >> 1u;
        uint midStart = g_Packets.Load(mid * 16u);
        if (midStart <= outByteIdx) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    uint4 pkt = g_Packets.Load4(lo * 16u);
    if (pkt.w != 0u) {
        return pkt.z & 0xFFu;
    }
    return ReadCompressedByte(pkt.z + (outByteIdx - pkt.x));
}

[numthreads(32, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint baseByte = dtid.x * 4u;
    if (baseByte >= g_UncompressedBytes) return;
    uint b0 = DecodeOutputByte(baseByte + 0u);
    uint b1 = DecodeOutputByte(baseByte + 1u);
    uint b2 = DecodeOutputByte(baseByte + 2u);
    uint b3 = DecodeOutputByte(baseByte + 3u);
    g_Output.Store(baseByte, b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u));
}
)";
        ID3DBlob* cs_blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "DStorageGpuSxcr.hlsl",
                                nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &cs_blob, &err_blob);
        if (err_blob) err_blob->Release();
        std::vector<uint8_t> out;
        if (SUCCEEDED(hr) && cs_blob) {
            const auto* ptr = static_cast<const uint8_t*>(cs_blob->GetBufferPointer());
            out.assign(ptr, ptr + cs_blob->GetBufferSize());
            cs_blob->Release();
        }
        return out;
    }();
    return bytecode;
}

// Real 32-lane SIMD bit-interleaved GDeflate GPU Compute Shader (1 HLSL thread = 1 GDeflate bitlane).
const std::vector<uint8_t>& GetGpuGDeflate32ComputeBytecode() {
    static const std::vector<uint8_t> bytecode = []() -> std::vector<uint8_t> {
        static const char kHlsl[] = R"(
cbuffer Params : register(b0) {
    uint g_TileCount;
    uint g_CompressedBytes;
    uint g_UncompressedBytes;
    uint g_MaxWaves;
};
ByteAddressBuffer g_Tiles                     : register(t0);
ByteAddressBuffer g_Compressed                : register(t1);
globallycoherent RWByteAddressBuffer g_Output : register(u0);

groupshared uint gs_WordCursor;
groupshared uint gs_OutPos;
groupshared uint gs_NeedWord[32];
groupshared uint gs_OutLen[32];
groupshared uint gs_IsMatch[32];
groupshared uint gs_LitOrLen[32];
groupshared uint gs_Dist[32];
groupshared uint gs_HasIntraOverlap;

uint Reverse5(uint v) {
    uint r = ((v & 1u) << 4u) | ((v & 2u) << 2u) | (v & 4u) | ((v >> 2u) & 2u) | ((v >> 4u) & 1u);
    return r;
}

uint Reverse9(uint v) {
    uint r = 0u;
    [unroll]
    for (uint i = 0u; i < 9u; ++i) {
        r = (r << 1u) | ((v >> i) & 1u);
    }
    return r;
}

uint GetLenExtra(uint idx) {
    if (idx < 8u || idx >= 28u) return 0u;
    return (idx - 4u) >> 2u;
}

uint GetLenBase(uint idx) {
    if (idx < 8u) return 3u + idx;
    if (idx >= 28u) return 258u;
    uint ex = (idx - 4u) >> 2u;
    return 3u + ((4u + (idx & 3u)) << ex);
}

uint GetDistExtra(uint idx) {
    if (idx < 4u) return 0u;
    return (idx - 2u) >> 1u;
}

uint GetDistBase(uint idx) {
    if (idx < 4u) return 1u + idx;
    uint ex = (idx - 2u) >> 1u;
    return 1u + ((2u + (idx & 1u)) << ex);
}

uint ReadCompByte(uint idx) {
    if (idx >= g_CompressedBytes) return 0u;
    uint w = g_Compressed.Load(idx & ~3u);
    return (w >> ((idx & 3u) * 8u)) & 0xFFu;
}

uint ReadCompWord(uint bytePos) {
    if ((bytePos & 3u) == 0u && bytePos + 4u <= g_CompressedBytes) {
        return g_Compressed.Load(bytePos);
    }
    return ReadCompByte(bytePos) |
           (ReadCompByte(bytePos + 1u) << 8u) |
           (ReadCompByte(bytePos + 2u) << 16u) |
           (ReadCompByte(bytePos + 3u) << 24u);
}

uint ReadOutByte(uint idx) {
    uint w = g_Output.Load(idx & ~3u);
    return (w >> ((idx & 3u) * 8u)) & 0xFFu;
}

void WriteOutByteAtomic(uint idx, uint bval) {
    uint dummy = 0u;
    g_Output.InterlockedOr(idx & ~3u, (bval & 0xFFu) << ((idx & 3u) * 8u), dummy);
}

[numthreads(32, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint lane = gtid.x;
    uint4 td = (gid.x < g_TileCount) ? g_Tiles.Load4(gid.x * 16u) : uint4(0u, 0u, 0u, 0u);
    uint compOff = td.x;
    uint compLen = td.y;
    uint outOff  = td.z;
    uint outCap  = td.w;
    uint alignedCap = (outCap + 3u) & ~3u;

    [loop]
    for (uint ci = 0u; ci < 512u; ++ci) {
        uint rel = (ci * 32u + lane) * 4u;
        if (gid.x < g_TileCount && rel < alignedCap) {
            g_Output.Store(outOff + rel, 0u);
        }
    }
    AllMemoryBarrierWithGroupSync();

    uint tileWords = compLen >> 2u;
    uint curWord  = (lane < tileWords) ? ReadCompWord(compOff + lane * 4u) : 0u;
    uint nextWord = (32u + lane < tileWords) ? ReadCompWord(compOff + (32u + lane) * 4u) : 0u;
    uint bitOff = 0u;
    uint laneDone = (gid.x < g_TileCount && outCap > 0u) ? 0u : 1u;

    if (lane == 0u) {
        gs_WordCursor = (tileWords > 64u) ? 64u : tileWords;
        gs_OutPos = 0u;
    }
    if (laneDone == 0u) {
        uint hdr = curWord & 0x7u;
        if (((hdr >> 1u) & 0x3u) != 1u) {
            laneDone = 1u;
        }
        bitOff = 3u;
    }
    GroupMemoryBarrierWithGroupSync();

    [loop]
    for (uint wave = 0u; wave < g_MaxWaves; ++wave) {
        uint isMatch = 0u;
        uint litOrLen = 0u;
        uint dist = 0u;
        uint outBytes = 0u;
        uint nbits = 0u;

        if (laneDone == 0u) {
            uint bits = (bitOff == 0u) ? curWord : ((curWord >> bitOff) | (nextWord << (32u - bitOff)));
            uint rev9 = Reverse9(bits & 0x1FFu);
            uint rev7 = rev9 >> 2u;
            uint rev8 = rev9 >> 1u;
            uint sym = 256u;
            uint cbits = 7u;
            if (rev7 <= 23u) {
                sym = 256u + rev7;
                cbits = 7u;
            } else if (rev8 >= 48u && rev8 <= 191u) {
                sym = rev8 - 48u;
                cbits = 8u;
            } else if (rev8 >= 192u && rev8 <= 199u) {
                sym = 280u + (rev8 - 192u);
                cbits = 8u;
            } else {
                sym = 144u + (rev9 - 400u);
                cbits = 9u;
            }

            if (sym < 256u) {
                litOrLen = sym;
                outBytes = 1u;
                nbits = cbits;
            } else if (sym == 256u) {
                laneDone = 1u;
                nbits = 7u;
            } else if (sym <= 285u) {
                uint lenIdx = sym - 257u;
                uint lexBits = GetLenExtra(lenIdx);
                uint lexVal = (lexBits > 0u) ? ((bits >> cbits) & ((1u << lexBits) - 1u)) : 0u;
                uint mlen = GetLenBase(lenIdx) + lexVal;
                uint posAfterLen = cbits + lexBits;
                uint distSym = Reverse5((bits >> posAfterLen) & 0x1Fu);
                uint dexBits = GetDistExtra(distSym);
                uint dexVal = (dexBits > 0u) ? ((bits >> (posAfterLen + 5u)) & ((1u << dexBits) - 1u)) : 0u;
                uint mdist = GetDistBase(distSym) + dexVal;
                isMatch = 1u;
                litOrLen = mlen;
                dist = mdist;
                outBytes = mlen;
                nbits = posAfterLen + 5u + dexBits;
            } else {
                laneDone = 1u;
            }
            bitOff += nbits;
        }

        uint needRefill = (bitOff >= 32u) ? 1u : 0u;
        gs_NeedWord[lane] = needRefill;
        gs_OutLen[lane] = outBytes;
        gs_IsMatch[lane] = isMatch;
        gs_LitOrLen[lane] = litOrLen;
        gs_Dist[lane] = dist;
        if (lane == 0u) gs_HasIntraOverlap = 0u;
        GroupMemoryBarrierWithGroupSync();

        if (needRefill != 0u) {
            uint wPrefix = 0u;
            [unroll]
            for (uint k = 0u; k < 32u; ++k) {
                if (k < lane) wPrefix += gs_NeedWord[k];
            }
            uint fetchIdx = gs_WordCursor + wPrefix;
            curWord = nextWord;
            nextWord = (fetchIdx < tileWords) ? ReadCompWord(compOff + fetchIdx * 4u) : 0u;
            bitOff -= 32u;
        }

        uint outPrefix = 0u;
        [unroll]
        for (uint p = 0u; p < 32u; ++p) {
            if (p < lane) outPrefix += gs_OutLen[p];
        }
        uint laneDstRel = gs_OutPos + outPrefix;
        if (isMatch != 0u && dist < (outPrefix + litOrLen)) {
            uint prevFlag = 0u;
            InterlockedOr(gs_HasIntraOverlap, 1u, prevFlag);
        }
        GroupMemoryBarrierWithGroupSync();

        if (gs_HasIntraOverlap == 0u) {
            if (outBytes > 0u && laneDstRel < outCap) {
                if (isMatch == 0u) {
                    WriteOutByteAtomic(outOff + laneDstRel, litOrLen);
                } else {
                    uint copyCnt = min(litOrLen, outCap - laneDstRel);
                    [loop]
                    for (uint m = 0u; m < copyCnt; ++m) {
                        uint b = (laneDstRel + m >= dist) ? ReadOutByte(outOff + laneDstRel + m - dist) : 0u;
                        WriteOutByteAtomic(outOff + laneDstRel + m, b);
                    }
                }
            }
        } else if (lane == 0u) {
            uint curRel = gs_OutPos;
            [loop]
            for (uint k = 0u; k < 32u; ++k) {
                uint kLen = gs_OutLen[k];
                if (kLen > 0u && curRel < outCap) {
                    if (gs_IsMatch[k] == 0u) {
                        WriteOutByteAtomic(outOff + curRel, gs_LitOrLen[k]);
                    } else {
                        uint kDist = gs_Dist[k];
                        uint copyCnt = min(kLen, outCap - curRel);
                        [loop]
                        for (uint m = 0u; m < copyCnt; ++m) {
                            uint b = (curRel + m >= kDist) ? ReadOutByte(outOff + curRel + m - kDist) : 0u;
                            WriteOutByteAtomic(outOff + curRel + m, b);
                        }
                    }
                }
                curRel += kLen;
            }
        }

        if (lane == 0u) {
            uint totalWords = 0u;
            uint totalOut = 0u;
            [unroll]
            for (uint q = 0u; q < 32u; ++q) {
                totalWords += gs_NeedWord[q];
                totalOut += gs_OutLen[q];
            }
            gs_WordCursor += totalWords;
            gs_OutPos += totalOut;
        }
        AllMemoryBarrierWithGroupSync();
    }
}
)";
        ID3DBlob* cs_blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "DStorageGpuGDeflate32.hlsl",
                                nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &cs_blob, &err_blob);
        if (err_blob) err_blob->Release();
        std::vector<uint8_t> out;
        if (SUCCEEDED(hr) && cs_blob) {
            const auto* ptr = static_cast<const uint8_t*>(cs_blob->GetBufferPointer());
            out.assign(ptr, ptr + cs_blob->GetBufferSize());
            cs_blob->Release();
        }
        return out;
    }();
    return bytecode;
}

// Per-ID3D12Device cached Compute RootSignature + PSOs attached via SetPrivateDataInterface
// so D3D12 automatically releases them when the device is destroyed.
static const GUID kScarlettDsGpuCacheGuid = {
    0x7c8e91a4u, 0x3f2du, 0x4b91u, {0x9au, 0x12u, 0x55u, 0xe4u, 0x08u, 0xc7u, 0x33u, 0x19u}};

class GpuDecompressPipelineCache final : public IUnknown {
public:
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso_sxcr = nullptr;
    ID3D12PipelineState* pso_gdeflate32 = nullptr;

    ~GpuDecompressPipelineCache() {
        if (pso_gdeflate32) pso_gdeflate32->Release();
        if (pso_sxcr) pso_sxcr->Release();
        if (root_sig) root_sig->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            AddRef();
            *ppvObject = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const uint32_t rem = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) delete this;
        return static_cast<ULONG>(rem);
    }

private:
    std::atomic<uint32_t> refs_{1};
};

GpuDecompressPipelineCache* AcquireOrCreateGpuPipelineCache(ID3D12Device* dev) {
    static std::mutex cache_mu;
    std::lock_guard<std::mutex> lk(cache_mu);

    GpuDecompressPipelineCache* existing = nullptr;
    UINT ptr_sz = sizeof(existing);
    if (SUCCEEDED(dev->GetPrivateData(kScarlettDsGpuCacheGuid, &ptr_sz, &existing)) &&
        ptr_sz == sizeof(existing) && existing) {
        return existing;  // GetPrivateData already called AddRef() on the stored IUnknown
    }

    const auto& sxcr_dxbc = GetGpuLzComputeBytecode();
    const auto& gdef_dxbc = GetGpuGDeflate32ComputeBytecode();
    if (sxcr_dxbc.empty() || gdef_dxbc.empty()) {
        return nullptr;
    }

    auto* cache = new (std::nothrow) GpuDecompressPipelineCache();
    if (!cache) return nullptr;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 0;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)) || !rs_blob) {
        if (rs_err) rs_err->Release();
        cache->Release();
        return nullptr;
    }
    if (rs_err) rs_err->Release();
    HRESULT hr = dev->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&cache->root_sig));
    rs_blob->Release();
    if (FAILED(hr) || !cache->root_sig) {
        cache->Release();
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = cache->root_sig;
    pso_desc.CS.pShaderBytecode = sxcr_dxbc.data();
    pso_desc.CS.BytecodeLength = sxcr_dxbc.size();
    if (FAILED(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&cache->pso_sxcr))) || !cache->pso_sxcr) {
        cache->Release();
        return nullptr;
    }

    pso_desc.CS.pShaderBytecode = gdef_dxbc.data();
    pso_desc.CS.BytecodeLength = gdef_dxbc.size();
    if (FAILED(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&cache->pso_gdeflate32))) ||
        !cache->pso_gdeflate32) {
        cache->Release();
        return nullptr;
    }

    (void)dev->SetPrivateDataInterface(kScarlettDsGpuCacheGuid, cache);
    return cache;  // Caller holds the initial ref (1) and releases in cleanup()
}

bool WaitForGpuFenceSafe(ID3D12Device* dev, ID3D12Fence* fence, uint64_t target_val, HANDLE ev) {
    while (fence->GetCompletedValue() < target_val) {
        if (dev && FAILED(dev->GetDeviceRemovedReason())) {
            return false;
        }
        if (FAILED(fence->SetEventOnCompletion(target_val, ev))) {
            return false;
        }
        WaitForSingleObject(ev, INFINITE);
    }
    return !dev || SUCCEEDED(dev->GetDeviceRemovedReason());
}

bool ParseSxcrPacketsForGpu(const uint8_t* src, uint32_t src_size,
                            std::vector<GpuLzPacket>& out_packets, uint32_t* out_expected) {
    if (!src || src_size < 8 || std::memcmp(src, kCodecMagic, 4) != 0) return false;
    const uint32_t expected = ReadLe32(src + 4);
    out_packets.clear();
    uint32_t in_pos = 8;
    uint32_t out_pos = 0;
    while (in_pos < src_size && out_pos < expected) {
        const uint8_t ctrl = src[in_pos++];
        if (ctrl & 0x80u) {
            if (in_pos >= src_size) return false;
            const uint32_t run = static_cast<uint32_t>(ctrl & 0x7Fu) + 3u;
            const uint8_t val = src[in_pos++];
            if (out_pos + run > expected) return false;
            out_packets.push_back({out_pos, run, val, 1u});
            out_pos += run;
        } else {
            const uint32_t lit = static_cast<uint32_t>(ctrl) + 1u;
            if (in_pos + lit > src_size || out_pos + lit > expected) return false;
            out_packets.push_back({out_pos, lit, in_pos, 0u});
            in_pos += lit;
            out_pos += lit;
        }
    }
    if (out_pos != expected) return false;
    if (out_expected) *out_expected = expected;
    return true;
}

bool ParseGDeflate32TilesForGpu(
    const uint8_t* src,
    uint32_t src_size,
    uint32_t dst_cap,
    std::vector<GpuGDeflateTileDesc>& out_tiles,
    uint32_t* out_expected,
    uint32_t* out_max_waves) {
    if (!src || src_size < 12u || src[0] != 0x04u || src[1] != 0xFBu) return false;
    const uint16_t num_tiles = static_cast<uint16_t>(src[2] | (static_cast<uint16_t>(src[3]) << 8));
    if (num_tiles == 0u) return false;
    const uint32_t table_end = 8u + static_cast<uint32_t>(num_tiles) * 4u;
    if (src_size < table_end) return false;

    const uint32_t param = ReadLe32(src + 4);
    if ((param & 0x3u) != 1u) return false;
    uint32_t last_tile_sz = (param >> 2u) & 0x3FFFFu;
    if (last_tile_sz == 0u) last_tile_sz = 65536u;
    if (last_tile_sz > 65536u) return false;

    const uint64_t total_expected64 =
        static_cast<uint64_t>(num_tiles - 1u) * 65536ull + static_cast<uint64_t>(last_tile_sz);
    if (total_expected64 == 0ull || total_expected64 > dst_cap) return false;

    std::vector<uint32_t> offs(num_tiles);
    for (uint16_t t = 0; t < num_tiles; ++t) {
        offs[t] = ReadLe32(src + 8u + static_cast<uint32_t>(t) * 4u);
    }
    const uint32_t last_comp = offs[0];
    if (last_comp == 0u) return false;
    const uint32_t last_rel = (num_tiles == 1u) ? 0u : offs[num_tiles - 1u];
    if (static_cast<uint64_t>(table_end) + last_rel + last_comp != src_size) return false;

    out_tiles.resize(num_tiles);
    uint32_t max_tile_comp = 0u;
    uint32_t max_tile_uncomp = 0u;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const uint32_t rel = (t == 0u) ? 0u : offs[t];
        uint32_t csz = 0u;
        if (t + 1u == num_tiles) {
            csz = last_comp;
        } else {
            if (offs[t + 1u] <= rel) return false;
            csz = offs[t + 1u] - rel;
        }
        if (csz < 256u || (csz % 4u) != 0u || table_end + rel + csz > src_size) return false;
        const uint8_t* tile_ptr = src + table_end + rel;
        for (uint32_t lane = 0; lane < 32u; ++lane) {
            const uint32_t w0 = ReadLe32(tile_ptr + lane * 4u);
            if ((w0 & 0x7u) != 0x3u) return false;  // Verify 32-lane BFINAL=1, BTYPE=01 Fixed-Huffman header
        }
        const uint32_t t_uncomp = (t + 1u == num_tiles) ? last_tile_sz : 65536u;
        out_tiles[t] = {table_end + rel, csz, static_cast<uint32_t>(t) * 65536u, t_uncomp};
        max_tile_comp = std::max(max_tile_comp, csz);
        max_tile_uncomp = std::max(max_tile_uncomp, t_uncomp);
    }

    if (out_expected) *out_expected = static_cast<uint32_t>(total_expected64);
    if (out_max_waves) {
        const uint32_t bit_upper = (max_tile_comp * 8u) / 7u + 2u;
        *out_max_waves = std::min<uint32_t>(max_tile_uncomp + 2u, bit_upper);
    }
    return true;
}

int32_t ExecuteGpuBufferDecompress(
    ID3D12Device* ext_dev,
    ID3D12CommandQueue* ext_queue,
    uint8_t format,
    const uint8_t* compressed_src,
    uint32_t compressed_size,
    ID3D12Resource* dst_buf,
    uint64_t dst_offset,
    uint32_t uncompressed_size) {
    if (!dst_buf || uncompressed_size == 0 || (!compressed_src && compressed_size > 0)) {
        return kE_INVALIDARG;
    }

    ID3D12Device* dev = ext_dev;
    bool owns_dev = false;
    if (!dev) {
        if (FAILED(dst_buf->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) {
            return kE_INVALIDARG;
        }
        owns_dev = true;
    }

    std::vector<GpuLzPacket> packets;
    std::vector<GpuGDeflateTileDesc> gdef_tiles;
    std::vector<uint8_t> fallback_stream;
    const uint8_t* gpu_stream_ptr = compressed_src;
    uint32_t gpu_stream_len = compressed_size;
    uint32_t target_bytes = uncompressed_size;
    uint32_t gdef_max_waves = 0u;
    bool use_native_gpu_gdeflate32 = false;

    if (format == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (compressed_size < uncompressed_size) {
            if (owns_dev) dev->Release();
            return kE_DSTORAGE_BUFFER_TOO_SMALL;
        }
        packets.push_back({0u, uncompressed_size, 0u, 0u});
    } else if (format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE ||
               format == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        uint32_t parsed_expected = 0;
        if (ParseSxcrPacketsForGpu(compressed_src, compressed_size, packets, &parsed_expected)) {
            if (uncompressed_size < parsed_expected) {
                if (owns_dev) dev->Release();
                return kE_DSTORAGE_BUFFER_TOO_SMALL;
            }
            target_bytes = parsed_expected;
        } else if (ParseGDeflate32TilesForGpu(
                       compressed_src, compressed_size, uncompressed_size,
                       gdef_tiles, &parsed_expected, &gdef_max_waves)) {
            target_bytes = parsed_expected;
            use_native_gpu_gdeflate32 = true;
        } else if (compressed_size >= 8 && compressed_src[0] == 0x04u && compressed_src[1] == 0xFBu) {
            fallback_stream.resize(uncompressed_size);
            size_t written = 0;
            if (XboxCodecGDeflateDecompress(
                    compressed_src, compressed_size, fallback_stream.data(), uncompressed_size, &written) != 0 ||
                written == 0) {
                if (owns_dev) dev->Release();
                return kE_FAIL;
            }
            target_bytes = static_cast<uint32_t>(written);
            gpu_stream_ptr = fallback_stream.data();
            gpu_stream_len = target_bytes;
            packets.push_back({0u, target_bytes, 0u, 0u});
        } else if (scarlettx::io::LooksLikeRfc1950ZlibHeader(compressed_src, compressed_size)) {
            fallback_stream.resize(uncompressed_size);
            size_t written = 0;
            if (!scarlettx::io::InflateZlibRfc1950(
                    compressed_src, compressed_size, fallback_stream.data(), uncompressed_size, written) ||
                written == 0) {
                if (owns_dev) dev->Release();
                return kE_FAIL;
            }
            target_bytes = static_cast<uint32_t>(written);
            gpu_stream_ptr = fallback_stream.data();
            gpu_stream_len = target_bytes;
            packets.push_back({0u, target_bytes, 0u, 0u});
        } else {
            if (owns_dev) dev->Release();
            return kE_FAIL;
        }
    } else {
        if (owns_dev) dev->Release();
        return kE_NOTIMPL;
    }

    GpuDecompressPipelineCache* pipe_cache = AcquireOrCreateGpuPipelineCache(dev);
    if (!pipe_cache) {
        if (owns_dev) dev->Release();
        return kE_FAIL;
    }

    ID3D12CommandQueue* queue = ext_queue;
    bool owns_queue = false;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* pkt_buf = nullptr;
    ID3D12Resource* cmp_buf = nullptr;
    ID3D12Resource* uav_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (uav_buf) uav_buf->Release();
        if (cmp_buf) cmp_buf->Release();
        if (pkt_buf) pkt_buf->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (pipe_cache) pipe_cache->Release();
        if (owns_queue && queue) queue->Release();
        if (owns_dev && dev) dev->Release();
    };

    D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (queue) {
        list_type = queue->GetDesc().Type;
    } else {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
            cleanup();
            return kE_FAIL;
        }
        owns_queue = true;
    }

    auto make_buf = [&](D3D12_HEAP_TYPE htype, uint64_t bytes, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state, ID3D12Resource** out_res) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = htype;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<uint64_t>(16ull, (bytes + 3ull) & ~3ull);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(out_res)));
    };

    const uint64_t meta_bytes = use_native_gpu_gdeflate32
                                    ? std::max<uint64_t>(16ull, gdef_tiles.size() * sizeof(GpuGDeflateTileDesc))
                                    : std::max<uint64_t>(16ull, packets.size() * sizeof(GpuLzPacket));
    const uint64_t cmp_bytes = std::max<uint64_t>(16ull, (static_cast<uint64_t>(gpu_stream_len) + 3ull) & ~3ull);
    const uint64_t uav_bytes = std::max<uint64_t>(16ull, (static_cast<uint64_t>(target_bytes) + 3ull) & ~3ull);

    if (!make_buf(D3D12_HEAP_TYPE_UPLOAD, meta_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &pkt_buf) ||
        !make_buf(D3D12_HEAP_TYPE_UPLOAD, cmp_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &cmp_buf) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, uav_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &uav_buf)) {
        cleanup();
        return kE_OUTOFMEMORY;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(pkt_buf->Map(0, nullptr, &mapped)) && mapped) {
        std::memset(mapped, 0, static_cast<size_t>(meta_bytes));
        if (use_native_gpu_gdeflate32) {
            std::memcpy(mapped, gdef_tiles.data(), gdef_tiles.size() * sizeof(GpuGDeflateTileDesc));
        } else if (!packets.empty()) {
            std::memcpy(mapped, packets.data(), packets.size() * sizeof(GpuLzPacket));
        }
        pkt_buf->Unmap(0, nullptr);
    }
    mapped = nullptr;
    if (SUCCEEDED(cmp_buf->Map(0, nullptr, &mapped)) && mapped) {
        std::memset(mapped, 0, static_cast<size_t>(cmp_bytes));
        if (gpu_stream_len > 0) std::memcpy(mapped, gpu_stream_ptr, gpu_stream_len);
        cmp_buf->Unmap(0, nullptr);
    }

    ID3D12PipelineState* active_pso =
        use_native_gpu_gdeflate32 ? pipe_cache->pso_gdeflate32 : pipe_cache->pso_sxcr;
    if (FAILED(dev->CreateCommandAllocator(list_type, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, list_type, alloc, active_pso, IID_PPV_ARGS(&cmd)))) {
        cleanup();
        return kE_FAIL;
    }

    D3D12_RESOURCE_BARRIER to_uav{};
    to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav.Transition.pResource = uav_buf;
    to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &to_uav);

    cmd->SetComputeRootSignature(pipe_cache->root_sig);
    if (use_native_gpu_gdeflate32) {
        const uint32_t consts[4] = {
            static_cast<uint32_t>(gdef_tiles.size()),
            gpu_stream_len,
            target_bytes,
            gdef_max_waves,
        };
        cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
    } else {
        const uint32_t consts[4] = {
            target_bytes,
            static_cast<uint32_t>(packets.size()),
            gpu_stream_len,
            0u,
        };
        cmd->SetComputeRoot32BitConstants(0, 4, consts, 0);
    }
    cmd->SetComputeRootShaderResourceView(1, pkt_buf->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, cmp_buf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, uav_buf->GetGPUVirtualAddress());

    if (use_native_gpu_gdeflate32) {
        cmd->Dispatch(static_cast<uint32_t>(gdef_tiles.size()), 1u, 1u);
    } else {
        const uint32_t dwords = (target_bytes + 3u) / 4u;
        const uint32_t groups_x = std::max<uint32_t>(1u, (dwords + 31u) / 32u);
        cmd->Dispatch(groups_x, 1u, 1u);
    }

    D3D12_RESOURCE_BARRIER to_src{};
    to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_src.Transition.pResource = uav_buf;
    to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &to_src);

    cmd->CopyBufferRegion(dst_buf, dst_offset, uav_buf, 0ull, target_bytes);

    if (FAILED(cmd->Close())) {
        cleanup();
        return kE_FAIL;
    }

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return kE_FAIL;
    }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        cleanup();
        return kE_FAIL;
    }

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence, 1ull)) || !WaitForGpuFenceSafe(dev, fence, 1ull, ev)) {
        cleanup();
        return kE_FAIL;
    }

    cleanup();
    return kS_OK;
}

int32_t ExecuteTextureRegion(const DSTORAGE_TEXTURE_REGION_DESC& desc) {
    if (!desc.DestinationTexture) return kE_INVALIDARG;

    std::vector<uint8_t> raw;
    const int32_t io_hr = ReadSourcePayload(
        desc.SourceType, desc.SourceFileHandle, desc.SourceFileOffset,
        desc.SourceMemory, desc.SourceSize, raw);
    if (io_hr != kS_OK) return io_hr;

    auto* dst_tex = static_cast<ID3D12Resource*>(desc.DestinationTexture);
    ID3D12Device* dev = static_cast<ID3D12Device*>(desc.D3D12Device);
    bool owns_dev = false;
    if (!dev) {
        if (FAILED(dst_tex->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) {
            return kE_INVALIDARG;
        }
        owns_dev = true;
    }

    const D3D12_RESOURCE_DESC tex_desc = dst_tex->GetDesc();
    const uint32_t mips = std::max<uint32_t>(1u, tex_desc.MipLevels);
    const uint32_t mip = desc.SubresourceIndex % mips;
    const uint32_t mip_w = std::max<uint32_t>(1u, static_cast<uint32_t>(tex_desc.Width >> mip));
    const uint32_t mip_h = std::max<uint32_t>(1u, tex_desc.Height >> mip);
    const uint32_t reg_w = (desc.RegionWidth > 0) ? desc.RegionWidth : mip_w;
    const uint32_t reg_h = (desc.RegionHeight > 0) ? desc.RegionHeight : mip_h;
    const uint32_t reg_d = (desc.RegionDepth > 0) ? desc.RegionDepth : 1u;

    const uint32_t dxgi_fmt = static_cast<uint32_t>(tex_desc.Format);
    const uint32_t bpe = scarlettx::gpu::xg::FormatBytesPerElement(dxgi_fmt);
    const bool is_bc = scarlettx::gpu::xg::IsBlockCompressed(dxgi_fmt);
    const uint32_t elems_x = is_bc ? std::max<uint32_t>(1u, (reg_w + 3u) / 4u) : reg_w;
    const uint32_t elems_y = is_bc ? std::max<uint32_t>(1u, (reg_h + 3u) / 4u) : reg_h;
    const uint32_t tight_row_bytes = elems_x * bpe;
    const uint32_t tight_slice_bytes = tight_row_bytes * elems_y;
    const uint32_t expected_linear_bytes = tight_slice_bytes * reg_d;
    const uint32_t decompress_cap = std::max<uint32_t>(
        desc.UncompressedSize ? desc.UncompressedSize : expected_linear_bytes,
        expected_linear_bytes);

    std::vector<uint8_t> decoded;
    if (desc.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        decoded = std::move(raw);
    } else if (desc.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE ||
               desc.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        decoded.resize(decompress_cap);
        uint32_t out_sz = 0;
        const int32_t d_hr = DecompressUnifiedGDeflateOrZlib(
            raw.data(), static_cast<uint32_t>(raw.size()), decoded.data(), decompress_cap, &out_sz);
        if (d_hr != kS_OK) {
            if (owns_dev) dev->Release();
            return d_hr;
        }
        decoded.resize(out_sz);
    } else if (desc.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_BCPACK) {
        decoded.resize(decompress_cap);
        const uint32_t bcfmt = desc.BcPackFormat ? desc.BcPackFormat : dxgi_fmt;
        if (XboxCodecBcPackDecompress(raw.data(), raw.size(), decoded.data(), decompress_cap, bcfmt) != 0) {
            if (owns_dev) dev->Release();
            return kE_FAIL;
        }
    } else {
        if (owns_dev) dev->Release();
        return kE_NOTIMPL;
    }

    std::vector<uint8_t> linear_texels;
    const uint8_t* linear_src = decoded.data();
    size_t linear_avail = decoded.size();
    if (desc.SwizzleMode != 0) {
        XG_TEXTURE2D_DESC td{};
        td.Width = reg_w;
        td.Height = reg_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_fmt;
        td.SampleDesc.Count = 1;
        td.SwizzleMode = desc.SwizzleMode;
        XG_RESOURCE_LAYOUT xgl{};
        scarlettx::gpu::xg::ComputeTexture2DLayout(td, &xgl);
        if (scarlettx::gpu::xg::IsAddressable(xgl) && decoded.size() >= xgl.SizeBytes) {
            linear_texels.resize(expected_linear_bytes, 0);
            if (!scarlettx::gpu::xg::CopyFromSubresource(xgl, decoded.data(), 0u, 0u, linear_texels.data(), tight_row_bytes)) {
                if (owns_dev) dev->Release();
                return kE_FAIL;
            }
            linear_src = linear_texels.data();
            linear_avail = linear_texels.size();
        }
    }

    if (linear_avail < expected_linear_bytes) {
        if (owns_dev) dev->Release();
        return kE_DSTORAGE_BUFFER_TOO_SMALL;
    }

    const uint32_t aligned_pitch =
        ((tight_row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) *
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const uint64_t upload_bytes = static_cast<uint64_t>(aligned_pitch) * elems_y * reg_d;

    ID3D12CommandQueue* queue = static_cast<ID3D12CommandQueue*>(desc.D3D12CommandQueue);
    bool owns_queue = false;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* upload_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (upload_buf) upload_buf->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (owns_queue && queue) queue->Release();
        if (owns_dev && dev) dev->Release();
    };

    if (!queue) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
            cleanup();
            return kE_FAIL;
        }
        owns_queue = true;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = std::max<uint64_t>(256ull, upload_bytes);
    up_desc.Height = 1;
    up_desc.DepthOrArraySize = 1;
    up_desc.MipLevels = 1;
    up_desc.SampleDesc.Count = 1;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &up_desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buf)))) {
        cleanup();
        return kE_OUTOFMEMORY;
    }

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped)) || !mapped) {
        cleanup();
        return kE_FAIL;
    }
    auto* dst_rows = static_cast<uint8_t*>(mapped);
    for (uint32_t z = 0; z < reg_d; ++z) {
        for (uint32_t y = 0; y < elems_y; ++y) {
            std::memcpy(dst_rows + (static_cast<size_t>(z) * elems_y + y) * aligned_pitch,
                        linear_src + (static_cast<size_t>(z) * elems_y + y) * tight_row_bytes,
                        tight_row_bytes);
        }
    }
    upload_buf->Unmap(0, nullptr);

    const D3D12_COMMAND_LIST_TYPE q_type = queue->GetDesc().Type;
    if (FAILED(dev->CreateCommandAllocator(q_type, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, q_type, alloc, nullptr, IID_PPV_ARGS(&cmd)))) {
        cleanup();
        return kE_FAIL;
    }

    const auto init_state = static_cast<D3D12_RESOURCE_STATES>(desc.InitialResourceState);
    if (q_type != D3D12_COMMAND_LIST_TYPE_COPY && init_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst_tex;
        b.Transition.Subresource = desc.SubresourceIndex;
        b.Transition.StateBefore = init_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload_buf;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = tex_desc.Format;
    src_loc.PlacedFootprint.Footprint.Width = reg_w;
    src_loc.PlacedFootprint.Footprint.Height = reg_h;
    src_loc.PlacedFootprint.Footprint.Depth = reg_d;
    src_loc.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst_tex;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = desc.SubresourceIndex;

    D3D12_BOX src_box{0u, 0u, 0u, reg_w, reg_h, reg_d};
    cmd->CopyTextureRegion(&dst_loc, desc.DstX, desc.DstY, desc.DstZ, &src_loc, &src_box);

    if (q_type != D3D12_COMMAND_LIST_TYPE_COPY && init_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst_tex;
        b.Transition.Subresource = desc.SubresourceIndex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = init_state;
        cmd->ResourceBarrier(1, &b);
    }

    if (FAILED(cmd->Close())) {
        cleanup();
        return kE_FAIL;
    }

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return kE_FAIL;
    }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        cleanup();
        return kE_FAIL;
    }

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence, 1ull)) || !WaitForGpuFenceSafe(dev, fence, 1ull, ev)) {
        cleanup();
        return kE_FAIL;
    }

    cleanup();
    return kS_OK;
}
#endif

int32_t ExecuteRequest(const DSTORAGE_REQUEST& req) {
    if (!req.DestinationMemory || req.DestinationSize == 0) {
        return kE_INVALIDARG;
    }

    std::vector<uint8_t> raw;
    const int32_t io_hr = ReadSourcePayload(
        req.SourceType, req.SourceFileHandle, req.SourceFileOffset,
        req.SourceMemory, req.SourceSize, raw);
    if (io_hr != kS_OK) return io_hr;

#if defined(_WIN32)
    if (req.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
        bool gpu_disabled = false;
        {
            std::lock_guard<std::mutex> lk(State().mu);
            gpu_disabled = (State().config.DisableGpuDecompression != 0);
        }
        const uint32_t target_sz = req.UncompressedSize ? req.UncompressedSize : req.DestinationSize;
        if (!gpu_disabled) {
            return ExecuteGpuBufferDecompress(
                nullptr, nullptr, req.CompressionFormat,
                raw.data(), static_cast<uint32_t>(raw.size()),
                static_cast<ID3D12Resource*>(req.DestinationMemory),
                0ull, target_sz);
        }
    }
#endif

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (raw.size() > req.DestinationSize) {
            return kE_DSTORAGE_BUFFER_TOO_SMALL;
        }
        if (!raw.empty()) {
            std::memcpy(req.DestinationMemory, raw.data(), raw.size());
        }
        return kS_OK;
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        uint32_t out_sz = 0;
        return DecompressUnifiedGDeflateOrZlib(
            raw.data(),
            static_cast<uint32_t>(raw.size()),
            static_cast<uint8_t*>(req.DestinationMemory),
            req.DestinationSize,
            &out_sz);
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_BCPACK) {
        return XboxCodecBcPackDecompress(raw.data(), raw.size(), req.DestinationMemory, req.DestinationSize, 1u);
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        uint32_t out_sz = 0;
        return DecompressLzRle(
            raw.data(),
            static_cast<uint32_t>(raw.size()),
            static_cast<uint8_t*>(req.DestinationMemory),
            req.DestinationSize,
            &out_sz);
    }

    return kE_NOTIMPL;
}

void QueueWorkerLoop(std::shared_ptr<QueueObject> q) {
#if defined(_WIN32)
    int win_prio = THREAD_PRIORITY_NORMAL;
    if (q->desc.Priority == DSTORAGE_PRIORITY_REALTIME || q->desc.Priority == DSTORAGE_PRIORITY_HIGH) {
        win_prio = THREAD_PRIORITY_ABOVE_NORMAL;
    } else if (q->desc.Priority == DSTORAGE_PRIORITY_LOW) {
        win_prio = THREAD_PRIORITY_BELOW_NORMAL;
    }
    SetThreadPriority(GetCurrentThread(), win_prio);
#endif
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(q->mu);
            q->cv.wait(lk, [&]() { return q->stop || q->submitted_count != 0; });
            if (q->submitted_count == 0) {
                return;
            }
            item = std::move(q->entries.front());
            q->entries.pop_front();
            --q->submitted_count;
            q->busy = true;
        }

        if (item.kind == QueueItemKind::kRequest || item.kind == QueueItemKind::kTextureRegion) {
            int32_t hr = kS_OK;
            if (!item.cancelled) {
                if (item.kind == QueueItemKind::kRequest) {
                    hr = ExecuteRequest(item.req);
                } else {
#if defined(_WIN32)
                    hr = ExecuteTextureRegion(item.tex_req);
#else
                    hr = kE_NOTIMPL;
#endif
                }
            }
            if (hr < 0) {
                std::lock_guard<std::mutex> lk(q->mu);
                if (q->error_record.FailureCount == 0) {
                    q->error_record.FirstFailureHResult = hr;
                    q->error_record.CommandType = 1;
                    std::memset(q->error_record.FailedRequestName, 0, sizeof(q->error_record.FailedRequestName));
                    std::strncpy(
                        q->error_record.FailedRequestName,
                        item.req_name.c_str(),
                        sizeof(q->error_record.FailedRequestName) - 1);
                }
                q->error_record.FailureCount++;
                q->last_batch_hr = hr;
            }
        } else if (item.kind == QueueItemKind::kSignal) {
            if (item.fence_ptr) {
                *item.fence_ptr = item.fence_val;
            }
            if (item.callback) {
                item.callback(item.fence_val, item.callback_ctx);
            }
        } else if (item.kind == QueueItemKind::kStatus) {
            std::shared_ptr<StatusArrayObject> arr;
            {
                std::lock_guard<std::mutex> lk(State().mu);
                auto it = State().status_arrays.find(item.status_array);
                if (it != State().status_arrays.end()) arr = it->second;
            }
            int32_t batch_hr = kS_OK;
            {
                std::lock_guard<std::mutex> lk(q->mu);
                batch_hr = q->last_batch_hr;
                q->last_batch_hr = kS_OK;
            }
            if (arr) {
                std::lock_guard<std::mutex> lk(arr->mu);
                if (item.status_index < arr->completed.size()) {
                    arr->hresults[item.status_index] = batch_hr;
                    arr->completed[item.status_index] = 1;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(q->mu);
            q->busy = false;
            if (q->submitted_count == 0) {
                q->idle_cv.notify_all();
            }
        }
        q->space_cv.notify_one();
    }
}

}  // namespace

extern "C" {

int32_t DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* config) {
    if (!config) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    State().config = *config;
    return kS_OK;
}

int32_t DStorageGetConfiguration(DSTORAGE_CONFIGURATION* out_config) {
    if (!out_config) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    *out_config = State().config;
    return kS_OK;
}

int32_t DStorageGetFactory(uint64_t* out_factory) {
    if (!out_factory) return kE_INVALIDARG;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(out_factory, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (!writable) {
            return kE_INVALIDARG;
        }
    }
#endif
    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().factories[h] = true;
    *out_factory = h;
    return kS_OK;
}

int32_t DStorageDestroyFactory(uint64_t factory) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().factories.erase(factory) == 0) return kE_HANDLE;
    return kS_OK;
}

int32_t DStorageOpenFile(uint64_t factory, const char* path, uint64_t* out_file) {
    if (!path || !out_file) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    const bool is_virtual_drive =
        (path[0] == 'G' || path[0] == 'g' || path[0] == 'D' || path[0] == 'd') && path[1] == ':';

    // Prefer PackageVfs::Global() on G:/D: paths when mounted (handles embedded .xvc/.xvd and host G: collisions)
    if (is_virtual_drive && scarlettx::PackageVfs::Global().Exists(path)) {
        uint64_t vfs_sz = 0;
        if (scarlettx::PackageVfs::Global().GetFileSize(path, vfs_sz)) {
            auto obj = std::make_shared<FileObject>();
            obj->is_vfs = true;
            obj->path = path;
            obj->size = vfs_sz;
            std::lock_guard<std::mutex> lk(State().mu);
            const uint64_t h = State().next_handle++;
            State().files[h] = std::move(obj);
            *out_file = h;
            return kS_OK;
        }
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        if (scarlettx::PackageVfs::Global().Exists(path)) {
            uint64_t vfs_sz = 0;
            if (scarlettx::PackageVfs::Global().GetFileSize(path, vfs_sz)) {
                auto obj = std::make_shared<FileObject>();
                obj->is_vfs = true;
                obj->path = path;
                obj->size = vfs_sz;
                std::lock_guard<std::mutex> lk(State().mu);
                const uint64_t h = State().next_handle++;
                State().files[h] = std::move(obj);
                *out_file = h;
                return kS_OK;
            }
        }
        return kE_DSTORAGE_FILE_NOT_FOUND;
    }
    const auto end_pos = ifs.tellg();
    if (end_pos < 0) return kE_FAIL;

    auto obj = std::make_shared<FileObject>();
    obj->is_xvd = false;
    obj->path = path;
    obj->size = static_cast<uint64_t>(end_pos);
#if defined(_WIN32)
    obj->hFile = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (obj->hFile == INVALID_HANDLE_VALUE) {
        obj->hFile = nullptr;
    }
#endif

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().files[h] = std::move(obj);
    *out_file = h;
    return kS_OK;
}

int32_t DStorageOpenXvdFile(uint64_t factory, const char* xvd_path, uint64_t* out_file) {
    if (!xvd_path || !out_file) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    auto obj = std::make_shared<FileObject>();
    obj->is_xvd = true;
    obj->path = xvd_path;
    auto st = obj->xvd.OpenFile(xvd_path);
    if (!st.ok()) return kE_FAIL;
    if (obj->xvd.Header().is_encrypted) {
        if (const char* env_key = std::getenv("SCARLETTX_VOLUME_KEY"); env_key && env_key[0] != '\0') {
            if (!obj->xvd.SetVolumeKeyHex(env_key).ok()) {
                (void)obj->xvd.LoadVolumeKeyFile(env_key);
            }
        }
        if (!obj->xvd.HasVolumeKey()) return kE_NOTIMPL;
    }
    obj->size = obj->xvd.Header().user_data_length;

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().files[h] = std::move(obj);
    *out_file = h;
    return kS_OK;
}

int32_t DStorageCloseFile(uint64_t file) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().files.erase(file) ? kS_OK : kE_HANDLE;
}

int32_t DStorageGetFileSize(uint64_t file, uint64_t* out_size) {
    if (!out_size) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().files.find(file);
    if (it == State().files.end()) return kE_HANDLE;
    *out_size = it->second->size;
    return kS_OK;
}

int32_t DStorageCreateQueue(
    uint64_t factory,
    const DSTORAGE_QUEUE_DESC* desc,
    uint64_t* out_queue) {
    if (!desc || !out_queue) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    auto q = std::make_shared<QueueObject>();
    q->desc = *desc;
    if (desc->Name) q->name = desc->Name;
    q->worker = std::thread(QueueWorkerLoop, q);
    q->worker_id = q->worker.get_id();

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().queues[h] = std::move(q);
    *out_queue = h;
    return kS_OK;
}

int32_t DStorageDestroyQueue(uint64_t queue) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = std::move(it->second);
        State().queues.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(q->mu);
        q->stop = true;
    }
    q->cv.notify_all();
    q->space_cv.notify_all();
    if (q->worker.joinable()) {
        q->worker.join();
    }
    return kS_OK;
}

int32_t DStorageEnqueueRequest(uint64_t queue, const DSTORAGE_REQUEST* request) {
    if (!request) return kE_INVALIDARG;
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kRequest;
    item.req = *request;
    if (request->Name) item.req_name = request->Name;
    return EnqueueItem(*q, std::move(item));
}

int32_t DStorageEnqueueSignal(
    uint64_t queue,
    uint64_t* fence_value_ptr,
    uint64_t value,
    DSTORAGE_SIGNAL_CALLBACK callback,
    void* context) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kSignal;
    item.fence_ptr = fence_value_ptr;
    item.fence_val = value;
    item.callback = callback;
    item.callback_ctx = context;
    return EnqueueItem(*q, std::move(item));
}

int32_t DStorageEnqueueStatus(uint64_t queue, uint64_t status_array, uint32_t index) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        if (State().status_arrays.find(status_array) == State().status_arrays.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kStatus;
    item.status_array = status_array;
    item.status_index = index;
    return EnqueueItem(*q, std::move(item));
}

int32_t DStorageSubmit(uint64_t queue) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::lock_guard<std::mutex> lk(q->mu);
    if (q->stop) return kE_HANDLE;
    SubmitLocked(*q);
    return kS_OK;
}

int32_t DStorageWaitForIdle(uint64_t queue, uint32_t timeout_ms) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::unique_lock<std::mutex> lk(q->mu);
    auto pred = [&]() { return !q->busy && q->submitted_count == 0; };
    if (timeout_ms == 0xFFFFFFFFu) {
        q->idle_cv.wait(lk, pred);
        return kS_OK;
    }
    const bool done = q->idle_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
    return done ? kS_OK : kE_FAIL;
}

int32_t DStorageCancelRequestsWithTag(uint64_t queue, uint64_t mask, uint64_t value) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::lock_guard<std::mutex> lk(q->mu);
    for (auto& item : q->entries) {
        const uint64_t tag = (item.kind == QueueItemKind::kTextureRegion)
                                 ? item.tex_req.CancellationTag
                                 : item.req.CancellationTag;
        if ((item.kind == QueueItemKind::kRequest || item.kind == QueueItemKind::kTextureRegion) &&
            (tag & mask) == value) {
            item.cancelled = true;
        }
    }
    return kS_OK;
}

int32_t DStorageCreateStatusArray(
    uint64_t factory,
    uint32_t capacity,
    const char* name,
    uint64_t* out_status_array) {
    if (capacity == 0 || !out_status_array) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }
    auto arr = std::make_shared<StatusArrayObject>();
    if (name) arr->name = name;
    arr->completed.assign(capacity, 0);
    arr->hresults.assign(capacity, kS_OK);

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().status_arrays[h] = std::move(arr);
    *out_status_array = h;
    return kS_OK;
}

int32_t DStorageDestroyStatusArray(uint64_t status_array) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().status_arrays.erase(status_array) ? kS_OK : kE_HANDLE;
}

uint32_t DStorageIsStatusComplete(uint64_t status_array, uint32_t index) {
    std::shared_ptr<StatusArrayObject> arr;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().status_arrays.find(status_array);
        if (it == State().status_arrays.end()) return 0;
        arr = it->second;
    }
    std::lock_guard<std::mutex> lk(arr->mu);
    if (index >= arr->completed.size()) return 0;
    return arr->completed[index] ? 1u : 0u;
}

int32_t DStorageGetStatusHResult(uint64_t status_array, uint32_t index) {
    std::shared_ptr<StatusArrayObject> arr;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().status_arrays.find(status_array);
        if (it == State().status_arrays.end()) return kE_HANDLE;
        arr = it->second;
    }
    std::lock_guard<std::mutex> lk(arr->mu);
    if (index >= arr->hresults.size()) return kE_INVALIDARG;
    return arr->hresults[index];
}

int32_t DStorageRetrieveErrorRecord(uint64_t queue, DSTORAGE_ERROR_RECORD* out_record) {
    if (!out_record) return kE_INVALIDARG;
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::lock_guard<std::mutex> lk(q->mu);
    *out_record = q->error_record;
    q->error_record = {};
    return kS_OK;
}

int32_t DStorageCompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_compressed_size) {
    if (format == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (!out_compressed_size) return kE_INVALIDARG;
        *out_compressed_size = src_size;
        if (!dst || dst_capacity < src_size) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        if (src_size > 0 && src) std::memcpy(dst, src, src_size);
        return kS_OK;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE ||
        format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        return CompressLzRle(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            out_compressed_size);
    }
    return kE_NOTIMPL;
}

int32_t DStorageDecompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_decompressed_size) {
    if (format == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (!out_decompressed_size) return kE_INVALIDARG;
        *out_decompressed_size = src_size;
        if (!dst || dst_capacity < src_size) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        if (src_size > 0 && src) std::memcpy(dst, src, src_size);
        return kS_OK;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        return DecompressUnifiedGDeflateOrZlib(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            out_decompressed_size);
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        return DecompressLzRle(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            out_decompressed_size);
    }
    return kE_NOTIMPL;
}

int32_t DStorageDecompressGpuCompute(
    void* d3d12_device,
    void* d3d12_command_queue,
    uint8_t format,
    const void* compressed_src,
    uint32_t compressed_size,
    void* dst_d3d12_buffer,
    uint64_t dst_buffer_offset,
    uint32_t uncompressed_size) {
#if defined(_WIN32)
    return ExecuteGpuBufferDecompress(
        static_cast<ID3D12Device*>(d3d12_device),
        static_cast<ID3D12CommandQueue*>(d3d12_command_queue),
        format,
        static_cast<const uint8_t*>(compressed_src),
        compressed_size,
        static_cast<ID3D12Resource*>(dst_d3d12_buffer),
        dst_buffer_offset,
        uncompressed_size);
#else
    (void)d3d12_device;
    (void)d3d12_command_queue;
    (void)format;
    (void)compressed_src;
    (void)compressed_size;
    (void)dst_d3d12_buffer;
    (void)dst_buffer_offset;
    (void)uncompressed_size;
    return kE_NOTIMPL;
#endif
}

int32_t DStorageEnqueueTextureRegion(
    uint64_t queue,
    const DSTORAGE_TEXTURE_REGION_DESC* desc) {
    if (!desc) return kE_INVALIDARG;
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kTextureRegion;
    item.tex_req = *desc;
    if (desc->Name) item.req_name = desc->Name;
    return EnqueueItem(*q, std::move(item));
}

}  // extern "C"
