#pragma once

#include <string>

#include "scarlettx/core/config.h"
#include "scarlettx/core/status.h"
#include "scarlettx/loader/game_config.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/pe_loader.h"

namespace scarlettx {

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
