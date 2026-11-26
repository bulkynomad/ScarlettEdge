#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

struct XUserProfile {
    uint64_t xuid = 0;
    std::string gamertag;
    bool signed_in = false;
};

class XTaskQueueHle {
public:
    void SubmitWork(std::function<void()> fn);
    void SubmitCompletion(std::function<void()> fn);
    size_t DispatchWork(size_t max_items = 0);
    size_t DispatchCompletion(size_t max_items = 0);
    size_t PendingWorkCount() const;
    size_t PendingCompletionCount() const;

private:
    mutable std::mutex mutex_;
    std::queue<std::function<void()>> work_queue_;
    std::queue<std::function<void()>> completion_queue_;
};

class XGameRuntimeHle {
public:
    static XGameRuntimeHle& Instance();

    Status Initialize(DeviceType device_type = DeviceType::XboxScarlettAnaconda);
    void Shutdown();
    bool IsInitialized() const noexcept;

    DeviceType GetDeviceType() const noexcept;
    std::string GetConsoleOsVersion() const;

    Status SignInDefaultUser(XUserProfile& out_user);
    bool GetDefaultUser(XUserProfile& out_user) const;

    XHandle CreateTaskQueue();
    XTaskQueueHle* GetTaskQueue(XHandle handle);
    void CloseTaskQueue(XHandle handle);

private:
    XGameRuntimeHle() = default;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    DeviceType device_type_ = DeviceType::XboxScarlettAnaconda;
    XUserProfile default_user_{};
    XHandle next_handle_ = 1;
    std::unordered_map<XHandle, std::unique_ptr<XTaskQueueHle>> queues_;
};

}  // namespace scarlettx
