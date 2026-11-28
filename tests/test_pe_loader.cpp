#include <algorithm>
#include "sx_check.h"
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "scarlettx/loader/pe_loader.h"

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

int main() {
    scarlettx::DllRedirectionTable redirects;
    const std::filesystem::path rt_exe =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "rt_showcase" / "rt_showcase.exe";
    SX_REQUIRE(std::filesystem::exists(rt_exe));

    scarlettx::PeImageInfo info;
    auto st = scarlettx::InspectPeFile(rt_exe.string(), redirects, info);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(info.is_pe64);
    SX_REQUIRE(!info.sections.empty());
    SX_REQUIRE(!info.imports.empty());

    bool found_d3d12_xs = false;
    bool found_xgr = false;
    bool found_bvh = false;
    for (const auto& mod : info.imports) {
        const std::string name = ToLower(mod.dll_name);
        if (name == "d3d12_xs.dll") {
            found_d3d12_xs = true;
            SX_REQUIRE(mod.is_scarlett_hle);
            SX_REQUIRE(!mod.symbols.empty());
        } else if (name == "xgameruntime.dll") {
            found_xgr = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        } else if (name == "xboxbvhbuilder.dll") {
            found_bvh = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        }
    }

    SX_REQUIRE(found_d3d12_xs);
    SX_REQUIRE(found_xgr);
    SX_REQUIRE(found_bvh);

    // Verify full Scarlett redirection table coverage (including PIX and Phase 2+ shims).
    const auto check_redirect = [&](const char* guest, const char* expected_host) {
        std::string host;
        bool is_hle = false;
        SX_REQUIRE(redirects.Resolve(guest, host, is_hle));
        SX_REQUIRE(is_hle);
        SX_REQUIRE(host == expected_host);
    };
    check_redirect("pixevt.dll", "pixevt_stub.dll");
    check_redirect("kernelx.dll", "kernelx.dll");
    check_redirect("xvc_crypto.dll", "xvc_crypto.dll");
    check_redirect("xbox_codecs.dll", "xbox_codecs.dll");
    check_redirect("xsapi.dll", "xsapi.dll");
    check_redirect("xs2_lifting.dll", "xs2_lifting.dll");
    check_redirect("vulkan_backend.dll", "vulkan_backend.dll");

    // VerifyInspectPeBytes parity with InspectPeFile (used by zero-staging InspectPackage).
    {
        std::ifstream ifs(rt_exe, std::ios::binary);
        SX_REQUIRE(ifs.good());
        const std::vector<uint8_t> raw_pe(
            (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        scarlettx::PeImageInfo mem_info;
        SX_REQUIRE(
            scarlettx::InspectPeBytes(raw_pe, "G:\\rt_showcase.exe", redirects, mem_info).ok());
        SX_REQUIRE(mem_info.is_pe64);
        SX_REQUIRE(mem_info.sections.size() == info.sections.size());
        SX_REQUIRE(mem_info.imports.size() == info.imports.size());
    }

    // Verify Microsoft.Xbox.Services.*.GDK.C.Thunks.dll and unversioned c.thunks.dll
    // are classified as package-first fallback middleware (-> xsapi.dll) and NEVER
    // hijacked by system-shim-first routing.
    for (const char* thunks_dll : {
             "microsoft.xbox.services.140.gdk.c.thunks.dll",
             "microsoft.xbox.services.141.gdk.c.thunks.dll",
             "microsoft.xbox.services.142.gdk.c.thunks.dll",
             "microsoft.xbox.services.c.thunks.dll",
             "Microsoft.Xbox.Services.142.GDK.C.Thunks.dll",
             "Microsoft.Xbox.Services.C.Thunks.dll",
         }) {
        SX_REQUIRE(scarlettx::LookupSystemShimTarget(thunks_dll) == nullptr);
        const char* fb = scarlettx::LookupFallbackMiddlewareTarget(thunks_dll);
        SX_REQUIRE(fb != nullptr && std::string(fb) == "xsapi.dll");
        check_redirect(thunks_dll, "xsapi.dll");
    }
    SX_REQUIRE(scarlettx::LookupSystemShimTarget("xgameruntime.dll") != nullptr);
    SX_REQUIRE(std::string(scarlettx::LookupSystemShimTarget("xgameruntime.dll")) ==
               "xgameruntime.dll");

    std::cout << "[test_pe_loader] PASSED (imports=" << info.imports.size() << ")\n";
    return 0;
}
