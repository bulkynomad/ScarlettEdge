#pragma once

#include <cstdint>
#include <string>

#include "scarlettx/core/config.h"
#include "scarlettx/core/status.h"
#include "scarlettx/loader/game_config.h"
#include "scarlettx/loader/pe_loader.h"

namespace scarlettx {

// Async-signal-safe process-wide shutdown flag set by SetConsoleCtrlHandler.
void RequestAsyncShutdown(uint32_t ctrl_type = 0) noexcept;
bool IsAsyncShutdownRequested() noexcept;
uint32_t GetAsyncShutdownCtrlType() noexcept;
void ClearAsyncShutdownRequest() noexcept;

struct PackageInspectionReport {
    GameConfig config;
    PeImageInfo main_pe;
    std::string executable_path;
};

class Emulator {
public:
    explicit Emulator(EmulatorConfig config = {});

    Status InspectPackage(const std::string& package_dir, PackageInspectionReport& out_report) const;
    Status BootPackage(const std::string& package_dir);

private:
    EmulatorConfig config_;
    DllRedirectionTable redirects_;
};

}  // namespace scarlettx
