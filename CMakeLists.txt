cmake_minimum_required(VERSION 3.20)

project(apitrace VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TOOLS "Build command-line tools" ON)
option(APITRACE_BUILD_WINDOWS_DLLS "Build Windows proxy DLLs when supported" ON)
option(APITRACE_BUILD_METAL_BACKEND "Build Metal backend sources when supported" ON)

# Native macOS D3D retrace via DXMT dylibs. Disabled by default; turn ON
# together with APITRACE_DXMT_NATIVE_BUILD_DIR pointing at dxmt/build-gs-native.
option(APITRACE_BUILD_D3D_NATIVE_RETRACE "Enable native macOS D3D retrace via DXMT" OFF)
set(APITRACE_DXMT_NATIVE_BUILD_DIR "" CACHE PATH "DXMT build-gs-native directory")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Standalone D3D / DXGI headers vendored under third_party/d3d-headers/.
add_library(apitrace_d3d_headers INTERFACE)
target_include_directories(apitrace_d3d_headers
    INTERFACE
        ${PROJECT_SOURCE_DIR}/third_party/d3d-headers/directx
        ${PROJECT_SOURCE_DIR}/third_party/d3d-headers/windows
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
target_link_libraries(apitrace_core PRIVATE apitrace_nlohmann_json)

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

add_library(apitrace_platform_windows_d3d11 SHARED
    src/d3d11/src/d3d11_proxy.cpp
    src/d3d11/src/d3d11_capture_internal.cpp
)

add_library(apitrace_platform_windows_d3d12 SHARED
    src/d3d12/src/d3d12_proxy.cpp
)

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

target_include_directories(apitrace_platform_apple_metal
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
    PRIVATE
        ${APITRACE_PRIVATE_INCLUDE_DIRS}
)

target_link_libraries(apitrace_platform_windows_d3d11 PRIVATE apitrace_core)
target_link_libraries(apitrace_platform_windows_d3d12 PRIVATE apitrace_core)
target_link_libraries(apitrace_platform_apple_metal PRIVATE apitrace_core)

target_compile_features(apitrace_platform_windows_d3d11 PUBLIC cxx_std_17)
target_compile_features(apitrace_platform_windows_d3d12 PUBLIC cxx_std_17)
target_compile_features(apitrace_platform_apple_metal PUBLIC cxx_std_17)

if(WIN32 AND APITRACE_BUILD_WINDOWS_DLLS)
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES OUTPUT_NAME d3d11 PREFIX "")
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES OUTPUT_NAME d3d12 PREFIX "")
    if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_options(apitrace_platform_windows_d3d11 PRIVATE -Wl,--add-stdcall-alias)
        target_link_options(apitrace_platform_windows_d3d12 PRIVATE -Wl,--add-stdcall-alias)
    endif()
else()
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES EXCLUDE_FROM_ALL TRUE)
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

if(APITRACE_BUILD_TOOLS)
    add_executable(apitrace_retrace src/tools/retrace/src/main.cpp)
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
endif()

if(WIN32)
    target_link_libraries(apitrace_core PRIVATE d3d11 d3d12 dxgi user32)
    target_link_libraries(apitrace_platform_windows_d3d12 PRIVATE uuid)
endif()

install(TARGETS apitrace_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(APITRACE_BUILD_TOOLS)
    install(TARGETS apitrace_retrace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WIN32 AND APITRACE_BUILD_WINDOWS_DLLS)
    install(TARGETS apitrace_platform_windows_d3d11
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(TARGETS apitrace_platform_windows_d3d12
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
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
