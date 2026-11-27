#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "scarlettx/core/flight_triage.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[test_triage_cli] FAILED at line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(x) Check((x), #x, __LINE__)

void WriteTextFile(const std::filesystem::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << content;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "scarlettx_test_triage_cli";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // Case 1: Clean run
    {
        const fs::path dir = base / "clean";
        fs::create_directories(dir, ec);
        WriteTextFile(dir / "flight_summary.json", R"({
  "report_type": "flight_summary",
  "title_id": "5CA01001",
  "uptime_ms": 1234,
  "last_phase": "shutdown",
  "total_events": 4,
  "heartbeats": 120,
  "missing_imports": [],
  "com_qi_misses": [],
  "vtable_slot_misses": [],
  "vfs_misses": []
})");
        scarlettx::FlightTriageReport rep;
        CHECK(scarlettx::TriageFlightDirectory(dir.string(), rep).ok());
        CHECK(rep.verdict == "CLEAN_RUN");
        CHECK(rep.title_id == "5CA01001");
        CHECK(rep.uptime_ms == 1234);
        CHECK(rep.heartbeats == 120);
        const std::string json = scarlettx::FormatFlightTriageJson(rep);
        CHECK(json.find("\"verdict\": \"CLEAN_RUN\"") != std::string::npos);
    }

    // Case 2: Null-vtable crash + ranked missing surfaces + --emit-stubs
    {
        const fs::path dir = base / "crash_and_gaps";
        fs::create_directories(dir, ec);
        WriteTextFile(dir / "flight_summary.json", R"({
  "report_type": "flight_summary",
  "title_id": "5CA02002",
  "uptime_ms": 512,
  "last_phase": "XTaskQueueCallback",
  "total_events": 19,
  "heartbeats": 12,
  "missing_imports": [
    {"symbol": "kernelx.dll!RareApi", "hits": 2, "first_caller": "game.exe+0x100"},
    {"symbol": "xgameruntime.dll!HotApi", "hits": 15, "first_caller": "game.exe+0x200"}
  ],
  "com_qi_misses": [
    {"interface": "ID3D12Device", "iid": "{11111111-2222-3333-4444-555555555555}", "hits": 7, "first_caller": "game.exe+0x300"}
  ],
  "vtable_slot_misses": [
    {"interface": "XDisplay", "slot": 5, "offset": "+0x28", "hits": 3, "first_caller": "game.exe+0x400"}
  ],
  "vfs_misses": [
    {"path": "G:\\missing.pak", "error": "0x00000002", "hits": 4, "first_caller": "game.exe+0x500"}
  ]
})");
        WriteTextFile(dir / "crash_report_1234_1.json", R"({
  "report_type": "crash_report",
  "exception_code": "0xC0000005",
  "exception_name": "EXCEPTION_ACCESS_VIOLATION",
  "access_type": "EXECUTE_DEP",
  "fault_rip": "0x0",
  "last_phase": "XTaskQueueCallback",
  "stack_frames": [
    {"index": 0, "is_guest": false, "formatted": "0x0"},
    {"index": 1, "is_guest": true, "formatted": "sample.exe+0x4159 [guest Ghidra=0x140004159]"}
  ]
})");
        scarlettx::FlightTriageReport rep;
        CHECK(scarlettx::TriageFlightDirectory(dir.string(), rep).ok());
        CHECK(rep.verdict == "CRASH_NULL_VTABLE");
        CHECK(rep.missing_imports.size() == 2);
        CHECK(rep.missing_imports[0].symbol == "xgameruntime.dll!HotApi");
        CHECK(rep.missing_imports[0].hits == 15);
        CHECK(rep.com_qi_misses.size() == 1);
        CHECK(rep.vtable_slot_misses.size() == 1);
        CHECK(rep.vtable_slot_misses[0].slot == 5);
        CHECK(rep.vfs_misses.size() == 1);
        CHECK(rep.crashes.size() == 1);
        CHECK(rep.crashes[0].guest_frames.size() == 1);

        const std::string text = scarlettx::FormatFlightTriageText(rep, true);
        CHECK(text.find("CRASH_NULL_VTABLE") != std::string::npos);
        CHECK(text.find("sample.exe+0x4159") != std::string::npos);
        CHECK(text.find("Stub_xgameruntime_dll_HotApi") != std::string::npos);
        CHECK(text.find("XDisplay_Slot5") != std::string::npos);
    }

    // Case 3: Missing directory returns error
    {
        scarlettx::FlightTriageReport rep;
        CHECK(!scarlettx::TriageFlightDirectory((base / "nonexistent").string(), rep).ok());
    }

    fs::remove_all(base, ec);

    if (g_failures != 0) {
        std::cerr << "[test_triage_cli] " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[test_triage_cli] PASS\n";
    return 0;
}
