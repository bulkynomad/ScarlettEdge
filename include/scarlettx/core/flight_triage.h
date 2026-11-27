#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct TriageImportGap {
    std::string symbol;
    uint64_t hits = 0;
    std::string first_caller;
};

struct TriageComQiGap {
    std::string interface_tag;
    std::string iid;
    uint64_t hits = 0;
    std::string first_caller;
};

struct TriageVtableGap {
    std::string interface_tag;
    int32_t slot = -1;
    std::string offset;
    uint64_t hits = 0;
    std::string first_caller;
};

struct TriageVfsGap {
    std::string path;
    std::string error;
    uint64_t hits = 0;
    std::string first_caller;
};

struct TriageCrashSummary {
    std::string file_name;
    std::string exception_code;
    std::string exception_name;
    std::string access_type;
    std::string fault_rip;
    std::string last_phase;
    std::vector<std::string> guest_frames;
};

struct FlightTriageReport {
    std::string directory;
    std::string verdict = "UNKNOWN";
    std::string title_id;
    std::string last_phase;
    uint64_t uptime_ms = 0;
    uint64_t total_events = 0;
    uint64_t heartbeats = 0;
    bool has_summary = false;

    std::vector<TriageImportGap> missing_imports;
    std::vector<TriageComQiGap> com_qi_misses;
    std::vector<TriageVtableGap> vtable_slot_misses;
    std::vector<TriageVfsGap> vfs_misses;
    std::vector<TriageCrashSummary> crashes;
    std::vector<std::string> hang_files;
    std::vector<std::string> dred_files;
    std::vector<std::string> shader_fail_files;
    std::vector<std::string> next_steps;
};

// Analyze a directory produced by `scarlettx boot --flight-recorder <dir>`.
Status TriageFlightDirectory(const std::string& directory,
                             FlightTriageReport& out_report);

// Format a human-readable ranked triage report.
std::string FormatFlightTriageText(const FlightTriageReport& report,
                                   bool emit_stubs = false);

// Format a machine-readable JSON triage report.
std::string FormatFlightTriageJson(const FlightTriageReport& report);

}  // namespace scarlettx
