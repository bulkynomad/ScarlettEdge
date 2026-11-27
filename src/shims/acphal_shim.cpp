#define SCARLETTX_BUILDING_ACPHAL 1
#include "scarlettx/audio/acphal_abi.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);

bool TraceEnabled() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

struct ApuAllocation {
    void* va = nullptr;
    uint32_t pa = 0;
    uint32_t size = 0;
    uint32_t alignment = 0;
    uint32_t flags = 0;
};

struct ApuState {
    std::mutex mu;
    uint32_t next_pa = 0x10000000u;
    uint32_t heap_capacity = 64u * 1024u * 1024u;
    bool heap_configured = false;
    uint32_t bytes_allocated = 0;
    ACP_SHAPE_CONTEXT_COUNTS shape_contexts{};
    std::unordered_map<uintptr_t, ApuAllocation> by_va;
    std::unordered_map<uint32_t, uintptr_t> by_pa;
};

ApuState& State() {
    static ApuState s;
    return s;
}

int32_t ApuAllocLocked(
    ApuState& st,
    void** virtualAddress,
    uint32_t* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    if (!virtualAddress || sizeInBytes == 0) return kE_INVALIDARG;
    if (alignmentInBytes > 0x40000000u) return kE_INVALIDARG;

    // Validate & normalize alignment to a power-of-2 >= 64 so _aligned_malloc never aborts
    const uint32_t align = std::bit_ceil(std::max<uint32_t>(64u, alignmentInBytes));

    if (st.heap_configured &&
        static_cast<uint64_t>(st.bytes_allocated) + static_cast<uint64_t>(sizeInBytes) > st.heap_capacity) {
        return kE_OUTOFMEMORY;
    }

    const uint64_t aligned_pa64 =
        (static_cast<uint64_t>(st.next_pa) + static_cast<uint64_t>(align) - 1ull) &
        ~static_cast<uint64_t>(align - 1u);
    if (aligned_pa64 + static_cast<uint64_t>(sizeInBytes) > 0xFFFFFFFFull) {
        return kE_OUTOFMEMORY;
    }

#if defined(_WIN32)
    void* mem = _aligned_malloc(sizeInBytes, align);
#else
    void* mem = nullptr;
    if (posix_memalign(&mem, align, sizeInBytes) != 0) mem = nullptr;
#endif
    if (!mem) return kE_OUTOFMEMORY;
    std::memset(mem, 0, sizeInBytes);

    const uint32_t aligned_pa = static_cast<uint32_t>(aligned_pa64);
    st.next_pa = aligned_pa + sizeInBytes;
    st.bytes_allocated += sizeInBytes;

    ApuAllocation rec{};
    rec.va = mem;
    rec.pa = aligned_pa;
    rec.size = sizeInBytes;
    rec.alignment = align;
    rec.flags = flags;
    st.by_va[reinterpret_cast<uintptr_t>(mem)] = rec;
    st.by_pa[aligned_pa] = reinterpret_cast<uintptr_t>(mem);

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
    st.bytes_allocated = st.bytes_allocated >= it->second.size ? (st.bytes_allocated - it->second.size) : 0u;
    st.by_pa.erase(it->second.pa);
    st.by_va.erase(it);
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
    const uint64_t total = static_cast<uint64_t>(cachedSizeInBytes) + static_cast<uint64_t>(nonCachedSizeInBytes);
    if (total > 0ull) {
        State().heap_capacity = static_cast<uint32_t>(std::min<uint64_t>(total, 0xFFFFFFFFull));
        State().heap_configured = true;
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
    std::memset(apuHeapState, 0, sizeof(*apuHeapState));
    apuHeapState->bytesAllocated = State().bytes_allocated;
    apuHeapState->bytesFree =
        State().heap_capacity > State().bytes_allocated ? (State().heap_capacity - State().bytes_allocated) : 0u;
    apuHeapState->maximumBlockSizeAvailable = apuHeapState->bytesFree;
    apuHeapState->allocationCount = static_cast<uint32_t>(State().by_va.size());
    return kS_OK;
}

int32_t SCARLETTX_ACP_CALL ApuIsVirtualAddressValid(const void* virtualAddress, uint32_t sizeInBytes) {
    if (!virtualAddress) return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const uintptr_t base = kv.first;
        const uintptr_t alloc_sz = static_cast<uintptr_t>(kv.second.size);
        // Overflow-safe bounds check (never computes addr + sizeInBytes!)
        if (addr >= base && (addr - base) <= alloc_sz &&
            static_cast<uintptr_t>(sizeInBytes) <= (alloc_sz - (addr - base))) {
            return 1;
        }
    }
    return 0;
}

void* SCARLETTX_ACP_CALL ApuMapApuAddress(uint32_t apuPhysicalAddress) {
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const auto& rec = kv.second;
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
    for (const auto& kv : State().by_va) {
        const uintptr_t base = kv.first;
        const uintptr_t alloc_sz = static_cast<uintptr_t>(kv.second.size);
        if (addr >= base && (addr - base) < alloc_sz) {
            return kv.second.pa + static_cast<uint32_t>(addr - base);
        }
    }
    return 0;
}

}  // extern "C"
