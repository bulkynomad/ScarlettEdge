#pragma once

#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

struct GameExecutableEntry {
    std::string name;
    std::string id;
    std::string target_device_family;
    std::string alias;
    bool is_dev_only = false;
};

struct GameConfig {
    std::string identity_name;
    std::string publisher;
    std::string version;
    std::string title_id;
    std::string display_name;
    std::string config_version;
    ConsoleTarget console_target = ConsoleTarget::Unknown;
    std::vector<GameExecutableEntry> executables;
    std::string source_path;

    bool IsNativeScarlett() const noexcept {
        return console_target == ConsoleTarget::XboxGen9Scarlett;
    }

    const GameExecutableEntry* PrimaryExecutable() const noexcept;
};

Status ParseGameConfigXml(const std::string& xml_content, GameConfig& out_config);
Status LoadGameConfigFile(const std::string& file_path, GameConfig& out_config);

}  // namespace scarlettx
