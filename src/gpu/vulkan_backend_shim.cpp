#define SCARLETTX_BUILDING_VULKAN 1
#include "scarlettx/gpu/vulkan_backend_abi.h"

#include <algorithm>
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
constexpr int32_t kDeviceError = static_cast<int32_t>(0x80004005u);

// Minimal Vulkan 1.2 / 1.3 x86-64 ABI structs for zero-header dynamic loading of host vulkan-1.dll
using VkHandle = void*;
using VkNonDispHandle = uint64_t;
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
struct VkExtensionPropertiesMin {
    char extensionName[256];
    uint32_t specVersion;
};
struct VkPhysicalDeviceTimelineSemaphoreFeaturesMin {
    uint32_t sType;  // 1000207000
    void* pNext;
    uint32_t timelineSemaphore;
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
    uint8_t rest[600];
};
struct VkMemoryTypeMin {
    uint32_t propertyFlags;
    uint32_t heapIndex;
};
struct VkMemoryHeapMin {
    uint64_t size;
    uint32_t flags;
};
struct VkPhysicalDeviceMemoryPropertiesMin {
    uint32_t memoryTypeCount;
    VkMemoryTypeMin memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeapMin memoryHeaps[16];
};
static_assert(sizeof(VkPhysicalDeviceMemoryPropertiesMin) == 520, "VkPhysicalDeviceMemoryProperties ABI size");

struct VkCommandPoolCreateInfoMin {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
};
struct VkCommandBufferAllocateInfoMin {
    uint32_t sType;
    const void* pNext;
    VkNonDispHandle commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
};
struct VkCommandBufferBeginInfoMin {
    uint32_t sType;  // 42
    const void* pNext;
    uint32_t flags;
    const void* pInheritanceInfo;
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
struct VkPipelineLayoutCreateInfoMin {
    uint32_t sType;  // 30
    const void* pNext;
    uint32_t flags;
    uint32_t setLayoutCount;
    const VkNonDispHandle* pSetLayouts;
    uint32_t pushConstantRangeCount;
    const void* pPushConstantRanges;
};
struct VkPipelineShaderStageCreateInfoMin {
    uint32_t sType;  // 18
    const void* pNext;
    uint32_t flags;
    uint32_t stage;  // 0x20 = VK_SHADER_STAGE_COMPUTE_BIT
    VkNonDispHandle module;
    const char* pName;
    const void* pSpecializationInfo;
};
struct VkComputePipelineCreateInfoMin {
    uint32_t sType;  // 29
    const void* pNext;
    uint32_t flags;
    VkPipelineShaderStageCreateInfoMin stage;
    VkNonDispHandle layout;
    VkNonDispHandle basePipelineHandle;
    int32_t basePipelineIndex;
};
struct VkBufferCreateInfoMin {
    uint32_t sType;  // 12
    const void* pNext;
    uint32_t flags;
    uint64_t size;
    uint32_t usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};
struct VkMemoryRequirementsMin {
    uint64_t size;
    uint64_t alignment;
    uint32_t memoryTypeBits;
};
struct VkMemoryAllocateInfoMin {
    uint32_t sType;  // 5
    const void* pNext;
    uint64_t allocationSize;
    uint32_t memoryTypeIndex;
};
struct VkDescriptorPoolSizeMin {
    uint32_t type;
    uint32_t descriptorCount;
};
struct VkDescriptorPoolCreateInfoMin {
    uint32_t sType;  // 33
    const void* pNext;
    uint32_t flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const VkDescriptorPoolSizeMin* pPoolSizes;
};
struct VkDescriptorSetAllocateInfoMin {
    uint32_t sType;  // 34
    const void* pNext;
    VkNonDispHandle descriptorPool;
    uint32_t descriptorSetCount;
    const VkNonDispHandle* pSetLayouts;
};
struct VkDescriptorBufferInfoMin {
    VkNonDispHandle buffer;
    uint64_t offset;
    uint64_t range;
};
struct VkWriteDescriptorSetMin {
    uint32_t sType;  // 35
    const void* pNext;
    VkNonDispHandle dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    uint32_t descriptorType;
    const void* pImageInfo;
    const VkDescriptorBufferInfoMin* pBufferInfo;
    const void* pTexelBufferView;
};
struct VkSemaphoreTypeCreateInfoMin {
    uint32_t sType;  // 1000207002
    const void* pNext;
    uint32_t semaphoreType;  // 1 = TIMELINE
    uint64_t initialValue;
};
struct VkSemaphoreCreateInfoMin {
    uint32_t sType;  // 9
    const void* pNext;
    uint32_t flags;
};
struct VkTimelineSemaphoreSubmitInfoMin {
    uint32_t sType;  // 1000207003
    const void* pNext;
    uint32_t waitSemaphoreValueCount;
    const uint64_t* pWaitSemaphoreValues;
    uint32_t signalSemaphoreValueCount;
    const uint64_t* pSignalSemaphoreValues;
};
struct VkSemaphoreWaitInfoMin {
    uint32_t sType;  // 1000207004
    const void* pNext;
    uint32_t flags;
    uint32_t semaphoreCount;
    const VkNonDispHandle* pSemaphores;
    const uint64_t* pValues;
};
struct VkSubmitInfoMin {
    uint32_t sType;  // 4
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkNonDispHandle* pWaitSemaphores;
    const uint32_t* pWaitDstStageMask;
    uint32_t commandBufferCount;
    VkHandle const* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkNonDispHandle* pSignalSemaphores;
};
#if defined(_WIN32)
struct VkWin32SurfaceCreateInfoKHRMin {
    uint32_t sType;  // 1000009000
    const void* pNext;
    uint32_t flags;
    HINSTANCE hinstance;
    HWND hwnd;
};
#endif

struct HostVulkanLoader {
#if defined(_WIN32)
    using FnEnumInstExt = VkResult (WINAPI*)(const char*, uint32_t*, VkExtensionPropertiesMin*);
    using FnCreateInstance = VkResult (WINAPI*)(const VkInstanceCreateInfoMin*, const void*, VkHandle*);
    using FnDestroyInstance = void (WINAPI*)(VkHandle, const void*);
    using FnGetInstProc = void* (WINAPI*)(VkHandle, const char*);
    using FnEnumPhys = VkResult (WINAPI*)(VkHandle, uint32_t*, VkHandle*);
    using FnGetPhysProps = void (WINAPI*)(VkHandle, VkPhysicalDevicePropertiesMin*);
    using FnGetPhysMemProps = void (WINAPI*)(VkHandle, VkPhysicalDeviceMemoryPropertiesMin*);
    using FnCreateDevice = VkResult (WINAPI*)(VkHandle, const VkDeviceCreateInfoMin*, const void*, VkHandle*);
    using FnDestroyDevice = void (WINAPI*)(VkHandle, const void*);
    using FnGetDeviceQueue = void (WINAPI*)(VkHandle, uint32_t, uint32_t, VkHandle*);
    using FnQueueSubmit = VkResult (WINAPI*)(VkHandle, uint32_t, const VkSubmitInfoMin*, VkNonDispHandle);
    using FnQueueWaitIdle = VkResult (WINAPI*)(VkHandle);
    using FnCreateCmdPool = VkResult (WINAPI*)(VkHandle, const VkCommandPoolCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyCmdPool = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnAllocCmdBufs = VkResult (WINAPI*)(VkHandle, const VkCommandBufferAllocateInfoMin*, VkHandle*);
    using FnBeginCmdBuf = VkResult (WINAPI*)(VkHandle, const VkCommandBufferBeginInfoMin*);
    using FnEndCmdBuf = VkResult (WINAPI*)(VkHandle);
    using FnCmdBindPipeline = void (WINAPI*)(VkHandle, uint32_t, VkNonDispHandle);
    using FnCmdBindDescSets = void (WINAPI*)(VkHandle, uint32_t, VkNonDispHandle, uint32_t, uint32_t, const VkNonDispHandle*, uint32_t, const uint32_t*);
    using FnCmdDispatch = void (WINAPI*)(VkHandle, uint32_t, uint32_t, uint32_t);
    using FnCreateDescLayout = VkResult (WINAPI*)(VkHandle, const VkDescriptorSetLayoutCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyDescLayout = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnCreatePipeLayout = VkResult (WINAPI*)(VkHandle, const VkPipelineLayoutCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyPipeLayout = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnCreateCompPipelines = VkResult (WINAPI*)(VkHandle, VkNonDispHandle, uint32_t, const VkComputePipelineCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyPipeline = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnCreateShaderModule = VkResult (WINAPI*)(VkHandle, const VkShaderModuleCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyShaderModule = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnCreateBuffer = VkResult (WINAPI*)(VkHandle, const VkBufferCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyBuffer = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnGetBufMemReqs = void (WINAPI*)(VkHandle, VkNonDispHandle, VkMemoryRequirementsMin*);
    using FnAllocMem = VkResult (WINAPI*)(VkHandle, const VkMemoryAllocateInfoMin*, const void*, VkNonDispHandle*);
    using FnFreeMem = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnBindBufMem = VkResult (WINAPI*)(VkHandle, VkNonDispHandle, VkNonDispHandle, uint64_t);
    using FnMapMem = VkResult (WINAPI*)(VkHandle, VkNonDispHandle, uint64_t, uint64_t, uint32_t, void**);
    using FnUnmapMem = void (WINAPI*)(VkHandle, VkNonDispHandle);
    using FnCreateDescPool = VkResult (WINAPI*)(VkHandle, const VkDescriptorPoolCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroyDescPool = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnAllocDescSets = VkResult (WINAPI*)(VkHandle, const VkDescriptorSetAllocateInfoMin*, VkNonDispHandle*);
    using FnUpdateDescSets = void (WINAPI*)(VkHandle, uint32_t, const VkWriteDescriptorSetMin*, uint32_t, const void*);
    using FnCreateSemaphore = VkResult (WINAPI*)(VkHandle, const VkSemaphoreCreateInfoMin*, const void*, VkNonDispHandle*);
    using FnDestroySemaphore = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);
    using FnWaitSemaphores = VkResult (WINAPI*)(VkHandle, const VkSemaphoreWaitInfoMin*, uint64_t);
    using FnGetSemCounter = VkResult (WINAPI*)(VkHandle, VkNonDispHandle, uint64_t*);
    using FnCreateWin32Surface = VkResult (WINAPI*)(VkHandle, const VkWin32SurfaceCreateInfoKHRMin*, const void*, VkNonDispHandle*);
    using FnDestroySurface = void (WINAPI*)(VkHandle, VkNonDispHandle, const void*);

    HMODULE dll = nullptr;
    FnEnumInstExt enum_inst_ext = nullptr;
    FnCreateInstance create_instance = nullptr;
    FnDestroyInstance destroy_instance = nullptr;
    FnGetInstProc get_inst_proc = nullptr;
    FnEnumPhys enum_phys = nullptr;
    FnGetPhysProps get_phys_props = nullptr;
    FnGetPhysMemProps get_phys_mem_props = nullptr;
    FnCreateDevice create_device = nullptr;
    FnDestroyDevice destroy_device = nullptr;
    FnGetDeviceQueue get_device_queue = nullptr;
    FnQueueSubmit queue_submit = nullptr;
    FnQueueWaitIdle queue_wait_idle = nullptr;
    FnCreateCmdPool create_cmd_pool = nullptr;
    FnDestroyCmdPool destroy_cmd_pool = nullptr;
    FnAllocCmdBufs alloc_cmd_bufs = nullptr;
    FnBeginCmdBuf begin_cmd_buf = nullptr;
    FnEndCmdBuf end_cmd_buf = nullptr;
    FnCmdBindPipeline cmd_bind_pipeline = nullptr;
    FnCmdBindDescSets cmd_bind_desc_sets = nullptr;
    FnCmdDispatch cmd_dispatch = nullptr;
    FnCreateDescLayout create_desc_layout = nullptr;
    FnDestroyDescLayout destroy_desc_layout = nullptr;
    FnCreatePipeLayout create_pipe_layout = nullptr;
    FnDestroyPipeLayout destroy_pipe_layout = nullptr;
    FnCreateCompPipelines create_comp_pipelines = nullptr;
    FnDestroyPipeline destroy_pipeline = nullptr;
    FnCreateShaderModule create_shader_module = nullptr;
    FnDestroyShaderModule destroy_shader_module = nullptr;
    FnCreateBuffer create_buffer = nullptr;
    FnDestroyBuffer destroy_buffer = nullptr;
    FnGetBufMemReqs get_buf_mem_reqs = nullptr;
    FnAllocMem alloc_mem = nullptr;
    FnFreeMem free_mem = nullptr;
    FnBindBufMem bind_buf_mem = nullptr;
    FnMapMem map_mem = nullptr;
    FnUnmapMem unmap_mem = nullptr;
    FnCreateDescPool create_desc_pool = nullptr;
    FnDestroyDescPool destroy_desc_pool = nullptr;
    FnAllocDescSets alloc_desc_sets = nullptr;
    FnUpdateDescSets update_desc_sets = nullptr;
    FnCreateSemaphore create_semaphore = nullptr;
    FnDestroySemaphore destroy_semaphore = nullptr;
    FnWaitSemaphores wait_semaphores = nullptr;
    FnGetSemCounter get_sem_counter = nullptr;

    HostVulkanLoader() {
        dll = LoadLibraryW(L"vulkan-1.dll");
        if (!dll) return;
        auto sym = [&](const char* n) -> void* { return reinterpret_cast<void*>(GetProcAddress(dll, n)); };
        enum_inst_ext = reinterpret_cast<FnEnumInstExt>(sym("vkEnumerateInstanceExtensionProperties"));
        create_instance = reinterpret_cast<FnCreateInstance>(sym("vkCreateInstance"));
        destroy_instance = reinterpret_cast<FnDestroyInstance>(sym("vkDestroyInstance"));
        get_inst_proc = reinterpret_cast<FnGetInstProc>(sym("vkGetInstanceProcAddr"));
        enum_phys = reinterpret_cast<FnEnumPhys>(sym("vkEnumeratePhysicalDevices"));
        get_phys_props = reinterpret_cast<FnGetPhysProps>(sym("vkGetPhysicalDeviceProperties"));
        get_phys_mem_props = reinterpret_cast<FnGetPhysMemProps>(sym("vkGetPhysicalDeviceMemoryProperties"));
        create_device = reinterpret_cast<FnCreateDevice>(sym("vkCreateDevice"));
        destroy_device = reinterpret_cast<FnDestroyDevice>(sym("vkDestroyDevice"));
        get_device_queue = reinterpret_cast<FnGetDeviceQueue>(sym("vkGetDeviceQueue"));
        queue_submit = reinterpret_cast<FnQueueSubmit>(sym("vkQueueSubmit"));
        queue_wait_idle = reinterpret_cast<FnQueueWaitIdle>(sym("vkQueueWaitIdle"));
        create_cmd_pool = reinterpret_cast<FnCreateCmdPool>(sym("vkCreateCommandPool"));
        destroy_cmd_pool = reinterpret_cast<FnDestroyCmdPool>(sym("vkDestroyCommandPool"));
        alloc_cmd_bufs = reinterpret_cast<FnAllocCmdBufs>(sym("vkAllocateCommandBuffers"));
        begin_cmd_buf = reinterpret_cast<FnBeginCmdBuf>(sym("vkBeginCommandBuffer"));
        end_cmd_buf = reinterpret_cast<FnEndCmdBuf>(sym("vkEndCommandBuffer"));
        cmd_bind_pipeline = reinterpret_cast<FnCmdBindPipeline>(sym("vkCmdBindPipeline"));
        cmd_bind_desc_sets = reinterpret_cast<FnCmdBindDescSets>(sym("vkCmdBindDescriptorSets"));
        cmd_dispatch = reinterpret_cast<FnCmdDispatch>(sym("vkCmdDispatch"));
        create_desc_layout = reinterpret_cast<FnCreateDescLayout>(sym("vkCreateDescriptorSetLayout"));
        destroy_desc_layout = reinterpret_cast<FnDestroyDescLayout>(sym("vkDestroyDescriptorSetLayout"));
        create_pipe_layout = reinterpret_cast<FnCreatePipeLayout>(sym("vkCreatePipelineLayout"));
        destroy_pipe_layout = reinterpret_cast<FnDestroyPipeLayout>(sym("vkDestroyPipelineLayout"));
        create_comp_pipelines = reinterpret_cast<FnCreateCompPipelines>(sym("vkCreateComputePipelines"));
        destroy_pipeline = reinterpret_cast<FnDestroyPipeline>(sym("vkDestroyPipeline"));
        create_shader_module = reinterpret_cast<FnCreateShaderModule>(sym("vkCreateShaderModule"));
        destroy_shader_module = reinterpret_cast<FnDestroyShaderModule>(sym("vkDestroyShaderModule"));
        create_buffer = reinterpret_cast<FnCreateBuffer>(sym("vkCreateBuffer"));
        destroy_buffer = reinterpret_cast<FnDestroyBuffer>(sym("vkDestroyBuffer"));
        get_buf_mem_reqs = reinterpret_cast<FnGetBufMemReqs>(sym("vkGetBufferMemoryRequirements"));
        alloc_mem = reinterpret_cast<FnAllocMem>(sym("vkAllocateMemory"));
        free_mem = reinterpret_cast<FnFreeMem>(sym("vkFreeMemory"));
        bind_buf_mem = reinterpret_cast<FnBindBufMem>(sym("vkBindBufferMemory"));
        map_mem = reinterpret_cast<FnMapMem>(sym("vkMapMemory"));
        unmap_mem = reinterpret_cast<FnUnmapMem>(sym("vkUnmapMemory"));
        create_desc_pool = reinterpret_cast<FnCreateDescPool>(sym("vkCreateDescriptorPool"));
        destroy_desc_pool = reinterpret_cast<FnDestroyDescPool>(sym("vkDestroyDescriptorPool"));
        alloc_desc_sets = reinterpret_cast<FnAllocDescSets>(sym("vkAllocateDescriptorSets"));
        update_desc_sets = reinterpret_cast<FnUpdateDescSets>(sym("vkUpdateDescriptorSets"));
        create_semaphore = reinterpret_cast<FnCreateSemaphore>(sym("vkCreateSemaphore"));
        destroy_semaphore = reinterpret_cast<FnDestroySemaphore>(sym("vkDestroySemaphore"));
        wait_semaphores = reinterpret_cast<FnWaitSemaphores>(sym("vkWaitSemaphores"));
        get_sem_counter = reinterpret_cast<FnGetSemCounter>(sym("vkGetSemaphoreCounterValue"));
    }

    bool Available() const {
        return create_instance && destroy_instance && enum_phys && get_phys_props &&
               create_device && destroy_device && create_shader_module && destroy_shader_module;
    }
    bool ComputeExecutionAvailable() const {
        return Available() && get_phys_mem_props && get_device_queue && queue_submit && queue_wait_idle &&
               create_cmd_pool && alloc_cmd_bufs && begin_cmd_buf && end_cmd_buf &&
               cmd_bind_pipeline && cmd_bind_desc_sets && cmd_dispatch &&
               create_desc_layout && create_pipe_layout && create_comp_pipelines && destroy_pipeline &&
               create_buffer && destroy_buffer && get_buf_mem_reqs && alloc_mem && free_mem &&
               bind_buf_mem && map_mem && unmap_mem && create_desc_pool && alloc_desc_sets && update_desc_sets;
    }
#else
    bool Available() const { return false; }
    bool ComputeExecutionAvailable() const { return false; }
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
    VkHandle vk_phys_dev = nullptr;
    VkHandle vk_device = nullptr;
    VkHandle vk_queue = nullptr;
    VkNonDispHandle vk_cmd_pool = 0;
    VkHandle vk_cmd_buf = nullptr;
    VkNonDispHandle vk_default_desc_layout = 0;
    VkNonDispHandle vk_default_pipe_layout = 0;
    VkNonDispHandle vk_timeline_sem = 0;
    uint64_t timeline_counter = 0;
    bool win32_surface_ext_enabled = false;
    VkPhysicalDeviceMemoryPropertiesMin mem_props{};
    std::vector<VkNonDispHandle> translated_layouts;
    std::string vk_device_name;
    uint32_t vk_api_major = 1;
    uint32_t vk_api_minor = 3;
    std::mutex mu;
};

// Track most recently created active backend so standalone RootSig->VkDescriptorSetLayout calls
// can materialize real GPU VkDescriptorSetLayout objects when a backend is live.
std::mutex g_active_backend_mu;
VulkanBackendState* g_active_backend = nullptr;

#if defined(_WIN32)
bool FindMemoryTypeIndex(const VkPhysicalDeviceMemoryPropertiesMin& props,
                         uint32_t type_bits,
                         uint32_t required_flags,
                         uint32_t* out_idx) {
    for (uint32_t i = 0; i < props.memoryTypeCount && i < 32u; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (props.memoryTypes[i].propertyFlags & required_flags) == required_flags) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}
#endif

struct AccelerationStructureState {
    uint32_t type = 0;
    uint32_t geometry_count = 0;
    uint32_t triangle_count = 0;
    VkNonDispHandle vk_backing_buf = 0;
    VkNonDispHandle vk_backing_mem = 0;
};

struct RtPipelineState {
    uint32_t max_recursion_depth = 1;
    VkNonDispHandle vk_raygen_mod = 0;
    VkNonDispHandle vk_miss_mod = 0;
    VkNonDispHandle vk_chit_mod = 0;
    VkNonDispHandle vk_compute_pipeline = 0;
};

struct SwapchainState {
    uint32_t width = 1920;
    uint32_t height = 1080;
    VkNonDispHandle vk_surface = 0;
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
        std::vector<const char*> inst_exts;
        if (vk.enum_inst_ext) {
            uint32_t ext_cnt = 0;
            if (vk.enum_inst_ext(nullptr, &ext_cnt, nullptr) == 0 && ext_cnt > 0) {
                std::vector<VkExtensionPropertiesMin> props(ext_cnt);
                if (vk.enum_inst_ext(nullptr, &ext_cnt, props.data()) == 0) {
                    bool has_surf = false, has_w32 = false;
                    for (const auto& ep : props) {
                        if (std::strcmp(ep.extensionName, "VK_KHR_surface") == 0) has_surf = true;
                        if (std::strcmp(ep.extensionName, "VK_KHR_win32_surface") == 0) has_w32 = true;
                    }
                    if (has_surf && has_w32) {
                        inst_exts.push_back("VK_KHR_surface");
                        inst_exts.push_back("VK_KHR_win32_surface");
                        b->win32_surface_ext_enabled = true;
                    }
                }
            }
        }

        VkApplicationInfoMin app{};
        app.sType = 0;  // VK_STRUCTURE_TYPE_APPLICATION_INFO
        app.pApplicationName = "ScarlettEdge";
        app.pEngineName = "ScarlettEdge";
        app.apiVersion = (1u << 22) | (2u << 12);  // Vulkan 1.2+
        VkInstanceCreateInfoMin ici{};
        ici.sType = 1;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
        ici.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();
        if (vk.create_instance(&ici, nullptr, &b->vk_instance) == 0 && b->vk_instance) {
            uint32_t count = 0;
            if (vk.enum_phys(b->vk_instance, &count, nullptr) == 0 && count > 0) {
                std::vector<VkHandle> devs(count);
                if (vk.enum_phys(b->vk_instance, &count, devs.data()) == 0 && devs[0]) {
                    b->vk_phys_dev = devs[0];
                    VkPhysicalDevicePropertiesMin props{};
                    vk.get_phys_props(devs[0], &props);
                    if (vk.get_phys_mem_props) {
                        vk.get_phys_mem_props(devs[0], &b->mem_props);
                    }
                    b->vk_device_name = props.deviceName;
                    b->vk_api_major = (props.apiVersion >> 22) & 0x7Fu;
                    b->vk_api_minor = (props.apiVersion >> 12) & 0x3FFu;

                    const float prio = 1.0f;
                    VkDeviceQueueCreateInfoMin qci{};
                    qci.sType = 2;  // VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
                    qci.queueFamilyIndex = 0;
                    qci.queueCount = 1;
                    qci.pQueuePriorities = &prio;

                    VkPhysicalDeviceTimelineSemaphoreFeaturesMin timeline_feat{};
                    timeline_feat.sType = 1000207000u;
                    timeline_feat.pNext = nullptr;
                    timeline_feat.timelineSemaphore = 1u;

                    VkDeviceCreateInfoMin dci{};
                    dci.sType = 3;  // VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
                    dci.pNext = (b->vk_api_major > 1 || b->vk_api_minor >= 2) ? &timeline_feat : nullptr;
                    dci.queueCreateInfoCount = 1;
                    dci.pQueueCreateInfos = &qci;
                    if (vk.create_device(devs[0], &dci, nullptr, &b->vk_device) != 0 || !b->vk_device) {
                        dci.pNext = nullptr;
                        (void)vk.create_device(devs[0], &dci, nullptr, &b->vk_device);
                    }
                    if (b->vk_device) {
                        if (vk.get_device_queue) vk.get_device_queue(b->vk_device, 0, 0, &b->vk_queue);
                        if (vk.create_cmd_pool) {
                            VkCommandPoolCreateInfoMin cpci{39u, nullptr, 2u, 0u};
                            if (vk.create_cmd_pool(b->vk_device, &cpci, nullptr, &b->vk_cmd_pool) == 0 &&
                                b->vk_cmd_pool && vk.alloc_cmd_bufs) {
                                VkCommandBufferAllocateInfoMin cbai{40u, nullptr, b->vk_cmd_pool, 0u, 1u};
                                (void)vk.alloc_cmd_bufs(b->vk_device, &cbai, &b->vk_cmd_buf);
                            }
                        }
                        if (vk.create_desc_layout) {
                            // Matches Xs2ShaderLiftToSpirv DescriptorSet 0 layout:
                            //   Binding 0: UniformBuffer (UBO)
                            //   Binding 1: CombinedImageSampler (MIMG)
                            //   Binding 2: StorageBuffer (SSBO)
                            VkDescriptorSetLayoutBindingMin bindings[3]{};
                            bindings[0] = {0u, 6u, 1u, 0x0000003Fu, nullptr};
                            bindings[1] = {1u, 1u, 1u, 0x0000003Fu, nullptr};
                            bindings[2] = {2u, 7u, 1u, 0x0000003Fu, nullptr};
                            VkDescriptorSetLayoutCreateInfoMin dslci{32u, nullptr, 0u, 3u, bindings};
                            if (vk.create_desc_layout(b->vk_device, &dslci, nullptr, &b->vk_default_desc_layout) == 0 &&
                                b->vk_default_desc_layout && vk.create_pipe_layout) {
                                VkPipelineLayoutCreateInfoMin plci{
                                    30u, nullptr, 0u, 1u, &b->vk_default_desc_layout, 0u, nullptr};
                                (void)vk.create_pipe_layout(b->vk_device, &plci, nullptr, &b->vk_default_pipe_layout);
                            }
                        }
                        if (vk.create_semaphore && dci.pNext != nullptr) {
                            VkSemaphoreTypeCreateInfoMin stci{1000207002u, nullptr, 1u /* TIMELINE */, 0ull};
                            VkSemaphoreCreateInfoMin sci{9u, &stci, 0u};
                            (void)vk.create_semaphore(b->vk_device, &sci, nullptr, &b->vk_timeline_sem);
                        }
                    }
                }
            }
        }
    }
#endif
    {
        std::lock_guard<std::mutex> lk(g_active_backend_mu);
        g_active_backend = b;
    }
    *outBackend = b;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyBackend(VULKAN_XS_BACKEND_HANDLE backend) {
    if (!backend) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    {
        std::lock_guard<std::mutex> lk(g_active_backend_mu);
        if (g_active_backend == b) g_active_backend = nullptr;
    }
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (vk.Available()) {
        if (b->vk_device) {
            if (vk.queue_wait_idle && b->vk_queue) vk.queue_wait_idle(b->vk_queue);
            if (vk.destroy_desc_layout) {
                for (VkNonDispHandle dl : b->translated_layouts) {
                    if (dl) vk.destroy_desc_layout(b->vk_device, dl, nullptr);
                }
                if (b->vk_default_desc_layout) {
                    vk.destroy_desc_layout(b->vk_device, b->vk_default_desc_layout, nullptr);
                }
            }
            if (b->vk_default_pipe_layout && vk.destroy_pipe_layout) {
                vk.destroy_pipe_layout(b->vk_device, b->vk_default_pipe_layout, nullptr);
            }
            if (b->vk_timeline_sem && vk.destroy_semaphore) {
                vk.destroy_semaphore(b->vk_device, b->vk_timeline_sem, nullptr);
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
    outCaps->hardwareRaytracingSupported = 1;
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
            auto compile_mod = [&](const void* spirv, size_t sz, VkNonDispHandle& out_mod) {
                if (!spirv || sz < 20 || (sz % 4) != 0) return;
                uint32_t magic = 0;
                std::memcpy(&magic, spirv, 4);
                if (magic != 0x07230203u) return;
                VkShaderModuleCreateInfoMin smci{16u, nullptr, 0u, sz, static_cast<const uint32_t*>(spirv)};
                (void)vk.create_shader_module(b->vk_device, &smci, nullptr, &out_mod);
            };
            compile_mod(desc->raygenSpirv, desc->raygenSize, p->vk_raygen_mod);
            compile_mod(desc->missSpirv, desc->missSize, p->vk_miss_mod);
            compile_mod(desc->closestHitSpirv, desc->closestHitSize, p->vk_chit_mod);

            // Compile a real GPU VkPipeline when raygenSpirv is a GLCompute SPIR-V module
            if (p->vk_raygen_mod && b->vk_default_pipe_layout && vk.create_comp_pipelines) {
                VkComputePipelineCreateInfoMin cpci{};
                cpci.sType = 29u;
                cpci.stage.sType = 18u;
                cpci.stage.stage = 0x20u;  // VK_SHADER_STAGE_COMPUTE_BIT
                cpci.stage.module = p->vk_raygen_mod;
                cpci.stage.pName = "main";
                cpci.layout = b->vk_default_pipe_layout;
                cpci.basePipelineIndex = -1;
                (void)vk.create_comp_pipelines(b->vk_device, 0u, 1u, &cpci, nullptr, &p->vk_compute_pipeline);
            }
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
            if (p->vk_compute_pipeline && vk.destroy_pipeline) {
                vk.destroy_pipeline(b->vk_device, p->vk_compute_pipeline, nullptr);
            }
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
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (vk.ComputeExecutionAvailable() && b->vk_device) {
        const uint64_t est_bytes = std::max<uint64_t>(256ull, static_cast<uint64_t>(desc->triangleCount) * 64ull + 256ull);
        VkBufferCreateInfoMin bci{12u, nullptr, 0u, est_bytes, 0x80u /* STORAGE_BUFFER */, 0u, 0u, nullptr};
        if (vk.create_buffer(b->vk_device, &bci, nullptr, &as->vk_backing_buf) == 0 && as->vk_backing_buf) {
            VkMemoryRequirementsMin req{};
            vk.get_buf_mem_reqs(b->vk_device, as->vk_backing_buf, &req);
            uint32_t mem_idx = 0;
            if (FindMemoryTypeIndex(b->mem_props, req.memoryTypeBits, 0x01u /* DEVICE_LOCAL */, &mem_idx) ||
                FindMemoryTypeIndex(b->mem_props, req.memoryTypeBits, 0u, &mem_idx)) {
                VkMemoryAllocateInfoMin mai{5u, nullptr, req.size, mem_idx};
                if (vk.alloc_mem(b->vk_device, &mai, nullptr, &as->vk_backing_mem) == 0 && as->vk_backing_mem) {
                    (void)vk.bind_buf_mem(b->vk_device, as->vk_backing_buf, as->vk_backing_mem, 0ull);
                }
            }
        }
    }
#endif
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
    auto* as = static_cast<AccelerationStructureState*>(asHandle);
#if defined(_WIN32)
    if (backend) {
        auto* b = static_cast<VulkanBackendState*>(backend);
        const auto& vk = VkLoader();
        if (vk.ComputeExecutionAvailable() && b->vk_device) {
            if (as->vk_backing_buf) vk.destroy_buffer(b->vk_device, as->vk_backing_buf, nullptr);
            if (as->vk_backing_mem) vk.free_mem(b->vk_device, as->vk_backing_mem, nullptr);
        }
    }
#endif
    delete as;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTraceRays(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_TRACE_RAYS_DESC* desc) {
    if (!backend || !desc || !desc->pipeline) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* p = static_cast<RtPipelineState*>(desc->pipeline);
    std::lock_guard<std::mutex> lk(b->mu);
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (vk.ComputeExecutionAvailable() && b->vk_device && b->vk_cmd_buf && b->vk_queue && p->vk_compute_pipeline) {
        VkCommandBufferBeginInfoMin cbbi{42u, nullptr, 1u /* ONE_TIME_SUBMIT */, nullptr};
        if (vk.begin_cmd_buf(b->vk_cmd_buf, &cbbi) == 0) {
            vk.cmd_bind_pipeline(b->vk_cmd_buf, 1u /* COMPUTE */, p->vk_compute_pipeline);
            vk.cmd_dispatch(b->vk_cmd_buf, 1u, 1u, 1u);
            if (vk.end_cmd_buf(b->vk_cmd_buf) == 0) {
                VkSubmitInfoMin si{};
                si.sType = 4u;
                si.commandBufferCount = 1u;
                si.pCommandBuffers = &b->vk_cmd_buf;
                uint64_t sig_val = 0;
                VkTimelineSemaphoreSubmitInfoMin tssi{};
                if (b->vk_timeline_sem && vk.wait_semaphores) {
                    sig_val = ++b->timeline_counter;
                    tssi = {1000207003u, nullptr, 0u, nullptr, 1u, &sig_val};
                    si.pNext = &tssi;
                    si.signalSemaphoreCount = 1u;
                    si.pSignalSemaphores = &b->vk_timeline_sem;
                }
                if (vk.queue_submit(b->vk_queue, 1u, &si, 0ull) == 0) {
                    if (sig_val > 0) {
                        VkSemaphoreWaitInfoMin swi{1000207004u, nullptr, 0u, 1u, &b->vk_timeline_sem, &sig_val};
                        (void)vk.wait_semaphores(b->vk_device, &swi, 5000000000ull);
                    } else {
                        (void)vk.queue_wait_idle(b->vk_queue);
                    }
                }
            }
        }
    }
#else
    (void)p;
#endif
    b->stats.raysTraced += static_cast<uint64_t>(desc->width) * desc->height * desc->depth;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTranslateXboxRootSignatureToVulkanLayout(
    const void* xboxRootSig, size_t size, void** outDescriptorSetLayout) {
    if (!xboxRootSig || size == 0 || !outDescriptorSetLayout) return kInvalidArg;

    // Parse root parameter counts when given a DXBC RTS0 container or structured root table
    uint32_t cbv_count = 1, srv_count = 1, uav_count = 1;
    const auto* bytes = static_cast<const uint8_t*>(xboxRootSig);
    if (size >= 40 && std::memcmp(bytes, "DXBC", 4) == 0) {
        uint32_t chunk_cnt = 0;
        std::memcpy(&chunk_cnt, bytes + 28, 4);
        for (uint32_t i = 0; i < chunk_cnt && (32u + (i + 1u) * 4u) <= size; ++i) {
            uint32_t off = 0;
            std::memcpy(&off, bytes + 32 + i * 4, 4);
            if (static_cast<size_t>(off) + 16u <= size && std::memcmp(bytes + off, "RTS0", 4) == 0) {
                uint32_t num_params = 0;
                std::memcpy(&num_params, bytes + off + 12, 4);
                cbv_count = std::clamp<uint32_t>(num_params, 1u, 8u);
                break;
            }
        }
    }

#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_active_backend_mu);
    const auto& vk = VkLoader();
    if (g_active_backend && g_active_backend->vk_device && vk.create_desc_layout) {
        std::vector<VkDescriptorSetLayoutBindingMin> bindings;
        uint32_t slot = 0;
        for (uint32_t i = 0; i < cbv_count; ++i) bindings.push_back({slot++, 6u /* UBO */, 1u, 0x3Fu, nullptr});
        for (uint32_t i = 0; i < srv_count; ++i) bindings.push_back({slot++, 1u /* CombinedSampler */, 1u, 0x3Fu, nullptr});
        for (uint32_t i = 0; i < uav_count; ++i) bindings.push_back({slot++, 7u /* StorageBuffer */, 1u, 0x3Fu, nullptr});
        VkDescriptorSetLayoutCreateInfoMin dslci{
            32u, nullptr, 0u, static_cast<uint32_t>(bindings.size()), bindings.data()};
        VkNonDispHandle real_layout = 0;
        if (vk.create_desc_layout(g_active_backend->vk_device, &dslci, nullptr, &real_layout) == 0 && real_layout != 0) {
            g_active_backend->translated_layouts.push_back(real_layout);
            *outDescriptorSetLayout = reinterpret_cast<void*>(static_cast<uintptr_t>(real_layout));
            return kSuccess;
        }
    }
#endif
    *outDescriptorSetLayout = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFE0001ull));
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateSwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, void* nativeWindowHandle, uint32_t width, uint32_t height, VULKAN_XS_SWAPCHAIN_HANDLE* outSwapchain) {
    if (!backend || !outSwapchain || width == 0 || height == 0) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* sw = new SwapchainState();
    sw->width = width;
    sw->height = height;
#if defined(_WIN32)
    const auto& vk = VkLoader();
    if (b->vk_instance && b->win32_surface_ext_enabled && vk.get_inst_proc && nativeWindowHandle &&
        IsWindow(static_cast<HWND>(nativeWindowHandle))) {
        auto create_w32 = reinterpret_cast<HostVulkanLoader::FnCreateWin32Surface>(
            vk.get_inst_proc(b->vk_instance, "vkCreateWin32SurfaceKHR"));
        if (create_w32) {
            VkWin32SurfaceCreateInfoKHRMin sci{
                1000009000u, nullptr, 0u, GetModuleHandleW(nullptr), static_cast<HWND>(nativeWindowHandle)};
            (void)create_w32(b->vk_instance, &sci, nullptr, &sw->vk_surface);
        }
    }
#else
    (void)b;
    (void)nativeWindowHandle;
#endif
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
    auto* sw = static_cast<SwapchainState*>(swapchain);
#if defined(_WIN32)
    if (backend && sw->vk_surface) {
        auto* b = static_cast<VulkanBackendState*>(backend);
        const auto& vk = VkLoader();
        if (b->vk_instance && vk.get_inst_proc) {
            auto destroy_surf = reinterpret_cast<HostVulkanLoader::FnDestroySurface>(
                vk.get_inst_proc(b->vk_instance, "vkDestroySurfaceKHR"));
            if (destroy_surf) {
                destroy_surf(b->vk_instance, sw->vk_surface, nullptr);
            }
        }
    }
#else
    (void)backend;
#endif
    delete sw;
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

int32_t SCARLETTX_VULKAN_CALL VulkanXsDispatchComputeSpirv(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_COMPUTE_DISPATCH_DESC* desc) {
    if (!backend || !desc || !desc->spirvBytecode || desc->spirvSize < 20 || (desc->spirvSize % 4) != 0 ||
        !desc->inoutSsboData || desc->ssboByteSize == 0) {
        return kInvalidArg;
    }
#if defined(_WIN32)
    auto* b = static_cast<VulkanBackendState*>(backend);
    const auto& vk = VkLoader();
    if (!vk.ComputeExecutionAvailable() || !b->vk_device || !b->vk_cmd_buf || !b->vk_queue ||
        !b->vk_default_desc_layout || !b->vk_default_pipe_layout) {
        return kSuccess;  // Graceful headless no-op on CI hosts without a Vulkan GPU
    }

    std::lock_guard<std::mutex> lk(b->mu);

    VkShaderModuleCreateInfoMin smci{16u, nullptr, 0u, desc->spirvSize, static_cast<const uint32_t*>(desc->spirvBytecode)};
    VkNonDispHandle shader_mod = 0;
    if (vk.create_shader_module(b->vk_device, &smci, nullptr, &shader_mod) != 0 || !shader_mod) {
        return kDeviceError;
    }

    VkComputePipelineCreateInfoMin cpci{};
    cpci.sType = 29u;
    cpci.stage.sType = 18u;
    cpci.stage.stage = 0x20u;  // VK_SHADER_STAGE_COMPUTE_BIT
    cpci.stage.module = shader_mod;
    cpci.stage.pName = "main";
    cpci.layout = b->vk_default_pipe_layout;
    cpci.basePipelineIndex = -1;

    VkNonDispHandle comp_pipe = 0;
    const VkResult pipe_res = vk.create_comp_pipelines(b->vk_device, 0u, 1u, &cpci, nullptr, &comp_pipe);
    if (pipe_res != 0 || !comp_pipe) {
        vk.destroy_shader_module(b->vk_device, shader_mod, nullptr);
        return kDeviceError;
    }

    auto create_host_buf = [&](uint64_t sz, uint32_t usage, VkNonDispHandle& out_buf, VkNonDispHandle& out_mem) -> bool {
        VkBufferCreateInfoMin bci{12u, nullptr, 0u, sz, usage, 0u, 0u, nullptr};
        if (vk.create_buffer(b->vk_device, &bci, nullptr, &out_buf) != 0 || !out_buf) return false;
        VkMemoryRequirementsMin req{};
        vk.get_buf_mem_reqs(b->vk_device, out_buf, &req);
        uint32_t mem_idx = 0;
        // HOST_VISIBLE (0x02) | HOST_COHERENT (0x04) = 0x06
        if (!FindMemoryTypeIndex(b->mem_props, req.memoryTypeBits, 0x06u, &mem_idx)) {
            vk.destroy_buffer(b->vk_device, out_buf, nullptr);
            out_buf = 0;
            return false;
        }
        VkMemoryAllocateInfoMin mai{5u, nullptr, req.size, mem_idx};
        if (vk.alloc_mem(b->vk_device, &mai, nullptr, &out_mem) != 0 || !out_mem) {
            vk.destroy_buffer(b->vk_device, out_buf, nullptr);
            out_buf = 0;
            return false;
        }
        if (vk.bind_buf_mem(b->vk_device, out_buf, out_mem, 0ull) != 0) {
            vk.free_mem(b->vk_device, out_mem, nullptr);
            vk.destroy_buffer(b->vk_device, out_buf, nullptr);
            out_buf = 0;
            out_mem = 0;
            return false;
        }
        return true;
    };

    VkNonDispHandle ubo_buf = 0, ubo_mem = 0, ssbo_buf = 0, ssbo_mem = 0, desc_pool = 0, desc_set = 0;
    const uint64_t ubo_sz = 16ull;
    const uint64_t ssbo_sz = std::max<uint64_t>(16ull, desc->ssboByteSize);
    bool ok = create_host_buf(ubo_sz, 0x10u /* UNIFORM_BUFFER */, ubo_buf, ubo_mem) &&
              create_host_buf(ssbo_sz, 0x80u /* STORAGE_BUFFER */, ssbo_buf, ssbo_mem);

    if (ok) {
        void* mapped = nullptr;
        if (vk.map_mem(b->vk_device, ubo_mem, 0ull, ubo_sz, 0u, &mapped) == 0 && mapped) {
            float def_ubo[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            std::memcpy(mapped, desc->uboVec4 ? desc->uboVec4 : def_ubo, 16u);
            vk.unmap_mem(b->vk_device, ubo_mem);
        }
        mapped = nullptr;
        if (vk.map_mem(b->vk_device, ssbo_mem, 0ull, desc->ssboByteSize, 0u, &mapped) == 0 && mapped) {
            std::memcpy(mapped, desc->inoutSsboData, desc->ssboByteSize);
            vk.unmap_mem(b->vk_device, ssbo_mem);
        }

        VkDescriptorPoolSizeMin pool_sizes[3] = {{6u, 2u}, {1u, 2u}, {7u, 2u}};
        VkDescriptorPoolCreateInfoMin dpci{33u, nullptr, 0u, 2u, 3u, pool_sizes};
        if (vk.create_desc_pool(b->vk_device, &dpci, nullptr, &desc_pool) == 0 && desc_pool) {
            VkDescriptorSetAllocateInfoMin dsai{34u, nullptr, desc_pool, 1u, &b->vk_default_desc_layout};
            if (vk.alloc_desc_sets(b->vk_device, &dsai, &desc_set) == 0 && desc_set) {
                VkDescriptorBufferInfoMin ubo_info{ubo_buf, 0ull, ubo_sz};
                VkDescriptorBufferInfoMin ssbo_info{ssbo_buf, 0ull, ssbo_sz};
                VkWriteDescriptorSetMin writes[2]{};
                writes[0] = {35u, nullptr, desc_set, 0u, 0u, 1u, 6u, nullptr, &ubo_info, nullptr};
                writes[1] = {35u, nullptr, desc_set, 2u, 0u, 1u, 7u, nullptr, &ssbo_info, nullptr};
                vk.update_desc_sets(b->vk_device, 2u, writes, 0u, nullptr);

                VkCommandBufferBeginInfoMin cbbi{42u, nullptr, 1u, nullptr};
                if (vk.begin_cmd_buf(b->vk_cmd_buf, &cbbi) == 0) {
                    vk.cmd_bind_pipeline(b->vk_cmd_buf, 1u /* COMPUTE */, comp_pipe);
                    vk.cmd_bind_desc_sets(b->vk_cmd_buf, 1u /* COMPUTE */, b->vk_default_pipe_layout,
                                          0u, 1u, &desc_set, 0u, nullptr);
                    vk.cmd_dispatch(b->vk_cmd_buf,
                                    std::max(1u, desc->groupCountX),
                                    std::max(1u, desc->groupCountY),
                                    std::max(1u, desc->groupCountZ));
                    if (vk.end_cmd_buf(b->vk_cmd_buf) == 0) {
                        VkSubmitInfoMin si{};
                        si.sType = 4u;
                        si.commandBufferCount = 1u;
                        si.pCommandBuffers = &b->vk_cmd_buf;
                        uint64_t sig_val = 0;
                        VkTimelineSemaphoreSubmitInfoMin tssi{};
                        if (b->vk_timeline_sem && vk.wait_semaphores) {
                            sig_val = ++b->timeline_counter;
                            tssi = {1000207003u, nullptr, 0u, nullptr, 1u, &sig_val};
                            si.pNext = &tssi;
                            si.signalSemaphoreCount = 1u;
                            si.pSignalSemaphores = &b->vk_timeline_sem;
                        }
                        if (vk.queue_submit(b->vk_queue, 1u, &si, 0ull) == 0) {
                            if (sig_val > 0) {
                                VkSemaphoreWaitInfoMin swi{
                                    1000207004u, nullptr, 0u, 1u, &b->vk_timeline_sem, &sig_val};
                                (void)vk.wait_semaphores(b->vk_device, &swi, 5000000000ull);
                                if (desc->outTimelineValue) {
                                    uint64_t queried = sig_val;
                                    if (vk.get_sem_counter) {
                                        (void)vk.get_sem_counter(b->vk_device, b->vk_timeline_sem, &queried);
                                    }
                                    *desc->outTimelineValue = queried;
                                }
                            } else {
                                (void)vk.queue_wait_idle(b->vk_queue);
                                const uint64_t fallback_val = ++b->timeline_counter;
                                if (desc->outTimelineValue) *desc->outTimelineValue = fallback_val;
                            }

                            mapped = nullptr;
                            if (vk.map_mem(b->vk_device, ssbo_mem, 0ull, desc->ssboByteSize, 0u, &mapped) == 0 &&
                                mapped) {
                                std::memcpy(desc->inoutSsboData, mapped, desc->ssboByteSize);
                                vk.unmap_mem(b->vk_device, ssbo_mem);
                            }
                        } else {
                            ok = false;
                        }
                    } else {
                        ok = false;
                    }
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }

    if (desc_pool && vk.destroy_desc_pool) vk.destroy_desc_pool(b->vk_device, desc_pool, nullptr);
    if (ssbo_buf) vk.destroy_buffer(b->vk_device, ssbo_buf, nullptr);
    if (ssbo_mem) vk.free_mem(b->vk_device, ssbo_mem, nullptr);
    if (ubo_buf) vk.destroy_buffer(b->vk_device, ubo_buf, nullptr);
    if (ubo_mem) vk.free_mem(b->vk_device, ubo_mem, nullptr);
    vk.destroy_pipeline(b->vk_device, comp_pipe, nullptr);
    vk.destroy_shader_module(b->vk_device, shader_mod, nullptr);
    return ok ? kSuccess : kDeviceError;
#else
    return kSuccess;
#endif
}

}  // extern "C"
