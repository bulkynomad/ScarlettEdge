#define SCARLETTX_BUILDING_ACPHAL 1
#include "scarlettx/audio/acphal_abi.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);

constexpr uint32_t kApuBasePa = 0x10000000u;
constexpr uint32_t kMinApuAlign = 64u;
constexpr uint32_t kDefaultHeapCapacity = 64u * 1024u * 1024u;
constexpr uint32_t kMaxHeapCapacity = 0xE0000000u;

bool TraceEnabled() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

struct FreeSpan {
    uint32_t pa = 0;
    uint32_t size = 0;
};

struct ApuAllocation {
    void* va = nullptr;
    uint32_t pa = 0;
    uint32_t size = 0;
    uint32_t span_size = 0;
    uint32_t alignment = 0;
    uint32_t flags = 0;
};

struct ApuState {
    std::mutex mu;
    uint32_t heap_capacity = kDefaultHeapCapacity;
    bool heap_configured = false;
    uint32_t bytes_allocated = 0;
    ACP_SHAPE_CONTEXT_COUNTS shape_contexts{};
    std::vector<FreeSpan> free_blocks{{kApuBasePa, kDefaultHeapCapacity}};
    std::map<uintptr_t, ApuAllocation> by_va;
    std::map<uint32_t, uintptr_t> by_pa;
};

ApuState& State() {
    static ApuState s;
    return s;
}

void RebuildFreeBlocksLocked(ApuState& st) {
    st.free_blocks.clear();
    const uint64_t end = static_cast<uint64_t>(kApuBasePa) + static_cast<uint64_t>(st.heap_capacity);
    uint64_t cursor = kApuBasePa;
    uint64_t live_bytes = 0;
    for (const auto& [pa, va_key] : st.by_pa) {
        const auto va_it = st.by_va.find(va_key);
        if (va_it == st.by_va.end()) continue;
        const uint32_t span = va_it->second.span_size;
        live_bytes += span;
        if (pa > cursor && cursor < end) {
            const uint64_t gap_end = std::min<uint64_t>(pa, end);
            if (gap_end > cursor) {
                st.free_blocks.push_back(
                    FreeSpan{static_cast<uint32_t>(cursor), static_cast<uint32_t>(gap_end - cursor)});
            }
        }
        cursor = std::max<uint64_t>(cursor, static_cast<uint64_t>(pa) + static_cast<uint64_t>(span));
    }
    if (cursor < end) {
        st.free_blocks.push_back(
            FreeSpan{static_cast<uint32_t>(cursor), static_cast<uint32_t>(end - cursor)});
    }
    st.bytes_allocated = static_cast<uint32_t>(std::min<uint64_t>(live_bytes, 0xFFFFFFFFull));
}

int32_t ApuAllocLocked(
    ApuState& st,
    void** virtualAddress,
    uint32_t* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    if (!virtualAddress) return kE_INVALIDARG;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0u;
    if (sizeInBytes == 0 || alignmentInBytes > 0x40000000u) return kE_INVALIDARG;

    // Validate & normalize alignment to a power-of-2 >= 64 so _aligned_malloc never aborts
    const uint32_t align = std::bit_ceil(std::max<uint32_t>(kMinApuAlign, alignmentInBytes));
    const uint64_t span_size64 =
        (static_cast<uint64_t>(sizeInBytes) + (kMinApuAlign - 1u)) & ~static_cast<uint64_t>(kMinApuAlign - 1u);
    if (span_size64 == 0ull || span_size64 > st.heap_capacity) {
        return kE_OUTOFMEMORY;
    }
    const uint32_t span_size = static_cast<uint32_t>(span_size64);

    size_t fit_idx = st.free_blocks.size();
    uint32_t aligned_pa = 0u;
    for (size_t i = 0; i < st.free_blocks.size(); ++i) {
        const uint64_t blk_start = st.free_blocks[i].pa;
        const uint64_t blk_end = blk_start + st.free_blocks[i].size;
        const uint64_t cand_pa =
            (blk_start + static_cast<uint64_t>(align) - 1ull) & ~static_cast<uint64_t>(align - 1u);
        if (cand_pa >= blk_start && cand_pa + span_size <= blk_end && cand_pa + span_size <= 0xFFFFFFFFull) {
            fit_idx = i;
            aligned_pa = static_cast<uint32_t>(cand_pa);
            break;
        }
    }
    if (fit_idx >= st.free_blocks.size()) {
        return kE_OUTOFMEMORY;
    }

#if defined(_WIN32)
    void* mem = _aligned_malloc(span_size, align);
#else
    void* mem = nullptr;
    if (posix_memalign(&mem, align, span_size) != 0) mem = nullptr;
#endif
    if (!mem) return kE_OUTOFMEMORY;
    std::memset(mem, 0, span_size);

    const uint64_t blk_start = st.free_blocks[fit_idx].pa;
    const uint64_t blk_end = blk_start + st.free_blocks[fit_idx].size;
    const uint32_t prefix_size = static_cast<uint32_t>(aligned_pa - blk_start);
    const uint32_t suffix_pa = aligned_pa + span_size;
    const uint32_t suffix_size = static_cast<uint32_t>(blk_end - suffix_pa);

    if (prefix_size == 0u && suffix_size == 0u) {
        st.free_blocks.erase(st.free_blocks.begin() + static_cast<std::ptrdiff_t>(fit_idx));
    } else if (prefix_size == 0u) {
        st.free_blocks[fit_idx].pa = suffix_pa;
        st.free_blocks[fit_idx].size = suffix_size;
    } else if (suffix_size == 0u) {
        st.free_blocks[fit_idx].size = prefix_size;
    } else {
        st.free_blocks[fit_idx].size = prefix_size;
        st.free_blocks.insert(
            st.free_blocks.begin() + static_cast<std::ptrdiff_t>(fit_idx + 1u),
            FreeSpan{suffix_pa, suffix_size});
    }

    st.bytes_allocated += span_size;

    ApuAllocation rec{};
    rec.va = mem;
    rec.pa = aligned_pa;
    rec.size = sizeInBytes;
    rec.span_size = span_size;
    rec.alignment = align;
    rec.flags = flags;
    const uintptr_t va_key = reinterpret_cast<uintptr_t>(mem);
    st.by_va[va_key] = rec;
    st.by_pa[aligned_pa] = va_key;

    *virtualAddress = mem;
    if (physicalAddress) *physicalAddress = aligned_pa;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] ApuAlloc(size=" << sizeInBytes << ", align=" << align
                  << ") -> va=" << mem << ", pa=0x" << std::hex << aligned_pa << std::dec << "\n";
    }
    return kS_OK;
}

int32_t ApuFreeLocked(ApuState& st, void* virtualAddress) {
    if (!virtualAddress) return kS_OK;
    auto it = st.by_va.find(reinterpret_cast<uintptr_t>(virtualAddress));
    if (it == st.by_va.end()) {
        return kE_INVALIDARG;
    }
    const uint32_t freed_pa = it->second.pa;
    const uint32_t freed_span = it->second.span_size;
    st.bytes_allocated = st.bytes_allocated >= freed_span ? (st.bytes_allocated - freed_span) : 0u;
    st.by_pa.erase(freed_pa);
    st.by_va.erase(it);

    // Return [freed_pa, freed_pa + freed_span) to the address-ordered free list and coalesce neighbors.
    auto pos = std::lower_bound(
        st.free_blocks.begin(), st.free_blocks.end(), freed_pa,
        [](const FreeSpan& blk, uint32_t pa) { return blk.pa < pa; });
    uint32_t merged_pa = freed_pa;
    uint32_t merged_size = freed_span;
    if (pos != st.free_blocks.end() &&
        static_cast<uint64_t>(merged_pa) + static_cast<uint64_t>(merged_size) == pos->pa) {
        merged_size += pos->size;
        pos = st.free_blocks.erase(pos);
    }
    if (pos != st.free_blocks.begin()) {
        auto prev = pos - 1;
        if (static_cast<uint64_t>(prev->pa) + static_cast<uint64_t>(prev->size) == merged_pa) {
            prev->size += merged_size;
        } else {
            st.free_blocks.insert(pos, FreeSpan{merged_pa, merged_size});
        }
    } else {
        st.free_blocks.insert(pos, FreeSpan{merged_pa, merged_size});
    }

#if defined(_WIN32)
    _aligned_free(virtualAddress);
#else
    std::free(virtualAddress);
#endif
    return kS_OK;
}

void ReleaseShapeContextsLocked(ApuState& st) {
    auto release_one = [&](void*& p) {
        if (p) {
            (void)ApuFreeLocked(st, p);
            p = nullptr;
        }
    };
    release_one(st.shape_contexts.srcContexts);
    release_one(st.shape_contexts.eqCompContexts);
    release_one(st.shape_contexts.filtVolContexts);
    release_one(st.shape_contexts.dmaContexts);
    release_one(st.shape_contexts.xmaContexts);
    release_one(st.shape_contexts.pcmContexts);
    std::memset(&st.shape_contexts, 0, sizeof(st.shape_contexts));
}

struct IAcpHalVtbl {
    int32_t (SCARLETTX_ACP_CALL* Connect)(void* self, uint32_t cmdQueueLen, uint32_t msgQueueLen);
    int32_t (SCARLETTX_ACP_CALL* Disconnect)(void* self);
    int32_t (SCARLETTX_ACP_CALL* SubmitCommand)(
        void* self, uint32_t type, uint32_t id, uint32_t flags, const void* data, uint32_t extra);
    uint8_t (SCARLETTX_ACP_CALL* PopMessage)(void* self, void* msg);
    int32_t (SCARLETTX_ACP_CALL* StubSlot4)(void* self);
    uint32_t (SCARLETTX_ACP_CALL* Release)(void* self);
    int32_t (SCARLETTX_ACP_CALL* StubSlot6)(void* self);
    int32_t (SCARLETTX_ACP_CALL* StubSlot7)(void* self);
};

struct AcpHalImpl {
    const IAcpHalVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<bool> disconnect_pending{false};
};

int32_t SCARLETTX_ACP_CALL AcpConnect(void* self, uint32_t cmdLen, uint32_t msgLen) {
    auto* obj = static_cast<AcpHalImpl*>(self);
    obj->disconnect_pending.store(false, std::memory_order_release);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] IAcpHal::Connect(cmd=" << cmdLen << ", msg=" << msgLen << ")\n";
    }
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL AcpDisconnect(void* self) {
    auto* obj = static_cast<AcpHalImpl*>(self);
    obj->disconnect_pending.store(true, std::memory_order_release);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] IAcpHal::Disconnect()\n";
    }
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL AcpSubmitCommand(
    void* /*self*/, uint32_t type, uint32_t id, uint32_t /*flags*/, const void* /*data*/, uint32_t /*extra*/) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] IAcpHal::SubmitCommand(type=0x" << std::hex << type
                  << ", id=" << std::dec << id << ")\n";
    }
    return kS_OK;
}

uint8_t SCARLETTX_ACP_CALL AcpPopMessage(void* self, void* msg) {
    auto* obj = static_cast<AcpHalImpl*>(self);
    if (obj->disconnect_pending.exchange(false, std::memory_order_acq_rel)) {
        if (msg) {
            std::memset(msg, 0, 24);
            uint32_t disc = 0x80u;
            std::memcpy(msg, &disc, sizeof(disc));
        }
        return 1;
    }
    return 0;
}

uint32_t SCARLETTX_ACP_CALL AcpRelease(void* self) {
    auto* obj = static_cast<AcpHalImpl*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return rem;
}

int32_t SCARLETTX_ACP_CALL AcpStubOk(void* /*self*/) {
    return kS_OK;
}

const IAcpHalVtbl kAcpVtbl = {
    &AcpConnect,
    &AcpDisconnect,
    &AcpSubmitCommand,
    &AcpPopMessage,
    &AcpStubOk,
    &AcpRelease,
    &AcpStubOk,
    &AcpStubOk,
};

}  // namespace

extern "C" {

int32_t SCARLETTX_ACP_CALL AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXT_COUNTS* ctx) {
    if (!ctx) return kE_INVALIDARG;

    std::lock_guard<std::mutex> lk(State().mu);
    ReleaseShapeContextsLocked(State());

    ACP_SHAPE_CONTEXT_COUNTS allocated{};
    allocated.numSrcContexts = ctx->numSrcContexts;
    allocated.numEqCompContexts = ctx->numEqCompContexts;
    allocated.numFiltVolContexts = ctx->numFiltVolContexts;
    allocated.numDmaContexts = ctx->numDmaContexts;
    allocated.numXmaContexts = ctx->numXmaContexts;
    allocated.numPcmContexts = ctx->numPcmContexts;

    // Allocate SHAPE contexts through ApuAlloc so ApuMapVirtualAddress, ApuMapApuAddress,
    // and ApuIsVirtualAddressValid succeed on every SHAPE context array!
    auto alloc_ctx_buf = [&](uint32_t count, size_t elem_bytes, void** out_ptr) -> int32_t {
        *out_ptr = nullptr;
        if (count == 0u) return kS_OK;
        const uint64_t total_bytes = static_cast<uint64_t>(count) * elem_bytes;
        if (total_bytes > 0x7FFFFFFFull) return kE_OUTOFMEMORY;
        return ApuAllocLocked(State(), out_ptr, nullptr, static_cast<uint32_t>(total_bytes), 64u, 0u);
    };

    int32_t hr = alloc_ctx_buf(ctx->numSrcContexts, 32u, &allocated.srcContexts);
    if (hr == kS_OK) hr = alloc_ctx_buf(ctx->numEqCompContexts, 64u, &allocated.eqCompContexts);
    if (hr == kS_OK) hr = alloc_ctx_buf(ctx->numFiltVolContexts, 64u, &allocated.filtVolContexts);
    if (hr == kS_OK) hr = alloc_ctx_buf(ctx->numDmaContexts, 16u, &allocated.dmaContexts);
    if (hr == kS_OK) hr = alloc_ctx_buf(ctx->numXmaContexts, 64u, &allocated.xmaContexts);
    if (hr == kS_OK) hr = alloc_ctx_buf(ctx->numPcmContexts, 64u, &allocated.pcmContexts);

    if (hr != kS_OK) {
        State().shape_contexts = allocated;
        ReleaseShapeContextsLocked(State());
        return hr;
    }

    *ctx = allocated;
    State().shape_contexts = allocated;

    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] AcpHalAllocateShapeContexts(src=" << ctx->numSrcContexts
                  << ", dma=" << ctx->numDmaContexts
                  << ", xma=" << ctx->numXmaContexts
                  << ", pcm=" << ctx->numPcmContexts
                  << ") -> src=" << ctx->srcContexts
                  << " dma=" << ctx->dmaContexts
                  << " xma=" << ctx->xmaContexts << "\n";
    }
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL AcpHalCreate(void** acpHal) {
    if (!acpHal) return kE_INVALIDARG;
    auto* obj = new AcpHalImpl();
    obj->lpVtbl = &kAcpVtbl;
    *acpHal = obj;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][AcpHal] AcpHalCreate -> " << obj << "\n";
    }
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL AcpHalReleaseShapeContexts(void) {
    std::lock_guard<std::mutex> lk(State().mu);
    ReleaseShapeContextsLocked(State());
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL ApuCreateHeap(uint32_t cachedSizeInBytes, uint32_t nonCachedSizeInBytes) {
    std::lock_guard<std::mutex> lk(State().mu);
    ApuState& st = State();
    const uint64_t total = static_cast<uint64_t>(cachedSizeInBytes) + static_cast<uint64_t>(nonCachedSizeInBytes);
    if (total > 0ull) {
        const uint64_t clamped = std::min<uint64_t>(total, kMaxHeapCapacity);
        const uint32_t aligned_cap = static_cast<uint32_t>(
            (clamped + (kMinApuAlign - 1u)) & ~static_cast<uint64_t>(kMinApuAlign - 1u));
        if (!st.by_pa.empty()) {
            const auto last_it = std::prev(st.by_pa.end());
            const auto va_it = st.by_va.find(last_it->second);
            if (va_it != st.by_va.end()) {
                const uint64_t highest_live_end =
                    static_cast<uint64_t>(last_it->first) + static_cast<uint64_t>(va_it->second.span_size);
                if (highest_live_end > static_cast<uint64_t>(kApuBasePa) + static_cast<uint64_t>(aligned_cap)) {
                    return kE_INVALIDARG;
                }
            }
        }
        st.heap_capacity = aligned_cap;
        st.heap_configured = true;
        RebuildFreeBlocksLocked(st);
    }
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL ApuAlloc(
    void** virtualAddress,
    uint32_t* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    std::lock_guard<std::mutex> lk(State().mu);
    return ApuAllocLocked(State(), virtualAddress, physicalAddress, sizeInBytes, alignmentInBytes, flags);
}

int32_t SCARLETTX_ACP_CALL ApuFree(void* virtualAddress) {
    std::lock_guard<std::mutex> lk(State().mu);
    return ApuFreeLocked(State(), virtualAddress);
}

int32_t SCARLETTX_ACP_CALL ApuHeapGetState(APU_HEAP_STATE* apuHeapState, uint32_t /*flags*/) {
    if (!apuHeapState) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    const ApuState& st = State();
    std::memset(apuHeapState, 0, sizeof(*apuHeapState));
    uint64_t free_total = 0;
    uint32_t max_block = 0;
    for (const FreeSpan& blk : st.free_blocks) {
        free_total += blk.size;
        if (blk.size > max_block) max_block = blk.size;
    }
    apuHeapState->bytesAllocated = st.bytes_allocated;
    apuHeapState->bytesFree = static_cast<uint32_t>(std::min<uint64_t>(free_total, 0xFFFFFFFFull));
    apuHeapState->maximumBlockSizeAvailable = max_block;
    apuHeapState->allocationCount = static_cast<uint32_t>(st.by_va.size());
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL ApuIsVirtualAddressValid(const void* virtualAddress, uint32_t sizeInBytes) {
    if (!virtualAddress) return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
    std::lock_guard<std::mutex> lk(State().mu);
    const ApuState& st = State();
    auto it = st.by_va.upper_bound(addr);
    if (it == st.by_va.begin()) return 0;
    --it;
    const uintptr_t base = it->first;
    const uintptr_t alloc_sz = static_cast<uintptr_t>(it->second.size);
    // Overflow-safe bounds check (never computes addr + sizeInBytes!)
    if ((addr - base) < alloc_sz &&
        static_cast<uintptr_t>(sizeInBytes) <= (alloc_sz - (addr - base))) {
        return 1;
    }
    return 0;
}

void* SCARLETTX_ACP_CALL ApuMapApuAddress(uint32_t apuPhysicalAddress) {
    std::lock_guard<std::mutex> lk(State().mu);
    const ApuState& st = State();
    auto it = st.by_pa.upper_bound(apuPhysicalAddress);
    if (it == st.by_pa.begin()) return nullptr;
    --it;
    const auto va_it = st.by_va.find(it->second);
    if (va_it != st.by_va.end()) {
        const auto& rec = va_it->second;
        if (apuPhysicalAddress >= rec.pa && (apuPhysicalAddress - rec.pa) < rec.size) {
            return static_cast<uint8_t*>(rec.va) + (apuPhysicalAddress - rec.pa);
        }
    }
    return nullptr;
}

uint32_t SCARLETTX_ACP_CALL ApuMapVirtualAddress(const void* virtualAddress) {
    if (!virtualAddress) return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
    std::lock_guard<std::mutex> lk(State().mu);
    const ApuState& st = State();
    auto it = st.by_va.upper_bound(addr);
    if (it == st.by_va.begin()) return 0;
    --it;
    const uintptr_t base = it->first;
    const uintptr_t alloc_sz = static_cast<uintptr_t>(it->second.size);
    if ((addr - base) < alloc_sz) {
        return it->second.pa + static_cast<uint32_t>(addr - base);
    }
    return 0;
}

}  // extern "C"
