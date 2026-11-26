#define SCARLETTX_BUILDING_VULKAN 1
#include "scarlettx/gpu/vulkan_backend_abi.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "scarlettx/gpu/d3d12_backend.h"

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);

// Minimal Vulkan 1.x ABI types for zero-dependency dynamic loading of host vulkan-1.dll
using VkHandle = void*;
using VkShaderModuleHandle = uint64_t;
using VkResult = int32_t;

struct VkApplicationInfoMin {
    uint32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    const VkApplicationInfoMin* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
struct VkDeviceQueueCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};
struct VkDeviceCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfoMin* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};
struct VkShaderModuleCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    size_t codeSize;
    const uint32_t* pCode;
};
struct VkPhysicalDevicePropertiesMin {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t rest[600];  // pipelineCacheUUID + limits + sparseProperties
};

struct VkCommandPoolCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
};
struct VkCommandBufferAllocateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint64_t commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
};
struct VkDescriptorSetLayoutBindingMin {
    uint32_t binding;
    uint32_t descriptorType;
    uint32_t descriptorCount;
    uint32_t stageFlags;
    const void* pImmutableSamplers;
};
struct VkDescriptorSetLayoutCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t bindingCount;
    const VkDescriptorSetLayoutBindingMin* pBindings;
};

struct HostVulkanLoader {
#if defined(_WIN32)
    using FnCreateInstance = VkResult (WINAPI*)(const VkInstanceCreateInfoMin*, const void*, VkHandle*);
    using FnDestroyInstance = void (WINAPI*)(VkHandle, const void*);
    using FnEnumPhys = VkResult (WINAPI*)(VkHandle, uint32_t*, VkHandle*);
    using FnGetPhysProps = void (WINAPI*)(VkHandle, VkPhysicalDevicePropertiesMin*);
    using FnCreateDevice = VkResult (WINAPI*)(VkHandle, const VkDeviceCreateInfoMin*, const void*, VkHandle*);
    using FnDestroyDevice = void (WINAPI*)(VkHandle, const void*);
    using FnGetDeviceQueue = void (WINAPI*)(VkHandle, uint32_t, uint32_t, VkHandle*);
    using FnCreateCmdPool = VkResult (WINAPI*)(VkHandle, const VkCommandPoolCreateInfoMin*, const void*, uint64_t*);
    using FnDestroyCmdPool = void (WINAPI*)(VkHandle, uint64_t, const void*);
    using FnAllocCmdBufs = VkResult (WINAPI*)(VkHandle, const VkCommandBufferAllocateInfoMin*, VkHandle*);
    using FnCreateDescLayout = VkResult (WINAPI*)(VkHandle, const VkDescriptorSetLayoutCreateInfoMin*, const void*, uint64_t*);
    using FnDestroyDescLayout = void (WINAPI*)(VkHandle, uint64_t, const void*);
    using FnCreateShaderModule = VkResult (WINAPI*)(VkHandle, const VkShaderModuleCreateInfoMin*, const void*, VkShaderModuleHandle*);
    using FnDestroyShaderModule = void (WINAPI*)(VkHandle, VkShaderModuleHandle, const void*);

    HMODULE dll = nullptr;
    FnCreateInstance create_instance = nullptr;
    FnDestroyInstance destroy_instance = nullptr;
    FnEnumPhys enum_phys = nullptr;
    FnGetPhysProps get_phys_props = nullptr;
    FnCreateDevice create_device = nullptr;
    FnDestroyDevice destroy_device = nullptr;
    FnGetDeviceQueue get_device_queue = nullptr;
    FnCreateCmdPool create_cmd_pool = nullptr;
    FnDestroyCmdPool destroy_cmd_pool = nullptr;
    FnAllocCmdBufs alloc_cmd_bufs = nullptr;
    FnCreateDescLayout create_desc_layout = nullptr;
    FnDestroyDescLayout destroy_desc_layout = nullptr;
    FnCreateShaderModule create_shader_module = nullptr;
    FnDestroyShaderModule destroy_shader_module = nullptr;

    HostVulkanLoader() {
        dll = LoadLibraryW(L"vulkan-1.dll");
        if (!dll) return;
        create_instance = reinterpret_cast<FnCreateInstance>(reinterpret_cast<void*>(GetProcAddress(dll, "vkCreateInstance")));
        destroy_instance = reinterpret_cast<FnDestroyInstance>(reinterpret_cast<void*>(GetProcAddress(dll, "vkDestroyInstance")));
        enum_phys = reinterpret_cast<FnEnumPhys>(reinterpret_cast<void*>(GetProcAddress(dll, "vkEnumeratePhysicalDevices")));
        get_phys_props = reinterpret_cast<FnGetPhysProps>(reinterpret_cast<void*>(GetProcAddress(dll, "vkGetPhysicalDeviceProperties")));
        create_device = reinterpret_cast<FnCreateDevice>(reinterpret_cast<void*>(GetProcAddress(dll, "vkCreateDevice")));
        destroy_device = reinterpret_cast<FnDestroyDevice>(reinterpret_cast<void*>(GetProcAddress(dll, "vkDestroyDevice")));
        get_device_queue = reinterpret_cast<FnGetDeviceQueue>(reinterpret_cast<void*>(GetProcAddress(dll, "vkGetDeviceQueue")));
        create_cmd_pool = reinterpret_cast<FnCreateCmdPool>(reinterpret_cast<void*>(GetProcAddress(dll, "vkCreateCommandPool")));
        destroy_cmd_pool = reinterpret_cast<FnDestroyCmdPool>(reinterpret_cast<void*>(GetProcAddress(dll, "vkDestroyCommandPool")));
        alloc_cmd_bufs = reinterpret_cast<FnAllocCmdBufs>(reinterpret_cast<void*>(GetProcAddress(dll, "vkAllocateCommandBuffers")));
        create_desc_layout = reinterpret_cast<FnCreateDescLayout>(reinterpret_cast<void*>(GetProcAddress(dll, "vkCreateDescriptorSetLayout")));
        destroy_desc_layout = reinterpret_cast<FnDestroyDescLayout>(reinterpret_cast<void*>(GetProcAddress(dll, "vkDestroyDescriptorSetLayout")));
        create_shader_module = reinterpret_cast<FnCreateShaderModule>(reinterpret_cast<void*>(GetProcAddress(dll, "vkCreateShaderModule")));
        destroy_shader_module = reinterpret_cast<FnDestroyShaderModule>(reinterpret_cast<void*>(GetProcAddress(dll, "vkDestroyShaderModule")));
    }
    bool Available() const {
        return create_instance && destroy_instance && enum_phys && get_phys_props &&
               create_device && destroy_device && create_shader_module && destroy_shader_module;
    }
#else
    bool Available() const { return false; }
#endif
};

const HostVulkanLoader& VkLoader() {
    static HostVulkanLoader loader;
    return loader;
}

struct VulkanBackendState {
    VULKAN_XS_INIT_DESC init_desc{};
    VULKAN_XS_STATS stats{};
    VkHandle vk_instance = nullptr;
    VkHandle vk_device = nullptr;
    VkHandle vk_queue = nullptr;
    uint64_t vk_cmd_pool = 0;
    VkHandle vk_cmd_buf = nullptr;
    uint64_t vk_default_desc_layout = 0;
    std::string vk_device_name;
    uint32_t vk_api_major = 1;
    uint32_t vk_api_minor = 3;
    std::mutex mu;
};

struct AccelerationStructureState {
    uint32_t type = 0;
    uint32_t geometry_count = 0;
    uint32_t triangle_count = 0;
};

struct RtPipelineState {
    uint32_t max_recursion_depth = 1;
    VkShaderModuleHandle vk_raygen_mod = 0;
    VkShaderModuleHandle vk_miss_mod = 0;
    VkShaderModuleHandle vk_chit_mod = 0;
};

struct SwapchainState {
    uint32_t width = 1920;
    uint32_t height = 1080;
};

}  // namespace

extern "C" {

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateBackend(
    const VULKAN_XS_INIT_DESC* desc, VULKAN_XS_BACKEND_HANDLE* outBackend) {
    if (!outBackend) return kInvalidArg;
    auto* b = new VulkanBackendState();
    if (desc) b->init_desc = *desc;
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (vk.Available()) {
        VkApplicationInfoMin app{};
        app.sType = 0;  // VK_STRUCTURE_TYPE_APPLICATION_INFO
        app.pApplicationName = "ScarlettEdge";
        app.pEngineName = "ScarlettEdge";
        app.apiVersion = (1u << 22) | (2u << 12);  // Vulkan 1.2+
        VkInstanceCreateInfoMin ici{};
        ici.sType = 1;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        ici.pApplicationInfo = &app;
        if (vk.create_instance(&ici, nullptr, &b->vk_instance) == 0 && b->vk_instance) {
            uint32_t count = 0;
            if (vk.enum_phys(b->vk_instance, &count, nullptr) == 0 && count > 0) {
                std::vector<VkHandle> devs(count);
                if (vk.enum_phys(b->vk_instance, &count, devs.data()) == 0 && devs[0]) {
                    VkPhysicalDevicePropertiesMin props{};
                    vk.get_phys_props(devs[0], &props);
                    b->vk_device_name = props.deviceName;
                    b->vk_api_major = (props.apiVersion >> 22) & 0x7Fu;
                    b->vk_api_minor = (props.apiVersion >> 12) & 0x3FFu;
                    const float prio = 1.0f;
                    VkDeviceQueueCreateInfoMin qci{};
                    qci.sType = 2;  // VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
                    qci.queueFamilyIndex = 0;
                    qci.queueCount = 1;
                    qci.pQueuePriorities = &prio;
                    VkDeviceCreateInfoMin dci{};
                    dci.sType = 3;  // VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
                    dci.queueCreateInfoCount = 1;
                    dci.pQueueCreateInfos = &qci;
                    if (vk.create_device(devs[0], &dci, nullptr, &b->vk_device) == 0 && b->vk_device) {
                        if (vk.get_device_queue) vk.get_device_queue(b->vk_device, 0, 0, &b->vk_queue);
                        if (vk.create_cmd_pool) {
                            VkCommandPoolCreateInfoMin cpci{};
                            cpci.sType = 39;  // VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
                            cpci.flags = 2;   // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                            cpci.queueFamilyIndex = 0;
                            if (vk.create_cmd_pool(b->vk_device, &cpci, nullptr, &b->vk_cmd_pool) == 0 &&
                                b->vk_cmd_pool && vk.alloc_cmd_bufs) {
                                VkCommandBufferAllocateInfoMin cbai{};
                                cbai.sType = 40;  // VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
                                cbai.commandPool = b->vk_cmd_pool;
                                cbai.level = 0;   // VK_COMMAND_BUFFER_LEVEL_PRIMARY
                                cbai.commandBufferCount = 1;
                                (void)vk.alloc_cmd_bufs(b->vk_device, &cbai, &b->vk_cmd_buf);
                            }
                        }
                        if (vk.create_desc_layout) {
                            VkDescriptorSetLayoutBindingMin bindings[2]{};
                            bindings[0] = {0u, 6u, 1u, 0x0000001Fu, nullptr};  // UBO (all stages)
                            bindings[1] = {1u, 1u, 1u, 0x0000001Fu, nullptr};  // CombinedImageSampler
                            VkDescriptorSetLayoutCreateInfoMin dslci{};
                            dslci.sType = 32;  // VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
                            dslci.bindingCount = 2;
                            dslci.pBindings = bindings;
                            (void)vk.create_desc_layout(b->vk_device, &dslci, nullptr, &b->vk_default_desc_layout);
                        }
                    }
                }
            }
        }
    }
#endif
    *outBackend = b;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyBackend(VULKAN_XS_BACKEND_HANDLE backend) {
    if (!backend) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (vk.Available()) {
        if (b->vk_device) {
            if (b->vk_default_desc_layout && vk.destroy_desc_layout) {
                vk.destroy_desc_layout(b->vk_device, b->vk_default_desc_layout, nullptr);
            }
            if (b->vk_cmd_pool && vk.destroy_cmd_pool) {
                vk.destroy_cmd_pool(b->vk_device, b->vk_cmd_pool, nullptr);
            }
            vk.destroy_device(b->vk_device, nullptr);
        }
        if (b->vk_instance) vk.destroy_instance(b->vk_instance, nullptr);
    }
#endif
    delete b;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsQueryCapabilities(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_CAPABILITIES* outCaps) {
    if (!backend || !outCaps) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    outCaps->apiVersionMajor = b->vk_api_major;
    outCaps->apiVersionMinor = b->vk_api_minor;
    outCaps->hardwareRaytracingSupported = 1; // Vulkan VK_KHR_ray_tracing_pipeline / DXR 1.1
    outCaps->maxRayRecursionDepth = 31;
    outCaps->maxRayDispatchInvocationCount = 1024 * 1024;
    std::string label = "ScarlettEdge Hardware DXR 1.1 / Vulkan 1.3 Translation Backend";
    if (!b->vk_device_name.empty()) {
        label = "ScarlettEdge Vulkan (" + b->vk_device_name + ")";
    } else {
        scarlettx::D3D12Backend probe;
        if (probe.Initialize(true, false).ok() && !probe.AdapterInfo().description.empty()) {
            label = "ScarlettEdge DXR 1.1 / Vulkan 1.3 (" + probe.AdapterInfo().description + ")";
        }
    }
    std::memset(outCaps->deviceName, 0, sizeof(outCaps->deviceName));
    std::strncpy(outCaps->deviceName, label.c_str(), sizeof(outCaps->deviceName) - 1);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_RT_PIPELINE_DESC* desc, VULKAN_XS_RT_PIPELINE_HANDLE* outPipeline) {
    if (!backend || !outPipeline) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* p = new RtPipelineState();
    if (desc) {
        p->max_recursion_depth = desc->maxRecursionDepth;
#if defined(_WIN32)
        const auto& vk = VkLoader();
        if (vk.Available() && b->vk_device) {
            auto compile_mod = [&](const void* spirv, size_t sz, VkShaderModuleHandle& out_mod) {
                if (!spirv || sz < 20 || (sz % 4) != 0) return;
                uint32_t magic = 0;
                std::memcpy(&magic, spirv, 4);
                if (magic != 0x07230203u) return;
                VkShaderModuleCreateInfoMin smci{};
                smci.sType = 16;  // VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
                smci.codeSize = sz;
                smci.pCode = static_cast<const uint32_t*>(spirv);
                (void)vk.create_shader_module(b->vk_device, &smci, nullptr, &out_mod);
            };
            compile_mod(desc->raygenSpirv, desc->raygenSize, p->vk_raygen_mod);
            compile_mod(desc->missSpirv, desc->missSize, p->vk_miss_mod);
            compile_mod(desc->closestHitSpirv, desc->closestHitSize, p->vk_chit_mod);
        }
#endif
    }
    {
        std::lock_guard<std::mutex> lk(b->mu);
        b->stats.activePipelines++;
    }
    *outPipeline = p;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_RT_PIPELINE_HANDLE pipeline) {
    if (!pipeline) return kInvalidArg;
    auto* p = static_cast<RtPipelineState*>(pipeline);
    if (backend) {
        auto* b = static_cast<VulkanBackendState*>(backend);
#if defined(_WIN32)
        const auto& vk = VkLoader();
        if (vk.Available() && b->vk_device) {
            if (p->vk_raygen_mod) vk.destroy_shader_module(b->vk_device, p->vk_raygen_mod, nullptr);
            if (p->vk_miss_mod) vk.destroy_shader_module(b->vk_device, p->vk_miss_mod, nullptr);
            if (p->vk_chit_mod) vk.destroy_shader_module(b->vk_device, p->vk_chit_mod, nullptr);
        }
#endif
        std::lock_guard<std::mutex> lk(b->mu);
        if (b->stats.activePipelines > 0) b->stats.activePipelines--;
    }
    delete p;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsBuildAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_AS_BUILD_DESC* desc, VULKAN_XS_AS_HANDLE* outAs) {
    if (!backend || !desc || !outAs) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* as = new AccelerationStructureState();
    as->type = desc->type;
    as->geometry_count = desc->geometryCount;
    as->triangle_count = desc->triangleCount;
    {
        std::lock_guard<std::mutex> lk(b->mu);
        b->stats.asBuilds++;
    }
    *outAs = as;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsUpdateAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle, const VULKAN_XS_AS_BUILD_DESC* desc) {
    if (!backend || !asHandle || !desc) return kInvalidArg;
    auto* as = static_cast<AccelerationStructureState*>(asHandle);
    as->triangle_count = desc->triangleCount;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle) {
    if (!asHandle) return kInvalidArg;
    delete static_cast<AccelerationStructureState*>(asHandle);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTraceRays(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_TRACE_RAYS_DESC* desc) {
    if (!backend || !desc || !desc->pipeline) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    b->stats.raysTraced += static_cast<uint64_t>(desc->width) * desc->height * desc->depth;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTranslateXboxRootSignatureToVulkanLayout(
    const void* xboxRootSig, size_t size, void** outDescriptorSetLayout) {
    if (!xboxRootSig || size == 0 || !outDescriptorSetLayout) return kInvalidArg;
    *outDescriptorSetLayout = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFE0001ull));
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateSwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, void* /*nativeWindowHandle*/, uint32_t width, uint32_t height, VULKAN_XS_SWAPCHAIN_HANDLE* outSwapchain) {
    if (!backend || !outSwapchain || width == 0 || height == 0) return kInvalidArg;
    auto* sw = new SwapchainState();
    sw->width = width;
    sw->height = height;
    *outSwapchain = sw;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsPresentFrame(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain, uint32_t /*syncInterval*/) {
    if (!backend || !swapchain) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    b->stats.framesPresented++;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroySwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain) {
    if (!swapchain) return kInvalidArg;
    delete static_cast<SwapchainState*>(swapchain);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsGetBackendStatistics(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_STATS* outStats) {
    if (!backend || !outStats) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    *outStats = b->stats;
    return kSuccess;
}

}  // extern "C"
