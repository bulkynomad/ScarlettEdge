#include "scarlettx/kernel/xruntime.h"

namespace scarlettx {

void XTaskQueueHle::SubmitWork(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    work_queue_.push(std::move(fn));
}

void XTaskQueueHle::SubmitCompletion(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    completion_queue_.push(std::move(fn));
}

size_t XTaskQueueHle::DispatchWork(size_t max_items) {
    size_t count = 0;
    while (max_items == 0 || count < max_items) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (work_queue_.empty()) {
                break;
            }
            fn = std::move(work_queue_.front());
            work_queue_.pop();
        }
        if (fn) {
            fn();
        }
        ++count;
    }
    return count;
}

size_t XTaskQueueHle::DispatchCompletion(size_t max_items) {
    size_t count = 0;
    while (max_items == 0 || count < max_items) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completion_queue_.empty()) {
                break;
            }
            fn = std::move(completion_queue_.front());
            completion_queue_.pop();
        }
        if (fn) {
            fn();
        }
        ++count;
    }
    return count;
}

size_t XTaskQueueHle::PendingWorkCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return work_queue_.size();
}

size_t XTaskQueueHle::PendingCompletionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_queue_.size();
}

XGameRuntimeHle& XGameRuntimeHle::Instance() {
    static XGameRuntimeHle instance;
    return instance;
}

Status XGameRuntimeHle::Initialize(DeviceType device_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_type_ = device_type;
    initialized_ = true;
    default_user_.xuid = 0x00090000005CA001ull;
    default_user_.gamertag = "ScarlettXUser";
    default_user_.signed_in = true;
    return Status::Ok();
}

void XGameRuntimeHle::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.clear();
    default_user_ = XUserProfile{};
    initialized_ = false;
}

bool XGameRuntimeHle::IsInitialized() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

DeviceType XGameRuntimeHle::GetDeviceType() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_type_;
}

std::string XGameRuntimeHle::GetConsoleOsVersion() const {
    return "10.0.26100.2499 (ScarlettX-HLE)";
}

Status XGameRuntimeHle::SignInDefaultUser(XUserProfile& out_user) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Status::Error(StatusCode::DeviceError, "XGameRuntime not initialized");
    }
    default_user_.signed_in = true;
    out_user = default_user_;
    return Status::Ok();
}

bool XGameRuntimeHle::GetDefaultUser(XUserProfile& out_user) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !default_user_.signed_in) {
        return false;
    }
    out_user = default_user_;
    return true;
}

XHandle XGameRuntimeHle::CreateTaskQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    const XHandle h = next_handle_++;
    queues_[h] = std::make_unique<XTaskQueueHle>();
    return h;
}

XTaskQueueHle* XGameRuntimeHle::GetTaskQueue(XHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(handle);
    return it == queues_.end() ? nullptr : it->second.get();
}

void XGameRuntimeHle::CloseTaskQueue(XHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.erase(handle);
}

}  // namespace scarlettx
