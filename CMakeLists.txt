cmake_minimum_required(VERSION 3.20)

project(apitrace VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TOOLS "Build command-line tools" ON)
option(APITRACE_BUILD_BUNDLE_FINALIZE "Build offline trace bundle finalization tool" ON)
option(APITRACE_BUILD_D3D12_NATIVE_ASSET_DUMP "Build standalone D3D12 native asset dump diagnostic tool" OFF)
option(APITRACE_BUILD_WINDOWS_DLLS "Build Windows proxy DLLs when supported" ON)
option(APITRACE_BUILD_METAL_BACKEND "Build Metal backend sources when supported" ON)
# Produce the PE-side capture units (d3d11/d3d12 capture + core) as STATIC
# archives suitable for embedding into a host PE module (e.g. DXMT's own
# d3d11.dll / d3d12.dll). When OFF, the platform_windows_d3d11/d3d12/dxgi
# targets remain SHARED proxy DLLs as before.
option(APITRACE_BUILD_PE_CAPTURE_STATIC "Emit PE capture units as STATIC archives for embedding" OFF)

# Native macOS D3D retrace via DXMT dylibs. Disabled by default; turn ON
# together with APITRACE_DXMT_NATIVE_BUILD_DIR pointing at dxmt/build-gs-native.
option(APITRACE_BUILD_D3D_NATIVE_RETRACE "Enable native macOS D3D retrace via DXMT" OFF)
set(APITRACE_DXMT_NATIVE_BUILD_DIR "" CACHE PATH "DXMT build-gs-native directory")
set(APITRACE_DXC_INCLUDE_DIR "" CACHE PATH "DXC include directory for native D3D12 asset dump tooling")

if(APPLE AND NOT APITRACE_DXC_INCLUDE_DIR)
    set(_APITRACE_DEFAULT_DXC_INCLUDE_DIR "$ENV{HOME}/.local/opt/dxc-linux-1.9.2602/include")
    if(EXISTS "${_APITRACE_DEFAULT_DXC_INCLUDE_DIR}/dxc/dxcapi.h")
        set(APITRACE_DXC_INCLUDE_DIR
            "${_APITRACE_DEFAULT_DXC_INCLUDE_DIR}"
            CACHE PATH
            "DXC include directory for native D3D12 asset dump tooling"
            FORCE)
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Standalone D3D / DXGI headers vendored under third_party/d3d-headers/.
add_library(apitrace_d3d_headers INTERFACE)
target_include_directories(apitrace_d3d_headers
    INTERFACE
        $<$<NOT:$<PLATFORM_ID:Windows>>:${PROJECT_SOURCE_DIR}/third_party/d3d-headers/directx>
        $<$<NOT:$<PLATFORM_ID:Windows>>:${PROJECT_SOURCE_DIR}/third_party/d3d-headers/windows>
)

if(APITRACE_BUILD_D3D_NATIVE_RETRACE)
    if(NOT APPLE)
        message(FATAL_ERROR "APITRACE_BUILD_D3D_NATIVE_RETRACE is currently only supported on Apple platforms.")
    endif()
    include(find_dxmt_native)
    if(NOT APITRACE_DXMT_NATIVE_FOUND)
        message(FATAL_ERROR "APITRACE_BUILD_D3D_NATIVE_RETRACE=ON but DXMT native dylibs were not found. "
                "Set APITRACE_DXMT_NATIVE_BUILD_DIR to dxmt/build-gs-native.")
    endif()
endif()

set(APITRACE_PUBLIC_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src/api/include
    ${PROJECT_SOURCE_DIR}/src/trace/include
    ${PROJECT_SOURCE_DIR}/src/capture/include
    ${PROJECT_SOURCE_DIR}/src/retrace/include
    ${PROJECT_SOURCE_DIR}/src/d3d11/include
    ${PROJECT_SOURCE_DIR}/src/d3d12/include
    ${PROJECT_SOURCE_DIR}/src/metal/include
    ${PROJECT_SOURCE_DIR}/src/platform/macos/include
    ${PROJECT_SOURCE_DIR}/src/tools/apitrace/include
)

set(APITRACE_PRIVATE_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src
)

add_library(apitrace_nlohmann_json INTERFACE)
target_include_directories(apitrace_nlohmann_json
    INTERFACE
        ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include
)

set(APITRACE_CORE_SOURCES
    src/api/src/version.cpp
    src/capture/src/capture_hook_plan.cpp
    src/capture/src/capture_runtime.cpp
    src/capture/src/capture_runtime_state.cpp
    src/d3d11/src/d3d11_capture.cpp
    src/d3d11/src/d3d11_replay.cpp
    src/d3d11/src/d3d11_state.cpp
    src/d3d12/src/d3d12_capture.cpp
    src/d3d12/src/d3d12_replay.cpp
    src/d3d12/src/d3d12_state.cpp
    src/d3d12/src/d3d12_submission.cpp
    src/capture/src/trace_session_state.cpp
    src/retrace/src/replay_session.cpp
    src/retrace/src/metal_replay_backend_factory.cpp
    src/retrace/src/d3d11_replay_parser.cpp
    src/trace/src/metal_callstream_writer.cpp
    src/trace/src/payload_object_refs.cpp
    src/trace/src/trace_bundle_reader.cpp
    src/trace/src/trace_bundle_writer.cpp
    src/trace/src/translation_link_writer.cpp
    src/capture/src/trace_session.cpp
)

if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
    list(APPEND APITRACE_CORE_SOURCES
        src/metal/src/metal_diagnostics.cpp
        src/metal/src/metal_replay.cpp
        src/metal/src/metal_capi.cpp
        src/metal/src/metal_state.cpp
        src/metal/src/metal_trace.cpp
        src/metal/src/translation_trace_recorder.cpp
    )
endif()

add_library(apitrace_core ${APITRACE_CORE_SOURCES})

target_include_directories(apitrace_core
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
    PRIVATE
        ${APITRACE_PRIVATE_INCLUDE_DIRS}
)

target_compile_features(apitrace_core PUBLIC cxx_std_17)
target_link_libraries(apitrace_core PRIVATE apitrace_nlohmann_json apitrace_d3d_headers)

if(WIN32)
    target_compile_definitions(apitrace_core PUBLIC APITRACE_HAS_D3D_NATIVE=1)
endif()

if(APITRACE_BUILD_D3D_NATIVE_RETRACE)
    target_compile_definitions(apitrace_core PUBLIC APITRACE_HAS_D3D_NATIVE=1)
    target_link_libraries(apitrace_core PUBLIC
        apitrace_d3d_headers
        apitrace_dxmt::winemetal
        apitrace_dxmt::dxgi
        apitrace_dxmt::d3d11
        apitrace_dxmt::d3d12
    )
endif()

if(APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_library(apitrace_platform_windows_d3d11 STATIC
        src/d3d11/src/d3d11_proxy.cpp
        src/d3d11/src/d3d11_capture_internal.cpp
    )

    add_library(apitrace_platform_windows_d3d12 STATIC
        src/d3d12/src/d3d12_proxy.cpp
    )
else()
    add_library(apitrace_platform_windows_d3d11 SHARED
        src/d3d11/src/d3d11_proxy.cpp
        src/d3d11/src/d3d11_capture_internal.cpp
    )

    add_library(apitrace_platform_windows_d3d12 SHARED
        src/d3d12/src/d3d12_proxy.cpp
        src/d3d12/src/d3d12_proxy/d3d12.def
    )

    add_library(apitrace_platform_windows_dxgi SHARED
        src/dxgi/src/dxgi_proxy.cpp
        src/dxgi/src/dxgi.def
    )
endif()

add_library(apitrace_platform_apple_metal
    src/metal/src/metal_bridge.cpp
)

if(APPLE)
    add_library(apitrace_platform_macos
        src/platform/macos/src/macos_window.mm
    )
    target_include_directories(apitrace_platform_macos
        PUBLIC
            ${PROJECT_SOURCE_DIR}/src/platform/macos/include
        PRIVATE
            ${APITRACE_PRIVATE_INCLUDE_DIRS}
    )
    target_compile_features(apitrace_platform_macos PUBLIC cxx_std_17)
    set_source_files_properties(src/platform/macos/src/macos_window.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(apitrace_platform_macos PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
    )
endif()

target_include_directories(apitrace_platform_windows_d3d11
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
    PRIVATE
        ${APITRACE_PRIVATE_INCLUDE_DIRS}
)

target_include_directories(apitrace_platform_windows_d3d12
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
    PRIVATE
        ${APITRACE_PRIVATE_INCLUDE_DIRS}
)

if(TARGET apitrace_platform_windows_dxgi)
    target_include_directories(apitrace_platform_windows_dxgi
        PUBLIC
            ${APITRACE_PUBLIC_INCLUDE_DIRS}
        PRIVATE
            ${APITRACE_PRIVATE_INCLUDE_DIRS}
    )
endif()

target_include_directories(apitrace_platform_apple_metal
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
    PRIVATE
        ${APITRACE_PRIVATE_INCLUDE_DIRS}
)

target_link_libraries(apitrace_platform_windows_d3d11 PRIVATE apitrace_core)
target_link_libraries(apitrace_platform_windows_d3d12 PRIVATE apitrace_core apitrace_d3d_headers)
if(TARGET apitrace_platform_windows_dxgi)
    target_link_libraries(apitrace_platform_windows_dxgi PRIVATE apitrace_core apitrace_d3d_headers)
endif()
target_link_libraries(apitrace_platform_apple_metal PRIVATE apitrace_core)

target_compile_features(apitrace_platform_windows_d3d11 PUBLIC cxx_std_17)
target_compile_features(apitrace_platform_windows_d3d12 PUBLIC cxx_std_17)
if(TARGET apitrace_platform_windows_dxgi)
    target_compile_features(apitrace_platform_windows_dxgi PUBLIC cxx_std_17)
endif()
target_compile_features(apitrace_platform_apple_metal PUBLIC cxx_std_17)

if(WIN32 AND APITRACE_BUILD_WINDOWS_DLLS AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES OUTPUT_NAME d3d11 PREFIX "")
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES OUTPUT_NAME d3d12 PREFIX "")
    set_target_properties(apitrace_platform_windows_dxgi PROPERTIES OUTPUT_NAME dxgi PREFIX "")
    if(MINGW)
        target_link_options(apitrace_platform_windows_d3d12 PRIVATE -Wl,--exclude-all-symbols)
        target_link_options(apitrace_platform_windows_dxgi PRIVATE -Wl,--exclude-all-symbols)
    endif()
    if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_options(apitrace_platform_windows_d3d11 PRIVATE -Wl,--add-stdcall-alias)
        target_link_options(apitrace_platform_windows_d3d12 PRIVATE -Wl,--add-stdcall-alias)
        target_link_options(apitrace_platform_windows_dxgi PRIVATE -Wl,--add-stdcall-alias)
    endif()
elseif(APITRACE_BUILD_PE_CAPTURE_STATIC)
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES
        OUTPUT_NAME apitrace_d3d11_capture
    )
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES
        OUTPUT_NAME apitrace_d3d12_capture
    )
else()
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(TARGET apitrace_platform_windows_dxgi)
        set_target_properties(apitrace_platform_windows_dxgi PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

if(NOT APPLE OR NOT APITRACE_BUILD_METAL_BACKEND)
    set_target_properties(apitrace_platform_apple_metal PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
    enable_language(OBJCXX)
    target_compile_definitions(apitrace_core PRIVATE APITRACE_HAVE_METAL_BACKEND=1)
    target_sources(apitrace_platform_apple_metal PRIVATE
        src/metal/src/metal_replay_native.mm
    )
    set_source_files_properties(src/metal/src/metal_replay_native.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(apitrace_platform_apple_metal PRIVATE
        apitrace_nlohmann_json
        apitrace_platform_macos
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
    )
endif()

if(APITRACE_BUILD_TOOLS AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_executable(apitrace_retrace
        src/tools/retrace/src/standalone_main.cpp
        src/tools/retrace/src/main.cpp)
    set_target_properties(apitrace_retrace PROPERTIES OUTPUT_NAME retrace)
    target_link_libraries(apitrace_retrace PRIVATE apitrace_core)
    target_include_directories(apitrace_retrace PRIVATE ${APITRACE_PUBLIC_INCLUDE_DIRS})
    target_include_directories(apitrace_retrace PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_retrace PRIVATE cxx_std_17)
    if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
        target_link_libraries(apitrace_retrace PRIVATE apitrace_platform_apple_metal)
    endif()
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_sources(apitrace_retrace PRIVATE src/platform/macos/src/dxmt_macdrv_shim.mm)
        set_source_files_properties(src/platform/macos/src/dxmt_macdrv_shim.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(apitrace_retrace PRIVATE apitrace_platform_macos)
        target_link_options(apitrace_retrace PRIVATE
            -Wl,-exported_symbol,_get_win_data
            -Wl,-exported_symbol,_release_win_data
            -Wl,-exported_symbol,_macdrv_view_create_metal_view
            -Wl,-exported_symbol,_macdrv_view_get_metal_layer
            -Wl,-exported_symbol,_macdrv_view_release_metal_view
        )
    endif()

    add_executable(apitrace_bundle_check
        src/tools/bundle-check/src/standalone_main.cpp
        src/tools/bundle-check/src/main.cpp)
    set_target_properties(apitrace_bundle_check PROPERTIES OUTPUT_NAME bundle-check)
    target_link_libraries(apitrace_bundle_check PRIVATE apitrace_core apitrace_nlohmann_json)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_bundle_check PRIVATE apitrace_platform_macos)
    endif()
    target_include_directories(apitrace_bundle_check PRIVATE ${APITRACE_PUBLIC_INCLUDE_DIRS} ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_bundle_check PRIVATE cxx_std_17)
endif()

if(APITRACE_BUILD_TOOLS AND APITRACE_BUILD_BUNDLE_FINALIZE AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_executable(apitrace_bundle_finalize
        src/tools/bundle-finalize/src/standalone_main.cpp
        src/tools/bundle-finalize/src/main.cpp)
    set_target_properties(apitrace_bundle_finalize PROPERTIES OUTPUT_NAME bundle-finalize)
    target_link_libraries(apitrace_bundle_finalize PRIVATE apitrace_core apitrace_nlohmann_json)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_bundle_finalize PRIVATE apitrace_platform_macos)
    endif()
    target_include_directories(apitrace_bundle_finalize PRIVATE ${APITRACE_PUBLIC_INCLUDE_DIRS} ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_bundle_finalize PRIVATE cxx_std_17)
endif()

# Unified CLI: `apitrace <command>` dispatches to the same run_*() entry points as the standalone
# tools (bundle-finalize / bundle-check / retrace). Built only when all three tools are available.
if(APITRACE_BUILD_TOOLS AND APITRACE_BUILD_BUNDLE_FINALIZE AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_executable(apitrace_cli
        src/tools/apitrace/src/main.cpp
        src/tools/retrace/src/main.cpp
        src/tools/bundle-check/src/main.cpp
        src/tools/bundle-finalize/src/main.cpp)
    set_target_properties(apitrace_cli PROPERTIES OUTPUT_NAME apitrace)
    target_link_libraries(apitrace_cli PRIVATE apitrace_core apitrace_nlohmann_json)
    target_include_directories(apitrace_cli PRIVATE ${APITRACE_PUBLIC_INCLUDE_DIRS} ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_cli PRIVATE cxx_std_17)
    if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
        target_link_libraries(apitrace_cli PRIVATE apitrace_platform_apple_metal)
    endif()
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_sources(apitrace_cli PRIVATE src/platform/macos/src/dxmt_macdrv_shim.mm)
        set_source_files_properties(src/platform/macos/src/dxmt_macdrv_shim.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(apitrace_cli PRIVATE apitrace_platform_macos)
        target_link_options(apitrace_cli PRIVATE
            -Wl,-exported_symbol,_get_win_data
            -Wl,-exported_symbol,_release_win_data
            -Wl,-exported_symbol,_macdrv_view_create_metal_view
            -Wl,-exported_symbol,_macdrv_view_get_metal_layer
            -Wl,-exported_symbol,_macdrv_view_release_metal_view
        )
    endif()
endif()

if(NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_executable(apitrace_test_trace_writer_assets test/src/test_trace_writer_assets.cpp)
    target_link_libraries(apitrace_test_trace_writer_assets PRIVATE apitrace_core)
    target_compile_features(apitrace_test_trace_writer_assets PRIVATE cxx_std_17)

    add_executable(apitrace_test_bundle_finalize_integrity test/src/test_bundle_finalize_integrity.cpp)
    target_link_libraries(apitrace_test_bundle_finalize_integrity PRIVATE apitrace_core)
    target_compile_features(apitrace_test_bundle_finalize_integrity PRIVATE cxx_std_17)

    add_executable(apitrace_test_payload_object_refs test/src/test_payload_object_refs.cpp)
    target_link_libraries(apitrace_test_payload_object_refs PRIVATE apitrace_core apitrace_nlohmann_json)
    target_include_directories(apitrace_test_payload_object_refs PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_payload_object_refs PRIVATE cxx_std_17)

    add_executable(apitrace_test_d3d12_capture_api test/src/test_d3d12_capture_api.cpp)
    target_link_libraries(apitrace_test_d3d12_capture_api PRIVATE apitrace_core apitrace_nlohmann_json apitrace_d3d_headers)
    target_compile_features(apitrace_test_d3d12_capture_api PRIVATE cxx_std_17)

    add_executable(apitrace_test_d3d11_replay_semantics test/src/test_d3d11_replay_semantics.cpp)
    target_link_libraries(apitrace_test_d3d11_replay_semantics PRIVATE apitrace_core)
    target_include_directories(apitrace_test_d3d11_replay_semantics PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_d3d11_replay_semantics PRIVATE cxx_std_17)

    add_executable(apitrace_test_d3d12_replay_semantics test/src/test_d3d12_replay_semantics.cpp)
    target_link_libraries(apitrace_test_d3d12_replay_semantics PRIVATE apitrace_core)
    target_compile_definitions(apitrace_test_d3d12_replay_semantics PRIVATE APITRACE_ENABLE_TEST_HOOKS)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_test_d3d12_replay_semantics PRIVATE apitrace_platform_macos)
    endif()
    target_compile_features(apitrace_test_d3d12_replay_semantics PRIVATE cxx_std_17)

    add_executable(apitrace_test_d3d12_replay_model_roundtrip test/src/test_d3d12_replay_model_roundtrip.cpp)
    target_link_libraries(apitrace_test_d3d12_replay_model_roundtrip PRIVATE apitrace_core apitrace_nlohmann_json)
    target_compile_definitions(apitrace_test_d3d12_replay_model_roundtrip PRIVATE APITRACE_ENABLE_TEST_HOOKS)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_test_d3d12_replay_model_roundtrip PRIVATE apitrace_platform_macos)
    endif()
    target_include_directories(apitrace_test_d3d12_replay_model_roundtrip PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_d3d12_replay_model_roundtrip PRIVATE cxx_std_17)

    add_executable(apitrace_test_d3d12_native_replay_smoke test/src/test_d3d12_native_replay_smoke.cpp)
    target_link_libraries(apitrace_test_d3d12_native_replay_smoke PRIVATE apitrace_core)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_test_d3d12_native_replay_smoke PRIVATE apitrace_platform_macos)
    endif()
    target_compile_features(apitrace_test_d3d12_native_replay_smoke PRIVATE cxx_std_17)

    if(APPLE AND APITRACE_BUILD_D3D12_NATIVE_ASSET_DUMP AND APITRACE_DXC_INCLUDE_DIR)
        add_executable(apitrace_d3d12_native_asset_dump test/src/d3d12_native_asset_dump.cpp)
        target_include_directories(apitrace_d3d12_native_asset_dump PRIVATE "${APITRACE_DXC_INCLUDE_DIR}")
        target_compile_features(apitrace_d3d12_native_asset_dump PRIVATE cxx_std_17)
        target_compile_options(apitrace_d3d12_native_asset_dump PRIVATE -arch x86_64)
        target_link_options(apitrace_d3d12_native_asset_dump PRIVATE -arch x86_64)
        set_target_properties(apitrace_d3d12_native_asset_dump PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    add_executable(apitrace_test_metal_callstream_roundtrip test/src/test_metal_callstream_roundtrip.cpp)
    target_link_libraries(apitrace_test_metal_callstream_roundtrip PRIVATE apitrace_core)
    target_include_directories(apitrace_test_metal_callstream_roundtrip PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_metal_callstream_roundtrip PRIVATE cxx_std_17)

    add_executable(apitrace_test_metal_replay_closure test/src/test_metal_replay_closure.cpp)
    target_link_libraries(apitrace_test_metal_replay_closure PRIVATE apitrace_core apitrace_nlohmann_json)
    if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
        target_link_libraries(apitrace_test_metal_replay_closure PRIVATE apitrace_platform_apple_metal)
    endif()
    target_include_directories(apitrace_test_metal_replay_closure PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_metal_replay_closure PRIVATE cxx_std_17)

    if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
        add_executable(apitrace_test_metal_native_replay_smoke test/src/test_metal_native_replay_smoke.mm)
        target_link_libraries(apitrace_test_metal_native_replay_smoke PRIVATE
            apitrace_core
            apitrace_platform_apple_metal
            "-framework Foundation"
            "-framework Metal"
        )
        target_compile_features(apitrace_test_metal_native_replay_smoke PRIVATE cxx_std_17)
        set_source_files_properties(test/src/test_metal_native_replay_smoke.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()

    add_executable(apitrace_test_cross_api_bundle_closure test/src/test_cross_api_bundle_closure.cpp)
    target_link_libraries(apitrace_test_cross_api_bundle_closure PRIVATE apitrace_core apitrace_nlohmann_json)
    if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
        target_link_libraries(apitrace_test_cross_api_bundle_closure PRIVATE apitrace_platform_macos)
    endif()
    target_include_directories(apitrace_test_cross_api_bundle_closure PRIVATE ${APITRACE_PRIVATE_INCLUDE_DIRS})
    target_compile_features(apitrace_test_cross_api_bundle_closure PRIVATE cxx_std_17)
endif()

if(WIN32)
    if(APITRACE_BUILD_PE_CAPTURE_STATIC)
        # Static archives are linked into a host PE module (DXMT's own
        # d3d11.dll / d3d12.dll). The host already imports d3d11/d3d12/dxgi/uuid
        # at its final link step, so do not redeclare those imports here.
    else()
        target_link_libraries(apitrace_core PRIVATE d3d11 d3d12 dxgi user32)
        target_link_libraries(apitrace_platform_windows_d3d12 PRIVATE uuid)
    endif()
endif()

if(APITRACE_BUILD_PE_CAPTURE_STATIC)
    add_library(apitrace_d3d_capture INTERFACE)
    target_link_libraries(apitrace_d3d_capture
        INTERFACE
            apitrace_platform_windows_d3d11
            apitrace_platform_windows_d3d12
            apitrace_core
    )
    target_include_directories(apitrace_d3d_capture
        INTERFACE
            ${APITRACE_PUBLIC_INCLUDE_DIRS}
    )
endif()

install(TARGETS apitrace_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(APITRACE_BUILD_TOOLS AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    install(TARGETS apitrace_retrace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(APITRACE_BUILD_TOOLS AND APITRACE_BUILD_BUNDLE_FINALIZE AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    install(TARGETS apitrace_bundle_finalize RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS apitrace_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WIN32 AND APITRACE_BUILD_WINDOWS_DLLS AND NOT APITRACE_BUILD_PE_CAPTURE_STATIC)
    install(TARGETS apitrace_platform_windows_d3d11
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(TARGETS apitrace_platform_windows_d3d12
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(TARGETS apitrace_platform_windows_dxgi
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(APITRACE_BUILD_PE_CAPTURE_STATIC)
    install(TARGETS apitrace_core apitrace_platform_windows_d3d11 apitrace_platform_windows_d3d12
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(APPLE AND APITRACE_BUILD_METAL_BACKEND)
    install(TARGETS apitrace_platform_apple_metal
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

install(DIRECTORY src/api/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/trace/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/capture/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/retrace/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/d3d11/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/d3d12/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/metal/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(APPLE AND APITRACE_BUILD_D3D_NATIVE_RETRACE)
    add_subdirectory(test/abi-check)
endif()
