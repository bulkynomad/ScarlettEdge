#include "scarlettx/core/flight_triage.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace scarlettx {

namespace {

bool ReadFileToString(const std::filesystem::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += ' ';
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

// Parse a JSON string literal starting at the opening quote `pos`.
// Advances `pos` past the closing quote.
bool ParseJsonString(const std::string& text, size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c == '\\' && pos < text.size()) {
            const char esc = text[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

void SkipWs(const std::string& text, size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
}

bool ExtractStringField(const std::string& obj, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    SkipWs(obj, pos);
    if (pos >= obj.size() || obj[pos] != ':') {
        return false;
    }
    ++pos;
    SkipWs(obj, pos);
    return ParseJsonString(obj, pos, out);
}

bool ExtractU64Field(const std::string& obj, const std::string& key, uint64_t& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    SkipWs(obj, pos);
    if (pos >= obj.size() || obj[pos] != ':') {
        return false;
    }
    ++pos;
    SkipWs(obj, pos);
    if (pos >= obj.size() || !std::isdigit(static_cast<unsigned char>(obj[pos]))) {
        return false;
    }
    char* endp = nullptr;
    out = std::strtoull(obj.c_str() + pos, &endp, 10);
    return endp != (obj.c_str() + pos);
}

bool ExtractI32Field(const std::string& obj, const std::string& key, int32_t& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    SkipWs(obj, pos);
    if (pos >= obj.size() || obj[pos] != ':') {
        return false;
    }
    ++pos;
    SkipWs(obj, pos);
    if (pos >= obj.size() ||
        (!std::isdigit(static_cast<unsigned char>(obj[pos])) && obj[pos] != '-')) {
        return false;
    }
    char* endp = nullptr;
    out = static_cast<int32_t>(std::strtol(obj.c_str() + pos, &endp, 10));
    return endp != (obj.c_str() + pos);
}

bool ExtractDoubleField(const std::string& obj, const std::string& key, double& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    SkipWs(obj, pos);
    if (pos >= obj.size() || obj[pos] != ':') {
        return false;
    }
    ++pos;
    SkipWs(obj, pos);
    if (pos >= obj.size() ||
        (!std::isdigit(static_cast<unsigned char>(obj[pos])) && obj[pos] != '-')) {
        return false;
    }
    char* endp = nullptr;
    out = std::strtod(obj.c_str() + pos, &endp);
    return endp != (obj.c_str() + pos);
}

bool ExtractBoolField(const std::string& obj, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    SkipWs(obj, pos);
    if (pos >= obj.size() || obj[pos] != ':') {
        return false;
    }
    ++pos;
    SkipWs(obj, pos);
    if (obj.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (obj.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

// Extract top-level JSON objects `{...}` inside a named array `"key": [ ... ]`.
std::vector<std::string> ExtractObjectArray(const std::string& text, const std::string& key) {
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return objects;
    }
    pos += needle.size();
    SkipWs(text, pos);
    if (pos >= text.size() || text[pos] != ':') {
        return objects;
    }
    ++pos;
    SkipWs(text, pos);
    if (pos >= text.size() || text[pos] != '[') {
        return objects;
    }
    ++pos;

    int bracket_depth = 1;
    bool in_str = false;
    while (pos < text.size() && bracket_depth > 0) {
        const char c = text[pos];
        if (in_str) {
            if (c == '\\' && pos + 1 < text.size()) {
                pos += 2;
                continue;
            }
            if (c == '"') {
                in_str = false;
            }
            ++pos;
            continue;
        }
        if (c == '"') {
            in_str = true;
            ++pos;
            continue;
        }
        if (c == '[') {
            ++bracket_depth;
            ++pos;
            continue;
        }
        if (c == ']') {
            --bracket_depth;
            ++pos;
            continue;
        }
        if (c == '{' && bracket_depth == 1) {
            const size_t start = pos;
            int brace_depth = 0;
            bool obj_str = false;
            while (pos < text.size()) {
                const char oc = text[pos];
                if (obj_str) {
                    if (oc == '\\' && pos + 1 < text.size()) {
                        pos += 2;
                        continue;
                    }
                    if (oc == '"') {
                        obj_str = false;
                    }
                } else {
                    if (oc == '"') {
                        obj_str = true;
                    } else if (oc == '{') {
                        ++brace_depth;
                    } else if (oc == '}') {
                        --brace_depth;
                        if (brace_depth == 0) {
                            ++pos;
                            objects.push_back(text.substr(start, pos - start));
                            break;
                        }
                    }
                }
                ++pos;
            }
            continue;
        }
        ++pos;
    }
    return objects;
}

bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t slen = std::char_traits<char>::length(suffix);
    return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
}

std::string SanitizeIdentifier(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

}  // namespace

Status TriageFlightDirectory(const std::string& directory, FlightTriageReport& out_report) {
    out_report = FlightTriageReport{};
    out_report.directory = directory;

    std::error_code ec;
    const std::filesystem::path dir_path(directory);
    if (!std::filesystem::exists(dir_path, ec) || !std::filesystem::is_directory(dir_path, ec)) {
        return Status::Error(StatusCode::NotFound,
                             "Flight recorder directory does not exist: " + directory);
    }

    // 1. Parse flight_summary.json if present
    const std::filesystem::path summary_path = dir_path / "flight_summary.json";
    std::string summary_text;
    if (ReadFileToString(summary_path, summary_text)) {
        out_report.has_summary = true;
        ExtractStringField(summary_text, "title_id", out_report.title_id);
        ExtractStringField(summary_text, "last_phase", out_report.last_phase);
        ExtractU64Field(summary_text, "uptime_ms", out_report.uptime_ms);
        ExtractU64Field(summary_text, "total_events", out_report.total_events);
        ExtractU64Field(summary_text, "heartbeats", out_report.heartbeats);
        ExtractU64Field(summary_text, "frames", out_report.frames_recorded);
        ExtractU64Field(summary_text, "stutters_gt_33ms", out_report.stutter_count);
        ExtractDoubleField(summary_text, "p95_ms", out_report.p95_ms);
        ExtractDoubleField(summary_text, "p99_ms", out_report.p99_ms);
        ExtractDoubleField(summary_text, "max_ms", out_report.max_ms);

        for (const auto& obj : ExtractObjectArray(summary_text, "missing_imports")) {
            TriageImportGap gap;
            ExtractStringField(obj, "symbol", gap.symbol);
            ExtractU64Field(obj, "hits", gap.hits);
            ExtractStringField(obj, "first_caller", gap.first_caller);
            if (!gap.symbol.empty()) {
                out_report.missing_imports.push_back(std::move(gap));
            }
        }
        for (const auto& obj : ExtractObjectArray(summary_text, "com_qi_misses")) {
            TriageComQiGap gap;
            ExtractStringField(obj, "interface", gap.interface_tag);
            ExtractStringField(obj, "iid", gap.iid);
            ExtractU64Field(obj, "hits", gap.hits);
            ExtractStringField(obj, "first_caller", gap.first_caller);
            if (!gap.iid.empty()) {
                out_report.com_qi_misses.push_back(std::move(gap));
            }
        }
        for (const auto& obj : ExtractObjectArray(summary_text, "vtable_slot_misses")) {
            TriageVtableGap gap;
            ExtractStringField(obj, "interface", gap.interface_tag);
            ExtractI32Field(obj, "slot", gap.slot);
            ExtractStringField(obj, "offset", gap.offset);
            ExtractU64Field(obj, "hits", gap.hits);
            ExtractStringField(obj, "first_caller", gap.first_caller);
            if (!gap.interface_tag.empty()) {
                out_report.vtable_slot_misses.push_back(std::move(gap));
            }
        }
        for (const auto& obj : ExtractObjectArray(summary_text, "vfs_misses")) {
            TriageVfsGap gap;
            ExtractStringField(obj, "path", gap.path);
            ExtractStringField(obj, "error", gap.error);
            ExtractU64Field(obj, "hits", gap.hits);
            ExtractStringField(obj, "first_caller", gap.first_caller);
            if (!gap.path.empty()) {
                out_report.vfs_misses.push_back(std::move(gap));
            }
        }
    }

    // 2. Enumerate crash_report_*.json, hang_snapshot_*.json, gpu_dred_*.json, shader_fail_*
    std::vector<std::filesystem::path> entries;
    for (const auto& de : std::filesystem::directory_iterator(dir_path, ec)) {
        if (de.is_regular_file(ec)) {
            entries.push_back(de.path());
        }
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& p : entries) {
        const std::string name = p.filename().string();
        if (StartsWith(name, "crash_report_") && EndsWith(name, ".json")) {
            std::string body;
            if (!ReadFileToString(p, body)) {
                continue;
            }
            TriageCrashSummary cs;
            cs.file_name = name;
            ExtractStringField(body, "exception_code", cs.exception_code);
            ExtractStringField(body, "exception_name", cs.exception_name);
            ExtractStringField(body, "access_type", cs.access_type);
            ExtractStringField(body, "fault_rip", cs.fault_rip);
            ExtractStringField(body, "last_phase", cs.last_phase);
            for (const auto& frame : ExtractObjectArray(body, "stack_frames")) {
                bool is_guest = false;
                if (ExtractBoolField(frame, "is_guest", is_guest) && is_guest) {
                    std::string formatted;
                    if (ExtractStringField(frame, "formatted", formatted) && !formatted.empty()) {
                        cs.guest_frames.push_back(std::move(formatted));
                    }
                }
            }
            out_report.crashes.push_back(std::move(cs));
        } else if (StartsWith(name, "hang_snapshot_") && EndsWith(name, ".json")) {
            out_report.hang_files.push_back(name);
        } else if (StartsWith(name, "gpu_dred_") && EndsWith(name, ".json")) {
            out_report.dred_files.push_back(name);
        } else if (StartsWith(name, "shader_fail_")) {
            out_report.shader_fail_files.push_back(name);
        }
    }

    if (!out_report.has_summary && out_report.crashes.empty() &&
        out_report.hang_files.empty() && out_report.dred_files.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "No flight recorder artifacts found in: " + directory);
    }

    // 3. Rank all gap tables by descending hit count
    std::sort(out_report.missing_imports.begin(), out_report.missing_imports.end(),
              [](const TriageImportGap& a, const TriageImportGap& b) {
                  return a.hits != b.hits ? a.hits > b.hits : a.symbol < b.symbol;
              });
    std::sort(out_report.com_qi_misses.begin(), out_report.com_qi_misses.end(),
              [](const TriageComQiGap& a, const TriageComQiGap& b) {
                  return a.hits != b.hits ? a.hits > b.hits : a.iid < b.iid;
              });
    std::sort(out_report.vtable_slot_misses.begin(), out_report.vtable_slot_misses.end(),
              [](const TriageVtableGap& a, const TriageVtableGap& b) {
                  return a.hits != b.hits ? a.hits > b.hits : a.slot < b.slot;
              });
    std::sort(out_report.vfs_misses.begin(), out_report.vfs_misses.end(),
              [](const TriageVfsGap& a, const TriageVfsGap& b) {
                  return a.hits != b.hits ? a.hits > b.hits : a.path < b.path;
              });

    // 4. Compute Verdict & Actionable Next Steps
    if (!out_report.crashes.empty()) {
        const auto& c = out_report.crashes.front();
        const bool is_null_call =
            (c.access_type == "EXECUTE_DEP" || c.access_type == "EXECUTE") &&
            (c.fault_rip == "0x0" || c.fault_rip == "0x0000000000000000");
        out_report.verdict = is_null_call ? "CRASH_NULL_VTABLE" : "CRASH_ACCESS_VIOLATION";
        if (is_null_call) {
            std::string hint = "Guest jumped to RIP=0x0 (likely an unpopulated COM/API vtable slot)";
            if (!c.guest_frames.empty()) {
                hint += "; inspect guest callsite " + c.guest_frames.front();
            }
            out_report.next_steps.push_back(std::move(hint));
        } else {
            std::string hint = "Investigate " + c.exception_name + " (" + c.exception_code +
                               ") at RIP=" + c.fault_rip;
            if (!c.guest_frames.empty()) {
                hint += " from " + c.guest_frames.front();
            }
            out_report.next_steps.push_back(std::move(hint));
        }
    } else if (!out_report.dred_files.empty()) {
        out_report.verdict = "GPU_DRED_FAULT";
        out_report.next_steps.push_back(
            "Inspect " + out_report.dred_files.front() +
            " for last completed GPU breadcrumb and page-fault VA.");
    } else if (!out_report.hang_files.empty()) {
        out_report.verdict = "WATCHDOG_HANG";
        out_report.next_steps.push_back(
            "Inspect " + out_report.hang_files.front() +
            " for deadlocked thread stacks (last_phase=" + out_report.last_phase + ").");
    } else if (!out_report.missing_imports.empty() || !out_report.com_qi_misses.empty() ||
               !out_report.vtable_slot_misses.empty() || !out_report.vfs_misses.empty() ||
               !out_report.shader_fail_files.empty()) {
        out_report.verdict = "MISSING_SURFACES";
    } else {
        out_report.verdict = "CLEAN_RUN";
        out_report.next_steps.push_back(
            "No crashes, hangs, DRED faults, or missing API/VFS surfaces recorded.");
    }

    if (!out_report.vtable_slot_misses.empty()) {
        const auto& v = out_report.vtable_slot_misses.front();
        out_report.next_steps.push_back("Implement " + v.interface_tag + " vtable slot " +
                                        std::to_string(v.slot) + " (" + v.offset + ", hits=" +
                                        std::to_string(v.hits) + ").");
    }
    if (!out_report.com_qi_misses.empty()) {
        const auto& q = out_report.com_qi_misses.front();
        out_report.next_steps.push_back("Wire COM QueryInterface IID " + q.iid + " on " +
                                        q.interface_tag + " (hits=" + std::to_string(q.hits) + ").");
    }
    if (!out_report.missing_imports.empty()) {
        const auto& m = out_report.missing_imports.front();
        out_report.next_steps.push_back("Implement missing import " + m.symbol +
                                        " (hits=" + std::to_string(m.hits) + ").");
    }
    if (!out_report.vfs_misses.empty()) {
        const auto& f = out_report.vfs_misses.front();
        out_report.next_steps.push_back("Resolve missing G:\\ asset " + f.path +
                                        " (error=" + f.error + ").");
    }

    return Status::Ok();
}

std::string FormatFlightTriageText(const FlightTriageReport& r, bool emit_stubs) {
    std::ostringstream os;
    os << "=== ScarlettEdge Flight-Recorder Triage ===\n"
       << "Directory:        " << r.directory << "\n"
       << "Verdict:          " << r.verdict << "\n";
    if (r.has_summary) {
        os << "Title ID:         " << (r.title_id.empty() ? "(unknown)" : r.title_id) << "\n"
           << "Uptime / Phase:   " << r.uptime_ms << " ms (" << r.last_phase << ")\n"
           << "Events / Beats:   " << r.total_events << " events, " << r.heartbeats << " heartbeats\n";
        if (r.frames_recorded > 0) {
            os << "Frame Pacing:     " << r.frames_recorded << " frames (p95=" << r.p95_ms
               << " ms, p99=" << r.p99_ms << " ms, max=" << r.max_ms
               << " ms, stutters=" << r.stutter_count << ")\n";
        }
    }
    os << "Artifacts:        " << r.crashes.size() << " crash(es), "
       << r.hang_files.size() << " hang(s), "
       << r.dred_files.size() << " DRED, "
       << r.shader_fail_files.size() << " shader fail(s)\n"
       << "Surface Gaps:     " << r.missing_imports.size() << " import(s), "
       << r.com_qi_misses.size() << " COM QI, "
       << r.vtable_slot_misses.size() << " vtable slot(s), "
       << r.vfs_misses.size() << " VFS miss(es)\n";

    if (!r.crashes.empty()) {
        os << "\n[Fatal Crashes]\n";
        for (const auto& c : r.crashes) {
            os << "  - " << c.file_name << ": " << c.exception_name << " (" << c.exception_code
               << ", " << c.access_type << ", RIP=" << c.fault_rip
               << ", phase=" << c.last_phase << ")\n";
            const size_t max_frames = std::min<size_t>(c.guest_frames.size(), 5);
            for (size_t i = 0; i < max_frames; ++i) {
                os << "      guest[" << i << "] " << c.guest_frames[i] << "\n";
            }
        }
    }

    if (!r.vtable_slot_misses.empty()) {
        os << "\n[Ranked Vtable Slot Misses]\n";
        for (const auto& v : r.vtable_slot_misses) {
            os << "  - " << v.interface_tag << " slot " << v.slot << " (" << v.offset
               << ") hits=" << v.hits << " caller=" << v.first_caller << "\n";
        }
    }

    if (!r.com_qi_misses.empty()) {
        os << "\n[Ranked COM QueryInterface Misses]\n";
        for (const auto& q : r.com_qi_misses) {
            os << "  - " << q.interface_tag << " IID=" << q.iid
               << " hits=" << q.hits << " caller=" << q.first_caller << "\n";
        }
    }

    if (!r.missing_imports.empty()) {
        os << "\n[Ranked Missing Imports]\n";
        for (const auto& m : r.missing_imports) {
            os << "  - " << m.symbol << " hits=" << m.hits
               << " caller=" << m.first_caller << "\n";
        }
    }

    if (!r.vfs_misses.empty()) {
        os << "\n[Ranked VFS Misses]\n";
        for (const auto& f : r.vfs_misses) {
            os << "  - " << f.path << " err=" << f.error << " hits=" << f.hits << "\n";
        }
    }

    if (!r.next_steps.empty()) {
        os << "\n[Recommended Next Steps]\n";
        for (size_t i = 0; i < r.next_steps.size(); ++i) {
            os << "  " << (i + 1) << ". " << r.next_steps[i] << "\n";
        }
    }

    if (emit_stubs && (!r.missing_imports.empty() || !r.vtable_slot_misses.empty() ||
                       !r.com_qi_misses.empty())) {
        os << "\n// === Clean-Room Stub Skeletons (--emit-stubs) ===\n";
        for (const auto& m : r.missing_imports) {
            const std::string fn = SanitizeIdentifier(m.symbol);
            os << "extern \"C\" HRESULT WINAPI Stub_" << fn << "() {\n"
               << "    // TODO: implement clean-room behavior for " << m.symbol << "\n"
               << "    return E_NOTIMPL;\n"
               << "}\n";
        }
        for (const auto& v : r.vtable_slot_misses) {
            const std::string fn = SanitizeIdentifier(v.interface_tag) + "_Slot" + std::to_string(v.slot);
            os << "static HRESULT WINAPI " << fn << "(void* self) {\n"
               << "    (void)self;\n"
               << "    // TODO: implement " << v.interface_tag << " slot " << v.slot
               << " (" << v.offset << ")\n"
               << "    return E_NOTIMPL;\n"
               << "}\n";
        }
        for (const auto& q : r.com_qi_misses) {
            os << "// QueryInterface case for " << q.interface_tag << " IID " << q.iid << "\n";
        }
    }

    return os.str();
}

std::string FormatFlightTriageJson(const FlightTriageReport& r) {
    std::ostringstream os;
    os << "{\n"
       << "  \"verdict\": \"" << JsonEscape(r.verdict) << "\",\n"
       << "  \"directory\": \"" << JsonEscape(r.directory) << "\",\n"
       << "  \"title_id\": \"" << JsonEscape(r.title_id) << "\",\n"
       << "  \"uptime_ms\": " << r.uptime_ms << ",\n"
       << "  \"last_phase\": \"" << JsonEscape(r.last_phase) << "\",\n"
       << "  \"counts\": {\n"
       << "    \"crashes\": " << r.crashes.size() << ",\n"
       << "    \"hangs\": " << r.hang_files.size() << ",\n"
       << "    \"dred\": " << r.dred_files.size() << ",\n"
       << "    \"shader_fails\": " << r.shader_fail_files.size() << ",\n"
       << "    \"missing_imports\": " << r.missing_imports.size() << ",\n"
       << "    \"com_qi_misses\": " << r.com_qi_misses.size() << ",\n"
       << "    \"vtable_slot_misses\": " << r.vtable_slot_misses.size() << ",\n"
       << "    \"vfs_misses\": " << r.vfs_misses.size() << "\n"
       << "  },\n"
       << "  \"next_steps\": [";
    for (size_t i = 0; i < r.next_steps.size(); ++i) {
        if (i > 0) os << ", ";
        os << "\"" << JsonEscape(r.next_steps[i]) << "\"";
    }
    os << "]\n}\n";
    return os.str();
}

}  // namespace scarlettx
