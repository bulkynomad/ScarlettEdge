#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct D3D12AdapterInfo {
    std::string description;
    bool is_warp = false;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t raytracing_tier = 0;  // Matches D3D12_RAYTRACING_TIER (0, 10=1.0, 11=1.1)
    bool supports_device5 = false;
};

struct FrameBufferRgba8 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, row-major, 4 bytes per pixel

    uint64_t ComputeFnv1aHash() const noexcept;
    Status WritePpm(const std::string& file_path) const;
};

class D3D12Backend {
public:
    D3D12Backend();
    ~D3D12Backend();

    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;

    Status Initialize(bool prefer_warp = true, bool enable_debug_layer = false);
    void Shutdown();
    bool IsInitialized() const noexcept;

    const D3D12AdapterInfo& AdapterInfo() const noexcept { return adapter_info_; }
    void* NativeDevice() const noexcept;
    void* NativeDevice5() const noexcept;

    // Compiles HLSL shaders via D3DCompile, renders an RGB triangle on D3D12, and reads back RGBA8.
    Status RenderTestTriangle(uint32_t width, uint32_t height, FrameBufferRgba8& out_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    D3D12AdapterInfo adapter_info_{};
};

}  // namespace scarlettx
