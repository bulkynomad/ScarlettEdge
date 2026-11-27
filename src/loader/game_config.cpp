#include "scarlettx/loader/game_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace scarlettx {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

void AppendUtf8CodePoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string DecodeUtf16ToUtf8(const std::string& raw, size_t byte_offset, bool big_endian) {
    std::string out;
    out.reserve(raw.size() / 2);
    size_t i = byte_offset;
    auto read_u16 = [&](size_t off) -> uint16_t {
        const uint8_t b0 = static_cast<uint8_t>(raw[off]);
        const uint8_t b1 = static_cast<uint8_t>(raw[off + 1]);
        return big_endian ? static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8) | b1)
                          : static_cast<uint16_t>(b0 | (static_cast<uint16_t>(b1) << 8));
    };
    while (i + 1 < raw.size()) {
        const uint16_t w1 = read_u16(i);
        i += 2;
        if (w1 >= 0xD800u && w1 <= 0xDBFFu && i + 1 < raw.size()) {
            const uint16_t w2 = read_u16(i);
            if (w2 >= 0xDC00u && w2 <= 0xDFFFu) {
                i += 2;
                const uint32_t cp =
                    0x10000u + ((static_cast<uint32_t>(w1 - 0xD800u) << 10) |
                                static_cast<uint32_t>(w2 - 0xDC00u));
                AppendUtf8CodePoint(out, cp);
                continue;
            }
        }
        AppendUtf8CodePoint(out, w1);
    }
    return out;
}

std::string PreprocessXml(const std::string& raw) {
    std::string s;
    if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFFu &&
        static_cast<uint8_t>(raw[1]) == 0xFEu) {
        s = DecodeUtf16ToUtf8(raw, 2, false);
    } else if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFEu &&
               static_cast<uint8_t>(raw[1]) == 0xFFu) {
        s = DecodeUtf16ToUtf8(raw, 2, true);
    } else if (raw.size() >= 3 && static_cast<uint8_t>(raw[0]) == 0xEFu &&
               static_cast<uint8_t>(raw[1]) == 0xBBu &&
               static_cast<uint8_t>(raw[2]) == 0xBFu) {
        s = raw.substr(3);
    } else {
        s = raw;
    }

    size_t comment = 0;
    while ((comment = s.find("<!--", comment)) != std::string::npos) {
        const size_t close = s.find("-->", comment + 4);
        if (close == std::string::npos) {
            s.erase(comment);
            break;
        }
        s.erase(comment, (close + 3) - comment);
    }
    return s;
}

constexpr std::string_view kXmlWhitespace = " \t\r\n";

std::string DecodeAttributeValue(std::string_view value) {
    static constexpr std::array<std::pair<std::string_view, char>, 5> entities = {{
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    }};

    std::string decoded;
    decoded.reserve(value.size());
    while (!value.empty()) {
        if (value.front() == '&') {
            const auto entity = std::find_if(entities.begin(), entities.end(), [value](const auto& entry) {
                return value.starts_with(entry.first);
            });
            if (entity != entities.end()) {
                decoded.push_back(entity->second);
                value.remove_prefix(entity->first.size());
                continue;
            }
        }
        decoded.push_back(value.front());
        value.remove_prefix(1);
    }
    return decoded;
}

bool ExtractAttribute(std::string_view tag, std::string_view attr_name, std::string& out_value) {
    size_t pos = tag.find_first_of(kXmlWhitespace);
    while ((pos = tag.find_first_not_of(kXmlWhitespace, pos)) != std::string_view::npos) {
        const size_t name_end = tag.find_first_of(" \t\r\n=/>", pos);
        if (name_end == pos || name_end == std::string_view::npos) {
            return false;
        }

        const size_t equals = tag.find_first_not_of(kXmlWhitespace, name_end);
        if (equals == std::string_view::npos || tag[equals] != '=') {
            return false;
        }

        const size_t quote = tag.find_first_not_of(kXmlWhitespace, equals + 1);
        if (quote == std::string_view::npos || (tag[quote] != '"' && tag[quote] != '\'')) {
            return false;
        }
        const size_t value_end = tag.find(tag[quote], quote + 1);
        if (value_end == std::string_view::npos) {
            return false;
        }
        if (tag.substr(pos, name_end - pos) == attr_name) {
            out_value = DecodeAttributeValue(tag.substr(quote + 1, value_end - quote - 1));
            return true;
        }
        pos = value_end + 1;
    }
    return false;
}

size_t FindTagEnd(const std::string& xml, size_t pos) {
    while ((pos = xml.find_first_of(">\"'", pos)) != std::string::npos) {
        if (xml[pos] == '>') {
            return pos;
        }
        pos = xml.find(xml[pos], pos + 1);
        if (pos == std::string::npos) {
            break;
        }
        ++pos;
    }
    return std::string::npos;
}

bool ExtractTagBlock(const std::string& xml,
                     const std::string& element,
                     std::string& out_tag,
                     size_t search_from = 0,
                     size_t* out_next = nullptr) {
    const std::string open_prefix = "<" + element;
    size_t pos = xml.find(open_prefix, search_from);
    while (pos != std::string::npos) {
        const size_t after = pos + open_prefix.size();
        if (after < xml.size() &&
            (std::isspace(static_cast<unsigned char>(xml[after])) ||
             xml[after] == '>' || xml[after] == '/')) {
            const size_t gt = FindTagEnd(xml, after);
            if (gt == std::string::npos) {
                return false;
            }
            out_tag = xml.substr(pos, gt - pos + 1);
            if (out_next) {
                *out_next = gt + 1;
            }
            return true;
        }
        pos = xml.find(open_prefix, after);
    }
    return false;
}

bool ExtractElementText(const std::string& xml, const std::string& element, std::string& out_text) {
    size_t cursor = 0;
    std::string open_tag;
    while (ExtractTagBlock(xml, element, open_tag, cursor, &cursor)) {
        if (open_tag.size() >= 2 && open_tag[open_tag.size() - 2] == '/') {
            out_text.clear();
            return true;
        }
        const std::string close_prefix = "</" + element;
        size_t close = xml.find(close_prefix, cursor);
        while (close != std::string::npos) {
            const size_t after = close + close_prefix.size();
            if (after < xml.size() &&
                (xml[after] == '>' || std::isspace(static_cast<unsigned char>(xml[after])))) {
                out_text = DecodeAttributeValue(Trim(xml.substr(cursor, close - cursor)));
                return true;
            }
            close = xml.find(close_prefix, after);
        }
        return false;
    }
    return false;
}

}  // namespace

const GameExecutableEntry* GameConfig::PrimaryExecutable() const noexcept {
    const GameExecutableEntry* dev_gen9 = nullptr;
    for (const auto& exe : executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            if (!exe.is_dev_only) {
                return &exe;
            }
            if (!dev_gen9) {
                dev_gen9 = &exe;
            }
        }
    }
    if (dev_gen9) {
        return dev_gen9;
    }
    for (const auto& exe : executables) {
        if (!exe.is_dev_only) {
            return &exe;
        }
    }
    if (!executables.empty()) {
        return &executables.front();
    }
    return nullptr;
}

Status ParseGameConfigXml(const std::string& xml_content, GameConfig& out_config) {
    out_config = GameConfig{};
    const std::string clean_xml = PreprocessXml(xml_content);

    std::string game_tag;
    if (!ExtractTagBlock(clean_xml, "Game", game_tag)) {
        return Status::Error(StatusCode::ParseError,
                             "Missing <Game> root element in MicrosoftGame.Config");
    }
    ExtractAttribute(game_tag, "configVersion", out_config.config_version);

    std::string identity_tag;
    if (ExtractTagBlock(clean_xml, "Identity", identity_tag)) {
        ExtractAttribute(identity_tag, "Name", out_config.identity_name);
        ExtractAttribute(identity_tag, "Publisher", out_config.publisher);
        ExtractAttribute(identity_tag, "Version", out_config.version);
    }

    ExtractElementText(clean_xml, "TitleId", out_config.title_id);

    std::string shell_tag;
    if (ExtractTagBlock(clean_xml, "ShellVisuals", shell_tag)) {
        ExtractAttribute(shell_tag, "DefaultDisplayName", out_config.display_name);
    }

    size_t cursor = 0;
    std::string exe_tag;
    while (ExtractTagBlock(clean_xml, "Executable", exe_tag, cursor, &cursor)) {
        GameExecutableEntry entry;
        ExtractAttribute(exe_tag, "Name", entry.name);
        ExtractAttribute(exe_tag, "Id", entry.id);
        ExtractAttribute(exe_tag, "TargetDeviceFamilyForXboxOne", entry.target_device_family);
        if (entry.target_device_family.empty()) {
            ExtractAttribute(exe_tag, "TargetDeviceFamily", entry.target_device_family);
        }
        ExtractAttribute(exe_tag, "Alias", entry.alias);
        std::string dev_only;
        if (ExtractAttribute(exe_tag, "IsDevOnly", dev_only)) {
            const std::string norm_dev = ToLower(Trim(dev_only));
            entry.is_dev_only = (norm_dev == "true" || norm_dev == "1");
        }
        if (!entry.name.empty()) {
            out_config.executables.push_back(std::move(entry));
        }
    }

    // Determine ConsoleTarget from executable TargetDeviceFamily or package metadata.
    const std::string lower_xml = ToLower(clean_xml);
    bool has_gen9 = false;
    bool has_gen9_aware = false;
    bool has_xbox_one = false;

    for (const auto& exe : out_config.executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            has_gen9 = true;
        } else if (tdf == "xboxonegen9aware") {
            has_gen9_aware = true;
        } else if (tdf == "xboxone" || tdf == "durango") {
            has_xbox_one = true;
        }
    }

    if (has_gen9 || lower_xml.find("xboxgen9") != std::string::npos ||
        lower_xml.find("gaming.xbox.scarlett.x64") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxGen9Scarlett;
    } else if (has_gen9_aware || lower_xml.find("xboxonegen9aware") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOneGen9Aware;
    } else if (has_xbox_one || lower_xml.find("xboxone") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOne;
    }

    if (out_config.executables.empty()) {
        return Status::Error(StatusCode::ParseError,
                             "MicrosoftGame.Config contains no <Executable> entries");
    }

    return Status::Ok();
}

Status LoadGameConfigFile(const std::string& file_path, GameConfig& out_config) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open MicrosoftGame.Config: " + file_path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    Status st = ParseGameConfigXml(oss.str(), out_config);
    if (st.ok()) {
        out_config.source_path = file_path;
    }
    return st;
}

}  // namespace scarlettx
