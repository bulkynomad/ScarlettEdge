cmake_minimum_required(VERSION 3.20)
project(ScarlettX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Place shared libraries and executables in deterministic directories across both
# single-config (Ninja) and multi-config (Visual Studio) CMake generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_sx_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_sx_cfg} "${CMAKE_BINARY_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_sx_cfg} "${CMAKE_BINARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_sx_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /wd4100 /wd4201 /wd4324)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Core emulator shared library (single process-wide singleton state across scarlettx.exe and all 22 shim DLLs)
add_library(scarlettx_core SHARED
    src/core/flight_recorder.cpp
    src/core/flight_triage.cpp
    src/core/status.cpp
    src/loader/game_config.cpp
    src/loader/pe_loader.cpp
    src/loader/pe_mapper.cpp
    src/loader/guest_modules.cpp
    src/loader/stub_thunk.cpp
    src/loader/package_vfs.cpp
    src/loader/xvd_container.cpp
    src/kernel/xmemory.cpp
    src/kernel/xruntime.cpp
    src/kernel/xstorage.cpp
    src/gpu/d3d12_backend.cpp
    src/gpu/rt_bvh.cpp
    src/gpu/shader_ir.cpp
    src/gpu/xg_layout.cpp
    src/runtime/emulator.cpp
    src/runtime/virtual_drive.cpp
)
set_target_properties(scarlettx_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME "scarlettx_core"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(scarlettx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(scarlettx_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        SCARLETTX_PLATFORM_WINDOWS=1
    )
    if(MSVC)
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler
            ole32
            uuid
        )
    else()
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler_47
            ole32
            uuid
        )
        # Also the thread runtime (libwinpthread): a shim that needs a DLL from
        # the toolchain only loads while the toolchain is on PATH.
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

# ---------------------------------------------------------------------------
# Scarlett HLE System DLL Shims
# ---------------------------------------------------------------------------
add_library(d3d12_xs SHARED src/shims/d3d12_xs_shim.cpp src/shims/d3d12_xs_exports.cpp)
set_target_properties(d3d12_xs PROPERTIES PREFIX "" OUTPUT_NAME "d3d12_xs")
target_link_libraries(d3d12_xs PRIVATE scarlettx_core $<$<BOOL:${WIN32}>:user32>)
target_compile_definitions(d3d12_xs PRIVATE SCARLETTX_BUILDING_D3D12_XS=1)

add_library(xg_xs SHARED src/shims/xg_xs_shim.cpp)
set_target_properties(xg_xs PROPERTIES PREFIX "" OUTPUT_NAME "xg_xs")
target_link_libraries(xg_xs PRIVATE scarlettx_core)
target_compile_definitions(xg_xs PRIVATE SCARLETTX_BUILDING_XG_XS=1)

add_library(AcpHal SHARED src/shims/acphal_shim.cpp)
set_target_properties(AcpHal PROPERTIES PREFIX "" OUTPUT_NAME "AcpHal")
target_link_libraries(AcpHal PRIVATE scarlettx_core)
target_compile_definitions(AcpHal PRIVATE SCARLETTX_BUILDING_ACPHAL=1)

add_library(xgameruntime SHARED
    src/shims/xgameruntime_shim.cpp
    src/shims/xgr_api_tables.cpp
    src/shims/xgr_task_queue.cpp)
set_target_properties(xgameruntime PROPERTIES PREFIX "" OUTPUT_NAME "xgameruntime")
target_link_libraries(xgameruntime PRIVATE scarlettx_core)
target_compile_definitions(xgameruntime PRIVATE SCARLETTX_BUILDING_XGAMERUNTIME=1)

add_library(XboxBvhBuilder SHARED src/shims/xbox_bvh_builder_shim.cpp)
set_target_properties(XboxBvhBuilder PROPERTIES PREFIX "" OUTPUT_NAME "XboxBvhBuilder")
target_link_libraries(XboxBvhBuilder PRIVATE scarlettx_core)
target_compile_definitions(XboxBvhBuilder PRIVATE SCARLETTX_BUILDING_XBOX_BVH=1)

add_library(xcrdapi SHARED src/shims/dstorage_guest.cpp)
set_target_properties(xcrdapi PROPERTIES PREFIX "" OUTPUT_NAME "xcrdapi")
target_link_libraries(xcrdapi PRIVATE scarlettx_core xbox_codecs)

add_library(dstorage SHARED src/shims/dstorage_shim.cpp)
set_target_properties(dstorage PROPERTIES PREFIX "" OUTPUT_NAME "dstorage")
target_link_libraries(dstorage PRIVATE scarlettx_core xbox_codecs)
target_compile_definitions(dstorage PRIVATE SCARLETTX_BUILDING_DSTORAGE=1)

add_library(dstorage_xs SHARED src/shims/dstorage_xs_forward.cpp src/shims/dstorage_xs.def)
set_target_properties(dstorage_xs PROPERTIES PREFIX "" OUTPUT_NAME "dstorage_xs")
target_link_libraries(dstorage_xs PRIVATE scarlettx_core dstorage xcrdapi)

add_library(xaudio2_9 SHARED src/shims/xaudio2_shim.cpp src/shims/xaudio2_guest.cpp src/shims/xaudio2_null_output.cpp src/shims/xaudio2_9.def)
set_target_properties(xaudio2_9 PROPERTIES PREFIX "" OUTPUT_NAME "xaudio2_9")
target_link_libraries(xaudio2_9 PRIVATE scarlettx_core)
target_compile_definitions(xaudio2_9 PRIVATE SCARLETTX_BUILDING_XAUDIO2=1)

add_library(GameInput SHARED src/shims/gameinput_shim.cpp)
set_target_properties(GameInput PROPERTIES PREFIX "" OUTPUT_NAME "GameInput")
target_link_libraries(GameInput PRIVATE scarlettx_core $<$<BOOL:${WIN32}>:user32>)
target_compile_definitions(GameInput PRIVATE SCARLETTX_BUILDING_GAMEINPUT=1)

add_library(Bink2Scarlett SHARED src/shims/bink2_scarlett_shim.cpp)
set_target_properties(Bink2Scarlett PROPERTIES PREFIX "" OUTPUT_NAME "Bink2Scarlett")
target_link_libraries(Bink2Scarlett PRIVATE scarlettx_core)
target_compile_definitions(Bink2Scarlett PRIVATE SCARLETTX_BUILDING_BINK2=1)

add_library(amd_fidelityfx_upscaler_xbox SHARED src/shims/ffx_upscaler_shim.cpp)
set_target_properties(amd_fidelityfx_upscaler_xbox PROPERTIES PREFIX "" OUTPUT_NAME "amd_fidelityfx_upscaler_xbox")
target_link_libraries(amd_fidelityfx_upscaler_xbox PRIVATE scarlettx_core)
target_compile_definitions(amd_fidelityfx_upscaler_xbox PRIVATE SCARLETTX_BUILDING_FFX_UPSCALER=1)

add_library(amd_fidelityfx_loader_xbox SHARED src/shims/ffx_loader_shim.cpp)
set_target_properties(amd_fidelityfx_loader_xbox PROPERTIES PREFIX "" OUTPUT_NAME "amd_fidelityfx_loader_xbox")
target_link_libraries(amd_fidelityfx_loader_xbox PRIVATE scarlettx_core amd_fidelityfx_upscaler_xbox)
target_compile_definitions(amd_fidelityfx_loader_xbox PRIVATE SCARLETTX_BUILDING_FFX_LOADER=1)

add_library(libHttpClient_GDK SHARED src/shims/libhttpclient_shim.cpp)
set_target_properties(libHttpClient_GDK PROPERTIES PREFIX "" OUTPUT_NAME "libHttpClient.GDK")
target_link_libraries(libHttpClient_GDK PRIVATE scarlettx_core xgameruntime)
target_compile_definitions(libHttpClient_GDK PRIVATE SCARLETTX_BUILDING_LIBHTTPCLIENT=1)

add_library(XCurl SHARED src/shims/xcurl_shim.cpp)
set_target_properties(XCurl PROPERTIES PREFIX "" OUTPUT_NAME "XCurl")
target_link_libraries(XCurl PRIVATE scarlettx_core)
target_compile_definitions(XCurl PRIVATE SCARLETTX_BUILDING_XCURL=1)

add_library(kernelx SHARED src/kernel/kernelx_shim.cpp)
set_target_properties(kernelx PROPERTIES PREFIX "" OUTPUT_NAME "kernelx")
target_link_libraries(kernelx PRIVATE scarlettx_core)
target_compile_definitions(kernelx PRIVATE SCARLETTX_BUILDING_KERNELX=1)

add_library(xvc_crypto SHARED src/loader/xvc_crypto.cpp)
set_target_properties(xvc_crypto PROPERTIES PREFIX "" OUTPUT_NAME "xvc_crypto")
target_link_libraries(xvc_crypto PRIVATE scarlettx_core)
target_compile_definitions(xvc_crypto PRIVATE SCARLETTX_BUILDING_XVC_CRYPTO=1)

add_library(xbox_codecs SHARED src/shims/codecs_shim.cpp)
set_target_properties(xbox_codecs PROPERTIES PREFIX "" OUTPUT_NAME "xbox_codecs")
target_link_libraries(xbox_codecs PRIVATE scarlettx_core)
target_compile_definitions(xbox_codecs PRIVATE SCARLETTX_BUILDING_CODECS=1)

add_library(xsapi SHARED src/shims/xsapi_shim.cpp)
set_target_properties(xsapi PROPERTIES PREFIX "" OUTPUT_NAME "xsapi")
target_link_libraries(xsapi PRIVATE scarlettx_core xgameruntime)
target_compile_definitions(xsapi PRIVATE SCARLETTX_BUILDING_XSAPI=1)

add_library(xs2_lifting SHARED src/gpu/xs2_lifting_shim.cpp)
set_target_properties(xs2_lifting PROPERTIES PREFIX "" OUTPUT_NAME "xs2_lifting")
target_link_libraries(xs2_lifting PRIVATE scarlettx_core)
target_compile_definitions(xs2_lifting PRIVATE SCARLETTX_BUILDING_XS2=1)

add_library(vulkan_backend SHARED src/gpu/vulkan_backend_shim.cpp)
set_target_properties(vulkan_backend PROPERTIES PREFIX "" OUTPUT_NAME "vulkan_backend")
target_link_libraries(vulkan_backend PRIVATE scarlettx_core)
target_compile_definitions(vulkan_backend PRIVATE SCARLETTX_BUILDING_VULKAN=1)

add_library(pixevt_stub SHARED src/shims/pixevt_stub.cpp)
set_target_properties(pixevt_stub PROPERTIES PREFIX "" OUTPUT_NAME "pixevt_stub")

# ---------------------------------------------------------------------------
# ScarlettX CLI Emulator Runner
# ---------------------------------------------------------------------------
add_executable(scarlettx src/main.cpp)
target_link_libraries(scarlettx PRIVATE scarlettx_core)
add_dependencies(scarlettx
    d3d12_xs
    xg_xs
    AcpHal
    xgameruntime
    XboxBvhBuilder
    dstorage
    dstorage_xs
    xcrdapi
    xaudio2_9
    GameInput
    Bink2Scarlett
    amd_fidelityfx_upscaler_xbox
    amd_fidelityfx_loader_xbox
    libHttpClient_GDK
    XCurl
    kernelx
    xvc_crypto
    xbox_codecs
    xsapi
    xs2_lifting
    vulkan_backend
    pixevt_stub
)

# ---------------------------------------------------------------------------
# Synthetic Scarlett GDK Test Games (compile as real x86-64 PEs linking HLE DLLs)
# ---------------------------------------------------------------------------
# Helper to keep synthetic package output directories identical under multi-config generators
function(scarlettx_set_package_output target_name pkg_subdir)
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${pkg_subdir}"
    )
    foreach(_sx_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_sx_cfg} "${CMAKE_BINARY_DIR}/packages/${pkg_subdir}"
        )
    endforeach()
endfunction()

add_executable(hello_scarlett tests/synthetic_games/hello_scarlett/main.cpp)
target_link_libraries(hello_scarlett PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(hello_scarlett PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
scarlettx_set_package_output(hello_scarlett hello_scarlett)
add_custom_command(TARGET hello_scarlett POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/hello_scarlett/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:hello_scarlett>/MicrosoftGame.Config"
)

add_executable(rt_showcase tests/synthetic_games/rt_showcase/main.cpp)
target_link_libraries(rt_showcase PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(rt_showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
scarlettx_set_package_output(rt_showcase rt_showcase)
add_custom_command(TARGET rt_showcase POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/rt_showcase/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:rt_showcase>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 3: iat_probe (in-process PE64 + ordinal + auto-stub test target)
if(WIN32)
    function(scarlettx_add_dummy_import_lib target_name c_src def_file)
        add_library(${target_name} SHARED ${c_src} ${def_file})
        set_target_properties(${target_name} PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_dlls"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_libs"
        )
        # Remove the dummy runtime DLL so it cannot be loaded by Windows LoadLibrary;
        # only its import library (.lib/.dll.a) is used at link time.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove -f "$<TARGET_FILE:${target_name}>"
        )
    endfunction()

    scarlettx_add_dummy_import_lib(xordinal_hle
        tests/synthetic_games/iat_probe/xordinal_hle_stub.c
        tests/synthetic_games/iat_probe/xordinal_hle.def
    )
    scarlettx_add_dummy_import_lib(xgameoverlay_missing
        tests/synthetic_games/iat_probe/xgameoverlay_missing_stub.c
        tests/synthetic_games/iat_probe/xgameoverlay_missing.def
    )
    scarlettx_add_dummy_import_lib(xtelemetry_missing
        tests/synthetic_games/iat_probe/xtelemetry_missing_stub.c
        tests/synthetic_games/iat_probe/xtelemetry_missing.def
    )

    add_library(iat_probe SHARED
        tests/synthetic_games/iat_probe/iat_probe.cpp
    )
    target_link_libraries(iat_probe PRIVATE
        xgameruntime
        xordinal_hle
        xgameoverlay_missing
        xtelemetry_missing
    )
    set_target_properties(iat_probe PROPERTIES PREFIX "")
    scarlettx_set_package_output(iat_probe iat_probe)
    add_custom_command(TARGET iat_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/iat_probe/MicrosoftGame.Config"
            "$<TARGET_FILE_DIR:iat_probe>/MicrosoftGame.Config"
    )
endif()

# Synthetic Scarlett Game 4: scarlett_arena (full-stack Gen9 E2E demo)
add_executable(scarlett_arena tests/synthetic_games/scarlett_arena/main.cpp)
target_link_libraries(scarlett_arena PRIVATE
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    xaudio2_9
    GameInput
)
target_include_directories(scarlett_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MINGW)
    target_link_options(scarlett_arena PRIVATE -Wl,--dynamicbase)
endif()
scarlettx_set_package_output(scarlett_arena scarlett_arena)
add_custom_command(TARGET scarlett_arena POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/scarlett_arena/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:scarlett_arena>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 5: ue5_slice (Encrypted GDFX .xvc Gen9 UE5 vertical slice)
add_executable(ue5_slice tests/synthetic_games/ue5_slice/main.cpp)
target_link_libraries(ue5_slice PRIVATE
    xgameruntime
    d3d12_xs
    dstorage
    xbox_codecs
    xaudio2_9
    Bink2Scarlett
    xsapi
    $<$<BOOL:${WIN32}>:ole32>
)
target_include_directories(ue5_slice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
scarlettx_set_package_output(ue5_slice ue5_slice)
add_custom_command(TARGET ue5_slice POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/ue5_slice/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:ue5_slice>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 6: openworld_slice (Open-World Gen9 Wave32 Vulkan + 2MB Alias vertical slice)
add_executable(openworld_slice tests/synthetic_games/openworld_slice/main.cpp)
target_link_libraries(openworld_slice PRIVATE
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    kernelx
    xs2_lifting
    vulkan_backend
    xsapi
    libHttpClient_GDK
)
target_include_directories(openworld_slice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
scarlettx_set_package_output(openworld_slice openworld_slice)
add_custom_command(TARGET openworld_slice POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/openworld_slice/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:openworld_slice>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 7: real_title_com (True mainCRTStartup SCARLETTX_REAL_TITLE=1 COM vertical slice)
add_executable(real_title_com tests/synthetic_games/real_title_com/main.cpp)
target_link_libraries(real_title_com PRIVATE
    xgameruntime
    d3d12_xs
    xcrdapi
    xaudio2_9
    GameInput
    kernelx
    libHttpClient_GDK
    $<$<BOOL:${WIN32}>:ole32>
)
target_include_directories(real_title_com PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
scarlettx_set_package_output(real_title_com real_title_com)
add_custom_command(TARGET real_title_com POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/real_title_com/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:real_title_com>/MicrosoftGame.Config"
)

# ---------------------------------------------------------------------------
# Autonomous Test Suite
# ---------------------------------------------------------------------------
function(scarlettx_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE scarlettx_core)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(${name} PRIVATE
        SCARLETTX_BINARY_DIR="$<TARGET_FILE_DIR:scarlettx_core>"
        SCARLETTX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

scarlettx_add_test(test_game_config tests/test_game_config.cpp)
scarlettx_add_test(test_pe_loader tests/test_pe_loader.cpp)
add_dependencies(test_pe_loader hello_scarlett rt_showcase)

if(WIN32)
    scarlettx_add_test(test_inproc_loader tests/test_inproc_loader.cpp)
    add_dependencies(test_inproc_loader iat_probe xgameruntime xvc_crypto pixevt_stub)
    set_tests_properties(test_inproc_loader PROPERTIES RESOURCE_LOCK "virtual_drive_g")
    scarlettx_add_test(test_guest_loader tests/test_guest_loader.cpp)
endif()

scarlettx_add_test(test_kernel_hle tests/test_kernel_hle.cpp)
scarlettx_add_test(test_xgameruntime_abi tests/test_xgameruntime_abi.cpp)
target_link_libraries(test_xgameruntime_abi PRIVATE xgameruntime)
scarlettx_add_test(test_query_api_impl tests/test_query_api_impl.cpp)
target_link_libraries(test_query_api_impl PRIVATE xgameruntime)
scarlettx_add_test(test_xtaskqueue_xasync tests/test_xtaskqueue_xasync.cpp)
target_link_libraries(test_xtaskqueue_xasync PRIVATE xgameruntime)
scarlettx_add_test(test_d3d12_warp tests/test_d3d12_warp.cpp)
scarlettx_add_test(test_xg_layout tests/test_xg_layout.cpp)
target_link_libraries(test_xg_layout PRIVATE xg_xs)
scarlettx_add_test(test_d3d12_guest_texture tests/test_d3d12_guest_texture.cpp)
target_link_libraries(test_d3d12_guest_texture PRIVATE d3d12_xs kernelx)
set_tests_properties(test_d3d12_guest_texture PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_list_layout tests/test_d3d12_list_layout.cpp)
target_link_libraries(test_d3d12_list_layout PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_list_layout PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_present_window tests/test_d3d12_present_window.cpp)
target_link_libraries(test_d3d12_present_window PRIVATE d3d12_xs $<$<BOOL:${WIN32}>:user32>)
set_tests_properties(test_d3d12_present_window PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_frame_pacing tests/test_d3d12_frame_pacing.cpp)
target_link_libraries(test_d3d12_frame_pacing PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_frame_pacing PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_xs_abi tests/test_d3d12_xs_abi.cpp)
target_link_libraries(test_d3d12_xs_abi PRIVATE d3d12_xs XboxBvhBuilder)
scarlettx_add_test(test_rt_pipeline tests/test_rt_pipeline.cpp)
scarlettx_add_test(test_dstorage_xvd tests/test_dstorage_xvd.cpp)
target_link_libraries(test_dstorage_xvd PRIVATE dstorage)
scarlettx_add_test(test_xaudio2 tests/test_xaudio2.cpp)
target_link_libraries(test_xaudio2 PRIVATE xaudio2_9)
scarlettx_add_test(test_xaudio2_guest tests/test_xaudio2_guest.cpp)
target_link_libraries(test_xaudio2_guest PRIVATE xaudio2_9)
scarlettx_add_test(test_dstorage_guest tests/test_dstorage_guest.cpp)
target_link_libraries(test_dstorage_guest PRIVATE xcrdapi)
scarlettx_add_test(test_gameinput tests/test_gameinput.cpp)
target_link_libraries(test_gameinput PRIVATE GameInput)

scarlettx_add_test(test_shader_pipeline tests/test_shader_pipeline.cpp)
target_link_libraries(test_shader_pipeline PRIVATE d3d12_xs)

scarlettx_add_test(test_middleware_net tests/test_middleware_net.cpp)
target_link_libraries(test_middleware_net PRIVATE
    Bink2Scarlett
    amd_fidelityfx_loader_xbox
    amd_fidelityfx_upscaler_xbox
    libHttpClient_GDK
    XCurl
    xgameruntime
)

scarlettx_add_test(test_kernelx tests/test_kernelx.cpp)
target_link_libraries(test_kernelx PRIVATE kernelx)

scarlettx_add_test(test_xvc_crypto tests/test_xvc_crypto.cpp)
target_link_libraries(test_xvc_crypto PRIVATE xvc_crypto)

scarlettx_add_test(test_codecs tests/test_codecs.cpp)
target_link_libraries(test_codecs PRIVATE xbox_codecs)

scarlettx_add_test(test_xsapi tests/test_xsapi.cpp)
target_link_libraries(test_xsapi PRIVATE xsapi xgameruntime)
set_tests_properties(test_xsapi PROPERTIES RESOURCE_LOCK "temp_saves")

scarlettx_add_test(test_xs2_lifting tests/test_xs2_lifting.cpp)
target_link_libraries(test_xs2_lifting PRIVATE xs2_lifting)

scarlettx_add_test(test_vulkan_backend tests/test_vulkan_backend.cpp)
target_link_libraries(test_vulkan_backend PRIVATE vulkan_backend xs2_lifting $<$<BOOL:${WIN32}>:user32>)

scarlettx_add_test(test_scarlett_arena_e2e tests/test_scarlett_arena_e2e.cpp)
target_link_libraries(test_scarlett_arena_e2e PRIVATE dstorage)
add_dependencies(test_scarlett_arena_e2e
    scarlett_arena
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    dstorage_xs
    xcrdapi
    xbox_codecs
    xaudio2_9
    GameInput
)
set_tests_properties(test_scarlett_arena_e2e PROPERTIES RESOURCE_LOCK "virtual_drive_g;temp_saves")

add_test(
    NAME test_boot_hello_scarlett
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/hello_scarlett" --headless --warp --frames 2
)
set_tests_properties(test_boot_hello_scarlett PROPERTIES RESOURCE_LOCK "virtual_drive_g")
add_test(
    NAME test_boot_rt_showcase
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/rt_showcase" --headless --warp --frames 2 --rt-dump "${CMAKE_BINARY_DIR}/rt_showcase_frame.ppm"
)
set_tests_properties(test_boot_rt_showcase PROPERTIES RESOURCE_LOCK "virtual_drive_g")

scarlettx_add_test(test_aaa_bringup_e2e tests/test_aaa_bringup_e2e.cpp)
target_link_libraries(test_aaa_bringup_e2e PRIVATE dstorage xbox_codecs xaudio2_9)
add_dependencies(test_aaa_bringup_e2e
    ue5_slice
    openworld_slice
    scarlettx
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    dstorage_xs
    xcrdapi
    xbox_codecs
    xaudio2_9
    Bink2Scarlett
    kernelx
    xs2_lifting
    vulkan_backend
    xsapi
    libHttpClient_GDK
)
set_tests_properties(test_aaa_bringup_e2e PROPERTIES RESOURCE_LOCK "virtual_drive_g;temp_saves")

scarlettx_add_test(test_retail_parity tests/test_retail_parity.cpp)
target_link_libraries(test_retail_parity PRIVATE
    dstorage
    xbox_codecs
    xaudio2_9
    xs2_lifting
    AcpHal
)
add_dependencies(test_retail_parity hello_scarlett pixevt_stub)

scarlettx_add_test(test_real_title_com_e2e tests/test_real_title_com_e2e.cpp)
add_dependencies(test_real_title_com_e2e
    real_title_com
    xgameruntime
    d3d12_xs
    xcrdapi
    dstorage_xs
    xaudio2_9
    GameInput
    kernelx
    libHttpClient_GDK
    xsapi
)
set_tests_properties(test_real_title_com_e2e PROPERTIES RESOURCE_LOCK "virtual_drive_g;temp_saves")

scarlettx_add_test(test_flight_recorder tests/test_flight_recorder.cpp)
target_link_libraries(test_flight_recorder PRIVATE
    d3d12_xs
    xgameruntime
    GameInput
    xcrdapi
)
add_dependencies(test_flight_recorder scarlettx hello_scarlett)

scarlettx_add_test(test_triage_cli tests/test_triage_cli.cpp)

scarlettx_add_test(test_engine_hardening tests/test_engine_hardening.cpp)
target_link_libraries(test_engine_hardening PRIVATE
    xs2_lifting
    kernelx
    xbox_codecs
)

scarlettx_add_test(test_d3d12_xs_proxy_rtx tests/test_d3d12_xs_proxy_rtx.cpp)
target_link_libraries(test_d3d12_xs_proxy_rtx PRIVATE
    d3d12_xs
    d3d12
    d3dcompiler
)

scarlettx_add_test(test_d3d12_xs_nextgen_rtx tests/test_d3d12_xs_nextgen_rtx.cpp)
target_link_libraries(test_d3d12_xs_nextgen_rtx PRIVATE
    d3d12_xs
    d3d12
)

scarlettx_add_test(test_winrt_middleware tests/test_winrt_middleware.cpp)
target_link_libraries(test_winrt_middleware PRIVATE
    GameInput
    xsapi
    xgameruntime
    $<$<BOOL:${WIN32}>:ole32>
)

scarlettx_add_test(test_diag_full tests/test_diag_full.cpp)
add_dependencies(test_diag_full scarlettx)
