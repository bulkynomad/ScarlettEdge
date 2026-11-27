#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

struct XUserProfile {
    uint64_t xuid = 0;
    std::string gamertag;
    bool signed_in = false;
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

private:
    XGameRuntimeHle() = default;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    DeviceType device_type_ = DeviceType::XboxScarlettAnaconda;
    XUserProfile default_user_{};
};

}  // namespace scarlettx
