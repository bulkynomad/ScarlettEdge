#include "scarlettx/gpu/d3d12_backend.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#endif

namespace scarlettx {

uint64_t FrameBufferRgba8::ComputeFnv1aHash() const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t b : pixels) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

Status FrameBufferRgba8::WritePpm(const std::string& file_path) const {
    if (width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height * 4) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid framebuffer dimensions");
    }
    std::ofstream ofs(file_path, std::ios::binary);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Cannot write PPM file: " + file_path);
    }
    ofs << "P6\n" << width << " " << height << "\n255\n";
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const char rgb[3] = {
            static_cast<char>(pixels[i * 4 + 0]),
            static_cast<char>(pixels[i * 4 + 1]),
            static_cast<char>(pixels[i * 4 + 2]),
        };
        ofs.write(rgb, 3);
    }
    return Status::Ok();
}

#if defined(_WIN32)

namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

constexpr const char* kTriangleHlsl = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float3 col : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    float2 positions[3] = {
        float2( 0.0f,  0.65f),
        float2( 0.65f, -0.65f),
        float2(-0.65f, -0.65f)
    };
    float3 colors[3] = {
        float3(1.0f, 0.15f, 0.15f),
        float3(0.15f, 1.0f, 0.15f),
        float3(0.15f, 0.35f, 1.0f)
    };
    VSOut o;
    o.pos = float4(positions[vid], 0.0f, 1.0f);
    o.col = colors[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    return float4(i.col, 1.0f);
}
)";

}  // namespace

struct D3D12Backend::Impl {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    ~Impl() { Cleanup(); }

    void Cleanup() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        SafeRelease(fence);
        SafeRelease(cmd_list);
        SafeRelease(allocator);
        SafeRelease(queue);
        SafeRelease(device5);
        SafeRelease(device);
        SafeRelease(adapter);
        SafeRelease(factory);
    }

    Status WaitForGpu() {
        const uint64_t target = ++fence_value;
        HRESULT hr = queue->Signal(fence, target);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "ID3D12CommandQueue::Signal failed");
        }
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, fence_event);
            if (FAILED(hr)) {
                return Status::Error(StatusCode::DeviceError, "SetEventOnCompletion failed");
            }
            WaitForSingleObject(fence_event, 15000);
        }
        return Status::Ok();
    }
};

D3D12Backend::D3D12Backend() : impl_(std::make_unique<Impl>()) {}
D3D12Backend::~D3D12Backend() = default;

bool D3D12Backend::IsInitialized() const noexcept {
    return impl_ && impl_->device != nullptr;
}

void* D3D12Backend::NativeDevice() const noexcept {
    return impl_ ? static_cast<void*>(impl_->device) : nullptr;
}

void* D3D12Backend::NativeDevice5() const noexcept {
    return impl_ ? static_cast<void*>(impl_->device5) : nullptr;
}

Status D3D12Backend::Initialize(bool prefer_warp, bool enable_debug_layer) {
    Shutdown();

    if (enable_debug_layer) {
        ID3D12Debug* debug_ctrl = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl))) && debug_ctrl) {
            debug_ctrl->EnableDebugLayer();
            debug_ctrl->Release();
        }
    }

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&impl_->factory));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory1 failed");
    }

    auto try_create_device = [&](IDXGIAdapter1* candidate) -> bool {
        if (!candidate) return false;
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) ||
            SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            impl_->device = dev;
            impl_->adapter = candidate;
            impl_->adapter->AddRef();
            return true;
        }
        return false;
    };

    if (prefer_warp) {
        IDXGIAdapter1* warp = nullptr;
        if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
            try_create_device(warp);
            warp->Release();
        }
    }

    if (!impl_->device) {
        IDXGIAdapter1* preferred = nullptr;
        IDXGIFactory6* factory6 = nullptr;
        if (SUCCEEDED(impl_->factory->QueryInterface(IID_PPV_ARGS(&factory6))) && factory6) {
            factory6->EnumAdapterByGpuPreference(
                0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&preferred));
            factory6->Release();
        }
        if (preferred) {
            DXGI_ADAPTER_DESC1 desc{};
            const bool is_sw = SUCCEEDED(preferred->GetDesc1(&desc)) &&
                               (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (!is_sw) {
                try_create_device(preferred);
            }
            preferred->Release();
        }
    }

    if (!impl_->device) {
        for (UINT idx = 0;; ++idx) {
            IDXGIAdapter1* hw = nullptr;
            if (impl_->factory->EnumAdapters1(idx, &hw) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            hw->GetDesc1(&desc);
            const bool is_sw = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (!is_sw && try_create_device(hw)) {
                hw->Release();
                break;
            }
            hw->Release();
        }
    }

    if (!impl_->device) {
        IDXGIAdapter1* warp = nullptr;
        if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
            try_create_device(warp);
            warp->Release();
        }
    }

    if (!impl_->device) {
        return Status::Error(StatusCode::DeviceError, "Failed to create any D3D12 device (HW or WARP)");
    }

    DXGI_ADAPTER_DESC1 adesc{};
    if (impl_->adapter && SUCCEEDED(impl_->adapter->GetDesc1(&adesc))) {
        adapter_info_.description = WideToUtf8(adesc.Description);
        adapter_info_.is_warp = (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        adapter_info_.vendor_id = adesc.VendorId;
        adapter_info_.device_id = adesc.DeviceId;
        std::cerr << "[ScarlettX][d3d12] Adapter: "
                  << (adapter_info_.description.empty() ? "unknown" : adapter_info_.description)
                  << (adapter_info_.is_warp ? " (WARP/software)" : " (hardware)") << "\n";
    }

    if (SUCCEEDED(impl_->device->QueryInterface(IID_PPV_ARGS(&impl_->device5))) && impl_->device5) {
        adapter_info_.supports_device5 = true;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if (SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        adapter_info_.raytracing_tier = static_cast<uint32_t>(opts5.RaytracingTier);
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = impl_->device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&impl_->queue));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "CreateCommandQueue failed");
    }

    hr = impl_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl_->allocator));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "CreateCommandAllocator failed");
    }

    hr = impl_->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->allocator, nullptr, IID_PPV_ARGS(&impl_->cmd_list));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "CreateCommandList failed");
    }
    impl_->cmd_list->Close();

    hr = impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "CreateFence failed");
    }
    impl_->fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->fence_event) {
        return Status::Error(StatusCode::DeviceError, "CreateEventA failed");
    }

    return Status::Ok();
}

void D3D12Backend::Shutdown() {
    if (impl_) {
        impl_->Cleanup();
    }
    adapter_info_ = D3D12AdapterInfo{};
}

Status D3D12Backend::RenderTestTriangle(uint32_t width, uint32_t height, FrameBufferRgba8& out_frame) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "D3D12Backend is not initialized");
    }
    if (width == 0 || height == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Render dimensions must be > 0");
    }

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    ID3DBlob* sig_blob = nullptr;
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* readback = nullptr;

    auto cleanup = [&]() {
        SafeRelease(readback);
        SafeRelease(render_target);
        SafeRelease(rtv_heap);
        SafeRelease(pso);
        SafeRelease(root_sig);
        SafeRelease(sig_blob);
        SafeRelease(err_blob);
        SafeRelease(ps_blob);
        SafeRelease(vs_blob);
    };

    HRESULT hr = D3DCompile(kTriangleHlsl, std::strlen(kTriangleHlsl), "scarlettx_tri.hlsl",
                            nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        std::string msg = err_blob ? std::string(static_cast<const char*>(err_blob->GetBufferPointer()),
                                                 err_blob->GetBufferSize())
                                   : "VS compile failed";
        cleanup();
        return Status::Error(StatusCode::ShaderError, msg);
    }

    SafeRelease(err_blob);
    hr = D3DCompile(kTriangleHlsl, std::strlen(kTriangleHlsl), "scarlettx_tri.hlsl",
                    nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) {
        std::string msg = err_blob ? std::string(static_cast<const char*>(err_blob->GetBufferPointer()),
                                                 err_blob->GetBufferSize())
                                   : "PS compile failed";
        cleanup();
        return Status::Error(StatusCode::ShaderError, msg);
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    SafeRelease(err_blob);
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "D3D12SerializeRootSignature failed");
    }

    hr = impl_->device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_sig));
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateRootSignature failed");
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig;
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    for (auto& rt : pso_desc.BlendState.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    hr = impl_->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateGraphicsPipelineState failed");
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = impl_->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateDescriptorHeap(RTV) failed");
    }

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = width;
    rt_desc.Height = height;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_val.Color[0] = 0.05f;
    clear_val.Color[1] = 0.08f;
    clear_val.Color[2] = 0.14f;
    clear_val.Color[3] = 1.0f;

    hr = impl_->device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clear_val, IID_PPV_ARGS(&render_target));
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource(RT) failed");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    impl_->device->CreateRenderTargetView(render_target, nullptr, rtv_handle);

    const UINT row_pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 readback_size = static_cast<UINT64>(row_pitch) * height;

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = readback_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = impl_->device->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource(Readback) failed");
    }

    impl_->allocator->Reset();
    impl_->cmd_list->Reset(impl_->allocator, pso);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    impl_->cmd_list->SetGraphicsRootSignature(root_sig);
    impl_->cmd_list->RSSetViewports(1, &vp);
    impl_->cmd_list->RSSetScissorRects(1, &scissor);
    impl_->cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    impl_->cmd_list->ClearRenderTargetView(rtv_handle, clear_val.Color, 0, nullptr);
    impl_->cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->cmd_list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = render_target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->cmd_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = render_target;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Offset = 0;
    dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst_loc.PlacedFootprint.Footprint.Width = width;
    dst_loc.PlacedFootprint.Footprint.Height = height;
    dst_loc.PlacedFootprint.Footprint.Depth = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

    impl_->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    impl_->cmd_list->Close();

    ID3D12CommandList* lists[] = {impl_->cmd_list};
    impl_->queue->ExecuteCommandLists(1, lists);

    Status wait_st = impl_->WaitForGpu();
    if (!wait_st.ok()) {
        cleanup();
        return wait_st;
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "Readback Map failed");
    }

    out_frame.width = width;
    out_frame.height = height;
    out_frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out_frame.pixels.data() + static_cast<size_t>(y) * width * 4,
                    src_bytes + static_cast<size_t>(y) * row_pitch,
                    static_cast<size_t>(width) * 4);
    }

    D3D12_RANGE empty_range{0, 0};
    readback->Unmap(0, &empty_range);
    cleanup();
    return Status::Ok();
}

#else

struct D3D12Backend::Impl {};
D3D12Backend::D3D12Backend() = default;
D3D12Backend::~D3D12Backend() = default;
bool D3D12Backend::IsInitialized() const noexcept { return false; }
Status D3D12Backend::Initialize(bool, bool) {
    return Status::Error(StatusCode::NotImplemented, "D3D12Backend requires Windows");
}
void D3D12Backend::Shutdown() {}
Status D3D12Backend::RenderTestTriangle(uint32_t, uint32_t, FrameBufferRgba8&) {
    return Status::Error(StatusCode::NotImplemented, "D3D12Backend requires Windows");
}

#endif

}  // namespace scarlettx
