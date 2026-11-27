#include "scarlettx/kernel/xruntime.h"

namespace scarlettx {

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

}  // namespace scarlettx
