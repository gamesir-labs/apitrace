cmake_minimum_required(VERSION 3.20)

project(apitrace VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(APITRACE_BUILD_TOOLS "Build command-line tools" ON)
option(APITRACE_BUILD_WINDOWS_DLLS "Build Windows proxy DLLs when supported" ON)
option(APITRACE_BUILD_METAL_BACKEND "Build Metal backend sources when supported" ON)

set(APITRACE_PUBLIC_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src/api/include
    ${PROJECT_SOURCE_DIR}/src/trace/include
    ${PROJECT_SOURCE_DIR}/src/capture/include
    ${PROJECT_SOURCE_DIR}/src/retrace/include
    ${PROJECT_SOURCE_DIR}/src/d3d11/include
    ${PROJECT_SOURCE_DIR}/src/d3d12/include
    ${PROJECT_SOURCE_DIR}/src/metal/include
)

add_library(apitrace_core
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
    src/metal/src/metal_diagnostics.cpp
    src/metal/src/metal_replay.cpp
    src/metal/src/metal_state.cpp
    src/metal/src/metal_trace.cpp
    src/metal/src/translation_trace_recorder.cpp
    src/capture/src/trace_session_state.cpp
    src/retrace/src/replay_session.cpp
    src/trace/src/trace_bundle_reader.cpp
    src/trace/src/trace_bundle_writer.cpp
    src/trace/src/translation_link_writer.cpp
    src/capture/src/trace_session.cpp
)

target_include_directories(apitrace_core
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
)

target_compile_features(apitrace_core PUBLIC cxx_std_17)

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

target_include_directories(apitrace_platform_windows_d3d11
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
)

target_include_directories(apitrace_platform_windows_d3d12
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
)

target_include_directories(apitrace_platform_apple_metal
    PUBLIC
        ${APITRACE_PUBLIC_INCLUDE_DIRS}
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
else()
    set_target_properties(apitrace_platform_windows_d3d11 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(apitrace_platform_windows_d3d12 PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(NOT APPLE OR NOT APITRACE_BUILD_METAL_BACKEND)
    set_target_properties(apitrace_platform_apple_metal PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(APITRACE_BUILD_TOOLS)
    add_executable(apitrace_retrace src/tools/retrace/src/main.cpp)
    set_target_properties(apitrace_retrace PROPERTIES OUTPUT_NAME retrace)
    target_link_libraries(apitrace_retrace PRIVATE apitrace_core)
    target_include_directories(apitrace_retrace PRIVATE ${APITRACE_PUBLIC_INCLUDE_DIRS})
    target_compile_features(apitrace_retrace PRIVATE cxx_std_17)
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
