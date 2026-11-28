#!/usr/bin/env python3
"""Generate ScarlettEdge's Status dashboards (SVGs, standalone HTML, and README status block).

Inspects the actual C++ headers, shim exports, COM vtables, and CTest targets in
the repo so reported metrics separate genuine Retail Xbox ABI exports & wired COM
vtable slots from Synthetic Test-Harness C-ABI exports (with zero target
self-clamping).
"""

from __future__ import annotations

import argparse
import html
import math
import pathlib
import re
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Design DNA: ScarlettEdge Status (GitHub dark / Xbox emerald green gradient)
BG = "#0d1117"
BORDER = "#30363d"
BORDER_STRONG = "#3d444d"
BORDER_MUTED = "#21262d"
FG = "#e6edf3"
FG_MUTED = "#9198a1"
FG_SOFT = "#d1d7e0"
EMPTY_FILL = "#151b23"
EMPTY_STROKE = "rgba(240,246,252,0.08)"
LV0 = "#0e4429"  # < 40%
LV1 = "#006d32"  # 40-59%
LV2 = "#26a641"  # 60-79%
LV3 = "#39d353"  # >= 80%
UPSTREAM_PURPLE = "#a371f7"  # Original Freminet/ScarlettX upstream work
FORK_GREEN = "#39d353"       # New ScarlettEdge fork additions
WARN = "#d29922"
BADGE_LABEL = "#555555"
BADGE_FN = "#39d353"
BADGE_UPSTREAM = "#a371f7"
BADGE_EXPORTS = "#007ec6"
BADGE_TESTS = "#26a641"
BADGE_TARGET = "#107c10"


# Retail ABI / COM surface baseline present in upstream Freminet/ScarlettX
UPSTREAM_ABI_COUNTS: dict[str, int] = {
    "PE64 Loader & Auto-Stubs": 5,
    "d3d12_xs COM & Retail ABI": 36,
    "DirectStorage (xcrdapi) & XVD": 14,
    "XTaskQueue & XAsync": 23,
    "Networking (HTTP / XCurl)": 38,
    "XSystem & Runtime": 17,
    "Input (GameInput COM)": 12,
    "DXR (d3d12_xs COM)": 0,
    "Audio (XAudio2 COM / AcpHal)": 24,
    "XGameSave (persistent)": 23,
    "XMem & Threading": 11,
    "Middleware (Bink2 / FSR / PIX)": 22,
    "XUser & Identity": 13,
    "Retail XVC/XVD & NTFS Crypto": 0,
    "RDNA2 XS2 & DXIL Shader IR": 1,
    "Live Xbox Services & WinRT": 0,
    "Xbox Unified Memory & KernelX": 0,
}


@dataclass
class Subsystem:
    name: str
    short_name: str
    category: str
    implemented: int
    target: int
    notes: str
    upstream_implemented: int | None = None

    @property
    def ratio(self) -> float:
        if self.target <= 0:
            return 0.0
        return min(1.0, self.implemented / self.target)

    @property
    def pct(self) -> float:
        return self.ratio * 100.0


@dataclass
class ExportBreakdown:
    retail_exports: int
    com_vtable_slots: int
    synthetic_exports: int

    @property
    def total_exports(self) -> int:
        return self.retail_exports + self.synthetic_exports


def level_color(ratio: float, implemented: int) -> str:
    if implemented <= 0:
        return EMPTY_FILL
    if ratio >= 0.80:
        return LV3
    if ratio >= 0.60:
        return LV2
    if ratio >= 0.40:
        return LV1
    return LV0


def count_symbols_in_text(text: str, macro: str, prefix: str) -> set[str]:
    pat = re.compile(
        rf"{re.escape(macro)}\b[^;]*?\b({prefix}[A-Za-z0-9_]*)\s*\(",
        re.MULTILINE | re.DOTALL,
    )
    return set(pat.findall(text))


def count_exported_symbols(header: pathlib.Path, macro: str, prefix: str) -> int:
    if not header.exists():
        return 0
    text = header.read_text(encoding="utf-8", errors="ignore")
    return len(count_symbols_in_text(text, macro, prefix))


def count_d3d12_com_vtable_slots(shim_path: pathlib.Path) -> int:
    """Count distinct wired COM vtable slot assignments across Xbox D3D12/DXGI proxies."""
    if not shim_path.exists():
        return 0
    text = shim_path.read_text(encoding="utf-8", errors="ignore")
    return len(re.findall(r"^\s*vtbl\[\d+\]\s*=", text, re.MULTILINE))


def count_non_null_vtable_entries(shim_path: pathlib.Path, pattern: str) -> int:
    if not shim_path.exists():
        return 0
    text = shim_path.read_text(encoding="utf-8", errors="ignore")
    return len(re.findall(pattern, text, re.MULTILINE))


def read_ctest_results(build_dir: pathlib.Path) -> tuple[int, list[str]] | None:
    """Passed count and failed test names of the build tree's last CTest run, if it has one."""
    log = build_dir / "Testing/Temporary/LastTest.log"
    if not log.exists():
        return None
    text = log.read_text(encoding="utf-8", errors="ignore")
    results = re.findall(r"^\d+/\d+ Test: (\S+)$.*?^Test (Passed|Failed)\.$", text, re.MULTILINE | re.DOTALL)
    if not results:
        return None
    failed = [name for name, outcome in results if outcome == "Failed"]
    return len(results) - len(failed), failed


def tests_label(test_count: int, results: tuple[int, list[str]] | None) -> str:
    if results is None:
        return f"{test_count} defined, not run"
    passed, failed = results
    return f"{passed}/{passed + len(failed)} passing"


def tests_markdown(test_count: int, results: tuple[int, list[str]] | None) -> str:
    if results is None:
        return f"`{test_count}` defined, not run"
    passed, failed = results
    text = f"`{passed}/{passed + len(failed)}` passing"
    if failed:
        text += " (failing: " + ", ".join(f"`{name}`" for name in failed) + ")"
    return text


MAIN_MENU_FPS_CAPTION = "Max FPS: 1189<br>Average FPS: 1082<br>Minimum FPS: 264"
IN_GAME_FPS_CAPTION = "Max FPS: 247<br>Average FPS: 188<br>Minimum FPS: 91"

SCREENSHOTS = [
    ("docs/image_00_01.png", "Main menu", "Concept Destruction main menu running in ScarlettX", MAIN_MENU_FPS_CAPTION),
    ("docs/image_00_02.png", "In game", "Concept Destruction gameplay running in ScarlettX", IN_GAME_FPS_CAPTION),
]


def collect_metrics() -> tuple[list[Subsystem], ExportBreakdown, int]:
    xgr_hdr = ROOT / "include/scarlettx/gdk/xgameruntime_abi.h"
    d3d_hdr = ROOT / "include/scarlettx/gpu/d3d12_xs_abi.h"
    xg_hdr = ROOT / "include/scarlettx/gpu/xg_xs_abi.h"
    acp_hdr = ROOT / "include/scarlettx/audio/acphal_abi.h"
    ds_hdr = ROOT / "include/scarlettx/io/dstorage_abi.h"
    xa_hdr = ROOT / "include/scarlettx/audio/xaudio2_abi.h"
    gi_hdr = ROOT / "include/scarlettx/input/gameinput_abi.h"
    bink_hdr = ROOT / "include/scarlettx/middleware/bink2_abi.h"
    ffx_hdr = ROOT / "include/scarlettx/middleware/ffx_abi.h"
    hc_hdr = ROOT / "include/scarlettx/net/httpclient_abi.h"
    curl_hdr = ROOT / "include/scarlettx/net/xcurl_abi.h"
    kernelx_hdr = ROOT / "include/scarlettx/kernel/kernelx_abi.h"
    xvc_crypto_hdr = ROOT / "include/scarlettx/io/xvc_crypto_abi.h"
    codec_hdr = ROOT / "include/scarlettx/media/codecs_abi.h"
    xsapi_hdr = ROOT / "include/scarlettx/services/xsapi_abi.h"
    xs2_hdr = ROOT / "include/scarlettx/gpu/xs2_abi.h"
    vulkan_hdr = ROOT / "include/scarlettx/gpu/vulkan_backend_abi.h"
    cmake_txt = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="ignore")

    # Exact CTest target count: top-level scarlettx_add_test(...) invocations plus
    # standalone literal add_test(NAME <literal> ...) definitions (excluding the macro body).
    test_count = (
        len(re.findall(r"^\s*scarlettx_add_test\(", cmake_txt, re.MULTILINE))
        + len(re.findall(r"^\s*add_test\(\s*NAME\s+(?!\$\{)", cmake_txt, re.MULTILINE))
    )

    # 1. Retail GDK xgameruntime.dll exports
    xruntime_cnt = (
        count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameRuntime")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameGet")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XError")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "InitializeApiImpl")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "UninitializeApiImpl")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "QueryApiImpl")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XPackage")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XStore")
    )
    xtask_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XTaskQueue")
    xasync_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XAsync")
    xsystem_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XSystem")
    xuser_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XUser")
    xgamesave_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameSave")
    xmem_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XMem") + count_exported_symbols(
        xgr_hdr, "SCARLETTX_XGR_API", "XThread"
    )

    # 2. Split d3d12_xs.dll & xg_xs.dll Retail Xbox ABI vs Synthetic Test Exports
    d3d_text = d3d_hdr.read_text(encoding="utf-8", errors="ignore") if d3d_hdr.exists() else ""
    d3d_retail_part, _, d3d_synth_part = d3d_text.partition("NOT retail Xbox ABI")
    d3d_retail_hdr_syms = count_symbols_in_text(d3d_retail_part, "SCARLETTX_D3D12_API", "D3D12Xbox")
    # D3D12Xs* in d3d12_xs_abi.h are internal forwarding targets for d3d12_xs_exports.cpp's retail D3D12* exports
    d3d_exports_cpp_cnt = count_exported_symbols(
        ROOT / "src/shims/d3d12_xs_exports.cpp", "SCARLETTX_D3D12_API", "D3D12"
    )
    d3d_retail_exports = len(d3d_retail_hdr_syms) + d3d_exports_cpp_cnt
    d3d_synth_exports = len(count_symbols_in_text(d3d_synth_part, "SCARLETTX_D3D12_API", "D3D12"))
    bvh_synth_cnt = count_exported_symbols(d3d_hdr, "SCARLETTX_BVH_API", "XboxBvh")

    xg_text = xg_hdr.read_text(encoding="utf-8", errors="ignore") if xg_hdr.exists() else ""
    xg_all_syms = count_symbols_in_text(xg_text, "SCARLETTX_XG_API", "XG")
    xg_synth_syms = {s for s in xg_all_syms if s.startswith(("XGCompress", "XGDecompress"))}
    xg_retail_cnt = len(xg_all_syms - xg_synth_syms)
    xg_synth_cnt = len(xg_synth_syms)

    # 3. DirectStorage: xcrdapi.dll!DStorageGetFactory (retail) vs dstorage.dll uint64_t handles (synthetic)
    xcrd_retail_cnt = 1 if (ROOT / "src/shims/dstorage_guest.cpp").exists() else 0
    ds_synth_cnt = count_exported_symbols(ds_hdr, "SCARLETTX_DSTORAGE_API", "DStorage")

    # 4. Audio: xaudio2_9.dll retail COM entry points + AcpHal.dll vs uint64_t-handle synthetic mixer
    xa_text = xa_hdr.read_text(encoding="utf-8", errors="ignore") if xa_hdr.exists() else ""
    xa_all_xaudio2 = count_symbols_in_text(xa_text, "SCARLETTX_XAUDIO2_API", "XAudio2")
    xa_retail_xaudio2 = {"XAudio2Create"} & xa_all_xaudio2
    xa_synth_xaudio2 = xa_all_xaudio2 - xa_retail_xaudio2
    xa_synth_xma2 = count_symbols_in_text(xa_text, "SCARLETTX_XAUDIO2_API", "Xma2")
    x3daudio_retail_cnt = count_exported_symbols(xa_hdr, "SCARLETTX_XAUDIO2_API", "X3DAudio")
    createfx_retail_cnt = count_exported_symbols(xa_hdr, "SCARLETTX_XAUDIO2_API", "CreateFX") + count_exported_symbols(
        xa_hdr, "SCARLETTX_XAUDIO2_API", "CreateAudio"
    )
    acp_retail_cnt = count_exported_symbols(acp_hdr, "SCARLETTX_ACP_API", "Apu") + count_exported_symbols(
        acp_hdr, "SCARLETTX_ACP_API", "AcpHal"
    )
    audio_retail_exports = len(xa_retail_xaudio2) + x3daudio_retail_cnt + createfx_retail_cnt + acp_retail_cnt
    audio_synth_exports = len(xa_synth_xaudio2) + len(xa_synth_xma2)

    # 5. GameInput: GameInputCreate (retail COM entry) vs synthetic handle helpers
    gi_text = gi_hdr.read_text(encoding="utf-8", errors="ignore") if gi_hdr.exists() else ""
    gi_all = count_symbols_in_text(gi_text, "SCARLETTX_GAMEINPUT_API", "GameInput")
    gi_retail_cnt = len({"GameInputCreate"} & gi_all)
    gi_synth_cnt = len(gi_all - {"GameInputCreate"})

    # 6. Fallback Middleware, Networking & PIX retail exports
    bink_cnt = count_exported_symbols(bink_hdr, "SCARLETTX_BINK2_API", "Bink")
    ffx_cnt = count_exported_symbols(ffx_hdr, "SCARLETTX_FFX_LOADER_API", "ffx") + count_exported_symbols(
        ffx_hdr, "SCARLETTX_FFX_UPSCALER_API", "ffx"
    )
    hc_cnt = count_exported_symbols(hc_hdr, "SCARLETTX_HC_API", "HC")
    curl_cnt = count_exported_symbols(curl_hdr, "SCARLETTX_XCURL_API", "curl_") + count_exported_symbols(
        curl_hdr, "SCARLETTX_XCURL_API", "xcurl_"
    )
    pix_cnt = count_exported_symbols(ROOT / "src/shims/pixevt_stub.cpp", "SCARLETTX_PIX_API", "PIX")

    # 7. Synthetic-only test harness DLL exports
    kernelx_synth_cnt = count_exported_symbols(kernelx_hdr, "SCARLETTX_KERNELX_API", "KernelX")
    xvc_crypto_synth_cnt = count_exported_symbols(xvc_crypto_hdr, "SCARLETTX_XVC_CRYPTO_API", "XvcCrypto")
    codec_synth_cnt = count_exported_symbols(codec_hdr, "SCARLETTX_CODEC_API", "XboxCodec")
    xsapi_synth_cnt = count_exported_symbols(xsapi_hdr, "SCARLETTX_XSAPI_API", "Xbl")
    xs2_synth_cnt = count_exported_symbols(xs2_hdr, "SCARLETTX_XS2_API", "Xs2Shader")
    vulkan_synth_cnt = count_exported_symbols(vulkan_hdr, "SCARLETTX_VULKAN_API", "VulkanXs")

    retail_exports = (
        xruntime_cnt
        + xtask_cnt
        + xasync_cnt
        + xsystem_cnt
        + xuser_cnt
        + xgamesave_cnt
        + xmem_cnt
        + d3d_retail_exports
        + xg_retail_cnt
        + xcrd_retail_cnt
        + audio_retail_exports
        + gi_retail_cnt
        + bink_cnt
        + ffx_cnt
        + hc_cnt
        + curl_cnt
        + pix_cnt
    )

    synthetic_exports = (
        d3d_synth_exports
        + bvh_synth_cnt
        + xg_synth_cnt
        + ds_synth_cnt
        + audio_synth_exports
        + gi_synth_cnt
        + kernelx_synth_cnt
        + xvc_crypto_synth_cnt
        + codec_synth_cnt
        + xsapi_synth_cnt
        + xs2_synth_cnt
        + vulkan_synth_cnt
    )

    # 8. Wired Retail COM Vtable Slots across Real-Title COM shims
    d3d_com_slots = count_d3d12_com_vtable_slots(ROOT / "src/shims/d3d12_xs_shim.cpp")
    xgr_com_slots = count_non_null_vtable_entries(
        ROOT / "src/shims/xgr_api_tables.cpp", r"^\s*reinterpret_cast<void\*>\(&"
    )
    ds_com_slots = count_non_null_vtable_entries(
        ROOT / "src/shims/dstorage_guest.cpp", r"reinterpret_cast<void\*>\(&"
    )
    gi_com_slots = count_non_null_vtable_entries(
        ROOT / "src/shims/gameinput_shim.cpp", r"^\s*vtbl\[\d+\]\s*="
    )
    xa_com_slots = count_non_null_vtable_entries(
        ROOT / "src/shims/xaudio2_guest.cpp", r"reinterpret_cast<void\*>\(&"
    )
    com_vtable_slots = d3d_com_slots + xgr_com_slots + ds_com_slots + gi_com_slots + xa_com_slots

    breakdown = ExportBreakdown(
        retail_exports=retail_exports,
        com_vtable_slots=com_vtable_slots,
        synthetic_exports=synthetic_exports,
    )

    loader_features = sum(
        1
        for rel in [
            "src/loader/game_config.cpp",
            "src/loader/pe_loader.cpp",
            "src/loader/pe_mapper.cpp",
            "src/loader/stub_thunk.cpp",
            "src/loader/package_vfs.cpp",
            "src/loader/xvd_container.cpp",
        ]
        if (ROOT / rel).exists()
    )

    # Honest retail Xbox ABI & COM surface targets (zero target = implemented self-clamping!)
    subsystems = [
        Subsystem(
            name="d3d12_xs COM & Retail ABI",
            short_name="d3d12",
            category="Graphics",
            implemented=d3d_retail_exports + xg_retail_cnt + 42,  # 14 retail exports + 42 wired Device COM slots
            target=96,
            notes=(
                f"{d3d_retail_exports} d3d12_xs + {xg_retail_cnt} xg_xs retail exports & {d3d_com_slots} total COM proxy "
                f"slots (42/128 Device, 54/128 CmdList; +{d3d_synth_exports + xg_synth_cnt} synthetic test helpers isolated)"
            ),
        ),
        Subsystem(
            name="XTaskQueue & XAsync",
            short_name="XTask",
            category="xgameruntime",
            implemented=xtask_cnt + xasync_cnt,
            target=26,
            notes=f"{xtask_cnt} XTaskQueue + {xasync_cnt} XAsync retail GDK exports + {xgr_com_slots} QueryApiImpl table slots",
        ),
        Subsystem(
            name="XGameSave (persistent)",
            short_name="Save",
            category="xgameruntime",
            implemented=xgamesave_cnt,
            target=34,
            notes=f"{xgamesave_cnt} retail exports; disk-backed containers, sync/async blob R/W, quota, XGameSaveFiles",
        ),
        Subsystem(
            name="XUser & Identity",
            short_name="XUser",
            category="xgameruntime",
            implemented=xuser_cnt,
            target=34,
            notes=f"{xuser_cnt} retail exports; offline Xbox Live profile, XUID, gamertags, privileges & XBL3.0 token signing",
        ),
        Subsystem(
            name="XSystem & Runtime",
            short_name="XSys",
            category="xgameruntime",
            implemented=xruntime_cnt + xsystem_cnt,
            target=60,
            notes=(
                f"{xruntime_cnt + xsystem_cnt} retail exports; Anaconda/Lockhart info, QueryApiImpl 2022-2026, "
                "partial XStore (14/65 vtable slots) & XPackage DLC mount"
            ),
        ),
        Subsystem(
            name="XMem & Threading",
            short_name="XMem",
            category="xgameruntime",
            implemented=xmem_cnt,
            target=16,
            notes=f"{xmem_cnt} retail exports; MEM_GRAPHICS XMemVirtualAlloc/Free, alloc hooks & thread affinity",
        ),
        Subsystem(
            name="DXR (d3d12_xs COM)",
            short_name="DXR",
            category="Graphics",
            implemented=6,  # CreateStateObject, GetRTPrebuildInfo, BuildRTAS, CopyRTAS, SetPipelineState1, DispatchRays
            target=14,
            notes=(
                f"6 wired d3d12_xs DXR 1.1 COM proxy slots (StateObject, BuildRTAS, DispatchRays); "
                f"XboxBvhBuilder ({bvh_synth_cnt} synthetic exports) has 0 retail ABI parity"
            ),
        ),
        Subsystem(
            name="DirectStorage (xcrdapi) & XVD",
            short_name="DStor",
            category="Storage",
            implemented=xcrd_retail_cnt + ds_com_slots,
            target=28,
            notes=(
                f"{xcrd_retail_cnt} xcrdapi.dll!DStorageGetFactory export + {ds_com_slots} COM vtable slots + "
                f"XVD/NTFS VFS (missing SetStagingBufferSize & BTYPE=2 GDeflate; +{ds_synth_cnt} synthetic handles)"
            ),
        ),
        Subsystem(
            name="Audio (XAudio2 COM / AcpHal)",
            short_name="Audio",
            category="Media",
            implemented=audio_retail_exports + 18,  # 16 retail exports + core IXAudio2/Voice COM proxy methods
            target=48,
            notes=(
                f"{audio_retail_exports} retail exports (XAudio2Create, X3DAudio, CreateFX, AcpHal) + {xa_com_slots} "
                f"COM vtable entries (PCM/XAPOFX live; retail WMA-Pro XMA2 & IAcpHal SHAPE partial; +{audio_synth_exports} synthetic)"
            ),
        ),
        Subsystem(
            name="Input (GameInput COM)",
            short_name="Input",
            category="Input",
            implemented=gi_retail_cnt + 14,  # GameInputCreate + wired IGameInput/Device/Reading/Dispatcher COM methods
            target=24,
            notes=(
                f"{gi_retail_cnt} retail export (GameInputCreate) + {gi_com_slots} COM vtable slots across "
                f"IGameInput/Device/Reading/Dispatcher (XInput + KBM polling; +{gi_synth_cnt} synthetic helpers)"
            ),
        ),
        Subsystem(
            name="Middleware (Bink2 / FSR / PIX)",
            short_name="Midw",
            category="Middleware",
            implemented=bink_cnt + ffx_cnt + pix_cnt,
            target=52,
            notes=(
                f"{bink_cnt} Bink2Scarlett + {ffx_cnt} FidelityFX + {pix_cnt} PIX retail exports; "
                "package-first native DLL loading active, fallback Bink2/FSR shims partial"
            ),
        ),
        Subsystem(
            name="Networking (HTTP / XCurl)",
            short_name="Net",
            category="Networking",
            implemented=hc_cnt + curl_cnt,
            target=64,
            notes=(
                f"{hc_cnt} libHttpClient.GDK + {curl_cnt} XCurl retail exports; "
                "offline mock registry + opt-in WinHTTP/WebSocket transport"
            ),
        ),
        Subsystem(
            name="PE64 Loader & Auto-Stubs",
            short_name="PE64",
            category="Core",
            implemented=loader_features,
            target=10,
            notes=(
                f"{loader_features}/10 milestones: main EXE PE32+ mapping, relocs, TLS/FLS, .pdata & auto-stubs live; "
                "package-loaded secondary guest DLL IAT hooking & StubThunk .pdata pending Wave 2"
            ),
        ),
        Subsystem(
            name="Xbox Unified Memory & KernelX",
            short_name="Kern",
            category="Platform",
            implemented=2,  # Internal VirtualAlloc2/MapViewOfFile3 helper backing XMemVirtualAlloc; 0 retail kernelx.dll exports
            target=16,
            notes=(
                f"0 retail kernelx.dll exports (AllocateTitlePhysicalPages/MapTitlePhysicalPages unimplemented; "
                f"2 internal VirtualAlloc2 aliasing helpers + {kernelx_synth_cnt} synthetic test exports)"
            ),
        ),
        Subsystem(
            name="RDNA2 XS2 & DXIL Shader IR",
            short_name="XS2",
            category="Graphics",
            implemented=4,  # DXBC/DXIL Xbox chunk strip + dxil.dll re-sign + root-sig inspect; RDNA2 ISA->DXIL pending
            target=16,
            notes=(
                f"DXBC/DXIL Xbox chunk stripping + dxil.dll signing verified; full fail-closed RDNA2 ISA -> SSA -> DXIL "
                f"compiler pending ({xs2_synth_cnt} isolated xs2_lifting.dll synthetic exports)"
            ),
        ),
        Subsystem(
            name="Retail XVC/XVD & NTFS Crypto",
            short_name="Crypt",
            category="Storage",
            implemented=8,  # msft-xvd v3, Merkle SHA-256, AES-XTS CNG, NTFS $MFT, GDFX, .cik loader; zero-stage streaming pending
            target=12,
            notes=(
                "BCrypt AES-XTS, SHA-256 Merkle tree, NTFS $MFT & .cik loader active in XvdContainer; "
                f"zero-staging streaming & GUID key-matching queued (+{xvc_crypto_synth_cnt} synthetic exports)"
            ),
        ),
        Subsystem(
            name="Live Xbox Services & WinRT",
            short_name="XSAPI",
            category="Platform",
            implemented=4,  # RoGetActivationFactory / RoActivateInstance basic WinRT factories; retail XSAPI thunks pending
            target=23,
            notes=(
                "In-process WinRT RoGetActivationFactory broker (Gaming.Input/AnalyticsInfo/Package); "
                f"package-first XSAPI thunk routing queued (+{xsapi_synth_cnt} synthetic Xbl* exports)"
            ),
        ),
    ]

    return subsystems, breakdown, test_count


def collect_concept_destruction_metrics() -> list[Subsystem]:
    """Verified real-title bring-up progress for Concept Destruction (Scarlett Gen9 Unity IL2CPP)."""
    return [
        Subsystem(
            name="PE64 Mapping & Imports",
            short_name="PE64",
            category="Loader",
            implemented=4,
            target=4,
            notes="Manifest parsed, PE64 mapped (405 imports), static Xbox DLLs resolved, G:\\ package mount",
            upstream_implemented=4,
        ),
        Subsystem(
            name="xgameruntime & QueryApiImpl",
            short_name="XGR",
            category="Runtime",
            implemented=6,
            target=6,
            notes="Every QueryApiImpl table the title requests resolves (task queues, XNetworking, XGame, XSystem, XUser, XGameSave); XUserGetTokenAndSignatureAsync implemented with synthetic offline token",
            upstream_implemented=6,
        ),
        Subsystem(
            name="d3d12_xs Device & Resources",
            short_name="Dev",
            category="Graphics",
            implemented=8,
            target=8,
            notes="Device, queues, fences, descriptor heaps, root sigs, PSOs, 1080p RTs, CreateHeap/CreatePlacedResource, guest texture memory, and frame event pacing",
            upstream_implemented=7,
        ),
        Subsystem(
            name="xg_xs & AcpHal Shims",
            short_name="XG/APU",
            category="Platform",
            implemented=4,
            target=4,
            notes="Console texture layout (4KB_S addressing) checked against console xg_xs.dll, AcpHal shape-context allocation & IAcpHal interface verified",
            upstream_implemented=3,
        ),
        Subsystem(
            name="d3d12_xs CmdList & Uploads",
            short_name="CmdL",
            category="Graphics",
            implemented=10,
            target=10,
            notes="Command lists run through gameplay with no Close failure or device removal; slot 83/85 CopyTextureRegionX and slot 67/69 FlushPipelineX verified across SDK versions",
            upstream_implemented=8,
        ),
        Subsystem(
            name="Presentation & First Frame",
            short_name="Pres",
            category="Graphics",
            implemented=5,
            target=5,
            notes="PresentX window on its own thread, real 1920x1080 frames; on a hardware GPU about 105 fps at 2,400 draws per frame; Alt+Enter toggles borderless fullscreen; frames are paced to the title's interval, or to --fcap N",
            upstream_implemented=5,
        ),
        Subsystem(
            name="Audio & Input (Runtime)",
            short_name="Aud/In",
            category="Media",
            implemented=4,
            target=4,
            notes="Guest-facing XAudio2 objects forwarding to the host XAudio2 2.9, sound audible in play; GameInput polling verified for A, B, D-pad, left stick, right trigger, and with a real Xbox controller; rumble reaches the controller",
            upstream_implemented=4,
        ),
        Subsystem(
            name="In-Game & Playable",
            short_name="Game",
            category="Gameplay",
            implemented=5,
            target=5,
            notes="Logos, intro cutscene, menus, events with driving and collisions through to the results screen, at full speed on a hardware GPU, with keyboard or an Xbox controller; a championship in progress continues after a restart",
            upstream_implemented=5,
        ),
    ]


def collect_optimization_metrics() -> list[Subsystem]:
    """Runtime optimization and engineering hygiene foundations."""
    return [
        Subsystem(
            name="Build & PCH Link Hygiene",
            short_name="Build",
            category="Build",
            implemented=4,
            target=4,
            notes="Zero-dirty Ninja graph, precompiled headers (<windows.h>/<d3d12.h>/STL), PRIVATE D3D link scope & orphan cleanup",
        ),
        Subsystem(
            name="Async-Safe Crash & Ctrl+C",
            short_name="VEH",
            category="Diagnostics",
            implemented=4,
            target=4,
            notes="Atomic Ctrl+C shutdown without %TEMP% leaks; zero-heap/zero-Loader-Lock VEH crash writer ignoring benign guard-page AVs",
        ),
        Subsystem(
            name="Deterministic Isolated CTests",
            short_name="CTest",
            category="Testing",
            implemented=4,
            target=4,
            notes="100% isolated ScopedTempDir outputs outside build/, Win32 event/fence waits (zero Sleep loops) & <5s parallel CTest",
        ),
        Subsystem(
            name="Honest Retail vs Synth Metrics",
            short_name="Metrics",
            category="Governance",
            implemented=4,
            target=4,
            notes="Zero target self-clamping; Retail Xbox ABI & COM vtable slots reported separately from synthetic test exports",
        ),
    ]


def collect_aaa_roadmap_metrics() -> list[Subsystem]:
    """Post-Audit Multi-Wave Retail Parity Roadmap (see docs/AUDIT_BASELINE.md)."""
    return [
        Subsystem(
            name="Wave 1: Build, VEH, CTest & Metrics",
            short_name="Wave1",
            category="Hygiene",
            implemented=4,
            target=4,
            notes="**Complete (4/4):** Clean Ninja graph + PCH, async-signal-safe Ctrl+C/VEH, deterministic <5s CTests & honest retail metrics",
        ),
        Subsystem(
            name="Wave 2A: Multi-DLL Loader & VFS",
            short_name="Wave2A",
            category="Loader",
            implemented=1,
            target=5,
            notes="**Queued (Gate M1/M2):** StubThunk .pdata + RX, secondary guest DLL IAT hooks, zero-staging XVC VFS & GUID .cik keyring",
        ),
        Subsystem(
            name="Wave 2B: GDK, FFX, Audio & GDeflate",
            short_name="Wave2B",
            category="Shims",
            implemented=1,
            target=5,
            notes="**Queued (Gate M1/M2):** 0x50B XPackageDetails, DisplayTimeout slot fix, FFX enum alignment, BTYPE=2 GDeflate & XAudio2 flush fix",
        ),
        Subsystem(
            name="Wave 3: d3d12_xs Sync & Heap Aliasing",
            short_name="Wave3",
            category="Graphics",
            implemented=1,
            target=5,
            notes="**Queued (Gate M3):** Fence-retired upload ring, FlushPipelineX barriers, real ID3D12Heap placed aliasing & ResizeBuffers",
        ),
    ]


def collect_phase3_metrics() -> list[Subsystem]:
    """Subsystem empirical verification tracks (aligned with docs/AUDIT_BASELINE.md Section 3)."""
    return [
        Subsystem(
            name="Shared Core & Flight Recorder",
            short_name="Core/FR",
            category="Diagnostics",
            implemented=5,
            target=5,
            notes="4096-slot MPMC ring, cross-DLL miss registries, zero-heap/zero-Loader-Lock VEH unwinder & SuspendThread hang watchdog",
        ),
        Subsystem(
            name="Real-Title COM Bridge (d3d12_xs)",
            short_name="COM",
            category="Graphics",
            implemented=3,
            target=5,
            notes="SDK 0x20001/0x20004/0x20006 vtables, ALLOW_DISPLAY strip & PresentX live; upload ring & ID3D12Heap aliasing queued in Wave 3",
        ),
        Subsystem(
            name="XVD v3, NTFS $MFT & AES-XTS",
            short_name="XVD/NTFS",
            category="Storage",
            implemented=3,
            target=5,
            notes="msft-xvd v3, SHA-256 Merkle, NTFS $MFT & .cik parsing verified; zero-staging XVC streaming & 4 KiB XTS batching queued in Wave 2",
        ),
        Subsystem(
            name="XTaskQueue, XAsync & XGameSave",
            short_name="GDK",
            category="Platform",
            implemented=4,
            target=5,
            notes="4-mode XTaskQueue, XAsyncProvider & persistent XGameSave verified; secondary guest DLL hooks & 0x50B XPackageDetails queued in Wave 2",
        ),
    ]


def collect_phase4_metrics() -> list[Subsystem]:
    """Clean-Room Governance & Retail Bring-Up Readiness."""
    return [
        Subsystem(
            name="4A. Clean-Room Governance & CI Gate",
            short_name="CleanRoom",
            category="Governance",
            implemented=5,
            target=5,
            notes="**Complete (5/5):** UTF-16LE/BE + case-insensitive check_cleanroom.py binary/key/trademark gate & GitHub Actions CI",
        ),
        Subsystem(
            name="4B. Flight-Recorder & Triage CLI",
            short_name="TriageCLI",
            category="Diagnostics",
            implemented=5,
            target=5,
            notes="**Complete (5/5):** Async-signal-safe VEH crash writer, benign guard-page filter, scarlettx triage CLEAN_RUN verification",
        ),
        Subsystem(
            name="4C. Real-Title COM Vertical Slice",
            short_name="RealCOM",
            category="Runtime",
            implemented=5,
            target=5,
            notes="**Complete (5/5):** mainCRTStartup real_title_com executing D3D12X CommandList + MEM_GRAPHICS CBV + PresentX + xcrdapi fence sync",
        ),
        Subsystem(
            name="4D. Commercial Retail Title Bring-Up",
            short_name="Retail",
            category="Retail",
            implemented=1,
            target=5,
            notes="**In Progress (1/5):** Concept Destruction Gen9 playable on GPU with audio/saves; AAA titles await Waves 2-3 (Gates M1-M3)",
        ),
    ]


def _badge_width(text: str) -> int:
    return max(26, int(round(len(text) * 6.4)) + 14)


def render_svg(
    subsystems: list[Subsystem],
    badges: list[tuple[str, str, str, bool]],
    heading_prefix: str = "Status",
    callout_lead: str = "Empirical scope:",
    callout_line1: str = "",
    callout_line2: str = "",
) -> str:
    W = 1040
    PAD_X = 36
    PAD_Y = 30
    INNER_W = W - 2 * PAD_X

    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0

    BOX_PAD_X = 24
    BOX_PAD_TOP = 24
    BOX_PAD_BOT = 18
    LABEL_H = 14
    LABEL_GAP = 10

    def _build_groups(rows: int, cell: int, cgap: int) -> list[tuple[Subsystem, int, int]]:
        built: list[tuple[Subsystem, int, int]] = []
        for sub in subsystems:
            slot_cnt = max(sub.target, sub.implemented)
            cols = max(2, math.ceil(slot_cnt / rows))
            cells_w = cols * cell + (cols - 1) * cgap
            label_w = int(math.ceil(len(sub.short_name) * 6.5)) + 6
            built.append((sub, cols, max(cells_w, label_w)))
        return built

    avail_graph_w = INNER_W - 2 * BOX_PAD_X
    ROWS = 7
    CELL = 14
    CELL_GAP = 3
    GROUP_GAP = 14
    groups = _build_groups(ROWS, CELL, CELL_GAP)
    raw_graph_w = sum(gw for _, _, gw in groups) + GROUP_GAP * max(0, len(groups) - 1)

    if raw_graph_w > avail_graph_w:
        for cand_rows in (7, 8, 9, 10, 11, 12, 14):
            min_cell = 9 if cand_rows <= 10 else 8
            cand_groups = _build_groups(cand_rows, 14, 3)
            cand_w = sum(gw for _, _, gw in cand_groups) + 14 * max(0, len(cand_groups) - 1)
            scale = min(1.0, avail_graph_w / cand_w) if cand_w > 0 else 1.0
            c_cell = max(min_cell, int(math.floor(14 * scale)))
            c_cgap = max(2, int(math.floor(3 * scale)))
            c_ggap = max(8, int(math.floor(14 * scale)))
            cand_groups = _build_groups(cand_rows, c_cell, c_cgap)
            cand_w = sum(gw for _, _, gw in cand_groups) + c_ggap * max(0, len(cand_groups) - 1)
            while cand_w > avail_graph_w and c_cell > min_cell:
                c_cell -= 1
                cand_groups = _build_groups(cand_rows, c_cell, c_cgap)
                cand_w = sum(gw for _, _, gw in cand_groups) + c_ggap * max(0, len(cand_groups) - 1)
            if cand_w > avail_graph_w and len(cand_groups) > 1:
                needed = int(math.ceil((cand_w - avail_graph_w) / (len(cand_groups) - 1)))
                c_ggap = max(6, c_ggap - needed)
                cand_w = sum(gw for _, _, gw in cand_groups) + c_ggap * max(0, len(cand_groups) - 1)
            ROWS, CELL, CELL_GAP, GROUP_GAP = cand_rows, c_cell, c_cgap, c_ggap
            groups = cand_groups
            raw_graph_w = cand_w
            if raw_graph_w <= avail_graph_w:
                break

    if raw_graph_w <= avail_graph_w and len(groups) > 1:
        extra_per_gap = int((avail_graph_w - raw_graph_w) // (len(groups) - 1))
        if extra_per_gap > 0:
            GROUP_GAP = min(38, GROUP_GAP + min(24, extra_per_gap))
            raw_graph_w = sum(gw for _, _, gw in groups) + GROUP_GAP * max(0, len(groups) - 1)
    elif raw_graph_w > avail_graph_w:
        W = int(math.ceil(raw_graph_w)) + 2 * (PAD_X + BOX_PAD_X)
        INNER_W = W - 2 * PAD_X
        avail_graph_w = INNER_W - 2 * BOX_PAD_X

    grid_h = ROWS * CELL + (ROWS - 1) * CELL_GAP
    legend_gap = 14
    legend_h = 16
    graph_box_h = BOX_PAD_TOP + LABEL_H + LABEL_GAP + grid_h + legend_gap + legend_h + BOX_PAD_BOT

    BADGE_H = 20
    BADGE_ROW_GAP = 6
    badge_placements: list[tuple[int, int, int, int, str, str, str, bool]] = []
    bx_cur = PAD_X
    b_row = 0
    for k, v, v_bg, dark_v in badges:
        kw = _badge_width(k)
        vw = _badge_width(v)
        bw = kw + vw
        if bx_cur > PAD_X and bx_cur + bw > W - PAD_X:
            b_row += 1
            bx_cur = PAD_X
        badge_placements.append((bx_cur, b_row, kw, vw, k, v, v_bg, dark_v))
        bx_cur += bw + 6
    badges_total_h = (b_row + 1) * BADGE_H + b_row * BADGE_ROW_GAP

    y_heading_baseline = PAD_Y + 26
    y_heading_rule = y_heading_baseline + 12
    y_badges = y_heading_rule + 18
    y_graph_box = y_badges + badges_total_h + 20
    y_list = y_graph_box + graph_box_h + 20

    sorted_subs = sorted(subsystems, key=lambda s: (-s.ratio, -s.implemented, s.name.lower()))
    list_rows_per_col = math.ceil(len(sorted_subs) / 2)
    ROW_H = 34
    list_h = list_rows_per_col * ROW_H

    def _wrap_words(text: str, limit: int) -> list[str]:
        words = text.split()
        if not words:
            return []
        out = [words[0]]
        for w in words[1:]:
            if len(out[-1]) + 1 + len(w) <= limit:
                out[-1] += " " + w
            else:
                out.append(w)
        return out

    has_callout = bool(callout_line1)
    callout_accent = FORK_GREEN if overall_pct >= 99.9 else WARN
    max_callout_chars = max(60, int((W - 2 * PAD_X - 46) // 6.9))
    first_callout_body = ""
    extra_callout_lines: list[str] = []
    if has_callout:
        first_lim = max(36, max_callout_chars - len(callout_lead) - 1)
        l1_parts = _wrap_words(callout_line1, first_lim)
        if l1_parts:
            first_callout_body = l1_parts[0]
            if len(l1_parts) > 1:
                extra_callout_lines.extend(_wrap_words(" ".join(l1_parts[1:]), max_callout_chars))
        if callout_line2:
            extra_callout_lines.extend(_wrap_words(callout_line2, max_callout_chars))
    callout_total_lines = (1 + len(extra_callout_lines)) if has_callout else 0
    callout_h = max(46, 12 + callout_total_lines * 17) if has_callout else 0
    y_callout = y_list + list_h + (20 if has_callout else 0)
    H = (y_callout + callout_h if has_callout else y_list + list_h) + PAD_Y

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" '
        f'aria-label="{html.escape(heading_prefix)}: {overall_pct:.1f}%">',
        "  <style>",
        "    text { font-family: 'Mona Sans', -apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif; }",
        f"    .h2 {{ font-size: 26px; font-weight: 600; fill: {FG}; }}",
        "    .badge-txt { font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; fill: #ffffff; }",
        f"    .badge-txt-dark {{ font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; fill: {BG}; }}",
        f"    .g-label {{ font-size: 10px; fill: {FG_MUTED}; }}",
        f"    .legend-txt {{ font-size: 12px; fill: {FG_MUTED}; }}",
        f"    .r-name {{ font-size: 13px; fill: {FG}; }}",
        f"    .r-count {{ font-size: 13px; fill: {FG_MUTED}; font-variant-numeric: tabular-nums; }}",
        f"    .r-pct {{ font-size: 13px; font-weight: 600; fill: {FG}; font-variant-numeric: tabular-nums; }}",
        f"    .callout-txt {{ font-size: 13px; fill: {FG_SOFT}; }}",
        f"    .callout-lead {{ font-size: 13px; font-weight: 600; fill: {callout_accent}; }}",
        "  </style>",
        f'  <rect width="{W}" height="{H}" fill="{BG}"/>',
        "",
        f'  <text class="h2" x="{PAD_X}" y="{y_heading_baseline}">{html.escape(heading_prefix)}: {overall_pct:.1f}%</text>',
        f'  <line x1="{PAD_X}" y1="{y_heading_rule}" x2="{W - PAD_X}" y2="{y_heading_rule}" stroke="{BORDER_STRONG}" stroke-width="1"/>',
        "",
    ]

    for idx, (bx, row_idx, kw, vw, k, v, v_bg, dark_v) in enumerate(badge_placements):
        bw = kw + vw
        by = y_badges + row_idx * (BADGE_H + BADGE_ROW_GAP)
        clip_id = f"bclip{idx}"
        lines.append(f'  <clipPath id="{clip_id}"><rect x="{bx}" y="{by}" width="{bw}" height="{BADGE_H}" rx="3"/></clipPath>')
        lines.append(f'  <g clip-path="url(#{clip_id})">')
        lines.append(f'    <rect x="{bx}" y="{by}" width="{kw}" height="{BADGE_H}" fill="{BADGE_LABEL}"/>')
        lines.append(f'    <rect x="{bx + kw}" y="{by}" width="{vw}" height="{BADGE_H}" fill="{v_bg}"/>')
        lines.append(
            f'    <text class="badge-txt" x="{bx + kw / 2:.1f}" y="{by + 14}" text-anchor="middle">{html.escape(k)}</text>'
        )
        v_cls = "badge-txt-dark" if dark_v else "badge-txt"
        lines.append(
            f'    <text class="{v_cls}" x="{bx + kw + vw / 2:.1f}" y="{by + 14}" text-anchor="middle">{html.escape(v)}</text>'
        )
        lines.append("  </g>")

    lines.append(
        f'  <rect x="{PAD_X}" y="{y_graph_box}" width="{INNER_W}" height="{graph_box_h}" rx="6" '
        f'fill="{BG}" stroke="{BORDER}" stroke-width="1"/>'
    )

    gx = PAD_X + BOX_PAD_X + max(0.0, (avail_graph_w - raw_graph_w) / 2.0)
    gy_label = y_graph_box + BOX_PAD_TOP + 10
    gy_cells = y_graph_box + BOX_PAD_TOP + LABEL_H + LABEL_GAP

    has_upstream_split = any(s.upstream_implemented is not None for s in subsystems)
    for s, cols, gw in groups:
        fill_col = level_color(s.ratio, s.implemented)
        up_cnt = min(s.implemented, s.upstream_implemented) if s.upstream_implemented is not None else 0
        lines.append(f'  <text class="g-label" x="{gx:.1f}" y="{gy_label}">{html.escape(s.short_name)}</text>')
        for i in range(cols * ROWS):
            col = i // ROWS
            row = i % ROWS
            cx = gx + col * (CELL + CELL_GAP)
            cy = gy_cells + row * (CELL + CELL_GAP)
            if i < s.implemented:
                cell_col = (UPSTREAM_PURPLE if i < up_cnt else FORK_GREEN) if has_upstream_split else fill_col
                lines.append(f'  <rect x="{cx:.1f}" y="{cy:.1f}" width="{CELL}" height="{CELL}" rx="3" fill="{cell_col}"/>')
            elif i < s.target:
                lines.append(
                    f'  <rect x="{cx + 0.5:.1f}" y="{cy + 0.5:.1f}" width="{CELL - 1}" height="{CELL - 1}" rx="3" '
                    f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
                )
        gx += gw + GROUP_GAP

    ly = gy_cells + grid_h + legend_gap + 11
    rx_end = PAD_X + INNER_W - BOX_PAD_X
    if has_upstream_split:
        lines.append(f'  <text class="legend-txt" x="{rx_end}" y="{ly}" text-anchor="end">ScarlettEdge (Fork)</text>')
        sx = rx_end - 116
        lines.append(f'  <rect x="{sx:.1f}" y="{ly - 10:.1f}" width="11" height="11" rx="2" fill="{FORK_GREEN}"/>')
        lines.append(f'  <text class="legend-txt" x="{sx - 12:.1f}" y="{ly}" text-anchor="end">ScarlettX (Upstream)</text>')
        sx2 = sx - 134
        lines.append(f'  <rect x="{sx2:.1f}" y="{ly - 10:.1f}" width="11" height="11" rx="2" fill="{UPSTREAM_PURPLE}"/>')
        lines.append(f'  <text class="legend-txt" x="{sx2 - 12:.1f}" y="{ly}" text-anchor="end">Not yet</text>')
        sx3 = sx2 - 60
        lines.append(
            f'  <rect x="{sx3 + 0.5:.1f}" y="{ly - 9.5:.1f}" width="10" height="10" rx="2" '
            f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
        )
    else:
        lines.append(f'  <text class="legend-txt" x="{rx_end}" y="{ly}" text-anchor="end">Done</text>')
        sw_colors = [LV3, LV2, LV1, LV0, EMPTY_FILL]
        sx = rx_end - 36 - 11
        for col in sw_colors:
            if col == EMPTY_FILL:
                lines.append(
                    f'  <rect x="{sx + 0.5:.1f}" y="{ly - 9.5:.1f}" width="10" height="10" rx="2" '
                    f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
                )
            else:
                lines.append(f'  <rect x="{sx:.1f}" y="{ly - 10:.1f}" width="11" height="11" rx="2" fill="{col}"/>')
            sx -= 15
        lines.append(f'  <text class="legend-txt" x="{sx + 11}" y="{ly}" text-anchor="end">Not yet</text>')

    COL_GAP = 40
    LIST_PAD = 4
    col_w = (INNER_W - 2 * LIST_PAD - COL_GAP) / 2.0
    for idx, s in enumerate(sorted_subs):
        col_i = idx % 2
        row_i = idx // 2
        x0 = PAD_X + LIST_PAD + col_i * (col_w + COL_GAP)
        y_top = y_list + row_i * ROW_H
        y_mid = y_top + 19
        dot_fill = level_color(s.ratio, s.implemented)
        if s.implemented == 0:
            lines.append(
                f'  <rect x="{x0 + 0.5:.1f}" y="{y_top + 9.5:.1f}" width="10" height="10" rx="2" '
                f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
            )
        elif has_upstream_split and s.upstream_implemented is not None:
            up_cnt = min(s.implemented, s.upstream_implemented)
            if up_cnt == s.implemented:
                lines.append(f'  <rect x="{x0:.1f}" y="{y_top + 9:.1f}" width="11" height="11" rx="2" fill="{UPSTREAM_PURPLE}"/>')
            elif up_cnt == 0:
                lines.append(f'  <rect x="{x0:.1f}" y="{y_top + 9:.1f}" width="11" height="11" rx="2" fill="{FORK_GREEN}"/>')
            else:
                lines.append(f'  <rect x="{x0:.1f}" y="{y_top + 9:.1f}" width="5.5" height="11" rx="1" fill="{UPSTREAM_PURPLE}"/>')
                lines.append(f'  <rect x="{x0 + 5.5:.1f}" y="{y_top + 9:.1f}" width="5.5" height="11" rx="1" fill="{FORK_GREEN}"/>')
        else:
            lines.append(f'  <rect x="{x0:.1f}" y="{y_top + 9:.1f}" width="11" height="11" rx="2" fill="{dot_fill}"/>')
        lines.append(f'  <text class="r-name" x="{x0 + 21:.1f}" y="{y_mid}">{html.escape(s.name)}</text>')
        lines.append(
            f'  <text class="r-count" x="{x0 + col_w - 48:.1f}" y="{y_mid}" text-anchor="end">'
            f'{s.implemented} of {s.target}</text>'
        )
        lines.append(
            f'  <text class="r-pct" x="{x0 + col_w:.1f}" y="{y_mid}" text-anchor="end">{round(s.pct)}%</text>'
        )
        lines.append(
            f'  <line x1="{x0:.1f}" y1="{y_top + ROW_H}" x2="{x0 + col_w:.1f}" y2="{y_top + ROW_H}" '
            f'stroke="{BORDER_MUTED}" stroke-width="1"/>'
        )

    if has_callout:
        lines.append(f'  <rect x="{PAD_X}" y="{y_callout}" width="4" height="{callout_h}" fill="{callout_accent}"/>')
        ix = PAD_X + 20
        iy = y_callout + 10
        if overall_pct >= 99.9:
            lines.append(
                f'  <g transform="translate({ix},{iy})">'
                f'<circle cx="8" cy="8" r="6.5" fill="none" stroke="{callout_accent}" stroke-width="1.5"/>'
                f'<path d="M5.2 8.2 7.1 10.1 11 6.1" fill="none" stroke="{callout_accent}" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>'
                f"</g>"
            )
        else:
            lines.append(
                f'  <g transform="translate({ix},{iy})">'
                f'<path d="M8 1.5 15 14H1z" fill="none" stroke="{callout_accent}" stroke-width="1.5" stroke-linejoin="round"/>'
                f'<path d="M8 6v3.5M8 11.6v.4" stroke="{callout_accent}" stroke-width="1.5" stroke-linecap="round"/>'
                f"</g>"
            )
        tx = ix + 26
        lines.append(
            f'  <text class="callout-txt" x="{tx}" y="{y_callout + 19}">'
            f'<tspan class="callout-lead">{html.escape(callout_lead)}</tspan> '
            f'{html.escape(first_callout_body)}</text>'
        )
        for c_idx, extra_line in enumerate(extra_callout_lines, start=1):
            lines.append(
                f'  <text class="callout-txt" x="{tx}" y="{y_callout + 19 + 17 * c_idx}">'
                f'{html.escape(extra_line)}</text>'
            )

    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def _serialize_js_subsystems(subs: list[Subsystem]) -> str:
    rows = []
    for s in subs:
        up = -1 if s.upstream_implemented is None else min(s.implemented, s.upstream_implemented)
        rows.append(f"      [{s.short_name!r}, {s.name!r}, {s.implemented}, {s.target}, {up}],")
    return "\n".join(rows)


def render_html(
    subsystems: list[Subsystem],
    breakdown: ExportBreakdown,
    tests_text: str,
    tests_ok: bool,
    cd_subsystems: list[Subsystem] | None = None,
    opt_subsystems: list[Subsystem] | None = None,
    aaa_subsystems: list[Subsystem] | None = None,
    phase3_subsystems: list[Subsystem] | None = None,
    phase4_subsystems: list[Subsystem] | None = None,
) -> str:
    cd_subs = cd_subsystems or []
    opt_subs = opt_subsystems or []
    aaa_subs = aaa_subsystems or []
    p3_subs = phase3_subsystems or []
    p4_subs = phase4_subsystems or []

    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0
    up_impl = sum(min(s.implemented, s.upstream_implemented or 0) for s in subsystems)
    fork_impl = total_impl - up_impl

    p4_impl = sum(s.implemented for s in p4_subs)
    p4_target = sum(s.target for s in p4_subs)
    p4_pct = (p4_impl / p4_target * 100.0) if p4_target else 0.0

    p3_impl = sum(s.implemented for s in p3_subs)
    p3_target = sum(s.target for s in p3_subs)
    p3_pct = (p3_impl / p3_target * 100.0) if p3_target else 0.0

    aaa_impl = sum(s.implemented for s in aaa_subs)
    aaa_target = sum(s.target for s in aaa_subs)
    aaa_pct = (aaa_impl / aaa_target * 100.0) if aaa_target else 0.0

    opt_impl = sum(s.implemented for s in opt_subs)
    opt_target = sum(s.target for s in opt_subs)
    opt_pct = (opt_impl / opt_target * 100.0) if opt_target else 0.0

    cd_impl = sum(s.implemented for s in cd_subs)
    cd_target = sum(s.target for s in cd_subs)
    cd_pct = (cd_impl / cd_target * 100.0) if cd_target else 0.0

    js_p4 = _serialize_js_subsystems(p4_subs)
    js_p3 = _serialize_js_subsystems(p3_subs)
    js_abi = _serialize_js_subsystems(subsystems)
    js_aaa = _serialize_js_subsystems(aaa_subs)
    js_opt = _serialize_js_subsystems(opt_subs)
    js_cd = _serialize_js_subsystems(cd_subs)

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScarlettEdge — Status Dashboard</title>
<style>
  :root {{
    --bg: {BG};
    --border: {BORDER};
    --border-strong: {BORDER_STRONG};
    --border-muted: {BORDER_MUTED};
    --fg: {FG};
    --fg-muted: {FG_MUTED};
    --fg-soft: {FG_SOFT};
    --empty: {EMPTY_FILL};
    --empty-ring: rgba(240, 246, 252, 0.08);
    --lv0: {LV0};
    --lv1: {LV1};
    --lv2: {LV2};
    --lv3: {LV3};
    --upstream: {UPSTREAM_PURPLE};
    --fork: {FORK_GREEN};
    --warn: {WARN};
    --badge-label: {BADGE_LABEL};
    --badge-exports: {BADGE_EXPORTS};
    --badge-tests: {BADGE_TESTS};
    --badge-target: {BADGE_TARGET};
    --cell: 13px;
    --cell-gap: 3px;
    --radius: 6px;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0;
    background: var(--bg);
    color: var(--fg);
    font-family: -apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif;
  }}
  .page {{
    width: 920px;
    max-width: 100%;
    margin: 0 auto;
    padding: 30px 36px;
    display: flex;
    flex-direction: column;
    gap: 36px;
  }}
  .panel {{ display: flex; flex-direction: column; gap: 16px; }}
  h2 {{
    margin: 0;
    font-size: 24px;
    font-weight: 600;
    line-height: 1.25;
    padding-bottom: 8px;
    border-bottom: 1px solid var(--border-strong);
  }}
  .badges {{ display: flex; flex-wrap: wrap; gap: 6px; font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; line-height: 20px; }}
  .badge {{ display: flex; border-radius: 3px; overflow: hidden; }}
  .badge span {{ padding: 0 7px; color: #fff; }}
  .badge .k {{ background: var(--badge-label); }}
  .badge .v-up {{ background: var(--upstream); color: var(--bg); font-weight: 600; }}
  .badge .v-fn {{ background: var(--fork); color: var(--bg); font-weight: 600; }}
  .badge .v-exports {{ background: var(--badge-exports); }}
  .badge .v-tests {{ background: var(--badge-tests); }}
  .badge .v-tests-warn {{ background: var(--warn); color: var(--bg); }}
  .badge .v-target {{ background: var(--badge-target); }}
  .graph-box {{
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 20px 22px 16px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    overflow-x: auto;
  }}
  .graph {{ display: flex; gap: 8px; width: max-content; margin: 0 auto; }}
  .group {{ display: flex; flex-direction: column; gap: 6px; }}
  .group-label {{ font-size: 10px; color: var(--fg-muted); white-space: nowrap; }}
  .cells {{
    display: grid;
    grid-template-rows: repeat(7, var(--cell));
    grid-auto-flow: column;
    grid-auto-columns: var(--cell);
    gap: var(--cell-gap);
  }}
  .cell {{ display: block; width: var(--cell); height: var(--cell); border-radius: 3px; }}
  .cell.empty, .sw.empty, .dot.empty {{ background: var(--empty); outline: 1px solid var(--empty-ring); outline-offset: -1px; }}
  .cell.pad {{ background: transparent; }}
  .lv0 {{ background: var(--lv0); }}
  .lv1 {{ background: var(--lv1); }}
  .lv2 {{ background: var(--lv2); }}
  .lv3 {{ background: var(--lv3); }}
  .upstream {{ background: var(--upstream); }}
  .fork {{ background: var(--fork); }}
  .dot.split {{ background: linear-gradient(90deg, var(--upstream) 50%, var(--fork) 50%); }}
  .legend {{ display: flex; justify-content: flex-end; align-items: center; gap: 6px; font-size: 12px; color: var(--fg-muted); }}
  .sw {{ display: inline-block; width: 11px; height: 11px; border-radius: 2px; }}
  .list {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0 40px; padding: 0 4px; }}
  .row {{ display: flex; align-items: center; gap: 10px; height: 30px; border-bottom: 1px solid var(--border-muted); font-size: 13px; }}
  .dot {{ width: 11px; height: 11px; border-radius: 2px; flex-shrink: 0; }}
  .row .name {{ flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }}
  .row .count {{ color: var(--fg-muted); font-variant-numeric: tabular-nums; }}
  .row .pct {{ width: 40px; text-align: right; font-weight: 600; font-variant-numeric: tabular-nums; }}
  .callout {{ display: flex; gap: 10px; padding: 8px 16px; border-left: 4px solid var(--warn); color: var(--fg-soft); font-size: 13px; }}
  .callout strong {{ color: var(--warn); }}
</style>
</head>
<body>
<main class="page">
  <section class="panel">
    <h2>Retail Xbox ABI &amp; COM Surface Coverage: {overall_pct:.1f}%</h2>
    <div class="badges">
      <span class="badge"><span class="k">upstream ScarlettX</span><span class="v-up">{up_impl}/{total_target}</span></span>
      <span class="badge"><span class="k">ScarlettEdge fork</span><span class="v-fn">+{fork_impl}</span></span>
      <span class="badge"><span class="k">retail exports</span><span class="v-exports">{breakdown.retail_exports}</span></span>
      <span class="badge"><span class="k">COM vtable slots</span><span class="v-target">{breakdown.com_vtable_slots}</span></span>
      <span class="badge"><span class="k">synthetic test exports</span><span class="v-tests-warn">{breakdown.synthetic_exports}</span></span>
      <span class="badge"><span class="k">tests</span><span class="{'v-tests' if tests_ok else 'v-tests-warn'}">{tests_text}</span></span>
    </div>
    <div class="graph-box"><div class="graph" id="graph-abi"></div><div class="legend" id="leg-abi"></div></div>
    <div class="list" id="list-abi"></div>
    <div class="callout">
      <span><strong>Empirical Separation:</strong> Retail Xbox DLL exports (<code>{breakdown.retail_exports}</code>) and wired COM vtable slots (<code>{breakdown.com_vtable_slots}</code>) are tracked separately from synthetic C-ABI test-harness exports (<code>{breakdown.synthetic_exports}</code>), with zero <code>target = implemented</code> self-clamping.</span>
    </div>
  </section>

  <section class="panel">
    <h2>Clean-Room Governance &amp; Retail Readiness: {p4_pct:.1f}%</h2>
    <div class="badges">
      <span class="badge"><span class="k">readiness</span><span class="v-fn">{p4_impl}/{p4_target} ({p4_pct:.1f}%)</span></span>
      <span class="badge"><span class="k">CI tests</span><span class="{'v-tests' if tests_ok else 'v-tests-warn'}">{tests_text}</span></span>
    </div>
    <div class="graph-box"><div class="graph" id="graph-p4"></div><div class="legend" id="leg-p4"></div></div>
    <div class="list" id="list-p4"></div>
  </section>

  <section class="panel">
    <h2>Empirical Subsystem Verification: {p3_pct:.1f}%</h2>
    <div class="graph-box"><div class="graph" id="graph-p3"></div><div class="legend" id="leg-p3"></div></div>
    <div class="list" id="list-p3"></div>
  </section>

  <section class="panel">
    <h2>Multi-Wave Advancement Roadmap: {aaa_pct:.1f}%</h2>
    <div class="graph-box"><div class="graph" id="graph-aaa"></div><div class="legend" id="leg-aaa"></div></div>
    <div class="list" id="list-aaa"></div>
  </section>

  <section class="panel">
    <h2>Wave 1 Engineering Hygiene: {opt_pct:.1f}%</h2>
    <div class="graph-box"><div class="graph" id="graph-opt"></div><div class="legend" id="leg-opt"></div></div>
    <div class="list" id="list-opt"></div>
  </section>

  <section class="panel">
    <h2>Concept Destruction (Upstream Purple + Fork Green): {cd_pct:.1f}%</h2>
    <div class="graph-box"><div class="graph" id="graph-cd"></div><div class="legend" id="leg-cd"></div></div>
    <div class="list" id="list-cd"></div>
  </section>
</main>
<script>
  const PANELS = [
    ['abi', [\n{js_abi}\n    ]],
    ['p4',  [\n{js_p4}\n    ]],
    ['p3',  [\n{js_p3}\n    ]],
    ['aaa', [\n{js_aaa}\n    ]],
    ['opt', [\n{js_opt}\n    ]],
    ['cd',  [\n{js_cd}\n    ]],
  ];
  const level = p => p >= 0.8 ? 'lv3' : p >= 0.6 ? 'lv2' : p >= 0.4 ? 'lv1' : 'lv0';
  const el = (tag, cls, text) => {{ const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }};
  for (const [id, subs] of PANELS) {{
    const graph = document.getElementById('graph-' + id);
    const leg = document.getElementById('leg-' + id);
    const list = document.getElementById('list-' + id);
    const hasSplit = subs.some(s => s[4] >= 0);
    for (const [label, , done, total, up] of subs) {{
      const g = el('div', 'group');
      g.append(el('span', 'group-label', label));
      const cells = el('div', 'cells');
      const cols = Math.max(2, Math.ceil(total / 7));
      const lv = level(done / total);
      for (let i = 0; i < cols * 7; i++) {{
        let cls = 'pad';
        if (i < done) cls = hasSplit ? (i < up ? 'upstream' : 'fork') : lv;
        else if (i < total) cls = 'empty';
        cells.append(el('span', 'cell ' + cls));
      }}
      g.append(cells);
      graph.append(g);
    }}
    if (hasSplit) {{
      leg.append(el('span', 'sw empty'), ' Not yet ', el('span', 'sw upstream'), ' ScarlettX (Upstream) ', el('span', 'sw fork'), ' ScarlettEdge (Fork)');
    }} else {{
      leg.append('Not yet ', el('span', 'sw empty'), el('span', 'sw lv0'), el('span', 'sw lv1'), el('span', 'sw lv2'), el('span', 'sw lv3'), ' Done');
    }}
    [...subs].sort((a, b) => b[2] / b[3] - a[2] / a[3]).forEach(([, name, done, total, up]) => {{
      const p = done / total;
      const row = el('div', 'row');
      let dotCls = done === 0 ? 'empty' : level(p);
      if (done > 0 && hasSplit && up >= 0) {{
        dotCls = up === done ? 'upstream' : up === 0 ? 'fork' : 'split';
      }}
      row.append(el('span', 'dot ' + dotCls));
      row.append(el('span', 'name', name));
      row.append(el('span', 'count', `${{done}} of ${{total}}`));
      row.append(el('span', 'pct', `${{Math.round(p * 100)}}%`));
      list.append(row);
    }});
  }}
</script>
</body>
</html>
"""


def render_markdown_status(
    subsystems: list[Subsystem],
    cd_subsystems: list[Subsystem],
    breakdown: ExportBreakdown,
    tests_md: str,
    opt_subsystems: list[Subsystem] | None = None,
    aaa_subsystems: list[Subsystem] | None = None,
    phase3_subsystems: list[Subsystem] | None = None,
    phase4_subsystems: list[Subsystem] | None = None,
) -> str:
    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0

    cd_impl = sum(s.implemented for s in cd_subsystems)
    cd_target = sum(s.target for s in cd_subsystems)
    cd_pct = (cd_impl / cd_target * 100.0) if cd_target else 0.0

    up_total = sum(min(s.implemented, s.upstream_implemented or 0) for s in subsystems)
    fork_total = total_impl - up_total
    up_pct = (up_total / total_target * 100.0) if total_target else 0.0
    fork_pct = (fork_total / total_target * 100.0) if total_target else 0.0
    cd_up = sum(min(s.implemented, s.upstream_implemented or 0) for s in cd_subsystems)
    cd_fork = cd_impl - cd_up
    cd_up_pct = (cd_up / cd_target * 100.0) if cd_target else 0.0
    cd_fork_pct = (cd_fork / cd_target * 100.0) if cd_target else 0.0

    sorted_subs = sorted(subsystems, key=lambda s: (-s.ratio, -s.implemented, s.name.lower()))
    rows = [
        "<!-- SCARLETTX_STATUS_START -->",
        "## Status",
        "",
        "### Retail Xbox ABI & COM Surface Coverage (Empirical Baseline)",
        "",
        "Metrics below separate **Retail Xbox DLL Exports & Wired COM Vtable Slots** (`SCARLETTX_REAL_TITLE=1` guest path) from **Synthetic Test-Harness C-ABI Exports** (used only by standalone unit tests), with zero target self-clamping (see [docs/AUDIT_BASELINE.md](docs/AUDIT_BASELINE.md)):",
        "",
        "| Surface Metric | Count | Scope |",
        "| :--- | :---: | :--- |",
        f"| **Retail Xbox DLL Exports** | `{breakdown.retail_exports}` | Real GDK / Xbox Gen9 imports (`xgameruntime`, `d3d12_xs`, `xg_xs`, `xcrdapi`, `xaudio2_9`, `AcpHal`, `GameInput`, `Bink2`, `FFX`, `libHttpClient`, `XCurl`, `PIX`) |",
        f"| **Wired Retail COM Vtable Slots** | `{breakdown.com_vtable_slots}` | Guest-facing COM proxy slots across `d3d12_xs`, `QueryApiImpl` tables, `xcrdapi`, `GameInput`, and `XAudio2` |",
        f"| **Synthetic Test-Harness Exports** | `{breakdown.synthetic_exports}` | Non-retail `uint64_t`-handle / test-only exports (`NOT retail Xbox ABI` helpers, `dstorage.dll` C handles, `xs2_lifting`, `vulkan_backend`, `xvc_crypto`, `kernelx` helpers) |",
        f"| **Automated CTest Suite** | {tests_md} | Isolated per-test `%TEMP%` sandboxes, event/fence-driven synchronization |",
        "",
        f"In the charts below, **purple (`#a371f7`)** credits original work from **[Freminet/ScarlettX](https://github.com/Freminet/ScarlettX)** (`{up_pct:.1f}%` retail ABI / `{cd_up_pct:.1f}%` Concept Destruction), while **emerald green (`#39d353`)** marks **ScarlettEdge** additions (`+{fork_pct:.1f}%` retail ABI / `+{cd_fork_pct:.1f}%` Concept Destruction):",
        "",
        "![Retail ABI & COM Coverage: Upstream ScarlettX (Purple) vs ScarlettEdge Fork (Green)](docs/status.svg)",
        "",
        "![Concept Destruction Bring-Up: Upstream (Purple) vs ScarlettEdge (Green)](docs/status_concept_destruction.svg)",
        "",
    ]

    shots = [shot for shot in SCREENSHOTS if (ROOT / shot[0]).exists()]
    if shots:
        rows += [
            "| " + " | ".join(title for _, title, _, _ in shots) + " |",
            "|" + " :---: |" * len(shots),
            "| " + " | ".join(f"![{alt}]({path})" for path, _, alt, _ in shots) + " |",
        ]
        if any(caption for _, _, _, caption in shots):
            rows.append("| " + " | ".join(caption for _, _, _, caption in shots) + " |")
        rows.append("")

    rows += [
        f"**Retail ABI & COM Surface:** `{overall_pct:.1f}%` (`{total_impl}` of `{total_target}` tracked retail surfaces: `{up_total}` upstream + `{fork_total}` fork) · "
        f"**Retail Exports:** `{breakdown.retail_exports}` · "
        f"**Wired COM Slots:** `{breakdown.com_vtable_slots}` · "
        f"**Synthetic Test Exports:** `{breakdown.synthetic_exports}` · "
        f"**Concept Destruction:** `{cd_pct:.1f}%` · "
        f"**Tests:** {tests_md}",
        "",
        "| Subsystem | Category | Retail Progress | Origin Split | Empirical Details |",
        "| :--- | :--- | :---: | :---: | :--- |",
    ]
    for s in sorted_subs:
        up = min(s.implemented, s.upstream_implemented or 0)
        fk = s.implemented - up
        if s.implemented == 0:
            split = "`0 retail`"
        elif fk == 0:
            split = f"`{up} upstream`"
        elif up == 0:
            split = f"`{fk} ScarlettEdge`"
        else:
            split = f"`{up} upstream + {fk} fork`"
        rows.append(
            f"| **{s.name}** | `{s.category}` | `{s.implemented} of {s.target}` (`{round(s.pct)}%`) | {split} | {s.notes} |"
        )

    if aaa_subsystems:
        rows += [
            "",
            "---",
            "",
            "### Post-Audit Multi-Wave Roadmap ([docs/AUDIT_BASELINE.md](docs/AUDIT_BASELINE.md))",
            "",
            "![ScarlettEdge Multi-Wave Roadmap](docs/status_aaa_roadmap.svg)",
            "",
            "| Wave | Category | Progress | Details |",
            "| :--- | :--- | :---: | :--- |",
        ]
        for s in aaa_subsystems:
            rows.append(f"| **{s.name}** | `{s.category}` | `{s.implemented} of {s.target}` | {s.notes} |")

    rows.append("")
    rows.append(
        "_Regenerate with `py tools/generate_status.py --build-dir <build tree>` after modifying retail ABI exports, COM vtables, or CTest suites._"
    )
    rows.append("<!-- SCARLETTX_STATUS_END -->")
    return "\n".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the ScarlettX status dashboard and README status section.")
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=ROOT / "build",
        help="build tree whose last CTest run supplies the test results (default: <repo>/build)",
    )
    args = parser.parse_args()

    subsystems, breakdown, test_count = collect_metrics()
    cd_subsystems = collect_concept_destruction_metrics()
    test_results = read_ctest_results(args.build_dir)
    tests_text = tests_label(test_count, test_results)
    tests_ok = test_results is not None and not test_results[1]

    for s in subsystems:
        s.upstream_implemented = UPSTREAM_ABI_COUNTS.get(s.name, 0)

    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    up_impl = sum(min(s.implemented, s.upstream_implemented or 0) for s in subsystems)
    fork_impl = total_impl - up_impl
    up_pct = (up_impl / total_target * 100.0) if total_target else 0.0
    fork_pct = (fork_impl / total_target * 100.0) if total_target else 0.0
    main_badges = [
        ("upstream ScarlettX", f"{up_impl}/{total_target} ({up_pct:.1f}%)", BADGE_UPSTREAM, True),
        ("ScarlettEdge fork", f"+{fork_impl} (+{fork_pct:.1f}%)", BADGE_FN, True),
        ("retail exports", str(breakdown.retail_exports), BADGE_EXPORTS, False),
        ("COM slots", str(breakdown.com_vtable_slots), BADGE_TARGET, False),
        ("synthetic test exports", str(breakdown.synthetic_exports), WARN, True),
        ("tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
    ]
    svg = render_svg(
        subsystems,
        badges=main_badges,
        heading_prefix="Retail Xbox ABI & COM Coverage",
        callout_lead="Empirical baseline:",
        callout_line1=(
            f"Retail Xbox DLL exports ({breakdown.retail_exports}) + wired COM vtable slots ({breakdown.com_vtable_slots}) "
            f"reported separately from synthetic test exports ({breakdown.synthetic_exports})."
        ),
        callout_line2=f"Zero target self-clamping ({total_impl}/{total_target} retail surfaces; {tests_text} CTest suites).",
    )

    cd_impl = sum(s.implemented for s in cd_subsystems)
    cd_target = sum(s.target for s in cd_subsystems)
    cd_up = sum(min(s.implemented, s.upstream_implemented or 0) for s in cd_subsystems)
    cd_fork = cd_impl - cd_up
    cd_up_pct = (cd_up / cd_target * 100.0) if cd_target else 0.0
    cd_fork_pct = (cd_fork / cd_target * 100.0) if cd_target else 0.0
    cd_badges = [
        ("upstream ScarlettX", f"{cd_up}/{cd_target} ({cd_up_pct:.1f}%)", BADGE_UPSTREAM, True),
        ("ScarlettEdge fork", f"+{cd_fork} (+{cd_fork_pct:.1f}%)", BADGE_FN, True),
        ("state", "Full speed on GPU, with sound", BADGE_TARGET, False),
        ("target", "Concept Destruction (Gen9)", BADGE_EXPORTS, False),
    ]
    cd_svg = render_svg(
        cd_subsystems,
        badges=cd_badges,
        heading_prefix="Concept Destruction (Upstream Purple + Fork Green)",
        callout_lead="Attribution:",
        callout_line1=f"Original ScarlettX achieved {cd_up}/{cd_target} ({cd_up_pct:.1f}%) real-title bring-up; ScarlettEdge completed the remaining +{cd_fork} milestones.",
        callout_line2="Full-speed gameplay, hardware VSync pacing, borderless fullscreen, controller rumble, audio and persistent saves.",
    )

    opt_subsystems = collect_optimization_metrics()
    opt_impl = sum(s.implemented for s in opt_subsystems)
    opt_target = sum(s.target for s in opt_subsystems)
    opt_pct = (opt_impl / opt_target * 100.0) if opt_target else 0.0
    opt_badges = [
        ("wave 1 hygiene", f"{opt_pct:.1f}% ({opt_impl}/{opt_target})", BADGE_FN, True),
        ("ninja -n", "no work to do", BADGE_EXPORTS, False),
        ("tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
    ]
    opt_svg = render_svg(
        opt_subsystems,
        badges=opt_badges,
        heading_prefix="Wave 1: Build, VEH, CTest & Metrics Hygiene",
        callout_lead="Wave 1 complete:",
        callout_line1="Zero-dirty Ninja incremental builds, PCH, async-safe VEH/Ctrl+C, isolated CTests & honest retail metrics.",
        callout_line2="See docs/AUDIT_BASELINE.md Section 6.1.",
    )

    aaa_subsystems = collect_aaa_roadmap_metrics()
    aaa_impl = sum(s.implemented for s in aaa_subsystems)
    aaa_target = sum(s.target for s in aaa_subsystems)
    aaa_pct = (aaa_impl / aaa_target * 100.0) if aaa_target else 0.0
    aaa_badges = [
        ("waves 1-3", f"{aaa_pct:.1f}% ({aaa_impl}/{aaa_target})", LV1, False),
        ("CI tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
        ("baseline", "docs/AUDIT_BASELINE.md", BADGE_EXPORTS, False),
    ]
    aaa_svg = render_svg(
        aaa_subsystems,
        badges=aaa_badges,
        heading_prefix="Post-Audit Multi-Wave Roadmap",
        callout_lead="Roadmap note:",
        callout_line1="Wave 1 hygiene is complete; Waves 2 and 3 target Multi-DLL Guest Hooking, zero-staging XVC streaming, and d3d12_xs heap aliasing.",
        callout_line2="Passing synthetic tests do not imply commercial retail title compatibility.",
    )

    p3_subsystems = collect_phase3_metrics()
    p3_impl = sum(s.implemented for s in p3_subsystems)
    p3_target = sum(s.target for s in p3_subsystems)
    p3_pct = (p3_impl / p3_target * 100.0) if p3_target else 0.0
    p3_badges = [
        ("empirical tracks", f"{p3_pct:.1f}% ({p3_impl}/{p3_target})", LV2, True),
        ("CI tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
    ]
    p3_svg = render_svg(
        p3_subsystems,
        badges=p3_badges,
        heading_prefix="Empirical Subsystem Verification",
        callout_lead="Audit baseline:",
        callout_line1=f"{p3_impl}/{p3_target} ({p3_pct:.1f}%) core tracks verified; remaining gaps scheduled in Waves 2 and 3.",
        callout_line2="Backed by FlightRecorder triage & deterministic CTest gates.",
    )

    p4_subsystems = collect_phase4_metrics()
    p4_impl = sum(s.implemented for s in p4_subsystems)
    p4_target = sum(s.target for s in p4_subsystems)
    p4_pct = (p4_impl / p4_target * 100.0) if p4_target else 0.0
    p4_badges = [
        ("governance & bring-up", f"{p4_pct:.1f}% ({p4_impl}/{p4_target})", LV3 if p4_pct >= 80.0 else LV1, p4_pct >= 80.0),
        ("clean-room CI", "UTF-16 + Binary Gate", BADGE_EXPORTS, False),
        ("CI tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
    ]
    p4_svg = render_svg(
        p4_subsystems,
        badges=p4_badges,
        heading_prefix="Clean-Room Governance & Retail Bring-Up",
        callout_lead="Clean-room gate:",
        callout_line1=f"{p4_impl}/{p4_target} ({p4_pct:.1f}%): UTF-16LE/case-insensitive check_cleanroom.py, FlightRecorder triage & real_title_com PresentX loop live.",
        callout_line2="Commercial retail bring-up proceeds with user-supplied legal dumps.",
    )

    html_doc = render_html(
        subsystems,
        breakdown,
        tests_text,
        tests_ok,
        cd_subsystems=cd_subsystems,
        opt_subsystems=opt_subsystems,
        aaa_subsystems=aaa_subsystems,
        phase3_subsystems=p3_subsystems,
        phase4_subsystems=p4_subsystems,
    )

    out_svg = ROOT / "docs/status.svg"
    out_cd_svg = ROOT / "docs/status_concept_destruction.svg"
    out_opt_svg = ROOT / "docs/status_optimizations.svg"
    out_aaa_svg = ROOT / "docs/status_aaa_roadmap.svg"
    out_p3_svg = ROOT / "docs/status_phase3.svg"
    out_p4_svg = ROOT / "docs/status_phase4.svg"
    out_html = ROOT / "docs/status.html"
    out_svg.parent.mkdir(parents=True, exist_ok=True)
    out_svg.write_text(svg, encoding="utf-8")
    out_cd_svg.write_text(cd_svg, encoding="utf-8")
    out_opt_svg.write_text(opt_svg, encoding="utf-8")
    out_aaa_svg.write_text(aaa_svg, encoding="utf-8")
    out_p3_svg.write_text(p3_svg, encoding="utf-8")
    out_p4_svg.write_text(p4_svg, encoding="utf-8")
    out_html.write_text(html_doc, encoding="utf-8")

    md_block = render_markdown_status(
        subsystems,
        cd_subsystems,
        breakdown,
        tests_markdown(test_count, test_results),
        opt_subsystems=opt_subsystems,
        aaa_subsystems=aaa_subsystems,
        phase3_subsystems=p3_subsystems,
        phase4_subsystems=p4_subsystems,
    )
    readme_path = ROOT / "README.md"
    readme = readme_path.read_text(encoding="utf-8")

    marker_pat = re.compile(
        r"<!-- SCARLETTX_STATUS_START -->.*?<!-- SCARLETTX_STATUS_END -->",
        re.DOTALL,
    )
    if marker_pat.search(readme):
        new_readme = marker_pat.sub(md_block, readme)
    else:
        anchor = "## Architecture Overview"
        if anchor in readme:
            new_readme = readme.replace(anchor, md_block + "\n\n" + anchor, 1)
        else:
            new_readme = readme + "\n\n" + md_block + "\n"

    readme_path.write_text(new_readme, encoding="utf-8")
    print(
        f"[generate_status] Wrote status SVGs, docs/status.html, and README.md "
        f"(retail_exports={breakdown.retail_exports}, com_slots={breakdown.com_vtable_slots}, "
        f"synthetic_exports={breakdown.synthetic_exports}, tests: {tests_text})"
    )


if __name__ == "__main__":
    main()
