#define SCARLETTX_BUILDING_FFX_UPSCALER 1
#include "scarlettx/middleware/ffx_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

namespace {

constexpr uint64_t kFfxProviderVersionId = 0x00030102ull;
constexpr const char* kFfxProviderVersionName = "ScarlettX FidelityFX Upscaler 3.1.2-HLE";

struct FfxUpscalerContextState {
    uint32_t max_render_w = 0;
    uint32_t max_render_h = 0;
    uint32_t max_upscale_w = 0;
    uint32_t max_upscale_h = 0;
    float sharpness = 0.0f;
    bool enable_sharpening = false;
    uint64_t dispatch_count = 0;
};

std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<FfxUpscalerContextState>> g_contexts;

float QualityModeToRatio(uint32_t mode) {
    switch (mode) {
        case FFX_UPSCALE_QUALITY_MODE_NATIVEAA:          return 1.0f;
        case FFX_UPSCALE_QUALITY_MODE_QUALITY:           return 1.5f;
        case FFX_UPSCALE_QUALITY_MODE_BALANCED:          return 1.7f;
        case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE:       return 2.0f;
        case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: return 3.0f;
        default:                                         return 1.5f;
    }
}

float Halton(int32_t index, int32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    int32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

uint64_t Fnv1aBytes(const uint8_t* data, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

#if defined(_WIN32)
bool IsCommittedReadable(const void* ptr, size_t min_bytes) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_EXECUTE_WRITECOPY);
    if (!readable) return false;
    const auto* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= static_cast<const uint8_t*>(ptr) + min_bytes;
}

bool IsExecutableCodePtr(const void* fn_ptr) {
    if (!fn_ptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(fn_ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// Detects COM / guest-vtable objects (e.g. ID3D12GraphicsCommandList* or ID3D12Resource*)
// so CPU RGBA8 bilinear fallback NEVER overwrites a COM object's vtable!
bool LooksLikeComObject(const void* p) {
    if (!p || (reinterpret_cast<uintptr_t>(p) & (sizeof(void*) - 1u)) != 0u) return false;
    if (!IsCommittedReadable(p, sizeof(void*))) return false;
    void* const* vtbl = *static_cast<void* const* const*>(p);
    if (!vtbl || (reinterpret_cast<uintptr_t>(vtbl) & (sizeof(void*) - 1u)) != 0u) return false;
    if (!IsCommittedReadable(vtbl, sizeof(void*) * 3u)) return false;
    return IsExecutableCodePtr(vtbl[0]) && IsExecutableCodePtr(vtbl[1]) && IsExecutableCodePtr(vtbl[2]);
}

void TryGpuUpscaleCopyFallback(const ffxApiHeader* desc) {
    const auto* raw = reinterpret_cast<const uint8_t*>(desc);
    if (!IsCommittedReadable(raw, 0x20u)) return;

    void* cmd_candidate = *reinterpret_cast<void* const*>(raw + 0x10u);
    void* color_candidate = *reinterpret_cast<void* const*>(raw + 0x18u);
    if (!LooksLikeComObject(cmd_candidate) || !LooksLikeComObject(color_candidate)) return;

    ID3D12GraphicsCommandList* cmd_list = nullptr;
    if (FAILED(static_cast<IUnknown*>(cmd_candidate)->QueryInterface(IID_PPV_ARGS(&cmd_list))) || !cmd_list) {
        return;
    }

    ID3D12Resource* src_res = nullptr;
    if (FAILED(static_cast<IUnknown*>(color_candidate)->QueryInterface(IID_PPV_ARGS(&src_res))) || !src_res) {
        cmd_list->Release();
        return;
    }

    // Probe output.resource at official FFX 1.1 (+0x138) and FFX 1.0 (+0x108) offsets
    ID3D12Resource* dst_res = nullptr;
    const size_t kCandidateOffsets[2] = {0x138u, 0x108u};
    for (size_t off : kCandidateOffsets) {
        if (!IsCommittedReadable(raw, off + sizeof(void*))) continue;
        void* out_cand = *reinterpret_cast<void* const*>(raw + off);
        if (out_cand && out_cand != color_candidate && LooksLikeComObject(out_cand)) {
            if (SUCCEEDED(static_cast<IUnknown*>(out_cand)->QueryInterface(IID_PPV_ARGS(&dst_res))) && dst_res) {
                break;
            }
        }
    }

    if (dst_res) {
        const D3D12_RESOURCE_DESC sd = src_res->GetDesc();
        const D3D12_RESOURCE_DESC dd = dst_res->GetDesc();
        if (sd.Dimension == dd.Dimension && sd.Width == dd.Width &&
            sd.Height == dd.Height && sd.Format == dd.Format) {
            cmd_list->CopyResource(dst_res, src_res);
        }
        dst_res->Release();
    }

    src_res->Release();
    cmd_list->Release();
}
#endif

const ffxCreateContextDescUpscale* FindUpscaleCreateDesc(const ffxApiHeader* desc) {
    for (const ffxApiHeader* cur = desc; cur != nullptr; cur = cur->pNext) {
        if (cur->type == FFX_API_DESC_TYPE_UPSCALE_CREATE) {
            return reinterpret_cast<const ffxCreateContextDescUpscale*>(cur);
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

ffxReturnCode_t ffxUpscalerCreateContext(ffxContext* context, ffxApiHeader* desc) {
    if (!context || !desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const ffxCreateContextDescUpscale* cd = FindUpscaleCreateDesc(desc);
    if (!cd) {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    if (cd->maxRenderWidth == 0 || cd->maxRenderHeight == 0 ||
        cd->maxUpscaleWidth == 0 || cd->maxUpscaleHeight == 0) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    auto state = std::make_unique<FfxUpscalerContextState>();
    state->max_render_w = cd->maxRenderWidth;
    state->max_render_h = cd->maxRenderHeight;
    state->max_upscale_w = cd->maxUpscaleWidth;
    state->max_upscale_h = cd->maxUpscaleHeight;
    void* raw = state.get();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_contexts[raw] = std::move(state);
    }
    *context = raw;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxUpscalerDestroyContext(ffxContext* context) {
    if (!context || !*context) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t erased = g_contexts.erase(*context);
    *context = nullptr;
    return erased > 0 ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER;
}

ffxReturnCode_t ffxUpscalerConfigure(ffxContext* context, const ffxApiHeader* desc) {
    if (!desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1) {
        return FFX_API_RETURN_OK;
    }
    if (!context || !*context) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(*context);
    if (it == g_contexts.end()) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type == FFX_API_DESC_TYPE_UPSCALE_CONFIGURE) {
        const auto* cfg = reinterpret_cast<const ffxConfigureDescUpscale*>(desc);
        it->second->sharpness = std::clamp(cfg->sharpness, 0.0f, 1.0f);
        it->second->enable_sharpening = (cfg->enableSharpening != 0);
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

ffxReturnCode_t ffxUpscalerQuery(ffxContext* context, ffxApiHeader* desc) {
    (void)context;
    if (!desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    switch (desc->type) {
        case FFX_API_DESC_TYPE_GET_PROVIDER_VERSION: {
            auto* q = reinterpret_cast<ffxQueryDescGetProviderVersion*>(desc);
            q->versionId = kFfxProviderVersionId;
            q->versionName = kFfxProviderVersionName;
            return FFX_API_RETURN_OK;
        }
        case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
            auto* q = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
            if (!q->outputCount) return FFX_API_RETURN_ERROR_PARAMETER;
            if (*q->outputCount > 0u) {
                if (q->versionIds) q->versionIds[0] = kFfxProviderVersionId;
                if (q->versionNames) q->versionNames[0] = kFfxProviderVersionName;
            }
            *q->outputCount = 1u;
            return FFX_API_RETURN_OK;
        }
        case 0x00010001u:  // Official AMD FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE
        case FFX_API_DESC_TYPE_UPSCALE_GET_UPSCALERATIO: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(desc);
            if (!q->pOutUpscaleRatio) return FFX_API_RETURN_ERROR_PARAMETER;
            *q->pOutUpscaleRatio = QualityModeToRatio(q->qualityMode);
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_RENDERRESOLUTION: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(desc);
            if (!q->pOutRenderWidth || !q->pOutRenderHeight ||
                q->displayWidth == 0 || q->displayHeight == 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const float ratio = QualityModeToRatio(q->qualityMode);
            *q->pOutRenderWidth = std::max(1u, static_cast<uint32_t>(std::round(q->displayWidth / ratio)));
            *q->pOutRenderHeight = std::max(1u, static_cast<uint32_t>(std::round(q->displayHeight / ratio)));
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(desc);
            if (!q->pOutPhaseCount || q->renderWidth <= 0 || q->displayWidth <= 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const float ratio = static_cast<float>(q->displayWidth) / static_cast<float>(q->renderWidth);
            *q->pOutPhaseCount = static_cast<int32_t>(std::ceil(8.0f * ratio * ratio));
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset*>(desc);
            if (!q->pOutX || !q->pOutY || q->phaseCount <= 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const int32_t idx = (q->index % q->phaseCount) + 1;
            *q->pOutX = Halton(idx, 2) - 0.5f;
            *q->pOutY = Halton(idx, 3) - 0.5f;
            return FFX_API_RETURN_OK;
        }
        default:
            return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

ffxReturnCode_t ffxUpscalerDispatch(ffxContext* context, const ffxApiHeader* desc) {
    if (!context || !*context || !desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type != FFX_API_DESC_TYPE_UPSCALE_DISPATCH &&
        desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_contexts.find(*context);
        if (it == g_contexts.end()) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        ++it->second->dispatch_count;
    }

    const auto* d = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);

    // Official AMD ffx_api GPU dispatch (0x10004) or COM resource/commandList pointers at +0x10 / +0x18:
    // NEVER treat ID3D12Resource* as a CPU RGBA8 buffer!
    bool is_gpu_com_dispatch = (desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE);
#if defined(_WIN32)
    if (!is_gpu_com_dispatch && (LooksLikeComObject(d->colorRgba8) || LooksLikeComObject(d->outputRgba8))) {
        is_gpu_com_dispatch = true;
    }
#endif
    if (is_gpu_com_dispatch) {
#if defined(_WIN32)
        TryGpuUpscaleCopyFallback(desc);
#endif
        return FFX_API_RETURN_OK;
    }

    if (!d->colorRgba8 || !d->outputRgba8 ||
        d->renderWidth == 0 || d->renderHeight == 0 ||
        d->upscaleWidth == 0 || d->upscaleHeight == 0) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    // Deterministic CPU bilinear upscale for headless / unit-test RGBA8 buffers
    for (uint32_t y = 0; y < d->upscaleHeight; ++y) {
        const float fy = (d->upscaleHeight > 1)
                             ? (static_cast<float>(y) * static_cast<float>(d->renderHeight - 1) /
                                static_cast<float>(d->upscaleHeight - 1))
                             : 0.0f;
        const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
        const uint32_t y1 = std::min(y0 + 1u, d->renderHeight - 1u);
        const float wy = fy - static_cast<float>(y0);

        for (uint32_t x = 0; x < d->upscaleWidth; ++x) {
            const float fx = (d->upscaleWidth > 1)
                                 ? (static_cast<float>(x) * static_cast<float>(d->renderWidth - 1) /
                                    static_cast<float>(d->upscaleWidth - 1))
                                 : 0.0f;
            const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
            const uint32_t x1 = std::min(x0 + 1u, d->renderWidth - 1u);
            const float wx = fx - static_cast<float>(x0);

            const uint8_t* p00 = d->colorRgba8 + (static_cast<size_t>(y0) * d->renderWidth + x0) * 4ull;
            const uint8_t* p10 = d->colorRgba8 + (static_cast<size_t>(y0) * d->renderWidth + x1) * 4ull;
            const uint8_t* p01 = d->colorRgba8 + (static_cast<size_t>(y1) * d->renderWidth + x0) * 4ull;
            const uint8_t* p11 = d->colorRgba8 + (static_cast<size_t>(y1) * d->renderWidth + x1) * 4ull;

            uint8_t* dst = d->outputRgba8 + (static_cast<size_t>(y) * d->upscaleWidth + x) * 4ull;
            for (int c = 0; c < 4; ++c) {
                const float top = p00[c] * (1.0f - wx) + p10[c] * wx;
                const float bot = p01[c] * (1.0f - wx) + p11[c] * wx;
                const float val = top * (1.0f - wy) + bot * wy;
                dst[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(val)), 0, 255));
            }
        }
    }

    if (d->outFrameHash) {
        *d->outFrameHash = Fnv1aBytes(
            d->outputRgba8,
            static_cast<size_t>(d->upscaleWidth) * d->upscaleHeight * 4ull);
    }
    return FFX_API_RETURN_OK;
}

}  // extern "C"
